"""Strict synthetic-only tests of source-derived envelope-stress diagnostics."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
SCRIPT = ROOT / "stress_stationarity_human.py"
spec = importlib.util.spec_from_file_location("energy_stress", SCRIPT)
stress = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stress)


def synthetic_wav(frame_count=150, zero_every=0, only_one=False):
    raw = io.BytesIO()
    with wave.open(raw, "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(16000)
        for frame in range(frame_count):
            amp = (400 if frame % 3 == 0 else
                   1300 if frame % 3 == 1 else 3500)
            if zero_every and frame % zero_every == 0:
                block = [0] * 320
            elif only_one:
                block = [0] * 319 + [-32768]
            else:
                block = [amp if sample % 2 else -amp for sample in range(320)]
            out.writeframes(struct.pack("<320h", *block))
    return raw.getvalue()


def evaluate(source):
    return stress.evaluate_bytes(source, hashlib.sha256(source).hexdigest())


class EnvelopeStressTests(unittest.TestCase):
    def test_dynamic_synthetic_input_becomes_flat_only_after_explicit_processing(self):
        source = synthetic_wav()
        result = evaluate(source)
        self.assertEqual(result["source_frame_count"], 150)
        self.assertEqual(result["original_energy_hypothesis"], "dynamic_or_unknown")
        for attenuated in result["uniform_gain_reduction"].values():
            self.assertFalse(attenuated["predecode_guard_no_speech"])
            self.assertEqual(attenuated["frame_count"], 150)
        flattened = result["controlled_energy_equalization"]
        self.assertEqual(flattened["selected_nonzero_source_frames"], 120)
        self.assertEqual(flattened["source_frame_energy_hypothesis"], "dynamic_or_unknown")
        # A flat 20ms power envelope is NOT sufficient to reject this
        # coherent alternating-sign synthetic waveform in current ABI2.
        self.assertEqual(flattened["equalized_frame_energy_hypothesis"],
                         "dynamic_or_unknown")
        self.assertTrue(flattened["legacy_energy_only_would_reject"])
        self.assertFalse(flattened["correlation_aware_abi2_would_reject"])
        self.assertFalse(flattened["guard_on_predecode_no_speech"])
        self.assertGreater(flattened["lag1_abs_correlation"], 0.95)
        self.assertFalse(flattened["guard_off_predecode_no_speech"])
        self.assertLess(flattened["frame_power_cv"], 0.005)
        self.assertLess(flattened["frame_power_max_min_ratio"], 1.01)
        self.assertFalse(result["audible_intelligibility_or_wer_cer_measured"])
        self.assertFalse(result["human_population_false_rejection_rate_measured"])
        self.assertFalse(result["actual_whisper_model_used"])
        self.assertFalse(result["actual_c11_provider_used_on_source_audio"])
        self.assertTrue(result["no_audio_or_transcript_persisted_or_emitted"])
        text = json.dumps(result)
        self.assertNotIn(hashlib.sha256(source).hexdigest(), text)


    def test_abi2_guard_vs_legacy_energy_gate_is_differentiated(self):
        # A synthetic, deliberately coherent voice-like waveform can be
        # flat-power and yet should not be refused solely due to RMS.
        result = evaluate(synthetic_wav(150))
        stress_case = result["controlled_energy_equalization"]
        self.assertTrue(stress_case["legacy_energy_only_would_reject"])
        self.assertFalse(stress_case["correlation_aware_abi2_would_reject"])
        self.assertGreater(stress_case["lag1_abs_correlation"], 0.95)
        self.assertIn("strict_lag1_abi2", result["method"])
        self.assertFalse(result["human_population_false_rejection_rate_measured"])

    def test_zero_frames_dropped_only_in_controlled_derivative(self):
        source = synthetic_wav(130, zero_every=4)
        result = evaluate(source)
        selected = result["controlled_energy_equalization"]
        self.assertGreaterEqual(selected["selected_nonzero_source_frames"], 50)
        self.assertLess(selected["selected_nonzero_source_frames"], 130)
        self.assertEqual(selected["equalized_frame_energy_hypothesis"],
                         "dynamic_or_unknown")
        self.assertTrue(selected["legacy_energy_only_would_reject"])
        self.assertFalse(selected["correlation_aware_abi2_would_reject"])
        for record in result["uniform_gain_reduction"].values():
            # Gain-only transforms preserve entire source timeline.
            self.assertEqual(record["frame_count"], 130)
        self.assertFalse(selected["original_timing_and_amplitude_envelope_preserved"])

    def test_narrow_spike_is_normalized_without_s16_overflow(self):
        src = synthetic_wav(90, only_one=True)
        out = evaluate(src)
        self.assertEqual(out["controlled_energy_equalization"][
            "equalized_frame_energy_hypothesis"], "stationary_candidate")
        self.assertTrue(out["controlled_energy_equalization"][
            "correlation_aware_abi2_would_reject"])
        self.assertEqual(out["controlled_energy_equalization"][
            "lag1_abs_correlation"], 0.0)

    def test_sha_mismatch_partial_and_not_enough_active_frames_fail_closed(self):
        src = synthetic_wav(60)
        with self.assertRaises(ValueError):
            stress.evaluate_bytes(src, "0"*64)
        with self.assertRaises(ValueError):
            stress.evaluate_bytes(b"NOT WAV", hashlib.sha256(b"NOT WAV").hexdigest())
        for short in (synthetic_wav(49), synthetic_wav(80, zero_every=2)):
            with self.assertRaisesRegex(ValueError, "insufficient"):
                evaluate(short)
        with self.assertRaises(ValueError):
            stress.evaluate_bytes(src + b"padding", hashlib.sha256(src).hexdigest())

    def test_cli_outputs_only_metrics_and_rejects_private_source_path(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            wav = root / "private-speaker-raw.wav"
            source = synthetic_wav(150)
            wav.write_bytes(source)
            sha = hashlib.sha256(source).hexdigest()
            ok = subprocess.run(
                [sys.executable, str(SCRIPT), "--source-wav", str(wav),
                 "--source-sha256", sha],
                capture_output=True, text=True, check=False)
            self.assertEqual(ok.returncode, 0, ok.stderr)
            self.assertEqual(json.loads(ok.stdout)["source_frame_count"], 150)
            self.assertNotIn("private-speaker", ok.stdout)
            self.assertNotIn(sha, ok.stdout)
            bad = subprocess.run(
                [sys.executable, str(SCRIPT), "--source-wav", str(wav),
                 "--source-sha256", "0"*64],
                capture_output=True, text=True, check=False)
            self.assertEqual(bad.returncode, 2)
            self.assertEqual(bad.stdout, "")
            self.assertNotIn("private-speaker", bad.stderr)


if __name__ == "__main__":
    unittest.main()
