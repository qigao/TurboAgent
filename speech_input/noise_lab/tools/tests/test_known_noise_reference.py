"""Deterministic synthetic-only clean/known-noise scoring tests."""
import hashlib
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1]/"known_noise_reference.py"
spec = importlib.util.spec_from_file_location("known_reference", SCRIPT)
ref = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ref)


class KnownNoiseReferenceTests(unittest.TestCase):
    def test_prepare_is_private_pinned_no_clipping(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            os.chmod(root,0o700)
            voice = struct.pack("<%dh" % (320*60),
                                *([1500,-1500]*160*60))
            result = ref.prepare(voice,root)
            self.assertEqual(result["total_frames"],110)
            self.assertEqual(result["speech_frames"],60)
            self.assertTrue(result["no_raw_audio_in_report"])
            clean = root/"clean-reference.raw"
            noisy = root/"mixed-known-noise.raw"
            self.assertEqual(clean.stat().st_mode & 0o777,0o600)
            self.assertEqual(noisy.stat().st_mode & 0o777,0o600)
            self.assertEqual(len(clean.read_bytes()),len(noisy.read_bytes()))
            expected = ref.score(clean.read_bytes(),noisy.read_bytes(),
                                 noisy.read_bytes())
            self.assertAlmostEqual(expected["reference_ser_delta_db"],0.0)
            with self.assertRaises(ValueError):
                ref.prepare(voice,root)
            clean_raw = list(ref.samples(clean.read_bytes()))
            noisy_raw = list(ref.samples(noisy.read_bytes()))
            midway=[(x+y)//2 for x,y in zip(clean_raw,noisy_raw)]
            half_noise = struct.pack("<%dh" % len(midway),*midway)
            improved = ref.score(clean.read_bytes(),noisy.read_bytes(),half_noise)
            self.assertGreater(improved["reference_ser_delta_db"],5.5)
            self.assertFalse(improved["model_wer_cer_or_phoneme_intelligibility_measured"])

    def test_explicit_noise_levels_and_correlated_profile_are_reproducible(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = struct.pack("<%dh" % (320*60),
                                 *([750,-750]*160*60))
            results = {}
            for profile in ("white", "lowpass"):
                for peak in (100, 200, 400):
                    directory = root / (profile + str(peak))
                    directory.mkdir(mode=0o700)
                    info = ref.prepare(source,directory,peak,profile)
                    self.assertEqual(info["noise_profile"],profile)
                    self.assertEqual(info["noise_max_amplitude"],peak)
                    clean = (directory/"clean-reference.raw").read_bytes()
                    noisy = (directory/"mixed-known-noise.raw").read_bytes()
                    result = ref.score(clean,noisy,noisy,
                                       noise_peak=peak,noise_profile=profile)
                    self.assertEqual(result["noise_profile"],profile)
                    self.assertEqual(result["noise_max_amplitude"],peak)
                    self.assertEqual(result["reference_ser_delta_db"],0.0)
                    results[profile,peak] = result["known_input_noise_power"]
            self.assertLess(results["white",100],results["white",200])
            self.assertLess(results["white",200],results["white",400])
            for peak in (100,200,400):
                self.assertLess(results["lowpass",peak],results["white",peak])
            with self.assertRaises(ValueError):
                ref.prepare(source,root,999,"white")
            with self.assertRaises(ValueError):
                ref.prepare(source,root,200,"unknown")
            with self.assertRaises(ValueError):
                ref.score(clean,noisy,noisy,noise_peak=800)
            with self.assertRaises(ValueError):
                ref.score(clean,noisy,noisy,noise_profile="pink")

    def test_clean_only_distortion_is_independent_of_noise_removal(self):
        with tempfile.TemporaryDirectory() as td:
            directory = Path(td)
            os.chmod(directory,0o700)
            source = struct.pack("<%dh" % (320*60),
                                 *([1500,-1500]*160*60))
            ref.prepare(source,directory)
            clean = (directory/"clean-reference.raw").read_bytes()
            noisy = (directory/"mixed-known-noise.raw").read_bytes()
            samples = ref.samples(clean)
            filtered = struct.pack("<%dh" % len(samples),
                                   *[int(x/2) for x in samples])
            report = ref.score(clean,noisy,noisy,filtered)
            self.assertAlmostEqual(report["clean_only_filter_ser_db"],6.0206,
                                   delta=0.001)
            self.assertEqual(report["clean_only_source_nonzero_frames_zeroed"],0)
            self.assertEqual(report["reference_ser_delta_db"],0.0)
            mute = b"\x00" * len(clean)
            silenced = ref.score(clean,noisy,noisy,mute)
            self.assertEqual(silenced["clean_only_source_nonzero_frames_zeroed"],60)
            self.assertEqual(silenced["clean_only_filter_ser_db"],0.0)
            with self.assertRaises(ValueError):
                ref.score(clean,noisy,noisy,filtered[:-2])

    def test_sha_mismatch_and_ambiguous_raw_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            p = root/"private-speech.raw"
            p.write_bytes(struct.pack("<640h", *([300]*640)))
            with self.assertRaises(ValueError):
                ref.read_pinned(p, "0"*64)
            valid = hashlib.sha256(p.read_bytes()).hexdigest()
            self.assertEqual(ref.read_pinned(p,valid),p.read_bytes())
            p.write_bytes(p.read_bytes()[:-1])
            with self.assertRaises(ValueError):
                ref.read_pinned(p,hashlib.sha256(p.read_bytes()).hexdigest())
            p.unlink()
            p.symlink_to(root)
            with self.assertRaises(ValueError):
                ref.read_pinned(p, "0"*64)

    def test_missing_noise_and_insufficient_speech_fail_closed(self):
        voice = b"\x00" * 640
        with self.assertRaises(ValueError):
            ref.score(voice, voice,voice)
        with self.assertRaises(ValueError):
            ref.score(voice,voice,voice[:-1])
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            os.chmod(root,0o755)
            with self.assertRaises(ValueError):
                ref.prepare(voice,root)


if __name__=="__main__":
    unittest.main()
