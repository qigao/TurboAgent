#!/usr/bin/env python3
"""Ask canonical vcpkg for exact package/ABI references, then download with curl."""
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
preset = sys.argv[1]
if preset not in {"ci-linux-release", "ci-windows-release", "ci-macos-release", "ci-android-release"}:
    raise SystemExit("unsupported CI preset")
config = root / "build/cache-preflight" / preset / "packages.config"
config.parent.mkdir(parents=True, exist_ok=True)
config.unlink(missing_ok=True)
# Same preset, manifest, installed root, triplets, chainload and compiler as the
# real configure. Only the binary directory differs; no dependency is built.
command = ["cmake", "--preset", preset, "-B", str(config.parent),
           "-DTURBOAGENT_CACHE_PREFLIGHT=ON",
           f"-DVCPKG_INSTALL_OPTIONS=--dry-run;--x-write-nuget-packages-config={config.as_posix()}"]
subprocess.run(command, cwd=root, check=True)
if not config.is_file():
    raise SystemExit("canonical vcpkg did not write packages.config")
subprocess.run([sys.executable, str(root / "cmake/ci/github-packages.py"), "cache",
                str(config), os.environ["VCPKG_BINARY_CACHE"]], check=True)
