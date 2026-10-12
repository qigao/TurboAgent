#!/usr/bin/env python3
"""Fail-closed inspection of the exact-run TurboAgent.Native NuGet artifact.

This gate checks package structure/provenance. It does NOT substitute for an
independent compile/link/run consumer (tracked separately in issue #112).
"""
import argparse
import pathlib
import re
import sys
import zipfile
from collections import Counter

RIDS = ("linux-x64", "windows-x64", "macos-arm64", "android-arm64-v8a")
BINARY_SUFFIXES = (".so", ".a", ".dylib", ".dll", ".lib")
REQUIRED_META = {"version", "commit", "rid", "praktor_tools"}


def validate(package: pathlib.Path, sha: str, version: str) -> None:
    if not re.fullmatch(r"[0-9a-fA-F]{40}", sha):
        raise ValueError("expected a full 40-character commit SHA")
    if not package.is_file():
        raise ValueError(f"package does not exist: {package}")
    with zipfile.ZipFile(package) as archive:
        entries = archive.infolist()
        paths = [item.filename.replace("\\", "/") for item in entries]
        duplicates = [name for name, count in Counter(paths).items() if count > 1]
        if duplicates:
            raise ValueError(f"duplicate archive entries: {duplicates}")
        for item, name in zip(entries, paths):
            path = pathlib.PurePosixPath(name)
            if name.startswith("/") or ".." in path.parts or "\\" in item.filename:
                raise ValueError(f"unsafe archive path: {name}")
            if (item.external_attr >> 16) & 0o170000 == 0o120000:
                raise ValueError(f"symlink not permitted: {name}")
        package_id = f"TurboAgent.Native.{version}.nupkg"
        if package.name.lower() != package_id.lower():
            raise ValueError(f"unexpected package filename: {package.name}")
        for rid in RIDS:
            prefix = f"sdk/{rid}/"
            members = [name for name in paths if name.startswith(prefix) and not name.endswith("/")]
            if not members:
                raise ValueError(f"missing RID tree: {rid}")
            manifest_name = prefix + "turboagent-sdk-manifest.txt"
            if manifest_name not in members:
                raise ValueError(f"missing provenance manifest: {rid}")
            raw = archive.read(manifest_name).decode("utf-8")
            metadata = {}
            for line in raw.splitlines():
                if "=" not in line:
                    raise ValueError(f"malformed manifest line for {rid}")
                key, value = line.split("=", 1)
                if key in metadata:
                    raise ValueError(f"duplicate manifest field: {rid}/{key}")
                metadata[key] = value
            if not REQUIRED_META.issubset(metadata):
                raise ValueError(f"missing manifest fields for {rid}")
            if metadata["commit"].lower() != sha.lower():
                raise ValueError(f"wrong commit for {rid}: {metadata['commit']}")
            if metadata["version"] != version.split("-ci.")[0]:
                raise ValueError(f"version mismatch for {rid}: {metadata['version']}")
            if metadata["rid"] != rid:
                raise ValueError(f"RID mismatch: {rid}")
            if not any("/include/" in name for name in members):
                raise ValueError(f"missing public include directory: {rid}")
            if not any(name.lower().endswith("turboagentconfig.cmake") for name in members):
                raise ValueError(f"missing CMake package config: {rid}")
            binaries = [name for name in members if name.lower().endswith(BINARY_SUFFIXES)]
            if not binaries:
                raise ValueError(f"missing native libraries: {rid}")
            if any(archive.getinfo(name).file_size == 0 for name in binaries):
                raise ValueError(f"empty native library: {rid}")
            print(f"validated {rid}: {len(members)} files, {len(binaries)} native libraries, sha={sha[:12]}")
        extra = sorted({name.split("/")[1] for name in paths
                        if name.startswith("sdk/") and len(name.split("/")) > 2}
                       - set(RIDS))
        if extra:
            raise ValueError(f"unexpected platform SDK trees: {extra}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("package", type=pathlib.Path)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--package-version", required=True)
    args = parser.parse_args()
    try:
        validate(args.package, args.commit, args.package_version)
    except (ValueError, OSError, zipfile.BadZipFile, UnicodeError) as exc:
        print(f"NuGet package validation FAILED: {exc}", file=sys.stderr)
        return 1
    print("NuGet package structure/provenance PASS (not a runtime consumer test)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
