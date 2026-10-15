#!/usr/bin/env python3
"""Private, opt-in paired Whisper quality metrics. NO asset download or tools.

Caller explicitly supplies an already authorized SHA-pinned local model,
an exact local CLI and three exact S16LE WAV snapshots. This module never
retrieves weights, executes Agent tools, persists transcripts or uses VAD/NS.
"""
import argparse
import hashlib
import io
import json
import math
import os
from pathlib import Path
import re
import sys
import time
import unicodedata
import wave

WHISPER_TOOLS = Path(__file__).resolve().parents[2] / "whisper" / "tools"
sys.path.insert(0, str(WHISPER_TOOLS))
from evaluate_asr import _words, score  # noqa: E402
from measure_frame_stationarity import inspect_wav_bytes  # noqa: E402
from prepare_stationarity_wav import bounded_decoder_run  # noqa: E402

PIN = re.compile(r"[0-9a-f]{64}", flags=re.ASCII)
MAX_CLI_BYTES = 32 * 1024 * 1024
MAX_WAV_BYTES = 4 * 1024 * 1024
MAX_REPORT_BYTES = 32 * 1024
CONDITIONS = ("original", "ns12", "ns18")
MAX_MODEL_MIB = 1024


def _check_local_file(path: Path):
    if not isinstance(path, Path) or not path.is_absolute() or path.is_symlink():
        raise ValueError("local input must be absolute nonsymlink regular file")
    if not path.is_file():
        raise ValueError("missing private local input")


def _sha_file(path: Path, expected: str, max_bytes: int) -> int:
    _check_local_file(path)
    if not isinstance(expected, str) or not PIN.fullmatch(expected):
        raise ValueError("unapproved input SHA-256")
    if max_bytes <= 0 or path.stat().st_size > max_bytes:
        raise ValueError("input byte capacity exceeded")
    size = 0
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            chunk = source.read(65536)
            if not chunk:
                break
            size += len(chunk)
            if size > max_bytes:
                raise ValueError("input changed or exceeded byte capacity")
            digest.update(chunk)
    if not size or digest.hexdigest() != expected:
        raise ValueError("private local input SHA-256 mismatch")
    return size


def _verify_wav(path: Path, expected_sha: str) -> int:
    _sha_file(path, expected_sha, MAX_WAV_BYTES)
    with path.open("rb") as source:
        data = source.read(MAX_WAV_BYTES + 1)
    if hashlib.sha256(data).hexdigest() != expected_sha:
        raise ValueError("source WAV changed between checks")
    if (len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE"
            or int.from_bytes(data[4:8], "little") != len(data)-8):
        raise ValueError("unexpected WAV container")
    info = inspect_wav_bytes(data)
    with wave.open(io.BytesIO(data), "rb") as wav:
        if (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) != (16000, 1, 2):
            raise ValueError("WAV mismatch")
    return info["frame_count"]


def _text_from_cli(data: bytes) -> str:
    if not isinstance(data, bytes) or not 1 <= len(data) <= 4097:
        raise ValueError("invalid private transcript size")
    decoded = data.decode("utf-8", errors="strict").rstrip("\r\n")
    if not decoded or not decoded.strip():
        raise ValueError("empty transcript")
    if any(unicodedata.category(ch) in ("Cc", "Cf", "Cs") for ch in decoded):
        raise ValueError("unsafe control characters in private transcript")
    return decoded


