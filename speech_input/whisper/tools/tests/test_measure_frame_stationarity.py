"""Stationarity measurements are test-only, bounded and not acoustic VAD."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import wave

ROOT = Path(__file__).resolve().parents[1]
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

measure = load("stationarity_measurement", "measure_frame_stationarity.py")
noise = load("stationary_noise_generator", "generate_noise_fixtures.py")


class StationarityTests(unittest.TestCase):
    def test_noise_is_stationary_and_true_zero_is_distinct(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            noise.generate(root)
            zero = measure.inspect_wav(root / "digital-zero.wav")
            quiet = measure.inspect_wav(root / "stationary-noise-quiet.wav")
            medium = measure.inspect_wav(root / "stationary-noise-medium.wav")
            self.assertEqual(zero["stationarity_hypothesis"], "digital_zero")
            self.assertEqual(zero["frame_count"], 550)
            for candidate in (quiet, medium):
                self.assertFalse(candidate["voice_activity_certified"])
                self.assertEqual(candidate["stationarity_hypothesis"],
                                 "stationary_candidate")
                self.assertLess(candidate["frame_power_cv"], 0.055)
                self.assertGreater(candidate["frame_power_cv"], 0.045)
                self.assertLess(candidate["frame_power_max_min_ratio"], 1.5)

    def test_dynamic_amplitude_envelope_is_not_rejected_as_stationary(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "amplitude-modulated.wav"
            with wave.open(str(path), "wb") as output:
                output.setnchannels(1)
                output.setsampwidth(2)
                output.setframerate(16000)
                for index in range(100):
                    amplitude = (0 if index % 10 < 3 else
                                 500 if index % 10 < 6 else 7000)
                    output.writeframes(struct.pack("<320h", *(
                        amplitude if i % 2 else -amplitude for i in range(320))))
            summary = measure.inspect_wav(path)
            self.assertEqual(summary["stationarity_hypothesis"],
                             "dynamic_or_unknown")
            self.assertGreater(summary["frame_power_cv"], 0.4)
            self.assertIsNone(summary["frame_power_max_min_ratio"])

    def test_short_sample_cannot_establish_stationarity(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "short.wav"
            with wave.open(str(path), "wb") as output:
                output.setnchannels(1)
                output.setsampwidth(2)
                output.setframerate(16000)
                output.writeframes(struct.pack("<320h", *([100] * 320)))
            summary = measure.inspect_wav(path)
            self.assertEqual(summary["frame_count"], 1)
            self.assertEqual(summary["stationarity_hypothesis"],
                             "dynamic_or_unknown")

    def test_reject_incomplete_or_wrong_sample_rate(self):
        with tempfile.TemporaryDirectory() as folder:
            for frames, rate in ((319, 16000), (320, 8000)):
                path = Path(folder) / f"bad-{frames}-{rate}.wav"
                with wave.open(str(path), "wb") as output:
                    output.setnchannels(1)
                    output.setsampwidth(2)
                    output.setframerate(rate)
                    output.writeframes(struct.pack("<%dh" % frames,
                                                   *([100] * frames)))
                with self.assertRaises(ValueError):
                    measure.inspect_wav(path)


if __name__ == "__main__":
    unittest.main()
