"""Generated-PCM-only tests. No original human voice or model weights."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / "evaluate_ns_pcm_pair.py"
spec = importlib.util.spec_from_file_location("speex_ns_metrics", TOOL)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


def generated_pcm(amp=300, frames=60):
    frame = [amp if i % 2 else -amp for i in range(320)]
    return struct.pack("<%dh" % (frames * 320), *(frame * frames))


class SpeexNSPairedMetrics(unittest.TestCase):
    def test_byte_identical_and_attenuated_signal_not_asr(self):
        raw = generated_pcm()
        report = mod.evaluate(raw, raw, "whisper", -12)
        self.assertEqual(report["energy_ratio"], 1)
        self.assertEqual(report["aligned_cosine_similarity"], 1)
        self.assertEqual(report["changed_samples"], 0)
        self.assertEqual(report["unaligned_same_index_newly_zero_frames"], 0)
        self.assertFalse(report["no_speech_decision_authorized"])
        self.assertFalse(report["actual_snr_or_wer_cer_measured"])
        attenuated = generated_pcm(150)
        report2 = mod.evaluate(raw, attenuated, "whisper", -18)
        self.assertEqual(report2["energy_ratio"], 0.25)
        self.assertEqual(report2["aligned_cosine_similarity"], 1)
        self.assertEqual(report2["guard_after_ns_20ms_stationarity"],
                         "stationary_candidate")

    def test_known_160_sample_shift_is_aligned_not_marked_corruption(self):
        import random
        generator = random.Random(20261015)
        samples = [generator.randint(-2000, 2000) for _ in range(320*60)]
        raw = struct.pack("<%dh" % len(samples), *samples)
        shifted = struct.pack("<%dh" % len(samples),
                              *([0]*160 + samples[:-160]))
        result = mod.evaluate(raw, shifted, "steady", -12)
        self.assertEqual(result["best_bounded_alignment_shift_samples"], 160)
        self.assertGreater(result["aligned_cosine_similarity"], 0.999)
        self.assertLess(abs(result["zero_lag_cosine_similarity"]), 0.04)
        self.assertTrue(result["estimated_alignment_not_latency_certified"])

    def test_exact_one_frame_delay_does_not_count_voice_loss(self):
        import random
        generator = random.Random(12345)
        samples = [generator.randint(-3000, 3000) for _ in range(320*60)]
        raw = struct.pack("<%dh" % len(samples), *samples)
        one_frame_delay = struct.pack(
            "<%dh" % len(samples), *([0]*320 + samples[:-320]))
        report = mod.evaluate(raw, one_frame_delay, "whisper", -12)
        self.assertEqual(report["best_bounded_alignment_shift_samples"], 320)
        self.assertGreater(report["aligned_cosine_similarity"], 0.999)
        self.assertEqual(report["unaligned_same_index_newly_zero_frames"], 1)
        self.assertEqual(report["aligned_newly_zero_source_frames"], 0)
        self.assertEqual(report["aligned_source_frames_compared"], 59)
        self.assertEqual(report["undelivered_tail_samples_if_no_flush"], 320)

    def test_complete_silencing_is_reported_without_fabricating_vad(self):
        original = generated_pcm(500)
        silence = b"\x00" * len(original)
        result = mod.evaluate(original, silence, "road", -18)
        self.assertIsNone(result["aligned_cosine_similarity"])
        self.assertEqual(result["unaligned_same_index_newly_zero_frames"], 60)
        self.assertEqual(result["guard_after_ns_20ms_stationarity"], "digital_zero")
        self.assertFalse(result["no_speech_decision_authorized"])
        self.assertFalse(result["intelligibility_or_weak_consonants_preserved_certified"])

    def test_size_and_digest_are_strict_and_private(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            before = root / "private-whisper-audio.raw"
            after = root / "private-processed.raw"
            before.write_bytes(generated_pcm())
            after.write_bytes(generated_pcm(250))
            input_digest = hashlib.sha256(before.read_bytes()).hexdigest()
            output_digest = hashlib.sha256(after.read_bytes()).hexdigest()
            options = [sys.executable, str(TOOL),
                       "--before", str(before), "--before-sha256", input_digest,
                       "--after", str(after), "--after-sha256", output_digest,
                       "--kind", "whisper", "--db", "-12"]
            result = subprocess.run(options, capture_output=True,
                                    text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(result.stdout)
            self.assertEqual(data["frames"], 60)
            self.assertNotIn("private-whisper", result.stdout)
            self.assertNotIn(input_digest, result.stdout)
            self.assertNotIn(output_digest, result.stdout)
            options[options.index(input_digest)] = "0" * 64
            bad = subprocess.run(options, capture_output=True,
                                 text=True, check=False)
            self.assertEqual(bad.returncode, 2)
            self.assertEqual(bad.stdout, "")
            self.assertNotIn("private-whisper", bad.stderr)
            after.unlink()
            after.symlink_to(before)
            with self.assertRaises(ValueError):
                mod.verified_raw(after, hashlib.sha256(before.read_bytes()).hexdigest())

    def test_never_accept_incomplete_pcm_or_infinite_manifest(self):
        raw = generated_pcm(500)
        for sample in (raw[:-1], b"", raw + b"\x00", b"\x00" * (mod.MAX_RAW_BYTES + 640)):
            with self.assertRaises(ValueError):
                mod.evaluate(raw, sample, "steady", -12)
        for label in ("private-name", "\n", "../track"):
            with self.assertRaises(ValueError):
                mod.evaluate(raw, raw, label, -12)


if __name__ == "__main__":
    unittest.main()
