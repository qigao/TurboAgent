#!/usr/bin/env python3
"""Windows installed-NuGet relocation and deterministic reject tests (#112).

Invoke from an x64 VsDevCmd environment. This tests the exact-run unpacked SDK
and its explicitly restored providers, without a source/build-tree fallback.
"""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOTS = {
    "Salts": "SALTS_ROOT",
    "SaltsUtils": "SALTS_UTILS_ROOT",
    "Chttp": "CHTTP_ROOT",
    "TurboWasm": "TURBOWASM_ROOT",
}


def run(command: list[str], *, env: dict[str, str] | None = None,
        expect_failure: str | None = None, allow_failure: bool = False):
    result = subprocess.run(command, env=env, text=True, encoding="utf-8",
                            errors="replace", stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, check=False)
    if expect_failure is not None:
        if result.returncode == 0 or expect_failure.lower() not in result.stdout.lower():
            raise RuntimeError(
                f"negative case must reject for {expect_failure!r}: "
                f"status={result.returncode}\n{result.stdout}"
            )
        print(f"PASS: expected reject ({expect_failure})")
        return result
    if not allow_failure and result.returncode:
        raise RuntimeError(f"consumer command failed ({result.returncode}): "
                           f"{command}\n{result.stdout}")
    if not allow_failure:
        print(f"PASS: {command[0]} {command[1] if len(command) > 1 else ''}")
    return result


