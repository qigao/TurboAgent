#!/usr/bin/env python3
"""Fail-closed check of exact published Salts and SaltsUtils NuGet SDKs.

No model/media/vendor code is executed. This validates the *actual release
archives*, not a matching source tag alone. GitHub Actions downloads the
exact packages, hashes their bytes and then invokes this offline inspector.
"""
import argparse
import hashlib
from pathlib import Path
import sys
import zipfile

PINNED_PACKAGES = {
    "SaltsUtils.Native.4.3.0-rc.11.nupkg":
        ("0d15b3dba534b55a42069dc665070cf177062d0bd029b31639dfb2bb2d4bdf64", 29411473),
    "Salts.Native.2.3.0-rc.16.nupkg":
        ("0b8592cb0b27e91fb5a3ee2c5a888ca53c7593cf9eb8cd5adfbc38fac43659b7", 44257295),
}
PLAYBACK_RIDS = ("linux-x64", "windows-x64", "macos-arm64",
                 "android-arm64-v8a", "ios-arm64")
EXCLUDED_RIDS = ("linux-arm64",)


def check_archive(path: Path) -> set[str]:
    pin = PINNED_PACKAGES.get(path.name)
    if pin is None or not path.is_file() or path.is_symlink():
        raise ValueError("unexpected SDK release archive")
    digest = hashlib.sha256()
    seen = 0
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
            seen += len(block)
    if digest.hexdigest() != pin[0] or seen != pin[1]:
        raise ValueError("pinned published archive SHA256/size mismatch")
    with zipfile.ZipFile(path) as zipped:
        names = set(zipped.namelist())
        if len(names) != len(zipped.namelist()):
            raise ValueError("duplicate zip member")
        if any(name.startswith("/") or ".." in Path(name).parts for name in names):
            raise ValueError("unsafe archive member path")
        if path.name.startswith("SaltsUtils."):
            for rid in PLAYBACK_RIDS:
                prefix = f"sdk/{rid}/"
                required = (
                    prefix + "include/salts_playback.h",
                    prefix + "include/salts_playback_export.h",
                    prefix + "lib/cmake/SaltsUtils/SaltsUtilsTargets.cmake",
                    prefix + "lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake",
                )
                for name in required:
                    if name not in names:
                        raise ValueError(f"{rid}: release Playback header/CMake missing")
                exports = zipped.read(required[2]).decode("utf-8")
                if "Salts::Playback" not in exports:
                    raise ValueError(f"{rid}: release SDK lacks Salts::Playback export")
                candidates = [
                    name for name in names if name.startswith(prefix)
                    and "salts_playback" in name.lower()
                    and name.lower().endswith((".so", ".dylib", ".dll", ".lib", ".a"))
                ]
                if not candidates:
                    raise ValueError(f"{rid}: release playback link library missing")
            for rid in EXCLUDED_RIDS:
                if f"sdk/{rid}/include/salts_playback.h" in names:
                    raise ValueError("excluded linux-arm64 capture/playback unexpectedly present")
        else:
            for rid in PLAYBACK_RIDS:
                if f"sdk/{rid}/lib/cmake/Salts/SaltsConfig.cmake" not in names:
                    raise ValueError(f"{rid}: Salts dependency SDK config missing")
    return names


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--package-dir", type=Path, required=True)
    a = p.parse_args()
    try:
        if not a.package_dir.is_absolute():
            raise ValueError("package directory must be absolute")
        for filename in PINNED_PACKAGES:
            check_archive(a.package_dir / filename)
        print("PASS: exact published SaltsUtils rc.11 / Salts rc.16 SHA, Playback exports, five native RIDs")
        print("INFO: linux-arm64 release intentionally excludes Playback; no fallback")
        return 0
    except (OSError, ValueError, zipfile.BadZipFile, UnicodeError) as error:
        print("FAIL: published native SDK verification: " + str(error),
              file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
