#!/usr/bin/env python3
"""Test-only, model-free audio-envelope stress on an explicitly pinned WAV.

The source can be genuine speech, but its derived samples are NOT natural
speech recordings. Gain reduction and frame-RMS equalization cannot establish
audible intelligibility or a population false rejection rate. No transcript,
ASR, model download, external process, recording or sample persistence.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import struct
import sys
import wave

from measure_frame_stationarity import FRAME, RATE, MAX_FILE_BYTES, inspect_wav_bytes

PIN = re.compile(r"[0-9a-f]{64}", flags=re.ASCII)
ATTENUATION_DIVISORS = (4, 16, 64)
TARGET_FRAME_RMS = 512
MAX_STRESS_FRAMES = 120
MIN_STRESS_FRAMES = 50


def make_wav(raw_pcm: bytes) -> bytes:
    if not raw_pcm or len(raw_pcm) % (2 * FRAME):
        raise ValueError("invalid complete-frame PCM")
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(raw_pcm)
    return buffer.getvalue()


def evaluate_bytes(source_bytes: bytes, expected_sha256: str) -> dict:
    if (not isinstance(source_bytes, bytes) or len(source_bytes) > MAX_FILE_BYTES
            or not isinstance(expected_sha256, str)
            or not PIN.fullmatch(expected_sha256)
            or hashlib.sha256(source_bytes).hexdigest() != expected_sha256):
        raise ValueError("source WAV missing, unbounded or SHA-256 mismatch")
    if (len(source_bytes) < 12 or source_bytes[:4] != b"RIFF"
            or source_bytes[8:12] != b"WAVE"
            or int.from_bytes(source_bytes[4:8], "little") != len(source_bytes)-8):
        raise ValueError("source WAV RIFF container length mismatch")
    original = inspect_wav_bytes(source_bytes)
    with wave.open(io.BytesIO(source_bytes), "rb") as wav:
        source_pcm = wav.readframes(wav.getnframes())
    values = struct.unpack("<%dh" % (len(source_pcm) // 2), source_pcm)
    total_frames = len(values) // FRAME

    attenuation = {}
    for divisor in ATTENUATION_DIVISORS:
        # Division toward zero, not floor, so negative input samples do not
        # acquire a DC offset through rounding.
        samples = (int(x / divisor) for x in values)
        out = make_wav(struct.pack("<%dh" % len(values), *samples))
        report = inspect_wav_bytes(out)
        assert report["frame_count"] == total_frames
        attenuation[str(divisor)] = {
            "lag1_abs_correlation": report["lag1_abs_correlation"],
            "predecode_guard_no_speech": report["stationarity_hypothesis"]
                                         in ("digital_zero", "stationary_candidate"),
            "energy_hypothesis": report["stationarity_hypothesis"],
            "zero_pcm": report["zero_pcm"],
            "frame_count": report["frame_count"],
        }

    # Keep only the first bounded nonzero 20-ms source frames and normalize
    # each to an exactly targeted RMS. Splicing/normalization changes natural
    # timing and dynamics, so this is an adversarial *controlled derivative*,
    # not a sample from a natural low-volume or monotone speaker.
    selected = []
    for frame in range(total_frames):
        block = values[frame * FRAME:(frame + 1) * FRAME]
        sum_sq = sum(x * x for x in block)
        if sum_sq == 0:
            continue
        selected.append((block, sum_sq))
        if len(selected) >= MAX_STRESS_FRAMES:
            break
    if len(selected) < MIN_STRESS_FRAMES:
        raise ValueError("insufficient source-derived nonzero frames for stress")
    source_selected = []
    flattened = []
    for block, sum_sq in selected:
        source_selected.extend(block)
        frame_rms = (sum_sq / FRAME) ** 0.5
        multiplier = TARGET_FRAME_RMS / frame_rms
        normalized = [round(x * multiplier) for x in block]
        # For 320-sample S16 frames, RMS=512 guarantees max sample
        # <=512*sqrt(320) <32768; no clipping or source amplitudes invented.
        if any(x < -32768 or x > 32767 for x in normalized):
            raise ValueError("unexpected normalization range violation")
        flattened.extend(normalized)
    source_selected_wav = make_wav(
        struct.pack("<%dh" % len(source_selected), *source_selected))
    flattened_wav = make_wav(
        struct.pack("<%dh" % len(flattened), *flattened))
    before = inspect_wav_bytes(source_selected_wav)
    after = inspect_wav_bytes(flattened_wav)

    return {
        "schema": 1,
        "method": "model_free_frame_power_plus_strict_lag1_abi2_envelope_stress",
        "source_frame_count": total_frames,
        "original_energy_hypothesis": original["stationarity_hypothesis"],
        "original_lag1_abs_correlation": original["lag1_abs_correlation"],
        "uniform_gain_reduction": attenuation,
        "controlled_energy_equalization": {
            "selected_nonzero_source_frames": len(selected),
            "source_frame_energy_hypothesis": before["stationarity_hypothesis"],
            "equalized_frame_energy_hypothesis": after["stationarity_hypothesis"],
            "guard_off_predecode_no_speech": False,
            "guard_on_predecode_no_speech": after["stationarity_hypothesis"]
                                            == "stationary_candidate",
            "frame_power_cv": after["frame_power_cv"],
            "frame_power_max_min_ratio": after["frame_power_max_min_ratio"],
            "lag1_abs_correlation": after["lag1_abs_correlation"],
            "legacy_energy_only_would_reject": (
                len(selected) >= MIN_STRESS_FRAMES
                and after["frame_power_max_min_ratio"] is not None
                and after["frame_power_max_min_ratio"] <= 2.0
                and after["frame_power_cv"] <= 0.12
                and not after["zero_pcm"]
            ),
            "correlation_aware_abi2_would_reject": (
                after["stationarity_hypothesis"] == "stationary_candidate"
            ),
            "rms_target": TARGET_FRAME_RMS,
            "voice_waveform_structure_from_source": True,
            "original_timing_and_amplitude_envelope_preserved": False,
        },
        "real_world_low_volume_or_monotone_speaker_certified": False,
        "audible_intelligibility_or_wer_cer_measured": False,
        "human_population_false_rejection_rate_measured": False,
        "actual_whisper_model_used": False,
        "actual_c11_provider_used_on_source_audio": False,
        "no_audio_or_transcript_persisted_or_emitted": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-wav", type=Path, required=True)
    parser.add_argument("--source-sha256", required=True)
    args = parser.parse_args()
    try:
        if not args.source_wav.is_absolute() or args.source_wav.is_symlink():
            raise ValueError("source path must be absolute regular WAV")
        with args.source_wav.open("rb") as file:
            raw = file.read(MAX_FILE_BYTES + 1)
        result = evaluate_bytes(raw, args.source_sha256)
        print(json.dumps(result, sort_keys=True))  # numeric/booleans only
        return 0
    except (OSError, ValueError, TypeError, struct.error, EOFError, wave.Error):
        # No file path, PCM or transcript in logs, even on parsing failures.
        print("source-derived energy stress rejected", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
