#!/usr/bin/env python3
"""Create an adversarial *speech-bearing* fixed-RMS JFK variant.

Source must match immutable official whisper.cpp JFK WAV SHA256. Every
non-silent 20ms frame is gain-normalized to target RMS. Original exact
digital-zero frames get reproducible synthetic comfort noise before
normalization. Speech-bearing frames preserve waveform ordering; this is
an artificial AGC/noise-floor stress fixture, not original human audio
quality or a real microphone recording. No model or network is used.
Only SHA/count metadata, NEVER source/output PCM, is archived.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import wave

SOURCE_SHA256 = "59dfb9a4acb36fe2a2affc14bacbee2920ff435cb13cc314a08c13f66ba7860e"
FRAME = 320
RATE = 16000
SOURCE_FRAMES = 550
TARGET_RMS_S16 = 512
SEED = 0x57415645


def generate(source: Path, output: Path) -> dict:
    if not source.is_absolute() or not output.is_absolute():
        raise ValueError("source and output WAV paths must be absolute")
    source_bytes = source.read_bytes()
    actual_hash = hashlib.sha256(source_bytes).hexdigest()
    if actual_hash != SOURCE_SHA256:
        raise ValueError("unapproved source WAV SHA256")
    with wave.open(str(source), "rb") as w:
        if (w.getframerate(), w.getnchannels(), w.getsampwidth(),
                w.getnframes()) != (RATE, 1, 2, SOURCE_FRAMES * FRAME):
            raise ValueError("unpinned JFK source format")
        pcm = w.readframes(SOURCE_FRAMES * FRAME)
        if len(pcm) != SOURCE_FRAMES * FRAME * 2 or w.readframes(1):
            raise ValueError("incomplete PCM source")
    values = struct.unpack("<%dh" % (FRAME * SOURCE_FRAMES), pcm)
    normalized = bytearray(len(pcm))
    state = SEED
    filled = 0
    min_power = None
    max_power = 0
    for frame in range(SOURCE_FRAMES):
        chunk = values[frame * FRAME: (frame + 1) * FRAME]
        square_sum = sum(int(v) * int(v) for v in chunk)
        if square_sum == 0:
            filled += 1
            comfort = []
            for _ in range(FRAME):
                state ^= (state << 13) & 0xffffffff
                state ^= state >> 17
                state ^= (state << 5) & 0xffffffff
                state &= 0xffffffff
                comfort.append((state >> 16) - 32768)
            chunk = comfort
            square_sum = sum(v * v for v in chunk)
        gain = TARGET_RMS_S16 * math.sqrt(FRAME / square_sum)
        scaled = [round(v * gain) for v in chunk]
        if any(v < -32768 or v > 32767 for v in scaled):
            raise ValueError("unexpected normalizer clipping")
        power = sum(v * v for v in scaled)
        min_power = power if min_power is None else min(min_power, power)
        max_power = max(max_power, power)
        struct.pack_into("<320h", normalized, frame * FRAME * 2, *scaled)
    with wave.open(str(output), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(normalized)
    # The existing stationarity heuristic sees equal RMS and no zero
    # frames; this is intentional pressure against false speech rejection.
    if min_power is None or min_power == 0 or max_power > 2 * min_power:
        raise ValueError("fixture failed to flatten frame power")
    return {
        "source_kind": "pinned_jfk_gain_normalized_speech_with_synthetic_comfort_noise",
        "source_wav_sha256": SOURCE_SHA256,
        "output_wav_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "output_pcm_sha256": hashlib.sha256(normalized).hexdigest(),
        "sample_rate_hz": RATE,
        "samples_per_frame": FRAME,
        "frame_count": SOURCE_FRAMES,
        "target_rms_s16": TARGET_RMS_S16,
        "replaced_exact_zero_frames": filled,
        "normalization_clipping_count": 0,
        "frame_power_max_min_ratio": round(max_power / min_power, 6),
        "origin_contains_spoken_english": True,
        "real_world_agc_certified": False,
        "acoustic_vad_certified": False,
    }


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--report", type=Path, required=True)
    args = p.parse_args()
    try:
        result = generate(args.source, args.output)
        args.report.write_text(json.dumps(result, sort_keys=True, indent=2)
                               + "\n", encoding="utf-8")
        print("PASS: fixed-RMS JFK speech-bearing stress WAV generated locally")
        return 0
    except (OSError, EOFError, ValueError, wave.Error) as exc:
        sys.stderr.write(f"fixed-RMS JFK stress probe rejected: {exc}\n")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
