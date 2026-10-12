#!/usr/bin/env python3
"""Restore an immutable, explicitly pinned six-SDK Linux dependency graph.

Independent of the release job's "latest published" selection.  No cache of
previous TurboAgent packages, no source-tree or fallback dependency path.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import sys
import zipfile

RID = "linux-x64"
SALTS_SOURCE_SHA = "21bdb1145fbb2b39ee3fa644173ec037adba7b87"
PACKAGES = (
    ("SALTS_ROOT", "qigao/salts", "v2.3.0-rc.15",
     "Salts.Native.*.nupkg", "Salts"),
    ("SALTS_UTILS_ROOT", "qigao/salts-utils", "v4.3.0-rc.10",
     "SaltsUtils.Native.*.nupkg", "SaltsUtils"),
    ("CHTTP_ROOT", "qigao/chttp", "v2.1.0-rc.4",
     "CHttp.Native.*.nupkg", "Chttp"),
    ("TURBOSCRIPT_ROOT", "qigao/TurboScript", "v3.0.10",
     "TurboScript.Native.*.nupkg", "TurboScript"),
    ("PRAKTOR_ROOT", "qigao/praktor", "v0.4.8",
     "Praktor.Native.*.nupkg", "Praktor"),
    ("TURBOWASM_ROOT", "qigao/turbowasm", "v0.2.1",
     "TurboWasm.Native.*.nupkg", "TurboWasm"),
)


def main() -> None:
    root = Path(os.environ["RUNNER_TEMP"]).resolve() / "queued-dso-exact-sdks"
    source = Path(os.environ["GITHUB_WORKSPACE"]).resolve() / "salts-source"
    envfile = Path(os.environ["GITHUB_ENV"]).resolve()
    actual = subprocess.check_output(
        ["git", "-C", str(source), "rev-parse", "HEAD"], text=True
    ).strip()
    if actual != SALTS_SOURCE_SHA:
        raise RuntimeError(
            f"Salts checkout does not match exact release commit: {actual}"
        )
    if root.exists():
        raise RuntimeError("SDK restore root must be empty on this isolated run")
    root.mkdir(parents=True)
    report = {"rid": RID, "salts_source_sha": actual, "packages": []}
    exports = []
    for var, repo, tag, pattern, cmake_package in PACKAGES:
        location = root / var.lower()
        download = location / "download"
        download.mkdir(parents=True)
        subprocess.run([
            "gh", "release", "download", tag, "--repo", repo,
            "--pattern", pattern, "--dir", str(download)
        ], check=True)
        candidates = sorted(download.glob("*.nupkg"))
        if len(candidates) != 1:
            raise RuntimeError(
                f"{repo}@{tag} needs exactly one package, got {len(candidates)}"
            )
        pkg = candidates[0]
        digest = hashlib.sha256(pkg.read_bytes()).hexdigest()
        extracted = location / "package"
        extracted.mkdir()
        with zipfile.ZipFile(pkg) as archive:
            entries = archive.infolist()
            for info in entries:
                name = PurePosixPath(info.filename)
                if (name.is_absolute() or ".." in name.parts or
                        "\\" in info.filename or info.filename.startswith("/")):
                    raise RuntimeError(f"unsafe package member in {repo}@{tag}")
            archive.extractall(extracted)
        prefix = extracted / "sdk" / RID
        config = prefix / "lib" / "cmake" / cmake_package / (
            cmake_package + "Config.cmake"
        )
        if not config.is_file():
            raise RuntimeError(f"missing exact {repo}@{tag} SDK config {config}")
        exports.append(f"{var}={prefix.as_posix()}\n")
        report["packages"].append({
            "env": var, "repo": repo, "tag": tag,
            "asset": pkg.name, "sha256": digest,
            "cmake_config": str(config.relative_to(prefix)),
        })
        print(f"PINNED {repo}@{tag} {RID} {digest}", flush=True)
    with envfile.open("a", encoding="utf-8") as stream:
        stream.writelines(exports)
    provenance = root / "exact-source-and-sdks.json"
    provenance.write_text(
        json.dumps(report, sort_keys=True, indent=2) + "\n", encoding="utf-8"
    )
    print(f"PINNED-SDK-PROVENANCE={provenance}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (KeyError, RuntimeError, OSError, subprocess.CalledProcessError,
            zipfile.BadZipFile) as exc:
        print(f"exact SDK restore refused: {exc}", file=sys.stderr)
        raise SystemExit(1)
