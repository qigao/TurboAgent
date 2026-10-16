"""Generated fixtures only. No actual voice/noise media or model weights."""
import hashlib
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "mix_recorded_field_noise.py"
spec = importlib.util.spec_from_file_location("recorded_noise_mix", SCRIPT)
mix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mix)


def pcm(frames: int, amplitude: int) -> bytes:
    values = [amplitude if i % 2 else -amplitude
              for i in range(frames * 320)]
    return struct.pack("<%dh" % len(values), *values)


class RecordedNoiseMixtureTests(unittest.TestCase):
    def test_real_noise_source_consecutive_samples_two_known_scale_levels(self):
        voice = pcm(128, 900)
        noise = pcm(375, 4000)
        hashes=set()
        for divisor in (8, 16):
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                os.chmod(root, 0o700)
                report = mix.prepare(voice, noise, root, divisor)
                self.assertEqual(report["total_frames"], 178)
                self.assertEqual(report["speech_frames"], 128)
                self.assertEqual(report["recorded_noise_samples_used"],178*320)
                self.assertFalse(report["real_simultaneous_microphone_capture"])
                self.assertFalse(report["model_weights_used"])
                reference = (root / mix.CLEAN_NAME).read_bytes()
                overlay = (root / mix.MIX_NAME).read_bytes()
                self.assertEqual(len(reference), len(overlay))
                self.assertEqual(len(reference), 178*640)
                hashes.add(hashlib.sha256(overlay).hexdigest())
                ref_samples = mix.samples(reference)
                new_samples = mix.samples(overlay)
                self.assertTrue(all(x==0 for x in ref_samples[:50*320]))
                self.assertEqual(list(ref_samples[50*320:]), list(mix.samples(voice)))
                expected = mix._toward_zero_divide(-4000, divisor)
                self.assertEqual(new_samples[0], expected)
                for name in (mix.CLEAN_NAME, mix.MIX_NAME):
                    self.assertEqual((root/name).stat().st_mode & 0o777,0o600)
                before = mix.score(reference, overlay, overlay, divisor)
                self.assertEqual(before["reference_ser_delta_db"], 0.0)
                self.assertTrue(before["includes_ns_speech_distortion_in_post_error"])
                self.assertFalse(before["natural_simultaneous_voice_noise_recording"])
                self.assertEqual(before["signal"],
                    "licensed_whisper_plus_real_recorded_noise_software_overlay")
        self.assertEqual(len(hashes), 2)

    def test_clean_reference_improvement_metric_and_no_quality_claims(self):
        voice=pcm(128,2000)
        noise=pcm(375,1200)
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            os.chmod(root,0o700)
            mix.prepare(voice,noise,root,8)
            clean=(root/mix.CLEAN_NAME).read_bytes()
            observed=(root/mix.MIX_NAME).read_bytes()
            reference=mix.samples(clean)
            contaminated=mix.samples(observed)
            compensated=[int((x+y)/2) for x,y in zip(reference,contaminated)]
            filtered=struct.pack("<%dh" % len(compensated),*compensated)
            report=mix.score(clean,observed,filtered,8)
            self.assertGreater(report["reference_ser_delta_db"],5.3)
            self.assertFalse(report["wer_cer_or_hallucination_certified"])
            self.assertFalse(report["recording_snr_ground_truth"])
            self.assertFalse(report["speaker_population_inference"])

    def test_reject_source_noise_shortened_and_clipping_without_partial_outputs(self):
        original=pcm(128,2000)
        short_noise=pcm(127,500)
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            os.chmod(root,0o700)
            with self.assertRaisesRegex(ValueError,"bounded"):
                mix.prepare(original,short_noise,root,8)
            self.assertEqual(list(root.iterdir()),[])
        clipping_voice=pcm(128,32767)
        clipping_noise=pcm(375,30000)
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            os.chmod(root,0o700)
            with self.assertRaisesRegex(ValueError,"clip"):
                mix.prepare(clipping_voice,clipping_noise,root,8)
            self.assertEqual(list(root.iterdir()),[])

    def test_reject_nonprivate_or_nonempty_directory_and_invalid_divisor(self):
        voice=pcm(128,500)
        noise=pcm(375,500)
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            root.chmod(0o755)
            with self.assertRaisesRegex(ValueError,"0700"):
                mix.prepare(voice,noise,root,8)
            root.chmod(0o700)
            existing=root/"unrelated.txt"
            existing.write_text("preserve me")
            with self.assertRaisesRegex(ValueError,"0700"):
                mix.prepare(voice,noise,root,8)
            self.assertEqual(existing.read_text(),"preserve me")
            existing.unlink()
            for bad in (0, 1, 4, 32, True, "8"):
                with self.subTest(bad=bad), self.assertRaises(ValueError):
                    mix.prepare(voice,noise,root,bad)
            self.assertEqual(list(root.iterdir()),[])

    def test_private_sha_input_and_cli_fail_closed_no_path_leak(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            original=root/"secret-human-whisper.raw"
            noise=root/"secret-field-noise.raw"
            original.write_bytes(pcm(128,300))
            noise.write_bytes(pcm(375,500))
            out=root/"result"
            out.mkdir(mode=0o700)
            bad=subprocess.run([
                sys.executable,str(SCRIPT),"prepare",
                "--clean-raw",str(original),"--clean-sha256","0"*64,
                "--recorded-noise-raw",str(noise),
                "--recorded-noise-sha256",hashlib.sha256(noise.read_bytes()).hexdigest(),
                "--scale-divisor","8","--private-root",str(out),
            ],capture_output=True,text=True,check=False)
            self.assertEqual(bad.returncode,2)
            self.assertEqual(bad.stdout,"")
            self.assertNotIn("secret-human",bad.stderr)
            self.assertNotIn(str(original),bad.stderr)
            self.assertEqual(list(out.iterdir()),[])
            alias=root/"symlink.raw"
            alias.symlink_to(noise)
            with self.assertRaises(ValueError):
                mix.read_pinned(alias, hashlib.sha256(noise.read_bytes()).hexdigest())

    def test_nonnegative_divide_rounds_toward_zero_and_does_not_inject_dc(self):
        for divisor in mix.LEVELS:
            for sample in range(-100, 101):
                self.assertEqual(mix._toward_zero_divide(sample,divisor),
                                 int(sample/divisor))
            self.assertEqual(mix._toward_zero_divide(-1,divisor),0)


if __name__=="__main__":
    unittest.main()
