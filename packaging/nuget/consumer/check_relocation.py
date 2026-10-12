#!/usr/bin/env python3
"""Out-of-tree Linux NuGet SDK relocation and negative consumer gate (#112).

Run only after the same-run package has been verified, unpacked, and consumed
successfully once. Never use a source/build-tree TurboAgent library or a
previously published TurboAgent package.
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
    "Praktor": "PRAKTOR_ROOT",
    "TurboScript": "TURBOSCRIPT_ROOT",
}


def execute(argv: list[str], *, should_fail: bool = False, failure_text: str | None = None,
            env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(argv, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env, check=False)
    if should_fail:
        if result.returncode == 0:
            raise AssertionError(f"unexpected acceptance of invalid SDK input: {argv}\n{result.stdout}")
        if failure_text and failure_text not in result.stdout:
            raise AssertionError(
                f"invalid case rejected for unexpected reason ({failure_text!r} absent):\n{result.stdout}"
            )
        print(f"expected rejection: {failure_text or argv[-1]}")
        return result
    if result.returncode != 0:
        raise RuntimeError(f"consumer command failed (exit {result.returncode}): {argv}\n{result.stdout}")
    print(f"PASS: {argv[0]} {argv[1] if len(argv) > 1 else ''}")
    return result


def main() -> None:
    workspace = Path(os.environ["GITHUB_WORKSPACE"]).resolve()
    temporary = Path(os.environ["RUNNER_TEMP"]).resolve()
    source = workspace / "packaging/nuget/consumer"
    original = temporary / "turboagent-consumer-package/sdk/linux-x64"
    relocated = temporary / "turboagent-relocated-112/sdk/linux-x64"
    if not (original / "turboagent-sdk-manifest.txt").is_file():
        raise RuntimeError(f"verified, unpacked original SDK missing: {original}")
    if relocated.exists():
        raise RuntimeError(f"relocation target unexpectedly exists: {relocated}")

    root_dirs: dict[str, str] = {}
    for package, name in ROOTS.items():
        root = Path(os.environ[name]).resolve()
        config = root / f"lib/cmake/{package}/{package}Config.cmake"
        if not config.is_file():
            raise RuntimeError(f"missing restored {package} config: {config}")
        root_dirs[f"{package}_DIR"] = str(config.parent)

    relocated.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(original), str(relocated))
    if original.exists() or not (relocated / "turboagent-sdk-manifest.txt").is_file():
        raise RuntimeError("relocation must remove original SDK installation")

    # Exported CMake targets must not embed the build workspace or the old
    # extraction location; any such reference defeats relocatable consumption.
    forbidden = (str(original), str(workspace / "build") + "/")
    for cmake_file in relocated.rglob("*.cmake"):
        config = cmake_file.read_text(encoding="utf-8", errors="replace")
        if any(marker in config for marker in forbidden):
            raise RuntimeError(f"build/extract path leaked into installed CMake export: {cmake_file}")

    args = {
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_TOOLCHAIN_FILE": str(
            Path(os.environ["VCPKG_CACHE_REPOSITORY_ROOT"]) / "cmake/QigaoVcpkgToolchain.cmake"
        ),
        "VCPKG_TARGET_TRIPLET": "x64-linux",
        "VCPKG_HOST_TRIPLET": "x64-linux",
        "VCPKG_INSTALLED_DIR": str(workspace / "vcpkg_installed/linux"),
        "CMAKE_PREFIX_PATH": str(workspace / "vcpkg_installed/linux/x64-linux"),
        "EXPECTED_WITH_PRAKTOR": "ON",
        "EXPECTED_RID": "linux-x64",
        "EXPECTED_TURBOAGENT_PREFIX": str(relocated),
        **root_dirs,
    }

    def configure(build: Path, overrides: dict[str, str] | None = None,
                  *, should_fail: bool = False, failure_text: str | None = None):
        actual = dict(args)
        actual.update(overrides or {})
        invocation = ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja"]
        invocation.extend(f"-D{name}={value}" for name, value in actual.items())
        return execute(invocation, should_fail=should_fail, failure_text=failure_text)

    relocated_build = temporary / "turboagent-consumer-relocated-build"
    configure(relocated_build)
    execute(["cmake", "--build", str(relocated_build)])
    clean_env = dict(os.environ)
    clean_env.pop("LD_LIBRARY_PATH", None)
    execute(["ctest", "--test-dir", str(relocated_build), "--output-on-failure"], env=clean_env)

    executable = relocated_build / "turboagent_installed_consumer"
    loader_file = relocated_build / "runtime-library-path.txt"
    loader_dirs = loader_file.read_text(encoding="utf-8").strip()
    if not loader_dirs or not executable.is_file():
        raise RuntimeError("relocated executable or explicit deployment paths missing")
    if str(relocated) not in loader_dirs or str(original) in loader_dirs:
        raise RuntimeError("relocated runtime loader paths reference wrong SDK")
    loader_env = dict(clean_env, LD_LIBRARY_PATH=loader_dirs)
    linked = execute(["ldd", str(executable)], env=loader_env).stdout
    if "not found" in linked or str(relocated) not in linked:
        raise RuntimeError(f"relocated consumer did not resolve installed SDK libraries:\n{linked}")

    configure(temporary / "negative-original-prefix",
              {"EXPECTED_TURBOAGENT_PREFIX": str(original)},
              should_fail=True, failure_text="An exact unpacked SDK prefix is required")
    configure(temporary / "negative-wrong-rid",
              {"EXPECTED_RID": "windows-x64"},
              should_fail=True, failure_text="Consumer RID mismatch")
    configure(temporary / "negative-missing-salts",
              {"Salts_DIR": str(temporary / "missing-salts-config")},
              should_fail=True, failure_text="Missing exact Salts package config")

    corrupted = temporary / "corrupt-installed-sdk"
    shutil.copytree(relocated, corrupted, symlinks=True)
    broken_libs = sorted((corrupted / "lib").glob("libruntime_core.so*"))
    if not broken_libs:
        raise RuntimeError("expected installed runtime_core library absent from Linux package")
    for library in broken_libs:
        library.unlink()
    bad_build = temporary / "negative-broken-import"
    outcome = configure(bad_build, {"EXPECTED_TURBOAGENT_PREFIX": str(corrupted)})
    # The exported import may fail at configure or link, or during CTest's
    # loader resolution. Any of those is an acceptable fail-closed result;
    # succeeding at all stages is an acceptance bug.
    if outcome.returncode == 0:
        outcome = execute(["cmake", "--build", str(bad_build)])  # may fail
        if outcome.returncode == 0:
            outcome = execute(["ctest", "--test-dir", str(bad_build), "--output-on-failure"],
                              env=clean_env)
    if outcome.returncode == 0:
        raise AssertionError("consumer accepted a missing packaged runtime_core library")
    print("PASS: corrupt installed binary rejected")

    print("PASS: relocated SDK linked and executed; negative original/RID/Salts/library cases rejected")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, RuntimeError, OSError, subprocess.SubprocessError) as exc:
        print(f"installed NuGet relocation/negative acceptance FAILED: {exc}", file=sys.stderr)
        raise SystemExit(1)
