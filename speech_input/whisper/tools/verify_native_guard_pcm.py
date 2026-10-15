#!/usr/bin/env python3
"""Check actual C11 PCM/M1/M2/Whisper Provider vs immutable offline ABI2 guard.

Runs the REAL production C11 Provider/CLI with only a deterministic FAKE
Whisper inference image. Strictly local, source-SHA-pinned, no media/model
downloads, no microphone or Agent tools. It never logs/stores PCM, names,
paths, raw stdout/stderr or speech transcripts. No general VAD inference.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from measure_frame_stationarity import inspect_wav_bytes, MAX_FILE_BYTES

SHA = re.compile(r"[a-f0-9]{64}", flags=re.ASCII)
FAKE_MODEL_BYTES = b"MOCK_SIGNED_SPEECH_MODEL_ONLY"
FINAL_FAKE_TEXT = b" utterance"
LANGUAGES = ("en", "ja", "zh")
FAKE_TEXT_LIMIT = 4096
STDERR_LIMIT = 1 << 20


def _pinned_file(path: Path, sha256: str, *, max_bytes: int) -> bytes:
    if (not path.is_absolute() or path.is_symlink() or not path.is_file()
            or not isinstance(sha256, str) or not SHA.fullmatch(sha256)):
        raise ValueError("invalid pinned local input")
    with path.open("rb") as inp:
        raw = inp.read(max_bytes + 1)
    if not raw or len(raw) > max_bytes:
        raise ValueError("invalid local input size")
    if hashlib.sha256(raw).hexdigest() != sha256:
        raise ValueError("input source SHA-256 mismatch")
    return raw


def _call(cli: Path, model: Path, model_sha256: str, pcm_wav: Path,
          language: str, guard: int) -> str:
    command = [
        str(cli), "--model", str(model), "--sha256", model_sha256,
        "--wav", str(pcm_wav), "--language", language, "--threads", "1",
        "--stationarity-guard", str(guard), "--timeout-ms", "30000",
    ]
    try:
        outcome = subprocess.run(
            command, capture_output=True, check=False, timeout=40)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise ValueError("native fake inference unavailable or timed out") from exc
    if (outcome.returncode != 0 or
            len(outcome.stdout) > FAKE_TEXT_LIMIT or
            len(outcome.stderr) > STDERR_LIMIT or b"\x00" in outcome.stdout):
        raise ValueError("native Provider result was not bounded and terminal")
    if (outcome.stdout.rstrip(b"\r\n") == FINAL_FAKE_TEXT and
            b"NO_SPEECH" not in outcome.stderr):
        return "fake_vendor_final"
    if (not outcome.stdout and
            b"NO_SPEECH (no transcript emitted)" in outcome.stderr):
        return "no_speech_without_vendor"
    # The raw vendor text and command path must never appear in failures.
    raise ValueError("unexpected native Provider terminal classification")


def verify(input_wav: Path, input_sha: str,
           cli: Path, fake_model: Path, fake_model_sha: str,
           language: str, runner=None) -> dict:
    if language not in LANGUAGES:
        raise ValueError("unsupported language flag")
    if not cli.is_absolute() or cli.is_symlink() or not cli.is_file():
        raise ValueError("native C11 CLI must be a locally installed binary")
    if not os.access(cli, os.X_OK):
        raise ValueError("native C11 CLI is not executable")
    model = _pinned_file(fake_model, fake_model_sha, max_bytes=128)
    if model != FAKE_MODEL_BYTES:
        raise ValueError("real GGML model weights are not allowed in this test")
    source = _pinned_file(input_wav, input_sha, max_bytes=MAX_FILE_BYTES)
    metrics = inspect_wav_bytes(source)
    expected_off = ("no_speech_without_vendor" if
                    metrics["stationarity_hypothesis"] == "digital_zero"
                    else "fake_vendor_final")
    expected_on = ("no_speech_without_vendor" if
                   metrics["stationarity_hypothesis"] in
                   ("digital_zero", "stationary_candidate")
                   else "fake_vendor_final")
    if runner is None:
        runner = _call
    # Copy the EXACT previously pinned byte snapshot into private 0600
    # location. The CLI cannot reopen a caller-mutable original after a
    # different version has been inspected (no SHA-to-use race).
    with tempfile.TemporaryDirectory(prefix="private-abi2-") as temp:
        private = Path(temp)
        private.chmod(0o700)
        pcm_wav = private / "pinned-private.wav"
        with pcm_wav.open("xb") as output:
            output.write(source)
        pcm_wav.chmod(0o600)
        original = runner(cli, fake_model, fake_model_sha, pcm_wav, language, 0)
        gated = runner(cli, fake_model, fake_model_sha, pcm_wav, language, 1)
        if original != expected_off or gated != expected_on:
            raise ValueError("C11 Provider disagrees with offline ABI2 classification")
    return {
        "evidence_kind": "actual_strict_C11_PCM_M2_M1_Provider_fake_vendor",
        "predecode_estimator": "frame_power_and_strict_12_100_lag1_ABI2",
        "source_original_sha256_verified": True,
        "same_immutable_pcm_evaluated_and_supplied": True,
        "guard_off": original,
        "guard_on": gated,
        "frame_count": metrics["frame_count"],
        "source_language_declared_not_verified": language,
        "real_model_weights_used": False,
        "actual_human_voice_or_acoustic_quality_certified": False,
        "no_raw_audio_or_vendor_text_in_report": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-wav", required=True, type=Path)
    parser.add_argument("--source-sha256", required=True)
    parser.add_argument("--mock-cli", required=True, type=Path)
    parser.add_argument("--fake-model", required=True, type=Path)
    parser.add_argument("--fake-model-sha256", required=True)
    parser.add_argument("--language", required=True, choices=LANGUAGES)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    try:
        if not args.report.is_absolute() or args.report.is_symlink():
            raise ValueError("private metadata report must use absolute path")
        result = verify(args.source_wav, args.source_sha256,
                        args.mock_cli, args.fake_model,
                        args.fake_model_sha256, args.language)
        # A private metadata-only report is create-only, never overwrite.
        with args.report.open("x", encoding="utf-8") as output:
            os.chmod(args.report, 0o600)
            output.write(json.dumps(result, sort_keys=True, indent=2) + "\n")
        print(json.dumps(result, sort_keys=True))
        return 0
    except (ValueError, OSError, EOFError, TypeError) as exc:
        # No path, SHA, text or bytes in the public failure.
        sys.stderr.write("native ABI2 pinned PCM parity refused: "
                         + type(exc).__name__ + "\n")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
