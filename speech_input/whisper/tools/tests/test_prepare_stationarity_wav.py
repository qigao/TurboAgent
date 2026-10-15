"""No-network, no-model validation of private WAV source pinning and conversion."""
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
SCRIPT = ROOT / "prepare_stationarity_wav.py"
spec = importlib.util.spec_from_file_location("private_wav_preflight", SCRIPT)
prep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prep)


def make_source(root, name="original.wav", frames=60):
    path = root / name
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(16000)
        wav.writeframes(struct.pack("<%dh" % (320 * frames),
                                    *([1000, -1000] * (160 * frames))))
    return path


def decoder_response(pcm, expected_source):
    calls = []
    def fake(command, **kwargs):
        calls.append((command, kwargs))
        assert kwargs["input"] == expected_source
        assert kwargs["capture_output"] is True
        assert kwargs["timeout"] <= 45
        assert "-protocol_whitelist" in command
        assert command[command.index("-protocol_whitelist") + 1] == "pipe"
        assert "-nostdin" in command
        assert "-ar" in command and "16000" in command
        assert "-f" in command and "s16le" in command
        assert "pipe:0" in command and command[-1] == "pipe:1"
        return SimpleNamespace(returncode=0, stdout=pcm, stderr=b"")
    return fake, calls


def pcm_frames(frames=60, extra=0):
    count = frames * 320 + extra
    return struct.pack("<%dh" % count,
                       *(1000 if i % 2 else -1000 for i in range(count)))


