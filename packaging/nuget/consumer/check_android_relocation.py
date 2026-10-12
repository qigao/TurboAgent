#!/usr/bin/env python3
"""Android ARM64 exact-run NuGet SDK relocation and negative cross-link gate.

Runs on a Linux *host* with Android NDK. No Android executable is executed.
The source SDK is the independently verified, unpacked current-run NuGet
artifact; first-party dependency SDKs are explicitly restored per RID.
"""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys

SDK_ENV = {
    "Salts": "SALTS_ROOT",
    "SaltsUtils": "SALTS_UTILS_ROOT",
    "Chttp": "CHTTP_ROOT",
    "TurboWasm": "TURBOWASM_ROOT",
    "Praktor": "PRAKTOR_ROOT",
    "TurboScript": "TURBOSCRIPT_ROOT",
}
RID = "android-arm64-v8a"


def run(argv: list[str], *, reject: str | None = None,
        may_fail: bool = False) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            encoding="utf-8", errors="replace", text=True,
                            check=False)
    if reject is not None:
        if result.returncode == 0 or reject.lower() not in result.stdout.lower():
            raise RuntimeError(
                f"negative test must fail for {reject!r}: status={result.returncode}\n"
                f"{result.stdout}"
            )
        print(f"PASS: rejected invalid Android consumer ({reject})", flush=True)
    elif result.returncode and not may_fail:
        raise RuntimeError(
            f"unexpected Android consumer failure {result.returncode}: {argv}\n{result.stdout}"
        )
    elif not may_fail:
        print(f"PASS: {argv[0]} {argv[1] if len(argv) > 1 else ''}", flush=True)
    return result


