#!/usr/bin/env python3
"""Derive bounded soft, AGC-soft and short speech-bearing JFK WAV probes.

The only human speech source is the pinned official Whisper.cpp JFK sample.
No synthesized voice, microphone, model, network, third-party packages or
new copyright claims. Raw WAVs stay on the CI runner; metadata carries only
hashes and signal statistics. A 0.8s middle excerpt is a speech-DERIVED
window, not separately qualified intelligible speech.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import wave

from generate_agc_speech_probe import SOURCE_SHA256, FRAME, RATE, SOURCE_FRAMES
from generate_agc_speech_probe import generate as generate_agc

PROFILES = (
    ("jfk-soft-8", "original", 8, None),
    ("jfk-soft-64", "original", 64, None),
    ("jfk-agc-soft-8", "agc", 8, None),
    ("jfk-agc-soft-32", "agc", 32, None),
    ("jfk-mid-short-0p8s", "original", 1, (200, 40)),
)


def _read_exact(path: Path, *, validate_hash: bool) -> tuple[int, ...]:
    if not path.is_absolute() or path.stat().st_size > 400000:
        raise ValueError("unapproved WAV path or capacity")
    if validate_hash and hashlib.sha256(path.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise ValueError("source WAV does not match immutable upstream JFK")
    with wave.open(str(path), "rb") as w:
        if (w.getframerate(), w.getnchannels(), w.getsampwidth(),
            w.getnframes()) != (RATE, 1, 2, FRAME * SOURCE_FRAMES):
            raise ValueError("unexpected source sample shape")
        data = w.readframes(FRAME * SOURCE_FRAMES)
        if len(data) != 2 * FRAME * SOURCE_FRAMES or w.readframes(1):
            raise ValueError("partial or extra audio bytes")
    return struct.unpack("<%dh" % (FRAME * SOURCE_FRAMES), data)


def _attenuate(sample: int, divisor: int) -> int:
    # Truncation toward zero, portable between Python and C11.
    return (abs(sample) // divisor) * (-1 if sample < 0 else 1)


def _emit(path: Path, pcm: tuple[int, ...]) -> dict:
    if not pcm or len(pcm) % FRAME or len(pcm) > FRAME * SOURCE_FRAMES:
        raise ValueError("incomplete or oversized frame sequence")
    packed = struct.pack("<%dh" % len(pcm), *pcm)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(packed)
    return {
        "pcm_sha256": hashlib.sha256(packed).hexdigest(),
        "wav_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "pcm_samples": len(pcm),
        "frames": len(pcm) // FRAME,
        "nonzero_samples": sum(x != 0 for x in pcm),
        "peak_s16": max(abs(x) for x in pcm),
        "rms_s16": round((sum(x*x for x in pcm) / len(pcm)) ** .5, 6),
    }


def generate(source: Path, output_dir: Path) -> dict:
    if not source.is_absolute() or not output_dir.is_absolute():
        raise ValueError("source and output directory must be absolute")
    output_dir.mkdir(parents=True, exist_ok=True)
    original = _read_exact(source, validate_hash=True)
    agc_path = output_dir / "jfk-agc-base-PRIVATE.wav"
    agc_manifest = generate_agc(source, agc_path)
    agc = _read_exact(agc_path, validate_hash=False)
    result = {
        "kind": "pinned_jfk_speech_derived_stress_not_human_corpus",
        "official_source_sha256": SOURCE_SHA256,
        "agc_base_sha256": agc_manifest["output_wav_sha256"],
        "no_new_human_audio_source": True,
        "source_language": "en",
        "standalone_voice_activity_certified": False,
        "recognized_speech_certified": False,
        "profiles": {},
    }
    for name, origin, divisor, region in PROFILES:
        pcm = original if origin == "original" else agc
        if region:
            start, count = region
            pcm = pcm[start*FRAME:(start+count)*FRAME]
        transformed = tuple(_attenuate(v, divisor) for v in pcm)
        meta = _emit(output_dir / (name + ".wav"), transformed)
        meta.update({
            "origin": origin, "gain_divisor": divisor,
            "speech_derived": True,
            "short_window_unverified_words": region is not None,
        })
        if meta["nonzero_samples"] == 0:
            raise ValueError(f"{name}: invalid all-zero stress probe")
        result["profiles"][name] = meta
    # Never retain duplicate private AGC PCM when the five probes are built.
    agc_path.unlink()
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    try:
        payload = generate(args.source, args.output_dir)
        args.report.write_text(json.dumps(payload, sort_keys=True, indent=2) + "\n",
                               encoding="utf-8")
        print("PASS: five pinned JFK-derived soft/short WAV stress profiles (no weights)")
        return 0
    except (OSError, ValueError, EOFError, wave.Error) as exc:
        sys.stderr.write(f"speech stress probe rejected: {exc}\n")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
