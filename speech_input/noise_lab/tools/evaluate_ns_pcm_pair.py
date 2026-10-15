#!/usr/bin/env python3
"""Metadata-only paired 16kHz PCM NS diagnostics. Not ASR, VAD, or SNR proof."""
import argparse
import hashlib
import io
import json
import math
from pathlib import Path
import re
import struct
import sys
import wave

SAMPLES_PER_FRAME = 320
MAX_SAMPLES = 320 * 3000
MAX_RAW_BYTES = MAX_SAMPLES * 2
PIN = re.compile(r"[0-9a-f]{64}", flags=re.ASCII)
WHISPER_TOOLS = Path(__file__).resolve().parents[2] / "whisper" / "tools"
sys.path.insert(0, str(WHISPER_TOOLS))
from measure_frame_stationarity import inspect_wav_bytes  # noqa: E402


def verified_raw(path: Path, expected_sha256: str) -> bytes:
    if (not path.is_absolute() or path.is_symlink() or not path.is_file()
            or not isinstance(expected_sha256, str)
            or not PIN.fullmatch(expected_sha256)):
        raise ValueError("invalid absolute private PCM input or digest")
    with path.open("rb") as f:
        raw = f.read(MAX_RAW_BYTES + 1)
    if (not raw or len(raw) > MAX_RAW_BYTES
            or len(raw) % (SAMPLES_PER_FRAME * 2)
            or hashlib.sha256(raw).hexdigest() != expected_sha256):
        raise ValueError("invalid bounded exact RAW S16LE snapshot")
    return raw


def wrap_wav(raw: bytes) -> bytes:
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(16000)
        out.writeframes(raw)
    return buffer.getvalue()


from operator import mul


def bounded_offset_cosine(a, b):
    """Report best waveform offset, *not* certified DSP algorithm latency.

    Search only plus/minus one existing 320-sample 20ms frame and use a
    limited window to avoid unbounded quadratic Python correlation. The
    aligned full-clip score is diagnostic, not phoneme/intelligibility proof.
    """
    n = len(a)
    if n != len(b) or n < 320:
        raise ValueError("unalignable PCM")
    if not any(a) or not any(b):
        return None, None
    start = min(8000, max(320, n // 4))
    span = min(4096, n - start - 320)
    if span < 320:
        start = 320
        span = n - 640
    if span < 320:
        return None, None

    aa = a[start:start+span]
    norm_a = sum(v*v for v in aa)
    if norm_a == 0:
        return None, None
    best_lag, best_score = 0, -2.0
    def score(lag):
        bb = b[start+lag:start+lag+span]
        norm_b = sum(v*v for v in bb)
        if norm_b == 0:
            return -2.0
        return sum(map(mul, aa, bb)) / math.sqrt(norm_a * norm_b)
    for lag in range(-320,321,8):
        candidate = score(lag)
        if candidate > best_score:
            best_lag, best_score = lag, candidate
    for lag in range(max(-320,best_lag-8),min(320,best_lag+8)+1):
        candidate = score(lag)
        if candidate > best_score:
            best_lag, best_score = lag, candidate
    if best_lag >= 0:
        av,bv=a[:n-best_lag],b[best_lag:]
    else:
        av,bv=a[-best_lag:],b[:n+best_lag]
    e1=sum(v*v for v in av)
    e2=sum(v*v for v in bv)
    if not e1 or not e2:
        return best_lag, None
    return best_lag, round(sum(map(mul,av,bv)) / math.sqrt(e1*e2),6)



def evaluate(before: bytes, after: bytes, kind: str, db: int) -> dict:
    if kind not in ("steady", "whisper", "road") or db not in (-12, -18):
        raise ValueError("unsupported sample label or suppression level")
    if (not isinstance(before, bytes) or not isinstance(after, bytes)
            or not before or len(before) != len(after)
            or len(before) > MAX_RAW_BYTES
            or len(before) % (SAMPLES_PER_FRAME * 2)):
        raise ValueError("invalid PCM pair size")

    n = len(before) // 2
    a = struct.unpack("<%dh" % n, before)
    b = struct.unpack("<%dh" % n, after)
    before_energy = sum(x*x for x in a)
    after_energy = sum(y*y for y in b)
    if before_energy == 0:
        raise ValueError("original must be an audible nonzero diagnostic")
    dot = sum(x*y for x,y in zip(a,b))
    changed = sum(x != y for x,y in zip(a,b))
    power_a = [sum(x*x for x in a[i:i+SAMPLES_PER_FRAME])
               for i in range(0,n,SAMPLES_PER_FRAME)]
    power_b = [sum(y*y for y in b[i:i+SAMPLES_PER_FRAME])
               for i in range(0,n,SAMPLES_PER_FRAME)]
    voiced_a = sum(p>0 for p in power_a)
    newly_zero = sum(x>0 and y==0 for x,y in zip(power_a,power_b))
    original_kind = inspect_wav_bytes(wrap_wav(before))["stationarity_hypothesis"]
    denoised_kind = inspect_wav_bytes(wrap_wav(after))["stationarity_hypothesis"]
    denom = math.sqrt(before_energy*after_energy) if after_energy else 0.0
    aligned_lag, aligned_score = bounded_offset_cosine(a, b)

    return {
        "schema": 1,
        "signal": kind,
        "noise_suppress_db": db,
        "frame_samples": SAMPLES_PER_FRAME,
        "frames": n // SAMPLES_PER_FRAME,
        "before_power": before_energy,
        "after_power": after_energy,
        "energy_ratio": round(after_energy/before_energy, 6),
        "zero_lag_cosine_similarity": round(dot/denom, 6) if denom else None,
        "best_bounded_alignment_shift_samples": aligned_lag,
        "estimated_alignment_not_latency_certified": True,
        "aligned_cosine_similarity": aligned_score,
        "changed_samples": changed,
        "nonzero_original_frames": voiced_a,
        "newly_zero_original_frames": newly_zero,
        "guard_original_20ms_stationarity": original_kind,
        "guard_after_ns_20ms_stationarity": denoised_kind,
        "speex_vad_or_agc_enabled": False,
        "no_speech_decision_authorized": False,
        "actual_snr_or_wer_cer_measured": False,
        "intelligibility_or_weak_consonants_preserved_certified": False,
        "model_weights_used": False,
        "raw_audio_or_paths_emitted": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", required=True, type=Path)
    parser.add_argument("--before-sha256", required=True)
    parser.add_argument("--after", required=True, type=Path)
    parser.add_argument("--after-sha256", required=True)
    parser.add_argument("--kind", required=True, choices=("steady", "whisper", "road"))
    parser.add_argument("--db", required=True, type=int, choices=(-12, -18))
    opts = parser.parse_args()
    try:
        original = verified_raw(opts.before, opts.before_sha256)
        denoised = verified_raw(opts.after, opts.after_sha256)
        print(json.dumps(evaluate(original, denoised, opts.kind, opts.db),
                         sort_keys=True))
        return 0
    except (OSError, ValueError, TypeError, EOFError, wave.Error, struct.error):
        # Never echo a caller-provided private filename or original text.
        print("private SpeexDSP audio-pair diagnostic rejected", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
