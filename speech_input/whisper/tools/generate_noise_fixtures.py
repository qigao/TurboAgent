#!/usr/bin/env python3
"""Produce synthetic, reproducible non-speech WAV probes, without downloads.

These are GENERATED noise patterns, not licensed human audio, acoustic VAD,
or proof that Whisper will reject noise. No model, microphone, transcript,
network or external Python module is needed. Only SHA/levels are reported.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import wave

RATE = 16000
SAMPLES_PER_FRAME = 320
FRAMES = 550  # exactly 11.0 seconds; preserve complete 20ms M2 frames
SEED = 0x41535231
PROFILES = (("digital-zero", None), ("stationary-noise-quiet", 7),
            ("stationary-noise-medium", 4))


def _noise_samples():
    state = SEED
    for _ in range(FRAMES * SAMPLES_PER_FRAME):
        # Portable unsigned 32-bit xorshift, not Python's versioned RNG.
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        state &= 0xFFFFFFFF
        yield (state >> 16) - 32768


def generate(directory: Path) -> dict:
    if not directory.is_absolute():
        raise ValueError("probe output directory must be absolute")
    directory.mkdir(parents=True, exist_ok=True)
    source = tuple(_noise_samples())
    result = {
        "kind": "synthetic_generated_non_speech_diagnostic_NOT_VAD_certification",
        "generator": "xorshift32:13,17,5:high16:signed",
        "seed_hex": f"{SEED:08x}",
        "sample_rate_hz": RATE,
        "channels": 1,
        "sample_width_bytes": 2,
        "samples_per_frame": SAMPLES_PER_FRAME,
        "frames": FRAMES,
        "human_audio_corpus_used": False,
        "profiles": {},
    }
    for name, shift in PROFILES:
        pcm = bytearray(len(source) * 2)
        nonzero = 0
        square_sum = 0
        peak = 0
        for i, raw in enumerate(source):
            sample = 0 if shift is None else raw >> shift
            struct.pack_into("<h", pcm, 2 * i, sample)
            nonzero += sample != 0
            square_sum += sample * sample
            peak = max(peak, abs(sample))
        wav_path = directory / f"{name}.wav"
        with wave.open(str(wav_path), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(RATE)
            output.writeframes(pcm)
        header_and_audio = wav_path.read_bytes()
        if len(header_and_audio) != 44 + len(pcm):
            raise ValueError(f"{name}: unexpected RIFF serialization")
        result["profiles"][name] = {
            "sha256_wav": hashlib.sha256(header_and_audio).hexdigest(),
            "sha256_pcm": hashlib.sha256(pcm).hexdigest(),
            "size_bytes": len(header_and_audio),
            "pcm_samples": len(source),
            "nonzero_samples": nonzero,
            "peak_s16": peak,
            "rms_s16": round((square_sum / len(source)) ** 0.5, 3),
            "source_kind": "generated_no_human_speech",
        }
    if result["profiles"]["digital-zero"]["nonzero_samples"] != 0:
        raise AssertionError("digital-zero must be truly zero")
    if any(result["profiles"][name]["nonzero_samples"] == 0
           for name in ("stationary-noise-quiet", "stationary-noise-medium")):
        raise AssertionError("noise probes must be nonzero")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = generate(args.output_dir)
        args.report.write_text(json.dumps(report, indent=2, sort_keys=True)
                               + "\n", encoding="utf-8")
        print("PASS: three pinned 11s WAV probe profiles; no weights or speech audio")
        return 0
    except (OSError, ValueError) as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    raise SystemExit(main())
