#!/usr/bin/env python3
"""Measure real recorded field noise mixed *in software* with licensed speech.

Strictly test-only, no microphone, ASR weights, download, model invocation,
VAD, network, persisted transcripts or production DSP integration.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import sys

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from known_noise_reference import (  # noqa: E402
    PRE_ROLL_FRAMES, SAMPLES_FRAME, MAX_RAW_BYTES,
    read_pinned, samples, new_private, score as score_clean_reference,
)

LEVELS = (8, 16)
CLEAN_NAME = "clean-reference.raw"
MIX_NAME = "mixed-recorded-road.raw"
MAX_JSON_BYTES = 4096


def _toward_zero_divide(value: int, divisor: int) -> int:
    return value // divisor if value >= 0 else -((-value) // divisor)


def prepare(clean_pcm: bytes, field_noise_pcm: bytes, root: Path,
            scale_divisor: int) -> dict:
    if type(scale_divisor) is not int or scale_divisor not in LEVELS:
        raise ValueError("unapproved recorded-noise attenuation")
    if (not isinstance(root, Path) or not root.is_absolute()
            or root.is_symlink() or not root.is_dir()
            or stat.S_IMODE(root.stat().st_mode) != 0o700
            or next(root.iterdir(), None) is not None):
        raise ValueError("require empty 0700 private directory")
    prefix_samples = PRE_ROLL_FRAMES * SAMPLES_FRAME
    if (not isinstance(clean_pcm, bytes) or not clean_pcm
            or len(clean_pcm) % (2 * SAMPLES_FRAME)
            or len(clean_pcm) > MAX_RAW_BYTES-prefix_samples*2
            or not isinstance(field_noise_pcm, bytes)
            or len(field_noise_pcm) > MAX_RAW_BYTES
            or len(field_noise_pcm) % (2 * SAMPLES_FRAME)
            or len(field_noise_pcm) < len(clean_pcm) + prefix_samples * 2):
        raise ValueError("incompatible bounded original speech/field-noise frames")
    source = samples(clean_pcm)
    field = samples(field_noise_pcm)
    clean = ([0] * prefix_samples) + list(source)
    recorded_segment = field[:len(clean)]  # contiguous: no looping/splicing
    if len(recorded_segment) != len(clean) or not any(recorded_segment):
        raise ValueError("recorded segment has no measured noise")
    mixed = []
    injected_error = 0
    for value, recorded in zip(clean, recorded_segment):
        contribution = _toward_zero_divide(recorded, scale_divisor)
        out = value + contribution
        if out < -32768 or out > 32767:
            raise ValueError("recorded-noise mixture would clip signed PCM16")
        injected_error += contribution*contribution
        mixed.append(out)
    if injected_error == 0:
        raise ValueError("attenuation erased recorded noise")
    binary = "<%dh" % len(clean)
    clean_bytes = struct.pack(binary, *clean)
    mixed_bytes = struct.pack(binary, *mixed)
    first = root / CLEAN_NAME
    second = root / MIX_NAME
    new_private(first, clean_bytes)
    try:
        new_private(second, mixed_bytes)
    except BaseException:
        first.unlink(missing_ok=True)
        raise
    return {
        "schema": 1,
        "source_kind": "licensed_human_voice_plus_recorded_field_noise_software_overlay",
        "field_noise_origin": "previously_independently_recorded_truck_warning",
        "real_simultaneous_microphone_capture": False,
        "continuous_recorded_noise_segment": True,
        "field_noise_looping_or_sample_splicing": False,
        "scale_divisor": scale_divisor,
        "preroll_frames": PRE_ROLL_FRAMES,
        "speech_frames": len(clean_pcm) // (2*SAMPLES_FRAME),
        "total_frames": len(clean) // SAMPLES_FRAME,
        "recorded_noise_samples_used": len(recorded_segment),
        "known_added_noise_power": injected_error,
        "silent_clipping_or_fallback": False,
        "model_weights_used": False,
        "no_pcm_or_source_text_emitted": True,
        "speech_content_certified": False,
    }


def score(clean: bytes, mixed: bytes, filtered: bytes, divisor: int) -> dict:
    if type(divisor) is not int or divisor not in LEVELS:
        raise ValueError("unknown recorded noise scale")
    data = score_clean_reference(clean, mixed, filtered)
    # The common CER-free signal-to-error metric compares the SAME clean
    # signal across arms; its default synthetic-noise label is *not* correct
    # for this recorded-source overlay and must be rewritten explicitly.
    data["signal"] = "licensed_whisper_plus_real_recorded_noise_software_overlay"
    data["scale_divisor"] = divisor
    data["noise_source_origin"] = "genuine_licensed_recorded_field_noise"
    data["natural_simultaneous_voice_noise_recording"] = False
    data["recording_snr_ground_truth"] = False
    data["speaker_population_inference"] = False
    data["wer_cer_or_hallucination_certified"] = False
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="task", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--clean-raw", type=Path, required=True)
    p.add_argument("--clean-sha256", required=True)
    p.add_argument("--recorded-noise-raw", type=Path, required=True)
    p.add_argument("--recorded-noise-sha256", required=True)
    p.add_argument("--private-root", type=Path, required=True)
    p.add_argument("--scale-divisor", type=int, choices=LEVELS, required=True)
    q = sub.add_parser("score")
    for name in ("clean", "mixed", "filtered"):
        q.add_argument("--"+name, type=Path, required=True)
        q.add_argument("--"+name+"-sha256", required=True)
    q.add_argument("--scale-divisor", type=int, choices=LEVELS, required=True)
    args = parser.parse_args()
    try:
        if args.task == "prepare":
            report = prepare(
                read_pinned(args.clean_raw, args.clean_sha256),
                read_pinned(args.recorded_noise_raw, args.recorded_noise_sha256),
                args.private_root, args.scale_divisor)
        else:
            report = score(
                read_pinned(args.clean, args.clean_sha256),
                read_pinned(args.mixed, args.mixed_sha256),
                read_pinned(args.filtered, args.filtered_sha256),
                args.scale_divisor)
        if len(json.dumps(report)) > MAX_JSON_BYTES:
            raise ValueError("metadata result exceeded cap")
        print(json.dumps(report, sort_keys=True))
        return 0
    except (ValueError, OSError, TypeError, OverflowError, struct.error):
        # Never print private original paths, bytes, filenames or transcripts.
        print("private field-noise paired PCM refused", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
