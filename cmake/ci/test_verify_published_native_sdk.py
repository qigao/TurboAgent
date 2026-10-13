"""Offline positive/negative contracts for the published CHttp 2.1.0-rc.5 SDK graph."""
from __future__ import annotations

import hashlib
from pathlib import Path
import runpy
import tempfile
import unittest
import zipfile

verifier = runpy.run_path(
    str(Path(__file__).with_name("verify-published-native-sdk.py")),
    run_name="turboagent_sdk_verifier_tests")
SPECS = verifier["EXPECTED"]


def synthetic_archive(root: Path, repo: str, *, missing_rid: str = "",
                      legacy: bool = False, bad_dependency: bool = False) -> tuple[Path, str]:
    spec = SPECS[repo]
    file = root / (spec["id"] + "." + spec["version"] + ".nupkg")
    with zipfile.ZipFile(file, "w") as z:
        for rid in spec["rids"]:
            if rid == missing_rid:
                continue
            prefix = "sdk/" + rid + "/"
            facts = {
                "version": spec["version"],
                "commit": spec["commit"],
                "profile": "release",
            }
            if repo == "qigao/salts-utils":
                facts["salts_version"] = ("2.3.0-rc.14" if bad_dependency
                                          else SPECS["qigao/salts"]["version"])
            z.writestr(prefix + spec["manifest"],
                       "".join(f"{k}={v}\n" for k, v in facts.items()))
            z.writestr(prefix + spec["config"], "set(SDK_IMPORTED TRUE)\n")
            if repo == "qigao/salts":
                if rid == "windows-x64":
                    header = "volatile int state;\nSALTS_ONCE_INIT {0}\n"
                else:
                    header = "pthread_once_t native;\n"
                if legacy:
                    header = "non-native old once state\n"
                z.writestr(prefix + "include/salts/thread.h", header)
            if repo == "qigao/chttp":
                targets = "add_library(CHttp::Server INTERFACE)\nadd_library(CHttp::Client INTERFACE)\n"
                if legacy:
                    targets += "add_library(CHttp::RpcService INTERFACE)\n"
                z.writestr(prefix + "lib/cmake/Chttp/ChttpTargets.cmake", targets)
                for path in ("http_server/rpc.h", "http_server/rpc_service.h",
                             "http_client/rpc.h", "http_client/rpc_service.h"):
                    z.writestr(prefix + "include/" + path, "#pragma once\n")
        z.writestr(spec["id"] + ".nuspec",
                   "<package><metadata><id>" + spec["id"]
                   + "</id><version>" + spec["version"] + "</version></metadata></package>")
    return file, hashlib.sha256(file.read_bytes()).hexdigest()


class PublishedSDKVerifierTests(unittest.TestCase):
    def release(self, repo: str):
        spec = SPECS[repo]
        tag = "v" + spec["version"]
        return ({
            "tag_name": tag, "draft": False, "prerelease": True,
            "target_commitish": spec["commit"],
            "assets": [{"name": spec["id"] + "." + spec["version"] + ".nupkg",
                        "digest": "sha256:" + spec["sha256"],
                        "state": "uploaded", "size": 1200}],
        }, {"ref": "refs/tags/" + tag,
            "object": {"sha": spec["commit"], "type": "commit"}})

    def test_release_tag_sha_digest_and_channel_fail_closed(self):
        verify = verifier["verify_release"]
        for repo in SPECS:
            with self.subTest(repo=repo):
                release, tag = self.release(repo)
                verify(repo, release, tag)
                for field, wrong in (("draft", True), ("prerelease", False),
                                     ("tag_name", "v0.0.0"), ("target_commitish", "f" * 40)):
                    bad = dict(release, **{field: wrong})
                    with self.assertRaises(ValueError):
                        verify(repo, bad, tag)
                for field, wrong in (("sha", "f" * 40), ("type", "tag")):
                    bad_tag = dict(tag, object=dict(tag["object"], **{field: wrong}))
                    with self.assertRaises(ValueError):
                        verify(repo, release, bad_tag)
                for field, wrong in (("digest", "sha256:" + "0" * 64),
                                     ("state", "new"), ("size", 0)):
                    bad = dict(release, assets=[dict(release["assets"][0], **{field: wrong})])
                    with self.assertRaises(ValueError):
                        verify(repo, bad, tag)
                bad = dict(release, assets=release["assets"] * 2)
                with self.assertRaises(ValueError):
                    verify(repo, bad, tag)

    def test_archived_six_and_five_rid_real_zip_provenance(self):
        verify = verifier["verify_archive"]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for repo in SPECS:
                with self.subTest(repo=repo):
                    package, digest = synthetic_archive(root, repo)
                    verify(repo, package, digest)
                    with self.assertRaisesRegex(ValueError, "SHA-256"):
                        verify(repo, package, "0" * 64)

    def test_missing_rid_or_legacy_api_rejected_without_fallback(self):
        verify = verifier["verify_archive"]
        for repo, spec in SPECS.items():
            with self.subTest(repo=repo), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                package, digest = synthetic_archive(root, repo, missing_rid=spec["rids"][-1])
                with self.assertRaisesRegex(ValueError, "RID manifests"):
                    verify(repo, package, digest)
                if repo == "qigao/chttp" or repo == "qigao/salts":
                    package, digest = synthetic_archive(root, repo, legacy=True)
                    with self.assertRaises(ValueError):
                        verify(repo, package, digest)
                if repo == "qigao/salts-utils":
                    package, digest = synthetic_archive(root, repo, bad_dependency=True)
                    with self.assertRaisesRegex(ValueError, "ABI dependency"):
                        verify(repo, package, digest)

    def test_target_rid_and_extracted_config_are_required(self):
        verify = verifier["verify_installed"]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            repo = "qigao/chttp"
            with self.assertRaisesRegex(ValueError, "unsupported target RID"):
                verify(repo, root, "linux-arm64")
            with self.assertRaisesRegex(ValueError, "SDK Config missing"):
                verify(repo, root, "linux-x64")
            spec = SPECS[repo]
            config = root / "sdk/linux-x64" / spec["config"]
            manifest = root / "sdk/linux-x64" / spec["manifest"]
            config.parent.mkdir(parents=True)
            config.write_text("set(Chttp_FOUND TRUE)")
            with self.assertRaisesRegex(ValueError, "extracted manifest missing"):
                verify(repo, root, "linux-x64")
            manifest.write_text("version=2.1.0-rc.5\n")
            verify(repo, root, "linux-x64")


if __name__ == "__main__":
    unittest.main()
