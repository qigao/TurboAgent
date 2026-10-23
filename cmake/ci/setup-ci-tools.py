#!/usr/bin/env python3
"""Bootstrap the pinned shared vcpkg contract without its NuGet transport."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def run(*args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs).strip()


def main():
    infrastructure = Path(sys.argv[1]).resolve()
    workspace = Path(os.environ["GITHUB_WORKSPACE"]).resolve()
    temporary = Path(os.environ["RUNNER_TEMP"]).resolve()
    scripts = (infrastructure / "vcpkg-scripts-revision.txt").read_text().strip()
    tool = (infrastructure / "vcpkg-tool-version.txt").read_text().strip()
    contract = (infrastructure / "cache-contract-version.txt").read_text().strip()
    revision = run(sys.executable, str(infrastructure / "scripts/cache-contract-revision.py"), str(infrastructure))
    if not re.fullmatch(r"[0-9a-f]{40}", scripts) or not re.fullmatch(r"[0-9a-f]{64}", revision) or not contract.isdigit():
        raise ValueError("invalid canonical cache identity")
    root = temporary / f"qigao-vcpkg-{scripts}"
    # A fresh GitHub job owns this directory. Never reuse a shallow/promisor tree.
    if root.exists():
        raise ValueError(f"expected a fresh vcpkg directory: {root}")
    root.mkdir()
    run("git", "-C", str(root), "init")
    run("git", "-C", str(root), "remote", "add", "origin", "https://github.com/microsoft/vcpkg.git")
    run("git", "-C", str(root), "fetch", "--no-tags", "origin", scripts)
    run("git", "-C", str(root), "checkout", "--detach", "FETCH_HEAD")
    baselines = run(sys.executable, str(infrastructure / "scripts/vcpkg-required-commits.py"), str(infrastructure), str(workspace))
    for baseline in baselines.splitlines():
        if not re.fullmatch(r"[0-9a-f]{40}", baseline):
            raise ValueError("invalid baseline")
        exists = subprocess.run(["git", "-C", str(root), "cat-file", "-e", baseline + "^{commit}"], capture_output=True)
        if exists.returncode:
            run("git", "-C", str(root), "fetch", "--no-tags", "origin", baseline)
        run("git", "-C", str(root), "cat-file", "-e", baseline + ":versions/baseline.json")
    if run("git", "-C", str(root), "rev-parse", "HEAD") != scripts or run("git", "-C", str(root), "rev-parse", "--is-shallow-repository") != "false":
        raise ValueError("canonical scripts/history mismatch")
    bootstrap = ["cmd", "/c", str(root / "bootstrap-vcpkg.bat"), "-disableMetrics"] if os.name == "nt" else ["sh", str(root / "bootstrap-vcpkg.sh"), "-disableMetrics"]
    subprocess.run(bootstrap, cwd=root, check=True)
    exe = root / ("vcpkg.exe" if os.name == "nt" else "vcpkg")
    version = run(str(exe), "version").splitlines()[0].removeprefix("vcpkg package management program version ")
    if version != tool:
        raise ValueError(f"canonical tool mismatch: {version} != {tool}")
    assets = temporary / f"qigao-vcpkg-contract-v{contract}-{revision}"
    assets.mkdir()
    for directory in ("ports", "triplets"):
        shutil.copytree(infrastructure / directory, assets / directory)
    (assets / "cmake").mkdir()
    shutil.copy2(infrastructure / "cmake/QigaoVcpkgToolchain.cmake", assets / "cmake/QigaoVcpkgToolchain.cmake")
    cache = workspace / "build/vcpkg-binary-cache"
    cache.mkdir(parents=True, exist_ok=True)
    values = {
        "VCPKG_ROOT": root.as_posix(), "VCPKG_TOOL_REVISION": tool,
        "VCPKG_SCRIPTS_REVISION": scripts, "VCPKG_CACHE_CONTRACT_VERSION": contract,
        "VCPKG_CACHE_REVISION": revision, "VCPKG_CACHE_REPOSITORY_ROOT": assets.as_posix(),
        "VCPKG_OVERLAY_PORTS": (assets / "ports").as_posix(),
        "VCPKG_OVERLAY_TRIPLETS": (assets / "triplets").as_posix(),
        "QIGAO_VCPKG_TOOLCHAIN_FILE": (assets / "cmake/QigaoVcpkgToolchain.cmake").as_posix(),
        "QIGAO_VCPKG_WINDOWS_TRIPLET": "qigao-x64-windows-v145",
        "QIGAO_VCPKG_WINDOWS_HOST_TRIPLET": "qigao-x64-windows-v145",
        "VCPKG_BINARY_CACHE": cache.as_posix(), "VCPKG_BINARY_SOURCES": f"clear;files,{cache.as_posix()},read",
        "VCPKG_NUGET_REPOSITORY": "https://github.com/qigao/vcpkg-cache",
    }
    with open(os.environ["GITHUB_ENV"], "a", encoding="utf-8") as output:
        for key, value in values.items():
            output.write(f"{key}={value}\n")
    print(f"Canonical vcpkg contract v{contract} {revision}; scripts {scripts}; tool {tool}")


if __name__ == "__main__":
    main()
