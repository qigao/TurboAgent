#!/usr/bin/env python3
"""Build Salts Plugin/ComponentPlugin/Loader from the pinned *same* rc.15 source.

The resulting CMake-installed SDK replaces the released Salts runtime ONLY
inside a dedicated sanitizer process. No release artifact is overwritten.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

SOURCE_SHA = "21bdb1145fbb2b39ee3fa644173ec037adba7b87"
REQUIRED = (
    "plugin/src/plugin.c",
    "plugin/src/plugin_registry.c",
    "plugin/src/plugin_posix.c",
    "component-plugin/src/component_plugin.c",
    "component-plugin/src/component_plugin_runtime.c",
    "cmeta/src/interface.c",
    "cmeta/src/object.c",
    "cmeta/src/data.c",
    "cmeta/src/function.c",
    "platform/src/thread_posix.c",
)
ARCHIVES = ("libcmeta_plugin.a", "libsalts_component_plugin.a")


def command(*args: str) -> None:
    print("SOURCE BUILD: " + " ".join(shlex.quote(a) for a in args), flush=True)
    subprocess.run(args, check=True)


def main() -> None:
    if len(sys.argv) != 2 or sys.argv[1] not in ("thread", "address"):
        raise RuntimeError("usage: build_salts_source.py <thread|address>")
    kind = sys.argv[1]
    workspace = Path(os.environ["GITHUB_WORKSPACE"]).resolve()
    source = workspace / "salts-source"
    runner = Path(os.environ["RUNNER_TEMP"]).resolve()
    original = Path(os.environ["SALTS_ROOT"]).resolve()
    target = runner / ("queued-salts-instrumented-" + kind)
    build = workspace / "build" / ("salts-full-source-" + kind)
    toolchain = Path(os.environ["VCPKG_CACHE_REPOSITORY_ROOT"]).resolve() / (
        "cmake/QigaoVcpkgToolchain.cmake"
    )
    actual = subprocess.check_output(
        ["git", "-C", str(source), "rev-parse", "HEAD"], text=True,
    ).strip()
    if actual != SOURCE_SHA:
        raise RuntimeError(f"wrong Salts release source for full graph: {actual}")
    if target.exists() or build.exists():
        raise RuntimeError("source SDK build and install directories must be fresh")
    if not toolchain.is_file():
        raise RuntimeError("exact vcpkg-cache CMake toolchain was not restored")
    if not (original / "lib/cmake/Salts/SaltsConfig.cmake").is_file():
        raise RuntimeError("original SDK config missing before source replacement")

    # Explicit CMake arguments: upstream Salts already applies the same
    # sanitizer flags to all its targets via its own Sanitizers module.
    options = [
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DBUILD_TESTS=OFF",
        "-DENABLE_TESTS=OFF",
        "-DBUILD_EXAMPLES=OFF",
        "-DBUILD_BENCHMARKS=OFF",
        "-DCMETA_BUILD_BENCHMARKS=OFF",
        "-DNATIVE_IO_BUILD_BENCHMARKS=OFF",
        "-DCOROUTINE_BUILD_BENCHMARKS=OFF",
        "-DCNET_BUILD_BENCHMARKS=OFF",
        "-DCFLOW_BUILD_BENCHMARKS=OFF",
        "-DCONCURRENCY_BUILD_BENCHMARKS=OFF",
        "-DCMETA_BUILD_NATIVE_THUNKS=OFF",
        "-DCMAKE_SUPPRESS_REGENERATION=ON",
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        f"-DCMAKE_INSTALL_PREFIX={target}",
        "-DVCPKG_TARGET_TRIPLET=x64-linux",
        "-DVCPKG_HOST_TRIPLET=x64-linux",
        f"-DVCPKG_INSTALLED_DIR={workspace}/vcpkg_installed/salts-source-{kind}",
        "-DENABLE_SANITIZER_THREAD=" + ("ON" if kind == "thread" else "OFF"),
        "-DENABLE_SANITIZER_ADDRESS=" + ("ON" if kind == "address" else "OFF"),
        "-DENABLE_SANITIZER_UNDEFINED=" + ("ON" if kind == "address" else "OFF"),
    ]
    command("cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
            *options)
    command("cmake", "--build", str(build), "--parallel", "2")
    command("cmake", "--install", str(build), "--prefix", str(target))

    config = target / "lib/cmake/Salts/SaltsConfig.cmake"
    if not config.is_file():
        raise RuntimeError("source-built Salts has no installed CMake config")
    for rel in ("include/salts/plugin.h", "include/salts/component_plugin.h"):
        original_path = original / rel
        source_path = target / rel
        if not original_path.is_file() or not source_path.is_file():
            raise RuntimeError(f"public SDK ABI header missing: {rel}")
        if original_path.read_bytes() != source_path.read_bytes():
            raise RuntimeError(f"source-built vs released ABI header mismatch: {rel}")
    flag = "-fsanitize=thread" if kind == "thread" else "-fsanitize=address"
    commands = json.loads((build / "compile_commands.json").read_text(
        encoding="utf-8"
    ))
    covered = {}
    for rel in REQUIRED:
        path = (source / rel).resolve()
        found = []
        for row in commands:
            if Path(row["file"]).resolve() != path:
                continue
            args = row.get("arguments") or shlex.split(row["command"])
            if "-o" not in args:
                continue
            obj = (Path(row["directory"]) / args[args.index("-o") + 1]).resolve()
            if not obj.is_file():
                continue
            if flag not in args or "-fno-omit-frame-pointer" not in args:
                raise RuntimeError(f"Salts source compiled without {flag}: {rel}")
            if kind == "address" and "-fsanitize=undefined" not in args:
                raise RuntimeError(f"Salts source lacks UBSan: {rel}")
            found.append(str(obj.relative_to(build)))
        if not found:
            raise RuntimeError(f"required Salts source never built: {rel}")
        covered[rel] = found
        print(f"SOURCE INSTRUMENTED: {rel}", flush=True)

    for filename in ARCHIVES:
        archive = target / "lib" / filename
        if not archive.is_file():
            raise RuntimeError(f"source-built native Plugin archive absent: {archive}")
        result = subprocess.check_output(["nm", "-A", str(archive)], text=True)
        symbol = "__tsan_" if kind == "thread" else "__asan_"
        if symbol not in result:
            raise RuntimeError(f"source archive lacks sanitizer symbols: {archive}")
        print(f"SOURCE ARCHIVE: {filename}", flush=True)

    report = {
        "sanitizer": kind, "salts_source_commit": actual,
        "original_release_sdk": str(original),
        "installed_source_sdk": str(target),
        "source_objects": covered,
        "archives": list(ARCHIVES),
        "public_headers_byte_identical_to_rc15": True,
        "vcpkg_cache_contract": os.environ.get("VCPKG_CACHE_REVISION", "unset"),
        "vcpkg_scripts_revision": os.environ.get("VCPKG_SCRIPTS_REVISION", "unset"),
    }
    report_file = runner / ("queued-salts-source-" + kind + ".json")
    report_file.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    with Path(os.environ["GITHUB_ENV"]).open("a", encoding="utf-8") as stream:
        stream.write(f"SALTS_ROOT={target.as_posix()}\n")
        stream.write(f"SANITIZED_SALTS_ROOT={target.as_posix()}\n")
        stream.write(f"SALTS_SOURCE_BUILD_DIR={build.as_posix()}\n")
    print(f"INTEGRATED SALTS SOURCE SDK: {target}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.CalledProcessError,
            KeyError, ValueError) as exc:
        print(f"source-built Salts SDK rejected: {exc}", file=sys.stderr)
        raise SystemExit(1)
