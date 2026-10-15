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
from unittest import mock
import wave

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "evaluate_stationarity_corpus.py"
sys.path.insert(0, str(ROOT))
spec = importlib.util.spec_from_file_location("corpus", SCRIPT)
corpus = importlib.util.module_from_spec(spec)
spec.loader.exec_module(corpus)


def _noise_frame(amplitude: int, state: int) -> tuple[list[int], int]:
    """Fixed-seed uncorrelated signs; voice-like alternating sample polarity
    is strongly correlated and deliberately MUST NOT be rejected by ABI2."""
    values=[]
    for _ in range(320):
        state ^= (state << 13) & 0xffffffff
        state ^= state >> 17
        state ^= (state << 5) & 0xffffffff
        state &= 0xffffffff
        values.append(amplitude if state & 1 else -amplitude)
    return values,state


def make_wave(path: Path, kind: str, frames: int = 60):
    state=0x41535231
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(16000)
        for index in range(frames):
            amplitude = 0 if kind == "zero" else (
                1000 if kind == "constant" else
                (1000 if index % 2 else 5000))
            pcm,state=_noise_frame(amplitude,state)
            output.writeframes(struct.pack("<320h", *pcm))


def make_wave_amplitudes(path: Path, amplitudes: list[int]):
    state=0x41535231
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(16000)
        for amplitude in amplitudes:
            pcm,state=_noise_frame(amplitude,state)
            output.writeframes(struct.pack("<320h", *pcm))


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
            make_wave_amplitudes(c, [1200] * 60)  # distinct stationary noise
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

    def test_absent_language_and_noise_strata_are_explicitly_unknown(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            spoken = root / "private-voice.wav"
            make_wave(spoken, "dynamic")
            manifest = root / "private-manifest.json"
            manifest.write_text(json.dumps({"schema": 1, "samples": [
                entry(spoken, "human_voice", "en", "ordinary", "creator_consent")
            ]}), encoding="utf-8")
            report = corpus.evaluate(root, manifest)
            gaps = report["coverage_gaps"]
            self.assertEqual(gaps["missing_declared_voice_languages"],
                             ["ja", "mixed", "zh"])
            self.assertEqual(gaps["missing_declared_real_noise_conditions"],
                             sorted(corpus.SOURCE_CONDITIONS["real_noise"]))
            self.assertIn("low_volume", gaps["missing_declared_voice_conditions"])
            self.assertNotIn("ordinary", gaps["missing_declared_voice_conditions"])
            self.assertTrue(gaps["no_data_is_not_zero_error_rate"])
            self.assertFalse(gaps["all_required_strata_independently_qualified"])
            self.assertEqual(report["human_voice"]["by_language"]["en"]["denominator"], 1)
            for language in ("zh", "ja", "mixed"):
                self.assertIsNone(report["human_voice"]["by_language"][language]["rate"])
                self.assertIsNone(report["human_voice"]["by_language"][language]["wilson95"])
                self.assertEqual(report["human_voice"]["by_language"][language]["denominator"], 0)
            self.assertIsNone(report["real_noise"]["by_condition"]["fan"]["rate"])
            self.assertFalse(report["real_noise"]["hallucination_rate_measured"])
            public = json.dumps(report)
            self.assertNotIn(spoken.name, public)
            self.assertNotIn(str(root), public)
            self.assertNotIn(entry(spoken, "human_voice", "en", "ordinary",
                                   "creator_consent")["sha256"], public)

    def test_real_noise_condition_rates_are_predecode_only(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            constant = root / "quiet.wav"
            varying = root / "intermittent.wav"
            make_wave(constant, "constant")
            make_wave(varying, "dynamic")
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({"schema": 1, "samples": [
                entry(constant, "real_noise", "none", "fan", "public_license"),
                entry(varying, "real_noise", "none", "intermittent", "public_license")
            ]}), encoding="utf-8")
            report = corpus.evaluate(root, manifest)
            noise = report["real_noise"]
            self.assertEqual(noise["guard_on_predecode_rejections"]["denominator"], 2)
            self.assertEqual(noise["guard_on_predecode_rejections"]["count"], 1)
            self.assertEqual(noise["by_condition"]["fan"]["denominator"], 1)
            self.assertEqual(noise["by_condition"]["fan"]["rate"], 1)
            self.assertEqual(noise["by_condition"]["intermittent"]["rate"], 0)
            self.assertIsNone(noise["by_condition"]["traffic"]["rate"])
            self.assertEqual(report["coverage_gaps"]["missing_declared_voice_languages"],
                             ["en", "ja", "mixed", "zh"])
            self.assertFalse(noise["hallucination_rate_measured"])
            self.assertFalse(report["acoustic_vad_or_recognition_quality_certified"])

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

    def test_pcm_duplicate_with_distinct_valid_riff_headers_is_refused(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            original = root / "recording.wav"
            repackaged = root / "with-metadata.wav"
            make_wave(original, "constant")
            src = original.read_bytes()
            self.assertEqual(src[0:4], b"RIFF")
            self.assertEqual(src[8:12], b"WAVE")
            # A different legal JUNK chunk changes the WAV container
            # digest, but not the raw PCM being measured. Reject aliasing.
            modified = src[:12] + b"JUNK" + struct.pack("<I", 4) + b"ABCD" + src[12:]
            modified = modified[:4] + struct.pack("<I", len(modified)-8) + modified[8:]
            repackaged.write_bytes(modified)
            self.assertNotEqual(hashlib.sha256(src).digest(),
                                hashlib.sha256(modified).digest())
            one = corpus.inspect_wav_bytes(src, include_pcm_digest=True)
            two = corpus.inspect_wav_bytes(modified, include_pcm_digest=True)
            self.assertEqual(one["_pcm_sha256"], two["_pcm_sha256"])
            self.assertNotIn("_pcm_sha256", corpus.inspect_wav_bytes(src))
            manifest = root / "manifest.json"
            samples = [entry(original, "synthetic"),
                       entry(repackaged, "synthetic")]
            manifest.write_text(json.dumps({"schema": 1, "samples": samples}))
            with self.assertRaisesRegex(ValueError, "duplicate PCM samples"):
                corpus.evaluate(root, manifest)

    def test_same_snapshot_is_used_for_sha_and_stationarity(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            sample = root / "pinned.wav"
            make_wave(sample, "constant")
            pinned = sample.read_bytes()
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({"schema": 1, "samples": [
                entry(sample, "synthetic")]}))
            original_inspector = corpus.inspect_wav_bytes
            observed = []
            def mutate_after_read(raw, **kw):
                observed.append(raw)
                make_wave(sample, "dynamic")
                return original_inspector(raw, **kw)
            with mock.patch.object(corpus, "inspect_wav_bytes",
                                   side_effect=mutate_after_read):
                result = corpus.evaluate(root, manifest)
            self.assertEqual(observed, [pinned])
            self.assertEqual(
                result["synthetic_controls"]["guard_on_predecode_rejections"], 1)
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                corpus.evaluate(root, manifest)

    def test_schema2_speaker_groups_are_private_and_not_clip_weighted(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            paths = [root / f"sample-{i}.wav" for i in range(4)]
            make_wave_amplitudes(paths[0], [1000] * 60)
            make_wave_amplitudes(paths[1], [1100] * 60)
            make_wave_amplitudes(paths[2], [1000, 6000] * 30)
            make_wave_amplitudes(paths[3], [1200, 5500] * 120)
            sp_a, sp_b = "opaque_group_A1", "opaque_group_B2"
            samples = [
                {**entry(paths[0], "human_voice", "en", "monotone",
                         "creator_consent"), "speaker_group": sp_a},
                {**entry(paths[1], "human_voice", "zh", "low_volume",
                         "creator_consent"), "speaker_group": sp_a},
                {**entry(paths[2], "human_voice", "ja", "ordinary",
                         "public_license"), "speaker_group": sp_a},
                {**entry(paths[3], "human_voice", "mixed", "code_switch",
                         "public_license"), "speaker_group": sp_b},
            ]
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({"schema": 2, "samples": samples}))
            report = corpus.evaluate(root, manifest)
            self.assertEqual(report["schema"], 2)
            self.assertEqual(report["human_voice"]["guard_on_rejections"], 2)
            self.assertEqual(report["human_voice"]["guard_on_false_rejection"]["rate"], 0.5)
            grouped = report["human_voice"]["speaker_groups"]
            self.assertTrue(grouped["schema2_declared_groups"])
            self.assertEqual(grouped["unique_group_count"], 2)
            self.assertEqual(grouped["speaker_any_guard_rejection"]["rate"], 0.5)
            self.assertEqual(grouped["equal_weight_per_speaker_rejection_rate"], 0.333333)
            self.assertFalse(grouped["group_labels_or_identity_verified"])
            duration = report["human_voice"]["by_duration"]
            self.assertEqual(duration["1_to_3s"]["denominator"], 3)
            self.assertEqual(duration["3_to_10s"]["denominator"], 1)
            self.assertIsNone(duration["lt_1s"]["rate"])
            private = json.dumps(report)
            self.assertNotIn(sp_a, private)
            self.assertNotIn(sp_b, private)
            for sample in samples:
                self.assertNotIn(sample["sha256"], private)
                self.assertNotIn(sample["path"], private)
            invalid_variants = [
                {k:v for k,v in samples[0].items() if k != "speaker_group"},
                {**samples[0], "speaker_group": "real name with spaces"},
                {**samples[0], "speaker_group": "é" * 16},
            ]
            for invalid in invalid_variants:
                manifest.write_text(json.dumps({"schema": 2, "samples": [invalid]}))
                with self.assertRaises(ValueError):
                    corpus.evaluate(root, manifest)
            # Schema 1 must not silently accept a speaker token.
            manifest.write_text(json.dumps({"schema": 1, "samples": [samples[0]]}))
            with self.assertRaises(ValueError):
                corpus.evaluate(root, manifest)
            # Nonhuman rows cannot provide a pseudo speaker group either.
            bad_noise = {**entry(paths[0], "real_noise", "none", "fan",
                                 "public_license"), "speaker_group": sp_a}
            manifest.write_text(json.dumps({"schema": 2, "samples": [bad_noise]}))
            with self.assertRaises(ValueError):
                corpus.evaluate(root, manifest)

    def test_equal_power_coherent_wave_must_bypass_noise_guard(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            coherent=root/"coherent.wav"
            with wave.open(str(coherent),"wb") as f:
                f.setnchannels(1)
                f.setsampwidth(2)
                f.setframerate(16000)
                for _ in range(60):
                    f.writeframes(struct.pack("<320h", *(1000 if (i//40)%2 else -1000
                                                              for i in range(320))))
            data=corpus.inspect_wav_bytes(coherent.read_bytes())
            self.assertEqual(data["frame_power_cv"],0.0)
            self.assertEqual(data["frame_power_max_min_ratio"],1.0)
            self.assertGreater(data["lag1_abs_correlation"],0.8)
            self.assertEqual(data["stationarity_hypothesis"],"dynamic_or_unknown")
            m=root/"manifest.json"
            m.write_text(json.dumps({"schema":1,"samples":[entry(
                coherent,"human_voice","en","monotone","creator_consent")]}))
            report=corpus.evaluate(root,m)
            self.assertEqual(report["human_voice"]["guard_on_rejections"],0)
            self.assertIn("lag1",report["method"])
            self.assertFalse(report["acoustic_vad_or_recognition_quality_certified"])

    def test_strict_exact_lag1_ratio_matches_c11_at_and_around_12_percent(self):
        # This is one-to-one parity with C11 stationarity_exact_lag1_ratio_test.
        # No transcript or real-human voice, only fixed-power synthetic signs.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            sample = root / "exact-lag1.wav"
            manifest = root / "private-manifest.json"
            for extra141, expected, expected_cross in (
                    (35, 0, 3830),
                    (36, 0, 3828),
                    (37, 1, 3826)):
                with self.subTest(extra141=extra141):
                    with wave.open(str(sample), "wb") as out:
                        out.setnchannels(1)
                        out.setsampwidth(2)
                        out.setframerate(16000)
                        for frame in range(100):
                            flips = 140 + (frame < extra141)
                            pcm = [1000]
                            for index in range(1, 320):
                                pcm.append(-pcm[-1] if index <= flips else pcm[-1])
                            out.writeframes(struct.pack("<320h", *pcm))
                    report = corpus.inspect_wav_bytes(sample.read_bytes())
                    self.assertEqual(report["frame_count"], 100)
                    self.assertEqual(report["frame_power_cv"], 0)
                    self.assertEqual(report["frame_power_max_min_ratio"], 1)
                    self.assertEqual(report["lag1_abs_correlation"],
                                     round(expected_cross / 31900, 6))
                    manifest.write_text(json.dumps({"schema": 1, "samples": [
                        entry(sample, "synthetic", condition="equal_power")]}))
                    aggregate = corpus.evaluate(root, manifest)
                    self.assertEqual(
                        aggregate["synthetic_controls"]["guard_on_predecode_rejections"],
                        expected)
                    self.assertFalse(aggregate["acoustic_vad_or_recognition_quality_certified"])

    def test_unknown_rate_is_not_misreported_as_zero(self):
        self.assertIsNone(corpus.rate(0, 0)["rate"])
        self.assertIsNone(corpus.rate(0, 0)["wilson95"])
        self.assertEqual(corpus.rate(0, 1)["rate"], 0)
        self.assertEqual(corpus.rate(1, 1)["rate"], 1)


if __name__ == "__main__":
    unittest.main()
