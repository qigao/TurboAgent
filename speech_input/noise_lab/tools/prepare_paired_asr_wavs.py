#!/usr/bin/env python3
"""Prepare exact three-arm NS WAV pairs in a private empty directory.

TEST-ONLY C11 SpeexDSP adapter orchestration. No ASR model is loaded,
downloaded, searched for or inferred. No microphone, shell or Agent tools.
This creates exactly three SHA-pinned 16k/mono/S16 WAVs and one PRIVATE
manifest, deleting transient RAW PCM and all partial outputs on failure.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import struct
import sys
import wave

TOOLS = Path(__file__).resolve().parents[2] / "whisper" / "tools"
sys.path.insert(0, str(TOOLS))
from measure_frame_stationarity import inspect_wav_bytes  # noqa: E402
from prepare_stationarity_wav import bounded_decoder_run  # noqa: E402

FRAME = 320
MAX_FRAMES = 3000
MAX_RAW_BYTES = FRAME * 2 * MAX_FRAMES
MAX_WAV_BYTES = 4 * 1024 * 1024
MAX_PROBE_BYTES = 32 * 1024 * 1024
OUTPUT_NAMES = ("original.wav", "ns12.wav", "ns18.wav",
                "paired-private.json")
TRANSIENT_NAMES = ("temporary-input.raw", "temporary-ns12.raw",
                   "temporary-ns18.raw")


def digest(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def is_sha(value: str) -> bool:
    return (isinstance(value, str) and len(value) == 64
            and all(ch in "0123456789abcdef" for ch in value))


def bounded_file(path: Path, expected_sha: str, max_bytes: int) -> bytes:
    if (not isinstance(path, Path) or not path.is_absolute()
            or path.is_symlink() or not path.is_file()
            or not is_sha(expected_sha) or max_bytes < 1):
        raise ValueError("invalid private local source or SHA")
    with path.open("rb") as inp:
        raw = inp.read(max_bytes + 1)
    if not raw or len(raw) > max_bytes or digest(raw) != expected_sha:
        raise ValueError("private SHA mismatch or byte limit exceeded")
    return raw


def write_new(path: Path, raw: bytes) -> None:
    if not path.is_absolute() or path.exists() or path.is_symlink():
        raise ValueError("exclusive private destination required")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    fd = os.open(str(path), flags, 0o600)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "wb") as output:
            output.write(raw)
    except BaseException:
        path.unlink(missing_ok=True)
        raise


def _unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate NS probe JSON field")
        result[key] = value
    return result


def wav_pcm(pinned_wav: bytes) -> tuple[bytes, int]:
    if (len(pinned_wav) < 12 or pinned_wav[:4] != b"RIFF"
            or pinned_wav[8:12] != b"WAVE"
            or int.from_bytes(pinned_wav[4:8], "little") != len(pinned_wav) - 8):
        raise ValueError("invalid exact RIFF container")
    metrics = inspect_wav_bytes(pinned_wav)
    with wave.open(io.BytesIO(pinned_wav), "rb") as stream:
        frames = stream.getnframes()
        pcm = stream.readframes(frames)
    if (frames < 1 or frames > MAX_FRAMES or frames % FRAME != 0
            or len(pcm) != frames * 2
            or metrics["sample_count"] != frames):
        raise ValueError("invalid exact 320-S16 sample framing")
    return pcm, frames // FRAME


def wrapped_raw(raw: bytes, expected_frames: int) -> bytes:
    if (not isinstance(raw, bytes)
            or len(raw) != expected_frames * FRAME * 2):
        raise ValueError("SpeexDSP changed exact S16 frame count")
    output = io.BytesIO()
    with wave.open(output, "wb") as stream:
        stream.setnchannels(1)
        stream.setsampwidth(2)
        stream.setframerate(16000)
        stream.writeframes(raw)
    wave_bytes = output.getvalue()
    actual = inspect_wav_bytes(wave_bytes)
    if actual["frame_count"] != expected_frames:
        raise ValueError("canonical SpeexDSP WAV frame count mismatch")
    return wave_bytes


def prepare(source: Path, source_sha: str, executable: Path,
            executable_sha: str, private_root: Path,
            runner=bounded_decoder_run) -> dict:
    if (not isinstance(private_root, Path) or not private_root.is_absolute()
            or private_root.is_symlink() or not private_root.is_dir()
            or stat.S_IMODE(private_root.stat().st_mode) != 0o700):
        raise ValueError("require existing empty private 0700 output directory")
    if next(private_root.iterdir(), None) is not None:
        raise ValueError("private root must be empty: no overwrite or alias")
    original = bounded_file(source, source_sha, MAX_WAV_BYTES)
    source_pcm, expected_frames = wav_pcm(original)
    if len(source_pcm) > MAX_RAW_BYTES:
        raise ValueError("private speech frame capacity exceeded")
    native = bounded_file(executable, executable_sha, MAX_PROBE_BYTES)
    if not os.access(executable, os.X_OK):
        raise ValueError("SpeexDSP diagnostic executable not authorized")
    created = []
    digest_by_condition = {"original": source_sha}
    try:
        # The canonical source WAV is reproduced byte-exactly; do not
        # modify or re-encode a single original voice sample.
        dest = private_root / OUTPUT_NAMES[0]
        write_new(dest, original)
        created.append(dest)
        in_raw = private_root / TRANSIENT_NAMES[0]
        write_new(in_raw, source_pcm)
        created.append(in_raw)

        for db, label, raw_name, wav_name in (
            (-12, "ns12", TRANSIENT_NAMES[1], OUTPUT_NAMES[1]),
            (-18, "ns18", TRANSIENT_NAMES[2], OUTPUT_NAMES[2]),
        ):
            ns_raw_file = private_root / raw_name
            command = [str(executable), str(in_raw), str(ns_raw_file),
                       str(db), "--align-one-frame"]
            observed = runner(command, input=b"", capture_output=True,
                              timeout=45, check=False)
            # A failed native process may still leave a partial output file.
            # Register it for rollback BEFORE validating its return status.
            if ns_raw_file.exists() or ns_raw_file.is_symlink():
                created.append(ns_raw_file)
            if (type(observed.returncode) is not int
                    or observed.returncode != 0
                    or not isinstance(observed.stdout, bytes)
                    or not isinstance(observed.stderr, bytes)
                    or len(observed.stdout) > 4096
                    or len(observed.stderr) > 16384):
                raise ValueError("native C11 noise suppressor failed or overflowed")
            report = json.loads(observed.stdout.decode("utf-8"),
                                object_pairs_hook=_unique_pairs)
            if (not isinstance(report, dict)
                    or report.get("aligned_one_frame_with_drain") is not True
                    or type(report.get("frames")) is not int
                    or report["frames"] != expected_frames
                    or type(report.get("noise_suppress_db")) is not int
                    or report["noise_suppress_db"] != db
                    or report.get("frame_samples") != FRAME
                    or report.get("rate") != 16000
                    or report.get("vad_enabled") is not False
                    or report.get("agc_enabled") is not False
                    or report.get("no_model_download") is not True):
                raise ValueError("unapproved native SpeexDSP mode or frame count")
            if (not ns_raw_file.is_file() or ns_raw_file.is_symlink()
                    or ns_raw_file.stat().st_size > MAX_RAW_BYTES
                    or stat.S_IMODE(ns_raw_file.stat().st_mode) != 0o600):
                raise ValueError("missing, unbounded or nonprivate DSP output")
            with ns_raw_file.open("rb") as source_file:
                filtered = source_file.read(MAX_RAW_BYTES + 1)
            canonical = wrapped_raw(filtered, expected_frames)
            dest = private_root / wav_name
            write_new(dest, canonical)
            created.append(dest)
            digest_by_condition[label] = digest(canonical)

        # Hash-check all exact file inputs after the external native C11
        # decoder has returned. Any drift invalidates the entire comparison.
        if (bounded_file(source, source_sha, MAX_WAV_BYTES) != original
                or bounded_file(executable, executable_sha,
                                MAX_PROBE_BYTES) != native
                or (private_root / TRANSIENT_NAMES[0]).read_bytes() != source_pcm):
            raise ValueError("input/NS executable changed during paired preparation")
        for label, name in (("original", OUTPUT_NAMES[0]),
                            ("ns12", OUTPUT_NAMES[1]),
                            ("ns18", OUTPUT_NAMES[2])):
            current = private_root / name
            observed_bytes = bounded_file(
                current, digest_by_condition[label], MAX_WAV_BYTES)
            if label == "original" and observed_bytes != original:
                raise ValueError("inconsistent original private WAV")
            _, frame_count = wav_pcm(observed_bytes)
            if frame_count != expected_frames:
                raise ValueError("inconsistent private frame count")
        report = {
            "schema": 1,
            "test_kind": "private_source_identical_3_condition_ns_wav_bundle",
            "model_weights_or_inference_used": False,
            "frame_count": expected_frames,
            "sample_rate": 16000,
            "frame_samples": FRAME,
            "selected_speexdsp_executable_sha256": executable_sha,
            "source_wav_sha256": source_sha,
            "wavs": {
                name: {"filename": name+".wav",
                       "sha256": digest_by_condition[name],
                       "frames": expected_frames}
                for name in ("original", "ns12", "ns18")
            },
            "ns_configuration": {"ns12": -12, "ns18": -18,
                                 "VAD": False, "AGC": False,
                                 "tail_flush": "one_frame_explicit"},
            "no_transcripts_or_pcm_in_report": True,
            "speaker_population_quality_certified": False,
        }
        manifest = private_root / OUTPUT_NAMES[3]
        write_new(manifest, (json.dumps(report, sort_keys=True, indent=2)+"\n").encode())
        created.append(manifest)
        for transient_name in TRANSIENT_NAMES:
            transient_path = private_root / transient_name
            transient_path.unlink()
            created.remove(transient_path)
        return {
            "status": "private_three_arm_wavs_prepared",
            "frame_count": expected_frames,
            "arms": 3,
            "no_model_or_download": True,
            "quality_or_intelligibility_certified": False,
        }
    except BaseException:
        for name in reversed(created):
            name.unlink(missing_ok=True)
        raise


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source-wav", type=Path, required=True)
    p.add_argument("--source-sha256", required=True)
    p.add_argument("--ns-probe", type=Path, required=True)
    p.add_argument("--ns-probe-sha256", required=True)
    p.add_argument("--private-root", type=Path, required=True)
    args = p.parse_args()
    try:
        data = prepare(args.source_wav, args.source_sha256,
                       args.ns_probe, args.ns_probe_sha256,
                       args.private_root)
        print(json.dumps(data, sort_keys=True))
        return 0
    except (OSError, ValueError, TypeError, UnicodeError, EOFError,
            json.JSONDecodeError, wave.Error, struct.error):
        print("private SpeexDSP three-arm WAV preparation refused",
              file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