def _run_one(cli: Path, model: Path, model_sha: str, wav: Path, language: str,
             reference: str, critical_words: tuple, max_model_mib: int,
             runner=bounded_decoder_run) -> dict:
    command = [
        str(cli), "--model", str(model), "--sha256", model_sha,
        "--wav", str(wav), "--language", language,
        "--stationarity-guard", "0",
        "--threads", "2", "--timeout-ms", "120000",
        "--model-max-mib", str(max_model_mib)
    ]
    began = time.monotonic()
    # The bounded reader caps all stdout/stderr, kills on overflow/timeout.
    # Never forward stderr or transcripts to CI even if inference fails.
    outcome = runner(command, input=b"", capture_output=True,
                     timeout=180, check=False)
    elapsed = time.monotonic() - began
    stdout = outcome.stdout
    stderr = outcome.stderr
    if (type(outcome.returncode) is not int or not isinstance(stdout, bytes)
            or not isinstance(stderr, bytes)
            or len(stdout) > 4097 or len(stderr) > 16384):
        raise ValueError("unbounded or malformed inference result")
    if outcome.returncode != 0:
        raise ValueError("private ASR inference failed")
    if not stdout and b"NO_SPEECH (no transcript emitted)" in stderr:
        return {
            "status": "no_speech_without_guard",
            "dropped_speech_before_scoring": True,
            "wer": None, "cer": None,
            "critical_words_retained": 0,
            "elapsed_ms": round(1000 * elapsed, 2)
        }
    if not stdout or b"NO_SPEECH" in stderr:
        raise ValueError("inconsistent ASR outcome")
    transcript = _text_from_cli(stdout)
    scored = score(reference, transcript, language)
    retained = set(_words(transcript))
    return {
        "status": "final",
        "dropped_speech_before_scoring": False,
        "wer": scored["word_error_rate"],
        "cer": scored["character_error_rate"],
        "critical_words_retained": sum(word in retained for word in critical_words),
        "elapsed_ms": round(1000 * elapsed, 2)
    }


def evaluate(cli: Path, cli_sha: str, model: Path, model_sha: str,
             original: tuple, ns12: tuple, ns18: tuple,
             reference_file: Path, reference_sha: str, language: str,
             critical_words=(), model_max_mib=128, runner=bounded_decoder_run) -> dict:
    if language not in ("en", "zh", "ja"):
        raise ValueError("unsupported language")
    if type(model_max_mib) is not int or not 1 <= model_max_mib <= MAX_MODEL_MIB:
        raise ValueError("model size limit must be explicit and bounded")
    _sha_file(cli, cli_sha, MAX_CLI_BYTES)
    if not os.access(cli, os.X_OK):
        raise ValueError("CLI must be executable")
    _sha_file(model, model_sha, model_max_mib * 1024 * 1024)
    # Snapshot the reference once: hash and normalization must use identical
    # source bytes. No path, source content or digest is sent to the CLI.
    _check_local_file(reference_file)
    with reference_file.open("rb") as source:
        original_reference = source.read(4098)
    if (not 1 <= len(original_reference) <= 4097
            or not isinstance(reference_sha, str)
            or not PIN.fullmatch(reference_sha)
            or hashlib.sha256(original_reference).hexdigest() != reference_sha):
        raise ValueError("private reference SHA-256 mismatch")
    reference = _text_from_cli(original_reference)
    critical = []
    for word in critical_words:
        if not isinstance(word, str):
            raise ValueError("critical word must be text")
        parts = _words(word)
        if len(parts) != 1 or len(word.encode("utf-8")) > 64:
            raise ValueError("invalid critical word")
        if parts[0] not in set(_words(reference)):
            raise ValueError("critical word absent from reference")
        if parts[0] in critical:
            raise ValueError("duplicate critical word")
        critical.append(parts[0])
    wavs = (original, ns12, ns18)
    if any(not isinstance(v, tuple) or len(v) != 2 for v in wavs):
        raise ValueError("expected three (path,sha256) WAV sources")
    counts = [_verify_wav(path, sha) for path, sha in wavs]
    if counts[0] != counts[1] or counts[0] != counts[2]:
        raise ValueError("NS changed number of complete 20ms frames")
    measures = {}
    for name, (path, _) in zip(CONDITIONS, wavs):
        measures[name] = _run_one(cli, model, model_sha, path, language,
                                  reference, tuple(critical), model_max_mib, runner)
    # Recheck ALL original paths and immutable byte digests after inference,
    # including the CLI image and model. A file modified during a long
    # three-arm decode MUST NOT produce a reproducible-looking quality report.
    _sha_file(cli, cli_sha, MAX_CLI_BYTES)
    _sha_file(model, model_sha, model_max_mib * 1024 * 1024)
    _sha_file(reference_file, reference_sha, 4097)
    for path, expected_sha in wavs:
        _sha_file(path, expected_sha, MAX_WAV_BYTES)
    base = measures["original"]
    for key in ("ns12", "ns18"):
        current = measures[key]
        for metric in ("cer", "wer"):
            current[metric + "_delta_from_original"] = (
                round(current[metric]-base[metric], 6)
                if current[metric] is not None and base[metric] is not None
                else None)
        current["critical_word_retention_delta"] = (
            current["critical_words_retained"]-base["critical_words_retained"])
    return {
        "schema": 1,
        "test_kind": "same_utterance_three_arm_local_model_inference",
        "language": language,
        "frame_count": counts[0],
        "speech_samples_are_independent": False,
        "noise_suppression_settings_db": [-12,-18],
        "stationarity_guard_enabled": False,
        "reference_rights_or_truth_independently_verified": False,
        "critical_words_requested": len(critical),
        "conditions": measures,
        "model_and_cli_explicit_local_sha256_verified": True,
        "model_or_audio_downloaded_by_evaluator": False,
        "model_license_and_approval_caller_responsibility": True,
        "single_utterance_quality_certified": False,
        "real_room_or_noisy_microphone_quality_certified": False,
        "population_false_rejection_rate_certified": False,
        "weak_consonants_preserved_certified": False,
        "no_transcripts_model_bytes_or_source_paths_emitted": True,
    }


