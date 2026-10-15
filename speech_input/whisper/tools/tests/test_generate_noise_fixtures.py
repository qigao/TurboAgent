"""No-download acceptance for synthetic non-speech WAV probe provenance."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import wave

SCRIPT = Path(__file__).resolve().parents[1] / "generate_noise_fixtures.py"
spec = importlib.util.spec_from_file_location("synthetic_noise_probes", SCRIPT)
probes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probes)

# Exact canonical WAV digest is deliberate: changes to samples or RIFF bytes
# require an explicit new reviewer-acknowledged baseline.
EXPECTED_WAV_SHA = {
    "digital-zero": "1a092b23d9dfe64d53943d18262c0a49eaf32c5ab5f2b016cd8edd16ec9440dd",
    "stationary-noise-quiet": "1ff905f7cb91008204ee6bcc5b0eeaec26881bb50b126fa5c3bbb0308ac7e0e4",
    "stationary-noise-medium": "1b9a3368ac682060b001d9c20d828a38144d176e6e4173f06c6287626174f90f",
}


class SyntheticNoiseProvenance(unittest.TestCase):
    def test_exact_wave_container_samples_and_hashes(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            manifest = probes.generate(root)
            self.assertFalse(manifest["human_audio_corpus_used"])
            self.assertEqual(manifest["frames"], 550)
            self.assertEqual(set(manifest["profiles"]), set(EXPECTED_WAV_SHA))
            for name, expected in EXPECTED_WAV_SHA.items():
                with self.subTest(profile=name):
                    path = root / f"{name}.wav"
                    raw = path.read_bytes()
                    self.assertEqual(len(raw), 44 + 550 * 640)
                    self.assertEqual(hashlib.sha256(raw).hexdigest(), expected)
                    self.assertEqual(manifest["profiles"][name]["sha256_wav"], expected)
                    with wave.open(str(path), "rb") as audio:
                        self.assertEqual((audio.getnchannels(),
                                          audio.getsampwidth(),
                                          audio.getframerate(),
                                          audio.getnframes()),
                                         (1, 2, 16000, 550 * 320))
                        samples = audio.readframes(550 * 320)
                        self.assertEqual(len(samples), 550 * 640)
                        self.assertEqual(hashlib.sha256(samples).hexdigest(),
                                         manifest["profiles"][name]["sha256_pcm"])
            self.assertEqual(manifest["profiles"]["digital-zero"]["nonzero_samples"], 0)
            self.assertEqual(manifest["profiles"]["digital-zero"]["rms_s16"], 0.0)
            self.assertGreater(manifest["profiles"]["stationary-noise-quiet"]["nonzero_samples"],
                               550 * 320 * 0.99)
            self.assertGreater(manifest["profiles"]["stationary-noise-medium"]["rms_s16"],
                               manifest["profiles"]["stationary-noise-quiet"]["rms_s16"])

    def test_cli_report_is_metadata_only_and_no_network_dependency(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            report = root / "manifest.json"
            audio_dir = root / "wav"
            proc = subprocess.run(
                [sys.executable, str(SCRIPT),
                 "--output-dir", str(audio_dir),
                 "--report", str(report)],
                capture_output=True, text=True, check=False)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            metadata = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(set(metadata["profiles"]), set(EXPECTED_WAV_SHA))
            for name in EXPECTED_WAV_SHA:
                self.assertEqual(metadata["profiles"][name]["sha256_wav"],
                                 EXPECTED_WAV_SHA[name])
            self.assertNotIn(".wav", report.read_text(encoding="utf-8"))
            self.assertNotIn("transcript", report.read_text(encoding="utf-8"))
            self.assertEqual(len(list(audio_dir.glob("*.wav"))), 3)

    def test_relative_output_directory_fails_closed(self):
        with self.assertRaises(ValueError):
            probes.generate(Path("relative-dir"))


if __name__ == "__main__":
    unittest.main()
