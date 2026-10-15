#!/usr/bin/env python3
"""Compare actual OFF-vs-ON Whisper outputs on speech-derived WAV probes.

No inference, network or model in this evaluator. Never print or persist
transcript content, even when evaluating a false refusal. All results are
diagnostic: baseline vendor inference may fail on an artificially attenuated
or cut sample, so identical empty/rejected outcomes are NOT speech-quality
certification. Nonzero model exit, output partial or timeout must not be
misrepresented as a filtered-noise PASS.
"""
import argparse
import json
from pathlib import Path
import sys

from generate_speech_stress_probes import PROFILES

MAX_PREVIEW_BYTES = 4097
MAX_STDERR_BYTES = 2 * 1024 * 1024


def _read_bound(path: Path, limit: int) -> bytes:
    if path.stat().st_size > limit:
        raise ValueError(f"{path.name}: oversized private evidence")
    return path.read_bytes()


def _outcome(directory: Path, name: str, guard: int) -> tuple[str, bytes]:
    prefix = f"{name}-guard{guard}"
    rc = _read_bound(directory/(prefix+".exit"), 16)
    if rc not in (b"0\n", b"4\n"):
        raise ValueError(f"{prefix}: timeout/crash/untrusted status")
    stdout = _read_bound(directory/(prefix+".stdout"), MAX_PREVIEW_BYTES)
    stderr = _read_bound(directory/(prefix+".stderr"), MAX_STDERR_BYTES)
    if b"\x00" in stdout:
        raise ValueError(f"{prefix}: embedded NUL in terminal output")
    if rc == b"0\n" and stdout:
        return "recognized_text", stdout
    if rc == b"0\n" and not stdout and b"NO_SPEECH" in stderr:
        return "no_speech", stdout
    if rc == b"4\n" and not stdout and b"no transcript admitted" in stderr:
        return "model_or_provider_refused", stdout
    raise ValueError(f"{prefix}: invalid terminal evidence")


def compare(directory: Path) -> dict:
    findings = {}
    baseline_recognized = 0
    for name, _origin, _divisor, _region in PROFILES:
        before, baseline = _outcome(directory, name, 0)
        after, guarded = _outcome(directory, name, 1)
        if before != after or baseline != guarded:
            # Never print raw bytes or a phrase to failure logs.
            raise ValueError(f"{name}: guard changed baseline ASR admission or output")
        if before == "recognized_text":
            baseline_recognized += 1
        findings[name] = {
            "baseline_status": before,
            "guard_status": after,
            "baseline_stdout_bytes": len(baseline),
            "guard_stdout_bytes": len(guarded),
            "byte_identical": True,
            "signal_from_pinned_spoken_JFK": True,
            "this_profile_positive_quality_certified": False,
        }
    return {
        "test_kind": "real_model_low_volume_and_short_JFK_guard_comparison",
        "fixture_count": len(PROFILES),
        "baseline_recognized_profile_count": baseline_recognized,
        "same_status_and_exact_text_for_all_profiles": True,
        "genuine_human_corpus_diversity_certified": False,
        "real_world_voice_activity_certified": False,
        "profiles": findings,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = compare(args.input_dir)
        data = json.dumps(report, indent=2, sort_keys=True)+"\n"
        args.report.write_text(data, encoding="utf-8")
        sys.stdout.write(data)
        return 0
    except (OSError, ValueError) as exc:
        sys.stderr.write(f"speech false-refusal gate rejected: {exc}\n")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