def main():
    if os.name != "nt":
        raise RuntimeError("Windows SDK relocation test requires a Windows host")
    workspace = Path(os.environ["GITHUB_WORKSPACE"]).resolve()
    temporary = Path(os.environ["RUNNER_TEMP"]).resolve()
    src = workspace / "packaging" / "nuget" / "consumer"
    original = temporary / "turboagent-consumer-package" / "sdk" / "windows-x64"
    relocated = temporary / "turboagent-relocated-112" / "sdk" / "windows-x64"
    if not (original / "turboagent-sdk-manifest.txt").is_file():
        raise RuntimeError("original exact-run NuGet installation missing")
    if relocated.exists():
        raise RuntimeError("relocation destination must be unused")
    roots = {}
    for package, var in ROOTS.items():
        root = Path(os.environ[var]).resolve()
        config = root / "lib" / "cmake" / package / (package + "Config.cmake")
        if not config.is_file():
            raise RuntimeError(f"missing explicit provider config: {config}")
        roots[package + "_DIR"] = config.parent.as_posix()

    relocated.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(original), str(relocated))
    if original.exists() or not (relocated / "turboagent-sdk-manifest.txt").is_file():
        raise RuntimeError("exact SDK move did not remove old installation path")
    original_mark = original.as_posix().lower()
    source_build_mark = (workspace / "build").as_posix().lower() + "/"
    for cmake_file in relocated.rglob("*.cmake"):
        exported = cmake_file.read_text(encoding="utf-8", errors="replace").replace("\\", "/").lower()
        if original_mark in exported or source_build_mark in exported:
            raise RuntimeError(f"non-relocatable installed CMake export: {cmake_file}")

    triplet = os.environ["QIGAO_VCPKG_WINDOWS_TRIPLET"]
    host = os.environ["QIGAO_VCPKG_WINDOWS_HOST_TRIPLET"]
    installed = workspace / "vcpkg_installed" / "consumer-windows"
    toolchain = Path(os.environ["VCPKG_CACHE_REPOSITORY_ROOT"]) / "cmake" / "QigaoVcpkgToolchain.cmake"
    if not toolchain.is_file():
        raise RuntimeError(f"missing approved toolchain: {toolchain}")

    defs = {
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_TOOLCHAIN_FILE": toolchain.as_posix(),
        "VCPKG_TARGET_TRIPLET": triplet,
        "VCPKG_HOST_TRIPLET": host,
        "VCPKG_INSTALLED_DIR": installed.as_posix(),
        "CMAKE_PREFIX_PATH": (installed / triplet).as_posix(),
        "EXPECTED_WITH_PRAKTOR": "OFF",
        "EXPECTED_RID": "windows-x64",
        "EXPECTED_TURBOAGENT_PREFIX": relocated.as_posix(),
        **roots,
    }
    cmake = shutil.which("cmake")
    ctest = shutil.which("ctest")
    if not cmake or not ctest:
        raise RuntimeError("MSVC toolchain must provide cmake and ctest")

    def configure(build: Path, changes: dict[str, str] | None = None, *,
                  reject: str | None = None, allow_failure=False):
        params = dict(defs)
        params.update(changes or {})
        args = [cmake, "-S", str(src), "-B", str(build), "-G", "Ninja"]
        args.extend(f"-D{name}={value}" for name, value in params.items())
        return run(args, expect_failure=reject, allow_failure=allow_failure)

    build = temporary / "turboagent-relocated-112-windows-build"
    configure(build)
    run([cmake, "--build", str(build)])
    exe = build / "turboagent_installed_consumer.exe"
    if not exe.is_file():
        raise RuntimeError("relocated MSVC consumer executable missing")

    sdk_bin = relocated / "bin"
    if not sdk_bin.is_dir():
        raise RuntimeError(f"relocated TurboAgent DLL directory missing: {sdk_bin}")
    deployed = [sdk_bin]
    for var in ROOTS.values():
        optional_bin = Path(os.environ[var]) / "bin"
        if optional_bin.is_dir():
            deployed.append(optional_bin)
    vcpkg_bin = installed / triplet / "bin"
    if vcpkg_bin.is_dir():
        deployed.append(vcpkg_bin)
    system_root = Path(os.environ["SystemRoot"])
    deployed += [system_root / "System32", system_root]
    clean_env = dict(os.environ)
    clean_env["PATH"] = os.pathsep.join(str(p) for p in deployed)
    run([ctest, "--test-dir", str(build), "--output-on-failure"], env=clean_env)
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    if original_mark in cache.replace("\\", "/").lower():
        raise RuntimeError("relocated build still references original SDK prefix")

    configure(temporary / "negative-win-old-prefix",
              {"EXPECTED_TURBOAGENT_PREFIX": original.as_posix()},
              reject="An exact unpacked SDK prefix is required")
    configure(temporary / "negative-win-wrong-rid",
              {"EXPECTED_RID": "linux-x64"},
              reject="Consumer RID mismatch")
    configure(temporary / "negative-win-missing-salts",
              {"Salts_DIR": (temporary / "missing-salts-config").as_posix()},
              reject="Missing exact Salts package config")

    broken = temporary / "corrupt-win-installed-sdk"
    shutil.copytree(relocated, broken)
    candidates = [
        p for p in broken.rglob("*")
        if p.is_file() and p.name.lower() in ("turbo_runtime.lib", "turbo_runtime.dll")
    ]
    if not candidates:
        raise RuntimeError("required packaged turbo_runtime import/DLL artifact not found")
    for path in candidates:
        path.unlink()

    invalid_build = temporary / "negative-win-broken-runtime"
    outcome = configure(invalid_build,
                        {"EXPECTED_TURBOAGENT_PREFIX": broken.as_posix()},
                        allow_failure=True)
    if outcome.returncode == 0:
        outcome = run([cmake, "--build", str(invalid_build)], allow_failure=True)
        if outcome.returncode == 0:
            bad_deps = [broken / "bin"] + deployed[1:]
            bad_env = dict(clean_env, PATH=os.pathsep.join(str(p) for p in bad_deps))
            outcome = run([ctest, "--test-dir", str(invalid_build), "--output-on-failure"],
                          env=bad_env, allow_failure=True)
    if outcome.returncode == 0 or "turbo_runtime" not in outcome.stdout.lower():
        raise RuntimeError(f"broken installed native library was not correctly rejected:\n{outcome.stdout}")
    print("PASS: missing installed turbo_runtime artifact rejected")
    print("PASS: Windows SDK relocation, MSVC link, runtime smoke and negative cases")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"Windows NuGet relocation test FAILED: {exc}", file=sys.stderr)
        raise SystemExit(1)
