#!/usr/bin/env python3
"""Evaluate generated non-speech probe outcomes without exposing model text.

This is a DIAGNOSTIC: an observed nonzero-noise transcript is not accepted
as speech, but absence of such a transcript is not production VAD proof.
Reads a small private temporary directory and publishes classifications,
exit codes and byte counts; never publishes stdout/stderr or audio.
"""
import argparse
import json
from pathlib import Path
import sys

PROFILES = ("digital-zero", "stationary-noise-quiet", "stationary-noise-medium")
MAX_OUTPUT_BYTES = 4097
MAX_STDERR_BYTES = 2 * 1024 * 1024


def _read_bounded(path: Path, limit: int) -> bytes:
    if path.stat().st_size > limit:
        raise ValueError(f"unbounded probe output: {path.name}")
    return path.read_bytes()


def evaluate(directory: Path) -> dict:
    outcomes = {}
    for profile in PROFILES:
        rc_text = _read_bounded(directory / f"{profile}.exit", 16)
        if rc_text not in (b"0\n", b"4\n"):
            raise ValueError(f"{profile}: invalid exit status")
        rc = int(rc_text.strip())
        stdout = _read_bounded(directory / f"{profile}.stdout",
                               MAX_OUTPUT_BYTES)
        stderr = _read_bounded(directory / f"{profile}.stderr",
                               MAX_STDERR_BYTES)
        if b"\x00" in stdout:
            raise ValueError(f"{profile}: embedded NUL in CLI preview")
        if rc == 0 and not stdout and b"NO_SPEECH" in stderr:
            classification = "no_speech"
        elif rc == 0 and stdout:
            classification = "unprompted_text_observed"
        elif rc == 4 and not stdout and b"no transcript admitted" in stderr:
            classification = "provider_or_output_rejected"
        else:
            raise ValueError(f"{profile}: invalid terminal outcome")
        if profile == "digital-zero" and classification != "no_speech":
            raise ValueError("digital-zero must return NO_SPEECH with empty stdout")
        outcomes[profile] = {
            "classification": classification,
            "cli_exit_status": rc,
            "stdout_bytes": len(stdout),
        }
    return {
        "test_kind": "genuine_tiny_weights_generated_noise_diagnostic",
        "real_noise_rejection_certified": False,
        "language_quality_certified": False,
        "non_speech_cases": outcomes,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = evaluate(args.input_dir)
        payload = json.dumps(result, sort_keys=True, indent=2) + "\n"
        args.report.write_text(payload, encoding="utf-8")
        sys.stdout.write(payload)  # metadata ONLY; no raw model transcripts
        return 0
    except (OSError, ValueError) as exc:
        sys.stderr.write(f"non-speech diagnostic rejected: {exc}\n")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
