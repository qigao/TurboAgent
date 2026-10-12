#!/usr/bin/env python3
"""Verify immutable published Salts/SaltsUtils/CHttp NuGet bytes before use.

The three native SDK packages form one ABI epoch. No dynamic/latest selectors,
source checkouts, suppression, fallback package roots, or repackaged candidates.
Other unrelated tool SDKs retain their own selection rules.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from xml.etree import ElementTree
import zipfile


SIX_RIDS = ("linux-x64", "linux-arm64", "windows-x64", "macos-arm64",
            "android-arm64-v8a", "ios-arm64")
FIVE_RIDS = ("linux-x64", "windows-x64", "macos-arm64",
             "android-arm64-v8a", "ios-arm64")
EXPECTED = {
    "qigao/salts": {
        "id": "Salts.Native", "version": "2.3.0-rc.15",
        "commit": "21bdb1145fbb2b39ee3fa644173ec037adba7b87",
        "sha256": "fa73229c7b2506b393fe779bb56f8d42c387a79d30db54c0cf17b408b7ba7ad1",
        "rids": SIX_RIDS, "manifest": "salts-sdk-manifest.txt",
        "config": "lib/cmake/Salts/SaltsConfig.cmake",
    },
    "qigao/salts-utils": {
        "id": "SaltsUtils.Native", "version": "4.3.0-rc.10",
        "commit": "1410c9fd479f9faab31503523d8e31cc506b5b25",
        "sha256": "b4e5207b9e1f49fab6bddfcd36a4f27d9e99cabe09c28b41913c26f20e366491",
        "rids": SIX_RIDS, "manifest": "salts-utils-sdk-manifest.txt",
        "config": "lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake",
    },
    "qigao/chttp": {
        "id": "CHttp.Native", "version": "2.1.0-rc.5",
        "commit": "251d9ca60b1526ceab9d02f7f68e18fe060ce6f5",
        "sha256": "a627859d2ebd20e5edb6dad8b82da93dc4781da5f0200cb3f6420633d7389ae0",
        "rids": FIVE_RIDS, "manifest": "chttp-sdk-manifest.txt",
        "config": "lib/cmake/Chttp/ChttpConfig.cmake",
    },
}


def require(ok: bool, message: str) -> None:
    if not ok:
        raise ValueError(message)


def verify_release(repo: str, release: dict, tag_ref: dict) -> None:
    spec = EXPECTED[repo]
    tag = "v" + spec["version"]
    require(release.get("tag_name") == tag and release.get("draft") is False
            and release.get("prerelease") is True,
            f"{repo}: wrong published prerelease identity")
    require(tag_ref.get("ref") == "refs/tags/" + tag
            and tag_ref.get("object", {}).get("type") == "commit"
            and tag_ref["object"].get("sha") == spec["commit"],
            f"{repo}: immutable release Tag does not resolve to exact source")
    source = release.get("target_commitish")
    require(source in (spec["commit"], tag),
            f"{repo}: release target_commitish differs from verified source")
    expected_name = spec["id"] + "." + spec["version"] + ".nupkg"
    assets = [a for a in release.get("assets", []) if a.get("name") == expected_name]
    require(len(assets) == 1, f"{repo}: missing/ambiguous official NuGet asset")
    asset = assets[0]
    require(asset.get("state") == "uploaded"
            and asset.get("digest") == "sha256:" + spec["sha256"]
            and isinstance(asset.get("size"), int) and asset["size"] > 0,
            f"{repo}: GitHub Release asset digest/state mismatch")


def verify_archive(repo: str, archive: Path, expected_digest: str | None = None) -> None:
    spec = EXPECTED[repo]
    expected_name = spec["id"] + "." + spec["version"] + ".nupkg"
    require(archive.name.lower() == expected_name.lower(),
            f"{repo}: unexpected native NuGet filename")
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    require(digest == (expected_digest or spec["sha256"]),
            f"{repo}: downloaded NuGet bytes do not match immutable Release SHA-256")
    with zipfile.ZipFile(archive) as z:
        names = set(z.namelist())
        expected_manifests = {f"sdk/{rid}/{spec['manifest']}" for rid in spec["rids"]}
        manifests = {name for name in names
                     if name.endswith("/" + spec["manifest"])}
        require(manifests == expected_manifests,
                f"{repo}: SDK RID manifests missing or unexpected: {manifests ^ expected_manifests}")
        nuspec = [name for name in names if name.endswith(".nuspec")]
        require(len(nuspec) == 1, f"{repo}: NuGet metadata missing/ambiguous")
        meta = {el.tag.rsplit("}", 1)[-1]: el.text
                for el in ElementTree.fromstring(z.read(nuspec[0])).iter()
                if el.tag.rsplit("}", 1)[-1] in ("id", "version")}
        require((meta.get("id"), meta.get("version")) ==
                (spec["id"], spec["version"]), f"{repo}: NuGet identity/version drift")
        for rid in spec["rids"]:
            prefix = f"sdk/{rid}/"
            require(prefix + spec["config"] in names, f"{repo}/{rid}: installed CMake Config missing")
            fields = dict(line.split("=", 1) for line in
                          z.read(prefix + spec["manifest"]).decode("utf-8-sig").splitlines()
                          if "=" in line)
            for key, want in (("version", spec["version"]),
                              ("commit", spec["commit"]), ("profile", "release")):
                require(fields.get(key) == want,
                        f"{repo}/{rid}: {key} provenance mismatch")
            if repo == "qigao/salts-utils":
                require(fields.get("salts_version") == EXPECTED["qigao/salts"]["version"],
                        f"{repo}/{rid}: incorrect published Salts ABI dependency")
            if repo == "qigao/salts":
                header = z.read(prefix + "include/salts/thread.h")
                if rid == "windows-x64":
                    require(b"volatile int state;" in header and
                            b"SALTS_ONCE_INIT {0}" in header,
                            f"{repo}/{rid}: wrong native Windows once ABI")
                else:
                    require(b"pthread_once_t native;" in header,
                            f"{repo}/{rid}: old/non-native POSIX once ABI")
            if repo == "qigao/chttp":
                targets = z.read(prefix + "lib/cmake/Chttp/ChttpTargets.cmake")
                require(b"CHttp::Server" in targets and b"CHttp::Client" in targets,
                        f"{repo}/{rid}: Server/Client installed targets absent")
                require(b"CHttp::RpcService" not in targets,
                        f"{repo}/{rid}: forbidden standalone RpcService export")
                for header in ("http_server/rpc.h", "http_server/rpc_service.h",
                               "http_client/rpc.h", "http_client/rpc_service.h"):
                    require(prefix + "include/" + header in names,
                            f"{repo}/{rid}: public Owner RPC header missing")


def verify_installed(repo: str, extracted: Path, rid: str) -> None:
    spec = EXPECTED[repo]
    require(rid in spec["rids"], f"{repo}: unsupported target RID {rid}")
    root = extracted / "sdk" / rid
    require((root / spec["config"]).is_file(), f"{repo}/{rid}: installed SDK Config missing")
    require((root / spec["manifest"]).is_file(), f"{repo}/{rid}: extracted manifest missing")


def gh_api(endpoint: str) -> dict:
    return json.loads(subprocess.check_output(["gh", "api", endpoint], text=True))


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--repo", choices=sorted(EXPECTED), required=True)
    p.add_argument("--archive", type=Path, required=True)
    p.add_argument("--extracted", type=Path, required=True)
    p.add_argument("--rid", required=True)
    a = p.parse_args()
    tag = "v" + EXPECTED[a.repo]["version"]
    verify_release(
        a.repo,
        gh_api(f"repos/{a.repo}/releases/tags/{tag}"),
        gh_api(f"repos/{a.repo}/git/ref/tags/{tag}"),
    )
    verify_archive(a.repo, a.archive)
    verify_installed(a.repo, a.extracted, a.rid)
    print(f"QUALIFIED exact published {a.repo}@{tag} {EXPECTED[a.repo]['commit']}"
          f" sha256={EXPECTED[a.repo]['sha256']} rid={a.rid}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, KeyError, OSError, zipfile.BadZipFile,
            subprocess.CalledProcessError) as exc:
        print(f"Published SDK release rejected: {exc}", file=sys.stderr)
        sys.exit(1)
