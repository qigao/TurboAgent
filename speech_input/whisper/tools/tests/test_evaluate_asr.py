"""Dependency-free tests for real Whisper acoustic smoke quality accounting."""
from pathlib import Path
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "evaluate_asr.py"
spec = importlib.util.spec_from_file_location("whisper_asr_metrics", SCRIPT)
metrics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(metrics)

class ASRQualityMetrics(unittest.TestCase):
    def test_english_one_deletion(self):
        got = metrics.score("And so my fellow Americans!",
                            "and so my Americans", "en")
        self.assertAlmostEqual(got["word_error_rate"], 0.2)
        self.assertFalse(got["quality_certification"])

    def test_chinese_cer(self):
        got = metrics.score("你好，世界。", "你号世界", "zh")
        self.assertAlmostEqual(got["character_error_rate"], 0.25)

    def test_japanese_exact(self):
        got = metrics.score("今日は日本語です。", "今日は日本語です", "ja")
        self.assertEqual(got["character_error_rate"], 0)
        self.assertEqual(got["word_error_rate"], 0)

    def test_unicode_normalization(self):
        self.assertEqual(metrics.score("ＨＥＬＬＯ！", "hello", "en")["word_error_rate"], 0)
        self.assertEqual(metrics.score("ＡＢＣ", "abc", "en")["character_error_rate"], 0)

    def test_empty_and_wrong_language_rejected(self):
        with self.assertRaises(ValueError):
            metrics.score("one", "", "en")
        with self.assertRaises(ValueError):
            metrics.score("one", "two", "unsupported")
        with self.assertRaises(ValueError):
            metrics.score("", "word", "en")

    def test_cli_accepts_one_english_fixture_without_leaking_text(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            transcript = t / "out.txt"
            report = t / "metrics.json"
            transcript.write_text("Hello world\n", encoding="utf-8")
            p = subprocess.run([sys.executable, str(SCRIPT),
                                "--reference", "Hello, world!",
                                "--transcript-file", str(transcript),
                                "--language", "en",
                                "--max-wer", "0.0",
                                "--require-word", "hello",
                                "--report", str(report)],
                               capture_output=True, text=True, check=False)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(json.loads(report.read_text())["word_error_rate"], 0)
            self.assertNotIn("Hello", p.stdout)  # metrics only, no transcript

    def test_cli_fails_closed_and_does_not_write_metrics(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            transcript = t / "bad.txt"
            report = t / "report.json"
            for raw in (b"\x1b[31mhello", b"\xff", b""):
                transcript.write_bytes(raw)
                if report.exists():
                    report.unlink()
                p = subprocess.run([sys.executable, str(SCRIPT),
                                    "--reference", "hello",
                                    "--transcript-file", str(transcript),
                                    "--language", "en",
                                    "--report", str(report)],
                                   capture_output=True, text=True, check=False)
                self.assertEqual(p.returncode, 2, p.stdout)
                self.assertFalse(report.exists())

    def test_quality_threshold_is_enforced(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            transcript = t / "out.txt"
            report = t / "report.json"
            transcript.write_text("totally unrelated text", encoding="utf-8")
            p = subprocess.run([sys.executable, str(SCRIPT),
                                "--reference", "ask not what your country can do",
                                "--transcript-file", str(transcript),
                                "--language", "en", "--max-wer", "0.60",
                                "--require-word", "country",
                                "--report", str(report)],
                               capture_output=True, text=True, check=False)
            self.assertEqual(p.returncode, 2)
            self.assertFalse(report.exists())

if __name__ == "__main__":
    unittest.main()