def _write_private_report(dest: Path, payload: dict):
    if not dest.is_absolute() or dest.is_symlink() or dest.exists() or not dest.parent.is_dir():
        raise ValueError("report destination must be absolute new private file")
    data = (json.dumps(payload, sort_keys=True, indent=2) + "\n").encode("utf-8")
    if len(data) > MAX_REPORT_BYTES:
        raise ValueError("report exceeded bounded metadata limit")
    descriptor = os.open(str(dest), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
    except BaseException:
        dest.unlink(missing_ok=True)
        raise


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cli", type=Path, required=True)
    p.add_argument("--cli-sha256", required=True)
    p.add_argument("--model", type=Path, required=True)
    p.add_argument("--model-sha256", required=True)
    p.add_argument("--model-max-mib", type=int, default=128)
    for name in CONDITIONS:
        p.add_argument("--"+name+"-wav", type=Path, required=True)
        p.add_argument("--"+name+"-sha256", required=True)
    p.add_argument("--reference-file", type=Path, required=True)
    p.add_argument("--reference-sha256", required=True)
    p.add_argument("--language", choices=("en","zh","ja"), required=True)
    p.add_argument("--critical-word", action="append", default=[])
    p.add_argument("--report", type=Path, required=True)
    args = p.parse_args()
    try:
        wavs = [(getattr(args,n+"_wav"),getattr(args,n+"_sha256")) for n in CONDITIONS]
        if args.report in (args.cli, args.model, args.reference_file,
                           *(path for path, _ in wavs)):
            raise ValueError("report must be separate from private inputs")
        result = evaluate(
            args.cli,args.cli_sha256,args.model,args.model_sha256,
            *wavs,args.reference_file,args.reference_sha256,
            args.language,tuple(args.critical_word),args.model_max_mib)
        _write_private_report(args.report,result)
        print(json.dumps(result,sort_keys=True))  # metrics only
        return 0
    except (OSError, UnicodeError, ValueError, TypeError, OverflowError, wave.Error) as exc:
        # Never forward private model paths, recognition output, or text.
        print("private paired ASR evaluation refused",file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
