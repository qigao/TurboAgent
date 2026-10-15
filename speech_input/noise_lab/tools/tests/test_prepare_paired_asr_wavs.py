"""Three-arm private WAV preparation — synthetic only, no actual model."""
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
import wave

SCRIPT = Path(__file__).resolve().parents[1] / "prepare_paired_asr_wavs.py"
spec = importlib.util.spec_from_file_location("three_arm_preparation", SCRIPT)
pair = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pair)


def make_source(frames=65, rate=16000):
    data = io.BytesIO()
    with wave.open(data, "wb") as stream:
        stream.setnchannels(1)
        stream.setsampwidth(2)
        stream.setframerate(rate)
        for frame in range(frames):
            a = 500 + frame % 5 * 200
            stream.writeframes(struct.pack("<320h", *([a, -a] * 160)))
    return data.getvalue()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fixture(root):
    source = root/"private-licensed-original.wav"
    source.write_bytes(make_source())
    native = root/"local-speex-test-probe.bin"
    native.write_bytes(b"test-only executable fingerprint; cannot process samples")
    native.chmod(0o700)
    dest = root/"pairs"
    dest.mkdir(mode=0o700)
    return source, native, dest


class NativeProbeStub:
    def __init__(self):
        self.calls = []
        self.error = None
        self.bad_mode = None
        self.mutate_executable = False
        self.alter_source = False

    def __call__(self, cmd, **kwargs):
        self.calls.append((cmd, kwargs))
        assert kwargs["input"] == b""
        assert kwargs["capture_output"] is True
        assert kwargs["timeout"] == 45
        assert kwargs["check"] is False
        assert cmd[4] == "--align-one-frame"
        original = Path(cmd[1]).read_bytes()
        db = int(cmd[3])
        amp = 2 if db == -18 else 1
        values = struct.unpack("<%dh" % (len(original)//2), original)
        processed = struct.pack("<%dh" % len(values),
                                *(int(v/amp) for v in values))
        output = Path(cmd[2])
        with output.open("xb") as stream:
            stream.write(processed)
        output.chmod(0o600)
        if self.bad_mode == "nonprivate":
            output.chmod(0o644)
        if self.bad_mode == "wrong_count":
            output.write_bytes(processed[:-2])
        if self.bad_mode == "missing":
            output.unlink()
        if self.mutate_executable:
            Path(cmd[0]).write_bytes(b"test-only binary mutated after first use")
        if self.alter_source:
            Path(self.alter_source).write_bytes(make_source(70))
        report = {
            "library":"SpeexDSP-1.2.1",
            "frame_samples":320,
            "rate":16000,
            "frames":len(original)//640,
            "noise_suppress_db":db,
            "aligned_one_frame_with_drain":True,
            "vad_enabled":False,"agc_enabled":False,"no_model_download":True
        }
        if self.bad_mode == "wrong_report":
            report["aligned_one_frame_with_drain"] = False
        if self.bad_mode == "duplicate_json":
            json_text = '{"frames":1,"frames":2}'
        else:
            json_text = json.dumps(report)
        return SimpleNamespace(returncode=1 if self.error else 0,
                               stdout=json_text.encode("utf-8"),
                               stderr=b"internal private source and raw audio")


class AtomicThreeArmTests(unittest.TestCase):
    def test_exact_wav_sample_count_vs_20ms_frame_count_capacity(self):
        _, frames = pair.wav_pcm(make_source(65))
        self.assertEqual(frames, 65)
        _, max_frames = pair.wav_pcm(make_source(pair.MAX_FRAMES))
        self.assertEqual(max_frames, pair.MAX_FRAMES)
        with self.assertRaises(ValueError):
            pair.wav_pcm(make_source(pair.MAX_FRAMES + 1))
        with self.assertRaises(ValueError):
            pair.wav_pcm(make_source(0))

    def test_success_exact_original_two_separate_ns_states_and_no_raw_left(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            src, exe, out = fixture(root)
            probe = NativeProbeStub()
            result = pair.prepare(src,sha(src),exe,sha(exe),out,runner=probe)
            self.assertEqual(result["frame_count"],65)
            self.assertEqual(result["arms"],3)
            self.assertTrue(result["no_model_or_download"])
            self.assertEqual(len(probe.calls),2)
            self.assertEqual([x[0][3] for x in probe.calls],["-12","-18"])
            self.assertEqual((out/"original.wav").read_bytes(),src.read_bytes())
            wav = [out/x for x in ("original.wav","ns12.wav","ns18.wav")]
            for item in wav:
                self.assertEqual(stat.S_IMODE(item.stat().st_mode),0o600)
                _, frames = pair.wav_pcm(item.read_bytes())
                self.assertEqual(frames,65)
            for name in pair.TRANSIENT_NAMES:
                self.assertFalse((out/name).exists())
            self.assertEqual(sorted(x.name for x in out.iterdir()),
                             sorted(pair.OUTPUT_NAMES))
            manifest=json.loads((out/"paired-private.json").read_text())
            self.assertEqual(manifest["frame_count"],65)
            self.assertEqual(manifest["source_wav_sha256"],sha(src))
            for label,item in manifest["wavs"].items():
                self.assertEqual(sha(out/item["filename"]),item["sha256"])
            exposed=json.dumps(result)
            for private in ("licensed-original",sha(src),str(src),
                            "local-speex-test-probe","MODEL"):
                self.assertNotIn(private,exposed)

    def test_digest_or_invalid_wav_rejected_before_native_invocation(self):
        with tempfile.TemporaryDirectory() as temp:
            src,exe,out=fixture(Path(temp))
            stub=NativeProbeStub()
            for src_digest,exe_digest in (("0"*64,sha(exe)),
                                          (sha(src),"0"*64)):
                with self.assertRaisesRegex(ValueError,"SHA"):
                    pair.prepare(src,src_digest,exe,exe_digest,out,stub)
            self.assertFalse(any(out.iterdir()))
            self.assertFalse(stub.calls)
            src.write_bytes(make_source(60,rate=8000))
            with self.assertRaises(ValueError):
                pair.prepare(src,sha(src),exe,sha(exe),out,stub)
            self.assertFalse(stub.calls)

    def test_native_partial_output_rolls_back_and_preserves_existing(self):
        for failure in ("exit","wrong_report","missing","wrong_count",
                        "nonprivate","duplicate_json"):
            with self.subTest(failure=failure):
                with tempfile.TemporaryDirectory() as temp:
                    src,exe,out=fixture(Path(temp))
                    stub=NativeProbeStub()
                    if failure=="exit":
                        stub.error=True
                    else:
                        stub.bad_mode=failure
                    with self.assertRaises((ValueError,FileNotFoundError)):
                        pair.prepare(src,sha(src),exe,sha(exe),out,stub)
                    self.assertFalse(any(out.iterdir()),
                                     "Failed native must remove every RAW and WAV")

    def test_native_timeout_after_writing_partial_raw_rolls_back_all_outputs(self):
        for crash_arm in (1, 2):
            with self.subTest(crash_arm=crash_arm):
                with tempfile.TemporaryDirectory() as temp:
                    src, exe, out = fixture(Path(temp))
                    legitimate = NativeProbeStub()
                    call_count = [0]

                    def native_crash(command, **kwargs):
                        call_count[0] += 1
                        if call_count[0] == crash_arm:
                            Path(command[2]).write_bytes(b"\x12" * 640)
                            raise ValueError("native timeout after partial PCM")
                        return legitimate(command, **kwargs)

                    with self.assertRaisesRegex(ValueError, "timeout"):
                        pair.prepare(src, sha(src), exe, sha(exe),
                                     out, runner=native_crash)
                    self.assertEqual(call_count[0], crash_arm)
                    self.assertEqual(list(out.iterdir()), [])

    def test_binary_or_original_changes_during_native_inference(self):
        for kind in ("exe","source"):
            with self.subTest(kind=kind):
                with tempfile.TemporaryDirectory() as temp:
                    src,exe,out=fixture(Path(temp))
                    stub=NativeProbeStub()
                    if kind=="exe":
                        stub.mutate_executable=True
                    else:
                        stub.alter_source=src
                    with self.assertRaisesRegex(ValueError,"SHA|changed during"):
                        pair.prepare(src,sha(src),exe,sha(exe),out,stub)
                    self.assertEqual(list(out.iterdir()),[])

    def test_missing_private_directory_or_existing_output_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as temp:
            src,exe,out=fixture(Path(temp))
            stub=NativeProbeStub()
            out.chmod(0o755)
            with self.assertRaisesRegex(ValueError,"private 0700"):
                pair.prepare(src,sha(src),exe,sha(exe),out,stub)
            out.chmod(0o700)
            preexisting=out/"unrelated-owner.txt"
            preexisting.write_text("do not remove")
            with self.assertRaisesRegex(ValueError,"empty"):
                pair.prepare(src,sha(src),exe,sha(exe),out,stub)
            self.assertEqual(preexisting.read_text(),"do not remove")
            self.assertFalse(stub.calls)

    def test_cli_never_exposes_private_paths_when_model_or_source_missing(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            src,exe,out=fixture(root)
            proc=subprocess.run([
                sys.executable,str(SCRIPT),
                "--source-wav",str(src),
                "--source-sha256","0"*64,
                "--ns-probe",str(exe),
                "--ns-probe-sha256",sha(exe),
                "--private-root",str(out)
            ], capture_output=True,text=True,check=False)
            self.assertEqual(proc.returncode,2)
            self.assertEqual(proc.stdout,"")
            self.assertNotIn(str(src),proc.stderr)
            self.assertNotIn("licensed-original",proc.stderr)
            self.assertFalse(any(out.iterdir()))


if __name__ == "__main__":
    unittest.main()