def main() -> None:
    if not sys.platform.startswith("linux"):
        raise RuntimeError("Android NDK consumer gate requires Linux host")
    workspace = Path(os.environ["GITHUB_WORKSPACE"]).resolve()
    temporary = Path(os.environ["RUNNER_TEMP"]).resolve()
    source = workspace / "packaging/nuget/consumer"
    original = temporary / "turboagent-consumer-package/sdk" / RID
    relocated = temporary / "turboagent-relocated-112-android/sdk" / RID
    if not (original / "turboagent-sdk-manifest.txt").is_file():
        raise RuntimeError(f"verified current-run Android SDK missing: {original}")
    if relocated.exists():
        raise RuntimeError("relocated SDK destination must not preexist")
    ndk = Path(os.environ["ANDROID_NDK_HOME"]).resolve()
    chainload = ndk / "build/cmake/android.toolchain.cmake"
    if not chainload.is_file():
        raise RuntimeError(f"missing NDK toolchain: {chainload}")

    deps: dict[str, str] = {}
    for package, key in SDK_ENV.items():
        root = Path(os.environ[key]).resolve()
        config = root / "lib/cmake" / package / (package + "Config.cmake")
        if not config.is_file() or RID not in root.parts:
            raise RuntimeError(f"missing exact {RID} dependency: {config}")
        deps[package + "_DIR"] = config.parent.as_posix()

    relocated.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(original), str(relocated))
    if original.exists():
        raise RuntimeError("original extracted Android SDK remains after relocation")
    manifest = relocated / "turboagent-sdk-manifest.txt"
    if "rid=android-arm64-v8a" not in manifest.read_text(encoding="utf-8"):
        raise RuntimeError("moved SDK contains unexpected RID manifest")

    # An install-tree export must not be linked to the original extraction or
    # source build tree. Such references invalidate relocation even if link works.
    old_path = original.as_posix()
    workspace_build = (workspace / "build").as_posix() + "/"
    for cmake_file in relocated.rglob("*.cmake"):
        contents = cmake_file.read_text(encoding="utf-8", errors="replace").replace("\\", "/")
        if old_path in contents or workspace_build in contents:
            raise RuntimeError(f"nonrelocatable CMake export: {cmake_file}")

    installed = workspace / "vcpkg_installed/consumer-android"
    config = {
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_SYSTEM_NAME": "Android",
        "ANDROID_ABI": "arm64-v8a",
        "ANDROID_PLATFORM": "android-26",
        "ANDROID_STL": "c++_shared",
        "CMAKE_TOOLCHAIN_FILE": (
            Path(os.environ["VCPKG_CACHE_REPOSITORY_ROOT"])
            / "cmake/QigaoVcpkgToolchain.cmake"
        ).as_posix(),
        "VCPKG_CHAINLOAD_TOOLCHAIN_FILE": chainload.as_posix(),
        "VCPKG_TARGET_TRIPLET": "arm64-android",
        "VCPKG_HOST_TRIPLET": "x64-linux",
        "VCPKG_INSTALLED_DIR": installed.as_posix(),
        "CMAKE_PREFIX_PATH": (installed / "arm64-android").as_posix(),
        "EXPECTED_WITH_PRAKTOR": "ON",
        "EXPECTED_RID": RID,
        "EXPECTED_TURBOAGENT_PREFIX": relocated.as_posix(),
        **deps,
    }

    def configure(build: Path, override: dict[str, str] | None = None,
                  *, reject: str | None = None, may_fail=False) -> subprocess.CompletedProcess[str]:
        args = dict(config)
        args.update(override or {})
        cmd = ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja"]
        cmd.extend(f"-D{key}={value}" for key, value in args.items())
        return run(cmd, reject=reject, may_fail=may_fail)

    build = temporary / "turboagent-relocated-112-android-build"
    configure(build)
    run(["cmake", "--build", str(build)])
    exe = build / "turboagent_installed_consumer"
    if not exe.is_file():
        raise RuntimeError("relocated Android ARM64 cross-linked consumer is missing")
    header = run(["readelf", "-h", str(exe)]).stdout
    dynamic = run(["readelf", "-d", str(exe)]).stdout
    (temporary / "android-relocated-consumer-elf-header.txt").write_text(
        header, encoding="utf-8"
    )
    (temporary / "android-relocated-consumer-needed.txt").write_text(
        dynamic, encoding="utf-8"
    )
    if "Machine:                           AArch64" not in header:
        raise RuntimeError(f"unexpected target architecture (not AArch64):\n{header}")
    if "libruntime_core.so" not in dynamic or "libturbo_runtime.so" not in dynamic:
        raise RuntimeError(f"installed runtime shared library dependencies absent:\n{dynamic}")
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    if old_path in cache or relocated.as_posix() not in cache:
        raise RuntimeError("cross build still references old SDK prefix or not relocated SDK")

    configure(temporary / "android-negative-old-prefix",
              {"EXPECTED_TURBOAGENT_PREFIX": original.as_posix()},
              reject="An exact unpacked SDK prefix is required")
    configure(temporary / "android-negative-wrong-rid",
              {"EXPECTED_RID": "linux-x64"},
              reject="Consumer RID mismatch")
    configure(temporary / "android-negative-missing-salts",
              {"Salts_DIR": (temporary / "missing-salts-config").as_posix()},
              reject="Missing exact Salts package config")
    configure(temporary / "android-negative-wrong-abi",
              {"ANDROID_ABI": "x86_64"},
              reject="Consumer Android ABI mismatch")

    corrupt = temporary / "android-corrupt-installed-sdk"
    shutil.copytree(relocated, corrupt, symlinks=True)
    core_libraries = sorted((corrupt / "lib").glob("libruntime_core.so*"))
    if not core_libraries:
        raise RuntimeError("packaged Android runtime_core library missing before corruption")
    for path in core_libraries:
        path.unlink()
    corrupt_build = temporary / "android-negative-missing-core-library"
    outcome = configure(corrupt_build,
                        {"EXPECTED_TURBOAGENT_PREFIX": corrupt.as_posix()},
                        may_fail=True)
    if outcome.returncode == 0:
        outcome = run(["cmake", "--build", str(corrupt_build)], may_fail=True)
    if outcome.returncode == 0 or "runtime_core" not in outcome.stdout.lower():
        raise RuntimeError(
            "cross-link must reject removed runtime_core library for the expected reason:\n"
            + outcome.stdout
        )
    print("PASS: removed Android native runtime_core rejected at configure/link")
    print("PASS: Android ARM64 relocated NuGet SDK cross-link, ELF and negative gates")
    print("No Android device/runtime execution attempted")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        print(f"Android ARM64 NuGet relocation FAILED: {exc}", file=sys.stderr)
        raise SystemExit(1)
