"""No-model validator for opt-in guard false-refusal comparisons."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import compare_speech_stress_outcomes as comparison
from generate_speech_stress_probes import PROFILES


def put(root: Path, name: str, guard: int, kind: str, words=b"sample secret text\n"):
    prefix = root / f"{name}-guard{guard}"
    if kind == "text":
        status, out, err = 0, words, b""
    elif kind == "no_speech":
        status, out, err = 0, b"", b"NO_SPEECH (no transcript emitted)\n"
    elif kind == "rejected":
        status, out, err = 4, b"", b"no transcript admitted\n"
    elif kind == "timeout":
        status, out, err = 124, b"", b""
    else:
        raise ValueError(kind)
    Path(str(prefix)+".exit").write_text(f"{status}\n", encoding="ascii")
    Path(str(prefix)+".stdout").write_bytes(out)
    Path(str(prefix)+".stderr").write_bytes(err)


def fixtures(root: Path):
    for i, (name, *_other) in enumerate(PROFILES):
        kind = ("text" if i == 0 else "no_speech" if i == 1 else
                "rejected" if i == 2 else "text")
        for guard in (0, 1):
            put(root, name, guard, kind)


class SpeechFalseRefusalComparison(unittest.TestCase):
    def test_identical_outcomes_never_echo_transcripts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixtures(root)
            report = comparison.compare(root)
            self.assertEqual(report["fixture_count"], len(PROFILES))
            self.assertTrue(report["same_status_and_exact_text_for_all_profiles"])
            self.assertFalse(report["real_world_voice_activity_certified"])
            self.assertGreaterEqual(report["baseline_recognized_profile_count"], 1)
            self.assertNotIn("sample secret text", json.dumps(report))
            result = root/"result.json"
            p = subprocess.run([sys.executable, str(TOOLS/"compare_speech_stress_outcomes.py"),
                                "--input-dir", str(root), "--report", str(result)],
                               capture_output=True, text=True, check=False)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertNotIn("sample secret text", result.read_text())
            self.assertNotIn("sample secret text", p.stdout)

    def test_guard_silencing_baseline_voice_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixtures(root)
            name = PROFILES[0][0]
            put(root, name, 1, "no_speech")
            with self.assertRaisesRegex(ValueError, "changed baseline ASR"):
                comparison.compare(root)

    def test_guard_altered_words_fails_without_leaking_words(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixtures(root)
            put(root, PROFILES[0][0], 1, "text", b"UNTRUSTED MODEL WORDS\n")
            with self.assertRaisesRegex(ValueError, "changed baseline ASR") as x:
                comparison.compare(root)
            self.assertNotIn("UNTRUSTED", str(x.exception))

    def test_timeout_and_partial_failure_never_equal_success(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixtures(root)
            name = PROFILES[1][0]
            put(root, name, 1, "timeout")
            with self.assertRaises(ValueError):
                comparison.compare(root)
            put(root, name, 1, "rejected")
            with self.assertRaises(ValueError):
                comparison.compare(root)

    def test_bounded_private_transcript_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixtures(root)
            prefix = PROFILES[0][0]+"-guard0.stdout"
            (root/prefix).write_bytes(b"A"*4098)
            with self.assertRaises(ValueError):
                comparison.compare(root)


if __name__ == "__main__":
    unittest.main()
