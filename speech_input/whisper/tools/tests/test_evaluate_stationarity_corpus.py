"""Corpus evaluator tests. Fixtures are synthetic, NEVER human audio evidence."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import wave

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "evaluate_stationarity_corpus.py"
sys.path.insert(0, str(ROOT))
spec = importlib.util.spec_from_file_location("corpus", SCRIPT)
corpus = importlib.util.module_from_spec(spec)
spec.loader.exec_module(corpus)


def make_wave(path: Path, kind: str, frames: int = 60):
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(16000)
        for index in range(frames):
            amplitude = 0 if kind == "zero" else (
                1000 if kind == "constant" else
                (1000 if index % 2 else 5000))
            output.writeframes(struct.pack("<320h", *([amplitude, -amplitude] * 160)))


def make_wave_amplitudes(path: Path, amplitudes: list[int]):
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(16000)
        for amplitude in amplitudes:
            output.writeframes(
                struct.pack("<320h", *([amplitude, -amplitude] * 160)))


def entry(path, kind, language="none", condition="stationary",
          rights="generated_by_test"):
    return {"path": path.name,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "source_kind": kind, "language": language,
            "condition": condition, "rights_basis": rights}


class CorpusGateTests(unittest.TestCase):
    def test_aggregate_and_wilson_are_metadata_only(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            a, b, c, z = [root / name for name in
                           ("a.wav", "b.wav", "c.wav", "z.wav")]
            make_wave(a, "constant")
            make_wave(b, "dynamic")
            make_wave(c, "constant")
            make_wave(z, "zero")
            # These stand-ins exercise DECLARED human/noise metadata. They
            # cannot substantiate actual human-voice false-rejection rates.
            data = {"schema": 1, "samples": [
                entry(a, "human_voice", "en", "low_volume", "creator_consent"),
                entry(b, "human_voice", "ja", "monotone", "public_license"),
                entry(c, "real_noise", "none", "fan", "public_license"),
                entry(z, "synthetic", "none", "zero"),
            ]}
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps(data), encoding="utf-8")
            report = corpus.evaluate(root, manifest)
            spoken = report["human_voice"]["guard_on_false_rejection"]
            self.assertEqual((spoken["count"], spoken["denominator"]), (1, 2))
            self.assertEqual(spoken["rate"], 0.5)
            self.assertEqual(report["human_voice"]["guard_off_rejections"], 0)
            self.assertEqual(report["real_noise"]["guard_on_predecode_rejections"]["rate"], 1)
            self.assertEqual(report["synthetic_controls"]["guard_off_predecode_rejections"], 1)
            self.assertFalse(report["human_labels_and_rights_independently_verified"])
            self.assertFalse(report["acoustic_vad_or_recognition_quality_certified"])
            text = json.dumps(report)
            for private in ("a.wav", "b.wav", "manifest.json",
                            data["samples"][0]["sha256"]):
                self.assertNotIn(private, text)
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--audio-root", str(root),
                 "--manifest", str(manifest), "--report", str(root / "report.json")],
                text=True, capture_output=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn("a.wav", result.stdout)
            self.assertEqual(json.loads(result.stdout), report)

    def test_threshold_49_versus_50_frames_and_provenance_rejection(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            sample = root / "speech.wav"
            make_wave(sample, "constant", 49)
            good = entry(sample, "synthetic")
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({"schema": 1, "samples": [good]}))
            first = corpus.evaluate(root, manifest)
            self.assertEqual(first["synthetic_controls"]["guard_on_predecode_rejections"], 0)
            make_wave(sample, "constant", 50)
            # Digest is immutable: the altered source cannot reuse authorization.
            with self.assertRaises(ValueError):
                corpus.evaluate(root, manifest)
            good = entry(sample, "synthetic")
            manifest.write_text(json.dumps({"schema": 1, "samples": [good]}))
            second = corpus.evaluate(root, manifest)
            self.assertEqual(second["synthetic_controls"]["guard_on_predecode_rejections"], 1)
            for change in ({"sha256": "0"*64}, {"path": "../speech.wav"},
                           {"path": str(sample)}, {"rights_basis": "unknown"},
                           {"language": "ja"}):
                with self.subTest(change=change):
                    item = {**good, **change}
                    manifest.write_text(json.dumps({"schema": 1, "samples": [item]}))
                    with self.assertRaises((ValueError, OSError)):
                        corpus.evaluate(root, manifest)
            manifest.write_text('{"schema":1,"schema":1,"samples":[]}')
            with self.assertRaises(ValueError):
                corpus.evaluate(root, manifest)

    def test_symlink_fails_closed_without_public_audio_paths(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "private.wav"
            symlink = root / "link.wav"
            make_wave(source, "constant")
            symlink.symlink_to(source)
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({"schema": 1, "samples": [
                {**entry(source, "synthetic"), "path": symlink.name}]}))
            report = root / "report.json"
            process = subprocess.run(
                [sys.executable, str(SCRIPT), "--audio-root", str(root),
                 "--manifest", str(manifest), "--report", str(report)],
                text=True, capture_output=True, check=False)
            self.assertEqual(process.returncode, 2)
            self.assertEqual(process.stdout, "")
            self.assertNotIn(str(root), process.stderr)
            self.assertFalse(report.exists())

    def test_invalid_wav_does_not_leak_content_or_paths(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "secret_voice.wav"
            source.write_bytes(b"NOT A WAV private-content")
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({"schema": 1, "samples": [
                entry(source, "synthetic")]}))
            process = subprocess.run(
                [sys.executable, str(SCRIPT), "--audio-root", str(root),
                 "--manifest", str(manifest), "--report", str(root / "report.json")],
                text=True, capture_output=True, check=False)
            self.assertEqual(process.returncode, 2)
            self.assertEqual(process.stdout, "")
            self.assertNotIn("secret_voice", process.stderr)
            self.assertNotIn("private-content", process.stderr)

    def test_duplicate_audio_bytes_cannot_inflate_false_rejection_denominator(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            original = root / "voice-one.wav"
            copied = root / "voice-two.wav"
            make_wave(original, "constant")
            copied.write_bytes(original.read_bytes())
            manifest = root / "manifest.json"
            first = entry(original, "human_voice", "en", "monotone", "creator_consent")
            second = entry(copied, "human_voice", "ja", "low_volume", "creator_consent")
            manifest.write_text(json.dumps({"schema": 1, "samples": [first, second]}))
            with self.assertRaisesRegex(ValueError, "duplicate WAV bytes"):
                corpus.evaluate(root, manifest)
            # Not even counting identical source files under different labels.
            manifest.write_text(json.dumps({"schema": 1, "samples": [first, first]}))
            with self.assertRaisesRegex(ValueError, "duplicate WAV bytes"):
                corpus.evaluate(root, manifest)

    def test_cv_and_power_ratio_boundaries_follow_provider_rule(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            sample = root / "bounds.wav"
            manifest = root / "manifest.json"
            # 30 frames at amplitude 1000, 30 at 1120 give CV ~0.113
            # (candidate); 30 at 1150 give CV ~0.139 (decode).
            # Neither crosses max/min power ratio 2.
            # 99 frames of 1000 + one 1414 stay below power ratio 2;
            # replacing 1414 with 1415 exceeds the exact ratio boundary.
            probes = [
                ([1000] * 30 + [1120] * 30, 1),
                ([1000] * 30 + [1150] * 30, 0),
                ([1000] * 99 + [1414], 1),
                ([1000] * 99 + [1415], 0),
            ]
            for amplitudes, expected in probes:
                with self.subTest(last=amplitudes[-1]):
                    make_wave_amplitudes(sample, amplitudes)
                    manifest.write_text(json.dumps({"schema": 1, "samples": [
                        entry(sample, "synthetic")]}))
                    result = corpus.evaluate(root, manifest)
                    self.assertEqual(
                        result["synthetic_controls"]["guard_on_predecode_rejections"],
                        expected)
                    self.assertFalse(result["acoustic_vad_or_recognition_quality_certified"])

    def test_unknown_rate_is_not_misreported_as_zero(self):
        self.assertIsNone(corpus.rate(0, 0)["rate"])
        self.assertIsNone(corpus.rate(0, 0)["wilson95"])
        self.assertEqual(corpus.rate(0, 1)["rate"], 0)
        self.assertEqual(corpus.rate(1, 1)["rate"], 1)


if __name__ == "__main__":
    unittest.main()
