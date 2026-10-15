"""Test fail-closed metadata-only analysis of private non-speech probes."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "evaluate_non_speech_probe.py"
spec = importlib.util.spec_from_file_location("evaluate_non_speech_probe", SCRIPT)
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


def write_case(root: Path, name: str, rc: int, stdout: bytes, stderr: bytes):
    (root / f"{name}.exit").write_text(f"{rc}\n", encoding="ascii")
    (root / f"{name}.stdout").write_bytes(stdout)
    (root / f"{name}.stderr").write_bytes(stderr)


def default_cases(root: Path):
    write_case(root, "digital-zero", 0, b"", b"NO_SPEECH")
    write_case(root, "stationary-noise-quiet", 4, b"",
               b"no transcript admitted")
    write_case(root, "stationary-noise-medium", 0, b"unexpected words\n", b"")


class ProbeOutcomeTests(unittest.TestCase):
    def test_report_classifies_a_hallucination_candidate_without_leaking_words(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            default_cases(root)
            data = probe.evaluate(root)
            self.assertEqual(data["non_speech_cases"]["digital-zero"]["classification"],
                             "no_speech")
            self.assertEqual(data["non_speech_cases"]["stationary-noise-quiet"]["classification"],
                             "provider_or_output_rejected")
            self.assertEqual(data["non_speech_cases"]["stationary-noise-medium"]["classification"],
                             "unprompted_text_observed")
            self.assertFalse(data["real_noise_rejection_certified"])
            self.assertNotIn("unexpected words", json.dumps(data))

            report = root / "report.json"
            p = subprocess.run(
                [sys.executable, str(SCRIPT), "--input-dir", str(root),
                 "--report", str(report)],
                capture_output=True, text=True, check=False)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertNotIn("unexpected words", p.stdout)
            self.assertNotIn("unexpected words", report.read_text())
            self.assertEqual(json.loads(report.read_text())["non_speech_cases"],
                             data["non_speech_cases"])

    def test_zero_pcm_must_not_publish_any_transcript(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            default_cases(root)
            write_case(root, "digital-zero", 0, b"hallucinated\n", b"")
            with self.assertRaises(ValueError):
                probe.evaluate(root)

    def test_timeout_and_crash_cannot_masquerade_as_negative_acceptance(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            default_cases(root)
            for rc in (124, 137, 139, 1):
                with self.subTest(rc=rc):
                    write_case(root, "stationary-noise-quiet", rc, b"", b"")
                    with self.assertRaises(ValueError):
                        probe.evaluate(root)

    def test_terminal_failure_cannot_publish_partial_text(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            default_cases(root)
            write_case(root, "stationary-noise-quiet", 4, b"partial\n",
                       b"no transcript admitted")
            with self.assertRaises(ValueError):
                probe.evaluate(root)

    def test_reject_unbounded_or_missing_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            default_cases(root)
            (root / "stationary-noise-medium.stdout").write_bytes(b"X" * 4098)
            with self.assertRaises(ValueError):
                probe.evaluate(root)
            (root / "stationary-noise-medium.stdout").unlink()
            with self.assertRaises(OSError):
                probe.evaluate(root)


if __name__ == "__main__":
    unittest.main()