class PrivateWavPreflightTests(unittest.TestCase):
    def test_one_source_snapshot_and_private_reproducibility(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = make_source(root)
            original = src.read_bytes()
            mock_decode, calls = decoder_response(pcm_frames(60, 7), original)
            dest, provenance = root/"converted.wav", root/"private-metadata.json"
            ffmpeg = Path(sys.executable).resolve(strict=True)
            result = prep.prepare(src, hashlib.sha256(original).hexdigest(), ffmpeg,
                                  dest, provenance, True, mock_decode)
            self.assertEqual(len(calls), 1)
            self.assertEqual(result["frame_count"], 60)
            self.assertEqual(result["tail_samples_discarded"], 7)
            self.assertTrue(result["no_model_or_network"])
            self.assertNotIn("original.wav", json.dumps(result))
            self.assertNotIn("private-metadata", json.dumps(result))
            with wave.open(str(dest), "rb") as wav:
                self.assertEqual((wav.getnchannels(), wav.getsampwidth(),
                                  wav.getframerate(), wav.getnframes()),
                                 (1, 2, 16000, 320*60))
            self.assertEqual(stat.S_IMODE(dest.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(provenance.stat().st_mode), 0o600)
            info = json.loads(provenance.read_text())
            self.assertEqual(info["source_sha256"],
                             hashlib.sha256(original).hexdigest())
            self.assertEqual(info["normalized_wav_sha256"],
                             hashlib.sha256(dest.read_bytes()).hexdigest())
            self.assertEqual(info["tail_samples_discarded"], 7)
            self.assertTrue(info["rights_and_consent_user_declared_not_verified"])
            self.assertFalse(info["model_weights_used"])
            self.assertFalse(info["audio_downloaded"])
            with self.assertRaises(ValueError):
                prep.prepare(src, info["source_sha256"], ffmpeg,
                             dest, provenance, True, mock_decode)

    def test_source_sha_or_consent_fails_before_decoder(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = make_source(root)
            ffmpeg = Path(sys.executable).resolve(strict=True)
            spy = mock.Mock(side_effect=AssertionError("must not decode"))
            args = (src, "0"*64, ffmpeg, root/"out.wav",
                    root/"provenance.json", True, spy)
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                prep.prepare(*args)
            args = (src, hashlib.sha256(src.read_bytes()).hexdigest(), ffmpeg,
                    root/"out.wav", root/"provenance.json", False, spy)
            with self.assertRaisesRegex(ValueError, "acknowledged"):
                prep.prepare(*args)
            self.assertEqual(spy.call_count, 0)
            self.assertFalse((root/"out.wav").exists())
            linked = root / "linked.wav"
            linked.symlink_to(src)
            with self.assertRaisesRegex(ValueError, "source must be"):
                prep.prepare(linked, args[1], ffmpeg, root/"out.wav",
                             root/"provenance.json", True, spy)
            self.assertEqual(spy.call_count, 0)

    def test_mutated_decoder_fingerprint_rejected_before_private_output(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = make_source(root)
            digest = hashlib.sha256(source.read_bytes()).hexdigest()
            runner, calls = decoder_response(pcm_frames(60), source.read_bytes())
            ffmpeg = Path(sys.executable).resolve(strict=True)
            dest, provenance = root/"out.wav", root/"provenance.json"
            with mock.patch.object(prep, "digest_file",
                                   side_effect=["a"*64, "b"*64]):
                with self.assertRaisesRegex(ValueError,
                                            "executable changed during conversion"):
                    prep.prepare(source, digest, ffmpeg, dest, provenance,
                                 True, runner)
            self.assertEqual(len(calls), 1)
            self.assertFalse(dest.exists())
            self.assertFalse(provenance.exists())

    def test_decoder_crash_timeout_or_oversized_output_fails_closed(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = make_source(root)
            ffmpeg = Path(sys.executable).resolve(strict=True)
            source_digest = hashlib.sha256(src.read_bytes()).hexdigest()
            def call(runner):
                return prep.prepare(src, source_digest, ffmpeg,
                                    root/"out.wav", root/"provenance.json",
                                    True, runner)
            cases = [
                (lambda *_a, **_kw: SimpleNamespace(
                    returncode=2, stdout=b"", stderr=b"secret paths")),
                (lambda *_a, **_kw: (_ for _ in ()).throw(
                    subprocess.TimeoutExpired("fake", 5))),
                (lambda *_a, **_kw: SimpleNamespace(
                    returncode=0, stdout=pcm_frames(60) + b"\x7f",
                    stderr=b"")),
                (lambda *_a, **_kw: SimpleNamespace(
                    returncode=0, stdout=pcm_frames(3001),
                    stderr=b"")),
            ]
            for fake in cases:
                with self.subTest(fake=fake), self.assertRaises(ValueError):
                    call(fake)
                self.assertFalse((root/"out.wav").exists())
                self.assertFalse((root/"provenance.json").exists())

    def test_very_short_source_and_output_files_are_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = make_source(root)
            digest = hashlib.sha256(src.read_bytes()).hexdigest()
            ffmpeg = Path(sys.executable).resolve(strict=True)
            short, _ = decoder_response(pcm_frames(0, 319), src.read_bytes())
            with self.assertRaisesRegex(ValueError, "one complete frame"):
                prep.prepare(src, digest, ffmpeg, root/"out.wav",
                             root/"prov.json", True, short)
            self.assertFalse((root/"out.wav").exists())
            oversized = root / "huge.dat"
            oversized.write_bytes(b"0" * (prep.MAX_SOURCE_BYTES + 1))
            with self.assertRaises(ValueError):
                prep.read_pinned_source(oversized,
                                       hashlib.sha256(oversized.read_bytes()).hexdigest())

    def test_cli_rejects_missing_rights_without_exposing_path(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            sample = make_source(root, "private-home-person.wav")
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--source", str(sample),
                 "--source-sha256", hashlib.sha256(sample.read_bytes()).hexdigest(),
                 "--ffmpeg", str(Path(sys.executable).resolve()),
                 "--output-wav", str(root/"out.wav"),
                 "--private-provenance", str(root/"sidecar.json")],
                capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, "")
            self.assertNotIn("private-home-person", result.stderr)
            self.assertFalse((root/"out.wav").exists())
            self.assertFalse((root/"sidecar.json").exists())

    def test_bounded_external_decoder_flood_and_hang_are_killed(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            executable = Path(sys.executable).resolve(strict=True)
            flood = root / "flood-decoder"
            flood.write_text(
                "#!" + str(executable) + "\n"
                "import sys\n"
                "sys.stdout.buffer.write(b'F' * 4000000)\n"
                "sys.stdout.flush()\n", encoding="utf-8")
            flood.chmod(0o700)
            with self.assertRaisesRegex(ValueError, "bounded output"):
                prep.bounded_decoder_run(
                    [str(flood)], input=b"fake source", capture_output=True,
                    timeout=5, check=False)
            hung = root / "hung-decoder"
            hung.write_text(
                "#!" + str(executable) + "\n"
                "import time\n"
                "time.sleep(10)\n", encoding="utf-8")
            hung.chmod(0o700)
            with self.assertRaisesRegex(ValueError, "timed out"):
                prep.bounded_decoder_run(
                    [str(hung)], input=b"fake source", capture_output=True,
                    timeout=0.2, check=False)

    @unittest.skipUnless(shutil.which("ffmpeg"), "optional local CPU ffmpeg absent")
    def test_actual_local_vorbis_to_canonical_wav_through_pipe(self):
        # This fixture is synthesized digital PCM, not a human-voice recording.
        # It exercises the Ogg/Vorbis container path used by the licensed
        # source candidates, with no network URL or model-weight access.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = make_source(root)
            ogg = root / "synthetic.ogg"
            ffmpeg = Path(shutil.which("ffmpeg")).resolve(strict=True)
            encode = subprocess.run(
                [str(ffmpeg), "-nostdin", "-hide_banner", "-loglevel", "error",
                 "-i", str(source), "-c:a", "libvorbis", "-qscale:a", "4",
                 "-y", str(ogg)],
                capture_output=True, check=False, timeout=15)
            self.assertEqual(encode.returncode, 0, encode.stderr[:500])
            result = prep.prepare(
                ogg, hashlib.sha256(ogg.read_bytes()).hexdigest(), ffmpeg,
                root / "from-ogg.wav", root / "from-ogg.private.json", True)
            self.assertTrue(result["no_model_or_network"])
            self.assertGreaterEqual(result["frame_count"], 59)
            self.assertLessEqual(result["frame_count"], 60)
            with wave.open(str(root / "from-ogg.wav"), "rb") as wav:
                self.assertEqual((wav.getframerate(), wav.getnchannels(),
                                  wav.getsampwidth()), (16000, 1, 2))
                self.assertEqual(wav.getnframes() % 320, 0)

    @unittest.skipUnless(shutil.which("ffmpeg"), "optional local CPU ffmpeg absent")
    def test_actual_ffmpeg_pipe_only_cpu_decode(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = make_source(root)
            ffmpeg = Path(shutil.which("ffmpeg")).resolve(strict=True)
            result = prep.prepare(
                source, hashlib.sha256(source.read_bytes()).hexdigest(),
                ffmpeg, root/"out.wav", root/"info.json", True)
            self.assertEqual(result["frame_count"], 60)
            self.assertEqual(result["tail_samples_discarded"], 0)
            self.assertEqual(len((root/"out.wav").read_bytes()), 44 + 60*320*2)


if __name__ == "__main__":
    unittest.main()
