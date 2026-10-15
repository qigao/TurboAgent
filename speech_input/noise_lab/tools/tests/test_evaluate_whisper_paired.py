"""Synthetic-only paired evaluator tests. No actual model/weights are used."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
import wave

SCRIPT = Path(__file__).resolve().parents[1] / "evaluate_whisper_paired.py"
spec = importlib.util.spec_from_file_location("paired_asr", SCRIPT)
paired = importlib.util.module_from_spec(spec)
spec.loader.exec_module(paired)


def wave_bytes(amplitude: int, frames=60, rate=16000):
    raw=io.BytesIO()
    with wave.open(raw,"wb") as writer:
        writer.setnchannels(1)
        writer.setsampwidth(2)
        writer.setframerate(rate)
        for _ in range(frames):
            writer.writeframes(struct.pack("<320h", *([amplitude,-amplitude]*160)))
    return raw.getvalue()


def pinned(path: Path):
    return path, hashlib.sha256(path.read_bytes()).hexdigest()


def fixture(root: Path):
    cli=Path(sys.executable).resolve(strict=True)
    model=root/"caller-local-mock-weights.bin"
    model.write_bytes(b"TEST ONLY; NOT A WHISPER MODEL")
    reference=root/"private-reference.txt"
    reference.write_text("Hello world\n",encoding="utf-8")
    wavs=[]
    for index, amp in enumerate((120,240,360)):
        source=root/("private-audio-%s.wav"%index)
        source.write_bytes(wave_bytes(amp))
        wavs.append(pinned(source))
    return cli, pinned(cli)[1], *pinned(model), *wavs, *pinned(reference)


class PairedASRValidation(unittest.TestCase):
    def test_three_conditions_only_numeric_delta_and_explicit_digest(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            args=fixture(root)
            calls=[]
            outputs=[b"Hello world\n",b"hello\n",b"hello whirl\n"]
            def fake_run(cmd, **opts):
                calls.append((cmd,opts))
                return SimpleNamespace(returncode=0,stdout=outputs[len(calls)-1],
                                       stderr=b"")
            report=paired.evaluate(*args[:7],*args[7:9],"en",
                                   critical_words=("world",),runner=fake_run)
            self.assertEqual(len(calls),3)
            self.assertEqual(report["frame_count"],60)
            self.assertEqual(report["critical_words_requested"],1)
            self.assertEqual(report["conditions"]["original"]["wer"],0)
            self.assertEqual(report["conditions"]["ns12"]["wer"],0.5)
            self.assertEqual(report["conditions"]["ns18"]["wer"],0.5)
            self.assertEqual(report["conditions"]["ns12"]["wer_delta_from_original"],0.5)
            self.assertEqual(report["conditions"]["ns12"]["critical_word_retention_delta"],-1)
            for cmd,kw in calls:
                self.assertEqual(cmd[cmd.index("--stationarity-guard")+1],"0")
                self.assertEqual(cmd[cmd.index("--model")+1],str(args[2]))
                self.assertEqual(cmd[cmd.index("--sha256")+1],args[3])
                self.assertEqual(kw["capture_output"],True)
                self.assertFalse(kw["check"])
                self.assertEqual(kw["timeout"],180)
            text=json.dumps(report)
            for private in ("private-audio","private-reference","Hello world",
                            "whirl","TEST ONLY",args[3]):
                self.assertNotIn(private,text)
            self.assertTrue(report["model_or_audio_downloaded_by_evaluator"] is False)
            self.assertFalse(report["single_utterance_quality_certified"])

    def test_no_speech_variant_is_not_fabricated_good_wer(self):
        with tempfile.TemporaryDirectory() as td:
            args=fixture(Path(td))
            values=[
                SimpleNamespace(returncode=0,stdout=b"Hello world\n",stderr=b""),
                SimpleNamespace(returncode=0,stdout=b"",
                     stderr=b"NO_SPEECH (no transcript emitted)\n"),
                SimpleNamespace(returncode=0,stdout=b"Hello world\n",stderr=b"")
            ]
            def fake(cmd,**opts):
                return values.pop(0)
            report=paired.evaluate(*args[:7],*args[7:9],"en",runner=fake)
            n=report["conditions"]["ns12"]
            self.assertEqual(n["status"],"no_speech_without_guard")
            self.assertTrue(n["dropped_speech_before_scoring"])
            self.assertIsNone(n["wer"])
            self.assertIsNone(n["cer"])
            self.assertIsNone(n["wer_delta_from_original"])

    def test_model_digest_and_wav_format_fail_before_engine_runs(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            args=list(fixture(root))
            runs=[]
            def never_run(*a,**kw):
                runs.append(a)
                raise AssertionError("must not call model")
            args[3]="0"*64
            with self.assertRaises(ValueError):
                paired.evaluate(*args[:7],*args[7:9],"en",runner=never_run)
            self.assertEqual(runs,[])
            args=list(fixture(root))
            args[4][0].write_bytes(wave_bytes(100,rate=8000))
            args[4]=(args[4][0],hashlib.sha256(args[4][0].read_bytes()).hexdigest())
            with self.assertRaises(ValueError):
                paired.evaluate(*args[:7],*args[7:9],"en",runner=never_run)
            self.assertEqual(runs,[])

    def test_distinct_duration_fails_before_model_and_no_alternate_fallback(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            args=list(fixture(root))
            wav,sha=args[6]
            wav.write_bytes(wave_bytes(100,frames=61))
            args[6]=pinned(wav)
            def forbidden(*a,**kw):
                raise AssertionError("cannot infer before exact-frame validation")
            with self.assertRaisesRegex(ValueError,"NS changed number"):
                paired.evaluate(*args[:7],*args[7:9],"en",runner=forbidden)

    def test_vendor_error_and_unsafe_transcript_refuse_without_leak(self):
        with tempfile.TemporaryDirectory() as td:
            args=fixture(Path(td))
            def failed(*a,**kw):
                return SimpleNamespace(returncode=4,stdout=b"",
                                       stderr=b"private model path and transcript")
            with self.assertRaisesRegex(ValueError,"inference failed"):
                paired.evaluate(*args[:7],*args[7:9],"en",runner=failed)
            def injected(*a,**kw):
                return SimpleNamespace(returncode=0,stdout=b"\x1b[31mspoof",
                                       stderr=b"")
            with self.assertRaisesRegex(ValueError,"unsafe control"):
                paired.evaluate(*args[:7],*args[7:9],"en",runner=injected)

    def test_invalid_critical_keyword_and_raw_reference_fail_closed(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            args=fixture(root)
            def refuse(*a,**kw):
                raise AssertionError("unreachable")
            for word in ("absent", "two words","world world"):
                with self.assertRaises(ValueError):
                    paired.evaluate(*args[:7],*args[7:9],"en",
                                    critical_words=(word,),runner=refuse)
            reference=args[7]
            reference.write_bytes(b"hello\x00world")
            values=list(args)
            values[8]=hashlib.sha256(reference.read_bytes()).hexdigest()
            with self.assertRaises(ValueError):
                paired.evaluate(*values[:7],*values[7:9],"en",runner=refuse)

    def test_missing_or_unapproved_model_has_no_report_or_private_text(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            a=fixture(root)
            report=root/"private-observations.json"
            cmd=[sys.executable,str(SCRIPT),
                 "--cli",str(a[0]),"--cli-sha256",a[1],
                 "--model",str(root/"not-authorized-model.ggml"),
                 "--model-sha256","0"*64,
                 "--original-wav",str(a[4][0]),"--original-sha256",a[4][1],
                 "--ns12-wav",str(a[5][0]),"--ns12-sha256",a[5][1],
                 "--ns18-wav",str(a[6][0]),"--ns18-sha256",a[6][1],
                 "--reference-file",str(a[7]),"--reference-sha256",a[8],
                 "--language","en","--report",str(report)]
            result=subprocess.run(cmd,capture_output=True,text=True,check=False)
            self.assertEqual(result.returncode,2)
            self.assertEqual(result.stdout,"")
            self.assertFalse(report.exists())
            self.assertNotIn("not-authorized-model",result.stderr)
            self.assertNotIn("private-",result.stderr)

    def test_metadata_report_exclusive_0600(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            report=root/"metrics.json"
            paired._write_private_report(report,{"schema":1,"wer":0.5})
            self.assertEqual(report.stat().st_mode & 0o777,0o600)
            self.assertEqual(json.loads(report.read_text())["wer"],0.5)
            with self.assertRaises(ValueError):
                paired._write_private_report(report,{"schema":1,"wer":0.1})


if __name__=="__main__":
    unittest.main()
