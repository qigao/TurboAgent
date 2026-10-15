#!/usr/bin/env python3
"""Prepare a rights-reviewed, caller-supplied local audio clip for offline tests.

Never downloads assets, opens microphones or loads models. ffmpeg receives the
exact pinned source bytes through stdin and only the pipe protocol is allowed.
The tool writes one private strict 16k/mono/S16LE/20ms-aligned WAV plus a
private conversion evidence file. It never writes a public corpus report.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import threading
import wave
from types import SimpleNamespace

from measure_frame_stationarity import inspect_wav_bytes

MAX_SOURCE_BYTES = 16 * 1024 * 1024
RATE = 16000
FRAME = 320
MAX_SAMPLES = 60 * RATE
MAX_DECODE_BYTES = 62 * RATE * 2
MAX_SOURCE_SHA = re.compile(r"[a-f0-9]{64}", flags=re.ASCII)


def digest_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def digest_file(path: Path) -> str:
    if not path.is_absolute() or path.is_symlink() or not path.is_file():
        raise ValueError("decoder executable path invalid")
    h = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            block = stream.read(65536)
            if not block:
                break
            h.update(block)
    return h.hexdigest()


def read_pinned_source(path: Path, expected_sha: str) -> bytes:
    if (not path.is_absolute() or path.is_symlink()
            or not path.is_file() or not isinstance(expected_sha, str)
            or not MAX_SOURCE_SHA.fullmatch(expected_sha)):
        raise ValueError("source must be an absolute, pinned regular file")
    with path.open("rb") as stream:
        raw = stream.read(MAX_SOURCE_BYTES + 1)
    if not raw or len(raw) > MAX_SOURCE_BYTES:
        raise ValueError("source bytes missing or over limit")
    if digest_bytes(raw) != expected_sha:
        raise ValueError("source SHA-256 mismatch")
    return raw



def bounded_decoder_run(command, *, input, capture_output, timeout, check):
    """Avoid subprocess.run's unbounded capture when a decoder misbehaves.

    Read stdout/stderr concurrently with the child and kill on capped output.
    The input writer runs in a separate thread so the process cannot deadlock
    after filling its stdout pipe while waiting for more source bytes.
    """
    if not capture_output or check or not isinstance(input, bytes):
        raise ValueError("invalid bounded decoder invocation")
    process = subprocess.Popen(command, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stdout_chunks, stderr_chunks = [], []
    fault = [False]
    writer_errors = []

    def reader(stream, chunks, limit):
        used = 0
        try:
            while True:
                block = stream.read(65536)
                if not block:
                    break
                used += len(block)
                if used > limit:
                    fault[0] = True
                    process.kill()
                    break
                chunks.append(block)
        except (OSError, ProcessLookupError) as exc:
            writer_errors.append(exc)
        finally:
            stream.close()

    def writer():
        try:
            process.stdin.write(input)
            process.stdin.close()
        except (OSError, BrokenPipeError, ValueError):
            try:
                process.stdin.close()
            except OSError:
                pass

    threads = [
        threading.Thread(target=reader,
                         args=(process.stdout, stdout_chunks, MAX_DECODE_BYTES),
                         daemon=True),
        threading.Thread(target=reader, args=(process.stderr, stderr_chunks, 16384),
                         daemon=True),
        threading.Thread(target=writer, daemon=True),
    ]
    for thread in threads:
        thread.start()
    timed_out = False
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        process.kill()
        process.wait()
    finally:
        for thread in threads:
            thread.join(timeout=5)
    if timed_out or fault[0] or writer_errors or any(t.is_alive() for t in threads):
        raise ValueError("decoder timed out or exceeded bounded output")
    return SimpleNamespace(returncode=process.returncode,
                           stdout=b"".join(stdout_chunks),
                           stderr=b"".join(stderr_chunks))

def convert_snapshot(raw: bytes, ffmpeg: Path, runner=None):
    if not isinstance(raw, bytes) or not raw or len(raw) > MAX_SOURCE_BYTES:
        raise ValueError("invalid source snapshot")
    if not ffmpeg.is_absolute() or ffmpeg.is_symlink() or not ffmpeg.is_file():
        raise ValueError("ffmpeg must be explicit absolute regular executable")
    if not os.access(ffmpeg, os.X_OK):
        raise ValueError("ffmpeg is not executable")
    # Input is piped from the same bytes already verified. Disallow file,
    # HTTP(S), concat and any other secondary ffmpeg protocol resolution.
    # Decode at most 61 seconds to DETECT, not silently truncate, >60s input.
    command = [
        str(ffmpeg), "-nostdin", "-hide_banner", "-loglevel", "error",
        "-protocol_whitelist", "pipe",
        "-i", "pipe:0", "-map", "0:a:0", "-vn", "-sn", "-dn",
        "-threads", "1", "-ar", str(RATE), "-ac", "1",
        "-c:a", "pcm_s16le", "-f", "s16le",
        "-t", "61", "-fs", str(MAX_DECODE_BYTES), "pipe:1",
    ]
    if runner is None:
        runner = bounded_decoder_run
    try:
        result = runner(command, input=raw, capture_output=True,
                        timeout=45, check=False)
    except (subprocess.TimeoutExpired, OSError) as exc:
        raise ValueError("audio decoder unavailable or timed out") from exc
    if result.returncode != 0:
        raise ValueError("audio decoder rejected the input")
    pcm = result.stdout
    if not isinstance(pcm, bytes) or not pcm or len(pcm) % 2:
        raise ValueError("decoder returned invalid signed PCM16")
    count = len(pcm) // 2
    if count > MAX_SAMPLES:
        raise ValueError("source exceeds 60-second bounded admission")
    aligned_count = (count // FRAME) * FRAME
    if aligned_count == 0:
        raise ValueError("clip is shorter than one complete frame")
    trimmed = count - aligned_count
    # Trim at most one *incomplete* (<20ms) final frame. Never pad artificial
    # zero-signal energy into the stationary detector; never run VAD to crop.
    pcm = pcm[:aligned_count * 2]
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(RATE)
        output.writeframes(pcm)
    canonical = buffer.getvalue()
    metrics = inspect_wav_bytes(canonical)
    if metrics["sample_count"] != aligned_count:
        raise ValueError("canonical WAV frame count mismatch")
    return canonical, {
        "normalized_wav_sha256": digest_bytes(canonical),
        "decoded_pcm_sha256": digest_bytes(pcm),
        "frames": aligned_count // FRAME,
        "tail_samples_discarded": trimmed,
        "sample_rate": RATE,
        "channel_count": 1,
        "sample_format": "signed_16_bit_little_endian",
        "frame_samples": FRAME,
        "predecode_stationarity_hypothesis": metrics["stationarity_hypothesis"],
    }


def write_private_new(path: Path, blob: bytes) -> None:
    if not path.is_absolute() or not path.parent.is_dir() or path.is_symlink():
        raise ValueError("destination must be absolute with existing directory")
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, "wb") as out:
            out.write(blob)
        os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
    except BaseException:
        try:
            path.unlink()
        except OSError:
            pass
        raise


def prepare(source: Path, expected_sha: str, ffmpeg: Path,
            destination: Path, provenance: Path, rights_acknowledged: bool,
            runner=None) -> dict:
    if not rights_acknowledged:
        raise ValueError("source license/consent must be explicitly acknowledged")
    if destination == provenance or destination == source or provenance == source:
        raise ValueError("input and output destinations must be distinct")
    if destination.exists() or provenance.exists():
        raise ValueError("existing destination cannot be overwritten")
    if not destination.is_absolute() or not provenance.is_absolute():
        raise ValueError("all output paths must be absolute")
    source_bytes = read_pinned_source(source, expected_sha)
    # Pin the selected decoder before use and re-check after it exits.
    # A changed binary cannot be attributed to one immutable tool revision.
    decoder_sha_before = digest_file(ffmpeg)
    normalized, metadata = convert_snapshot(source_bytes, ffmpeg, runner)
    decoder_sha_after = digest_file(ffmpeg)
    if decoder_sha_before != decoder_sha_after:
        raise ValueError("decoder executable changed during conversion")
    evidence = {
        "schema": 1,
        "mode": "local_private_single_snapshot_only",
        "rights_and_consent_user_declared_not_verified": True,
        "source_sha256": expected_sha,
        "ffmpeg_executable_sha256": decoder_sha_before,
        "model_weights_used": False,
        "model_inference_used": False,
        "audio_downloaded": False,
        "source_and_pcm_recordings_private": True,
        **metadata,
    }
    write_private_new(destination, normalized)
    try:
        write_private_new(provenance,
                          (json.dumps(evidence, sort_keys=True, indent=2) + "\n").encode())
    except BaseException:
        destination.unlink(missing_ok=True)
        raise
    # No source path, file hashes, private utterance text or corpus labels on
    # stdout; provenance contains them in the separate private 0600 file.
    return {
        "status": "prepared_private_audio",
        "frame_count": metadata["frames"],
        "tail_samples_discarded": metadata["tail_samples_discarded"],
        "no_model_or_network": True,
        "not_speech_quality_certified": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--source-sha256", required=True)
    parser.add_argument("--ffmpeg", type=Path, required=True)
    parser.add_argument("--output-wav", type=Path, required=True)
    parser.add_argument("--private-provenance", type=Path, required=True)
    parser.add_argument("--acknowledge-rights", action="store_true",
                        help="Confirm you independently reviewed rights/consent")
    args = parser.parse_args()
    try:
        report = prepare(args.source, args.source_sha256, args.ffmpeg,
                         args.output_wav, args.private_provenance,
                         args.acknowledge_rights)
        print(json.dumps(report, sort_keys=True))
        return 0
    except (OSError, ValueError, TypeError, OverflowError) as exc:
        # Never expose source filenames, decoder stderr or source text.
        print(f"private WAV preparation rejected: {type(exc).__name__}",
              file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
