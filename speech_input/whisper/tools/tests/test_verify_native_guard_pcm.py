"""Model-free fail-closed tests for exact pinned PCM and C11-vs-Python parity."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import wave

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "verify_native_guard_pcm.py"
spec = importlib.util.spec_from_file_location("native_guard_pcm", SCRIPT)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def create_wave(path: Path, extra_flips: int):
    """Same exact 100-frame 12/100 boundary witnesses as production C11."""
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(16000)
        for frame in range(100):
            flips = 140 + (frame < extra_flips)
            samples = [1000]
            for sample in range(1, 320):
                samples.append(-samples[-1] if sample <= flips else samples[-1])
            out.writeframes(struct.pack("<320h", *samples))


def sha(path: Path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def runner_for(expected, calls, *, wrong=False):
    def runner(cli, model, model_sha256, private_wav, language, guard):
        calls.append((guard, private_wav))
        self_kind = expected[guard]
        if wrong and guard:
            self_kind = ("fake_vendor_final" if self_kind ==
                         "no_speech_without_vendor" else "no_speech_without_vendor")
        return self_kind
    return runner


class NativeGuardSnapshotTests(unittest.TestCase):
    def setup_fixture(self, root: Path, extra: int):
        wav = root / "sensitive-private.wav"
        create_wave(wav, extra)
        model = root / "fake-model.bin"
        model.write_bytes(checker.FAKE_MODEL_BYTES)
        return wav, model, Path(sys.executable).resolve(strict=True)

    def test_exact_12_percent_boundary_and_private_snapshot(self):
        for extra, gate in ((35, "fake_vendor_final"),
                            (36, "fake_vendor_final"),
                            (37, "no_speech_without_vendor")):
            with self.subTest(extra141=extra):
                with tempfile.TemporaryDirectory() as scratch:
                    root = Path(scratch)
                    wav, model, cli = self.setup_fixture(root, extra)
                    calls = []
                    result = checker.verify(
                        wav, sha(wav), cli, model, sha(model), "ja",
                        runner=runner_for(("fake_vendor_final", gate), calls))
                    self.assertEqual(result["guard_off"], "fake_vendor_final")
                    self.assertEqual(result["guard_on"], gate)
                    self.assertEqual([v[0] for v in calls], [0, 1])
                    self.assertNotEqual(calls[0][1], wav)
                    self.assertEqual(calls[0][1], calls[1][1])
                    self.assertFalse(calls[0][1].exists())
                    self.assertEqual(result["frame_count"], 100)
                    self.assertTrue(result["same_immutable_pcm_evaluated_and_supplied"])
                    self.assertFalse(result["real_model_weights_used"])
                    public = json.dumps(result)
                    self.assertNotIn(str(root), public)
                    self.assertNotIn(wav.name, public)
                    self.assertNotIn(sha(wav), public)

    def test_malformed_sha_unapproved_real_weights_and_missing_rights_provenance(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            wav, model, cli = self.setup_fixture(root, 37)
            spy = mock.Mock(side_effect=AssertionError("must not run the C11 CLI"))
            for invalid_source in ("0"*64, "not-sha", "F"*64):
                with self.subTest(invalid_source=invalid_source):
                    with self.assertRaises(ValueError):
                        checker.verify(wav, invalid_source, cli, model, sha(model),
                                       "en", runner=spy)
            with self.assertRaises(ValueError):
                checker.verify(wav, sha(wav), cli, model, "0"*64,
                               "en", runner=spy)
            model.write_bytes(b"GGML actual model weights not authorized")
            with self.assertRaisesRegex(ValueError, "real GGML"):
                checker.verify(wav, sha(wav), cli, model, sha(model),
                               "en", runner=spy)
            self.assertEqual(spy.call_count, 0)

    def test_incorrect_guard_result_rejected_without_leak(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            wav, model, cli = self.setup_fixture(root, 37)
            recorded = []
            spy = runner_for(("fake_vendor_final", "no_speech_without_vendor"),
                             recorded, wrong=True)
            with self.assertRaisesRegex(ValueError, "disagrees") as raised:
                checker.verify(wav, sha(wav), cli, model, sha(model),
                               "en", runner=spy)
            self.assertNotIn(str(root), str(raised.exception))
            self.assertFalse(recorded[0][1].exists())

    def test_untrusted_cli_terminal_rejects_timeout_and_partial_content(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            wav, model, cli = self.setup_fixture(root, 36)
            with mock.patch.object(checker.subprocess, "run", return_value=(
                    subprocess.CompletedProcess([], 124, b"SECRET\n", b"timeout"))):
                with self.assertRaisesRegex(ValueError, "bounded and terminal"):
                    checker._call(cli, model, sha(model), wav, "en", 1)
            with mock.patch.object(checker.subprocess, "run", return_value=(
                    subprocess.CompletedProcess([], 0, b"SECRET SPEECH\n", b""))):
                with self.assertRaisesRegex(ValueError, "terminal classification"):
                    checker._call(cli, model, sha(model), wav, "en", 1)
            with mock.patch.object(checker.subprocess, "run", side_effect=(
                    subprocess.TimeoutExpired("fake engine", 1))):
                with self.assertRaisesRegex(ValueError, "timed out"):
                    checker._call(cli, model, sha(model), wav, "en", 1)

    def test_public_cli_failure_is_generic_and_does_not_create_report(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            wav, model, cli = self.setup_fixture(root, 37)
            report = root / "report.json"
            result = subprocess.run([
                sys.executable, str(SCRIPT), "--source-wav", str(wav),
                "--source-sha256", "0"*64,
                "--mock-cli", str(cli),
                "--fake-model", str(model),
                "--fake-model-sha256", sha(model),
                "--language", "en",
                "--report", str(report),
            ], capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, "")
            self.assertNotIn(wav.name, result.stderr)
            self.assertNotIn(str(root), result.stderr)
            self.assertFalse(report.exists())


if __name__ == "__main__":
    unittest.main()
