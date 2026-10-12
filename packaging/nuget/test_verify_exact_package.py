#!/usr/bin/env python3
"""Offline negative/positive fixtures for NuGet archive validation (#112)."""
import importlib.util
import pathlib
import tempfile
import unittest
import zipfile

SCRIPT = pathlib.Path(__file__).with_name("verify_exact_package.py")
spec = importlib.util.spec_from_file_location("verify_exact_package", SCRIPT)
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)

SHA = "a" * 40
VERSION = "1.3.0-ci.8.1"


class PackageValidationTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = pathlib.Path(self.tmp.name) / f"TurboAgent.Native.{VERSION}.nupkg"

    def make_zip(self, override=None, omit=None):
        override = override or {}
        omit = set(omit or ())
        with zipfile.ZipFile(self.path, "w") as archive:
            for rid in validator.RIDS:
                prefix = f"sdk/{rid}/"
                files = {
                    prefix + "turboagent-sdk-manifest.txt": (
                        f"version=1.3.0\ncommit={SHA}\nrid={rid}\npraktor_tools=off\n"
                    ).encode(),
                    prefix + "include/turbo_agent_api.h": b"#pragma once\n",
                    prefix + "lib/cmake/TurboAgent/TurboAgentConfig.cmake": b"# package config\n",
                    prefix + "lib/libturboagent.a": b"not-empty",
                }
                for name, data in files.items():
                    if name not in omit:
                        archive.writestr(name, override.get(name, data))

    def test_valid_package(self):
        self.make_zip()
        validator.validate(self.path, SHA, VERSION)

    def test_missing_rid_manifest(self):
        self.make_zip(omit={"sdk/windows-x64/turboagent-sdk-manifest.txt"})
        with self.assertRaisesRegex(ValueError, "missing provenance manifest"):
            validator.validate(self.path, SHA, VERSION)

    def test_wrong_commit(self):
        name = "sdk/linux-x64/turboagent-sdk-manifest.txt"
        self.make_zip(override={name: (
            f"version=1.3.0\ncommit={'b' * 40}\nrid=linux-x64\npraktor_tools=on\n"
        ).encode()})
        with self.assertRaisesRegex(ValueError, "wrong commit"):
            validator.validate(self.path, SHA, VERSION)

    def test_empty_binary(self):
        self.make_zip(override={"sdk/macos-arm64/lib/libturboagent.a": b""})
        with self.assertRaisesRegex(ValueError, "empty native library"):
            validator.validate(self.path, SHA, VERSION)

    def test_missing_cmake_export(self):
        self.make_zip(omit={"sdk/android-arm64-v8a/lib/cmake/TurboAgent/TurboAgentConfig.cmake"})
        with self.assertRaisesRegex(ValueError, "missing CMake package config"):
            validator.validate(self.path, SHA, VERSION)


    def test_wrong_version(self):
        name = "sdk/linux-x64/turboagent-sdk-manifest.txt"
        self.make_zip(override={name: (
            f"version=9.9.9\\ncommit={SHA}\\nrid=linux-x64\\npraktor_tools=on\\n"
        ).encode()})
        with self.assertRaisesRegex(ValueError, "version mismatch"):
            validator.validate(self.path, SHA, VERSION)

    def test_wrong_manifest_rid(self):
        name = "sdk/windows-x64/turboagent-sdk-manifest.txt"
        self.make_zip(override={name: (
            f"version=1.3.0\\ncommit={SHA}\\nrid=linux-x64\\npraktor_tools=off\\n"
        ).encode()})
        with self.assertRaisesRegex(ValueError, "RID mismatch"):
            validator.validate(self.path, SHA, VERSION)

    def test_missing_public_headers(self):
        self.make_zip(omit={"sdk/linux-x64/include/turbo_agent_api.h"})
        with self.assertRaisesRegex(ValueError, "missing public include"):
            validator.validate(self.path, SHA, VERSION)

    def test_missing_native_libraries(self):
        self.make_zip(omit={"sdk/windows-x64/lib/libturboagent.a"})
        with self.assertRaisesRegex(ValueError, "missing native libraries"):
            validator.validate(self.path, SHA, VERSION)

    def test_extra_unexpected_platform(self):
        self.make_zip()
        with zipfile.ZipFile(self.path, "a") as archive:
            archive.writestr("sdk/unsupported-rid/lib/unexpected.a", b"not-empty")
        with self.assertRaisesRegex(ValueError, "unexpected platform SDK trees"):
            validator.validate(self.path, SHA, VERSION)

    def test_unsafe_archive_traversal(self):
        self.make_zip()
        with zipfile.ZipFile(self.path, "a") as archive:
            archive.writestr("sdk/../escape.cmake", b"malicious")
        with self.assertRaisesRegex(ValueError, "unsafe archive path"):
            validator.validate(self.path, SHA, VERSION)

    def test_duplicate_archive_member(self):
        import warnings

        self.make_zip()
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(self.path, "a") as archive:
                archive.writestr("sdk/linux-x64/include/turbo_agent_api.h", b"bad duplicate")
        with self.assertRaisesRegex(ValueError, "duplicate archive entries"):
            validator.validate(self.path, SHA, VERSION)

    def test_invalid_requested_sha(self):
        self.make_zip()
        with self.assertRaisesRegex(ValueError, "full 40-character commit SHA"):
            validator.validate(self.path, "invalid", VERSION)


if __name__ == "__main__":
    unittest.main()
