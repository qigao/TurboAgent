"""End-to-end private source -> immutable canonical WAV -> ABI2 corpus metrics.

Only locally generated synthetic PCM is used. The actual pinned SHA of the
chosen local FFmpeg executable is checked. The test does NOT obtain a human
recording, download model weights, invoke Whisper, or certify voice activity.
"""
import hashlib
import json
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import prepare_stationarity_wav as preparation
import evaluate_stationarity_corpus as evaluator

FRAMES = 60
SAMPLES = 320


def synthetic_wav(path: Path, *, correlated: bool) -> None:
    state = 0x41535231
    pcm = []
    for _ in range(FRAMES):
        for sample in range(SAMPLES):
            if correlated:
                value = -1000 if (sample // 40) & 1 else 1000
            else:
                state ^= (state << 13) & 0xFFFFFFFF
                state ^= state >> 17
                state ^= (state << 5) & 0xFFFFFFFF
                state &= 0xFFFFFFFF
                value = 1000 if state & 1 else -1000
            pcm.append(value)
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(1)
        stream.setsampwidth(2)
        stream.setframerate(16000)
        stream.writeframes(struct.pack("<%dh" % len(pcm), *pcm))


def manifest_record(path: Path, *, condition: str) -> dict:
    return {
        "path": path.name,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "source_kind": "synthetic",
        "language": "none",
        "condition": condition,
        "rights_basis": "generated_by_test",
    }


class PrivateIntakeToCorrelatedMetrics(unittest.TestCase):
    @unittest.skipUnless(shutil.which("ffmpeg"), "explicit local FFmpeg not installed")
    def test_real_cpu_ffmpeg_to_snapshot_dedup_and_lag1_guard(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            private = root / "private"
            private.mkdir()
            ffmpeg = Path(shutil.which("ffmpeg")).resolve(strict=True)
            ffmpeg_sha = preparation.digest_file(ffmpeg)
            samples = []
            # Same frame-power across both. Different adjacent sample
            # coherence is the ONLY reason guard=ON treats them differently.
            for label, coherent in (("noise", False), ("coherent", True)):
                source = root / (label + "-source.wav")
                synthetic_wav(source, correlated=coherent)
                prepared = private / (label + ".wav")
                provenance = private / (label + ".private-provenance.json")
                before = source.read_bytes()
                result = preparation.prepare(
                    source, hashlib.sha256(before).hexdigest(), ffmpeg,
                    prepared, provenance, True, ffmpeg_sha)
                self.assertEqual(result["frame_count"], FRAMES)
                self.assertEqual(result["tail_samples_discarded"], 0)
                self.assertTrue(result["not_speech_quality_certified"])
                metadata = json.loads(provenance.read_text(encoding="utf-8"))
                self.assertEqual(metadata["source_sha256"],
                                 hashlib.sha256(before).hexdigest())
                self.assertEqual(metadata["normalized_wav_sha256"],
                                 hashlib.sha256(prepared.read_bytes()).hexdigest())
                self.assertTrue(metadata["rights_and_consent_user_declared_not_verified"])
                self.assertFalse(metadata["model_weights_used"])
                self.assertFalse(metadata["audio_downloaded"])
                if sys.platform != "win32":
                    self.assertEqual(stat.S_IMODE(prepared.stat().st_mode), 0o600)
                    self.assertEqual(stat.S_IMODE(provenance.stat().st_mode), 0o600)
                samples.append(manifest_record(prepared, condition=(
                    "equal_power" if coherent else "stationary")))
            manifest = root / "manifest-private.json"
            manifest.write_text(json.dumps({"schema": 2, "samples": samples}),
                                encoding="utf-8")
            report = evaluator.evaluate(private, manifest)
            self.assertEqual(report["sample_count"], 2)
            self.assertEqual(report["synthetic_controls"]["count"], 2)
            self.assertEqual(
                report["synthetic_controls"]["guard_on_predecode_rejections"], 1)
            self.assertEqual(report["synthetic_controls"]["guard_off_predecode_rejections"], 0)
            self.assertIn("lag1_correlation_guard_abi2", report["method"])
            self.assertFalse(report["model_inference_used"])
            self.assertFalse(report["acoustic_vad_or_recognition_quality_certified"])
            self.assertEqual(report["real_noise"]["guard_on_predecode_rejections"]["denominator"], 0)
            self.assertIsNone(report["real_noise"]["guard_on_predecode_rejections"]["rate"])
            self.assertEqual(report["coverage_gaps"]["missing_declared_voice_languages"],
                             ["en", "ja", "mixed", "zh"])
            public = json.dumps(report, sort_keys=True)
            for item in samples:
                self.assertNotIn(item["path"], public)
                self.assertNotIn(item["sha256"], public)
            for name in ("private", "manifest-private.json", "provenance"):
                self.assertNotIn(name, public)
            # Repackaging the same samples must not create another recording
            # or inflate confidence interval denominators.
            copy_path = private / "noise-duplicated.wav"
            copy_path.write_bytes((private / "noise.wav").read_bytes())
            samples.append(manifest_record(copy_path, condition="other"))
            manifest.write_text(json.dumps({"schema": 2, "samples": samples}),
                                encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicate WAV bytes"):
                evaluator.evaluate(private, manifest)

    @unittest.skipUnless(shutil.which("ffmpeg"), "explicit local FFmpeg not installed")
    def test_rights_absent_refuses_before_audio_or_provenance_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source = root / "local-original.wav"
            synthetic_wav(source, correlated=False)
            ffmpeg = Path(shutil.which("ffmpeg")).resolve(strict=True)
            destination = root / "private-converted.wav"
            provenance = root / "private-evidence.json"
            with self.assertRaisesRegex(ValueError, "acknowledged"):
                preparation.prepare(
                    source, hashlib.sha256(source.read_bytes()).hexdigest(),
                    ffmpeg, destination, provenance, False,
                    preparation.digest_file(ffmpeg))
            self.assertFalse(destination.exists())
            self.assertFalse(provenance.exists())


if __name__ == "__main__":
    unittest.main()
