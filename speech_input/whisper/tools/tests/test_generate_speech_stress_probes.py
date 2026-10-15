"""Pinned JFK-derived soft/short speech transform tests; no model/Internet."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from unittest import mock
import unittest
import wave

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import generate_agc_speech_probe as agc
import generate_speech_stress_probes as probes


def test_source(path: Path) -> str:
    with wave.open(str(path), "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(16000)
        # 550 complete frames with energy variation and exact-zero portions.
        for frame in range(550):
            amplitude = (0 if frame % 25 == 0 else
                         1000 if frame % 4 else 3000)
            data = [amplitude if (i//30) % 2 else -amplitude for i in range(320)]
            f.writeframes(struct.pack("<320h", *data))
    return hashlib.sha256(path.read_bytes()).hexdigest()


class StressProbeTests(unittest.TestCase):
    def test_reject_mismatched_unlicensed_speech_source(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            src = root / "wrong.wav"
            test_source(src)
            with self.assertRaises(ValueError):
                probes.generate(src, root / "out")
            self.assertFalse((root / "out" / "jfk-soft-8.wav").exists())

    def test_stable_waveforms_and_attenuation_and_short_window(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            src = root / "synthetic-source.wav"
            digest = test_source(src)
            with mock.patch.object(agc, "SOURCE_SHA256", digest), \
                 mock.patch.object(probes, "SOURCE_SHA256", digest):
                a = probes.generate(src, root / "first")
                b = probes.generate(src, root / "second")
            self.assertEqual(a, b)
            self.assertEqual(len(a["profiles"]), 5)
            self.assertFalse((root / "first" / "jfk-agc-base-PRIVATE.wav").exists())
            original = probes._read_exact
            with wave.open(str(src), "rb") as f:
                before = struct.unpack("<176000h", f.readframes(176000))
            for name, meta in a["profiles"].items():
                self.assertEqual(meta["wav_sha256"],
                                 hashlib.sha256((root / "first" / (name+".wav")).read_bytes()).hexdigest())
                self.assertGreater(meta["nonzero_samples"], 0)
                with wave.open(str(root / "first" / (name+".wav")), "rb") as f:
                    self.assertEqual((f.getnchannels(), f.getsampwidth(), f.getframerate()),
                                     (1, 2, 16000))
                    raw = f.readframes(f.getnframes())
                    samples = struct.unpack("<%dh" % f.getnframes(), raw)
                self.assertEqual(meta["pcm_samples"], len(samples))
                self.assertEqual(meta["pcm_sha256"], hashlib.sha256(raw).hexdigest())
                if name == "jfk-soft-8":
                    self.assertEqual(samples[321],
                                     probes._attenuate(before[321], 8))
                if name == "jfk-mid-short-0p8s":
                    self.assertEqual(len(samples), 40*320)
                    self.assertEqual(samples[0], before[200*320])

    def test_nonabsolute_path_and_extreme_short_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(ValueError):
                probes.generate(Path("relative.wav"), Path(folder))
            with self.assertRaises(ValueError):
                probes._emit(Path(folder)/"bad.wav", tuple([100]*319))
            with self.assertRaises(ValueError):
                probes._emit(Path(folder)/"empty.wav", tuple())


if __name__ == "__main__":
    unittest.main()
