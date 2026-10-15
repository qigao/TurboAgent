#!/usr/bin/env python3
"""Inspect bounded frame-power stationarity WITHOUT classifying actual speech.

This independent no-model diagnostic compares generated noise against the
immutable upstream JFK spoken WAV. It is not a VAD, classifier or policy
admission; thresholds are explicit test hypotheses for a future optional C11
preflight. No raw audio, model or transcript is uploaded.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import wave

FRAME = 320
RATE = 16000
MAX_FRAMES = 3000
MAX_FILE_BYTES = 4 * 1024 * 1024
MIN_FRAMES = 50
MAX_STATIONARY_CV = 0.12
MAX_STATIONARY_POWER_RATIO = 2.0
MAX_STATIONARY_LAG1_CORRELATION = 0.12


def inspect_wav_bytes(raw_bytes: bytes, *, include_pcm_digest=False) -> dict:
    """Calculate exact energy and optional PCM fingerprint from ONE byte snapshot.

    The optional fingerprint is for private corpus deduplication only; it
    must never appear in published aggregate reports or default diagnostics.
    """
    if not isinstance(raw_bytes, bytes) or len(raw_bytes) > MAX_FILE_BYTES:
        raise ValueError("oversized or invalid WAV snapshot")
    with wave.open(io.BytesIO(raw_bytes), "rb") as wav:
        if (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) != (RATE, 1, 2):
            raise ValueError("expected strict mono 16000Hz signed PCM16")
        frames = wav.getnframes()
        if not 1 <= frames // FRAME <= MAX_FRAMES or frames % FRAME:
            raise ValueError("incomplete or oversized 20ms frame sequence")
        blocks = frames // FRAME
        power = []
        lag_cross, lag_energy = 0, 0
        pcm_digest = hashlib.sha256() if include_pcm_digest else None
        for _ in range(blocks):
            raw = wav.readframes(FRAME)
            if len(raw) != FRAME * 2:
                raise ValueError("partial PCM read")
            if pcm_digest is not None:
                pcm_digest.update(raw)
            pcm = struct.unpack("<320h", raw)
            power.append(sum(int(x) * int(x) for x in pcm))
            lag_cross += sum(int(pcm[i]) * int(pcm[i-1]) for i in range(1, FRAME))
            lag_energy += sum(int(pcm[i]) * int(pcm[i]) for i in range(1, FRAME))
        if wav.readframes(1):
            raise ValueError("undeclared additional PCM")
    count = len(power)
    lag1_abs_correlation = abs(lag_cross) / lag_energy if lag_energy else 0.0
    total = sum(power)
    if total == 0:
        kind = "digital_zero"
        cv = 0.0
        ratio = 0.0
    else:
        avg = total / count
        variance = sum((p - avg) ** 2 for p in power) / count
        cv = (variance ** 0.5) / avg
        smallest = min(power)
        ratio = max(power) / smallest if smallest else None
        # Strict 12/100 ratio is checked using full integers, exactly like
        # the bounded C11 Provider. At equality voice/noise is unknown,
        # so fail open to decode. This is not general acoustic VAD.
        kind = ("stationary_candidate" if count >= MIN_FRAMES and
                cv <= MAX_STATIONARY_CV and ratio is not None and
                ratio <= MAX_STATIONARY_POWER_RATIO and
                lag_energy > 0 and
                100 * abs(lag_cross) < 12 * lag_energy
                else "dynamic_or_unknown")
    report = {
        "frame_count": count,
        "sample_count": frames,
        "zero_pcm": total == 0,
        "stationarity_hypothesis": kind,
        "frame_power_cv": round(cv, 6),
        "lag1_abs_correlation": round(lag1_abs_correlation, 6),
        "frame_power_max_min_ratio": None if ratio is None else round(ratio, 6),
        "minimum_frame_power": min(power),
        "maximum_frame_power": max(power),
        "voice_activity_certified": False,
    }
    if pcm_digest is not None:
        report["_pcm_sha256"] = pcm_digest.hexdigest()
    return report


def inspect_wav(path: Path) -> dict:
    # Single bounded read, including the case where the file grows after
    # being opened. No re-open between source bytes and measured PCM.
    with path.open("rb") as stream:
        raw = stream.read(MAX_FILE_BYTES + 1)
    return inspect_wav_bytes(raw)

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jfk-wav", type=Path, required=True)
    parser.add_argument("--noise-dir", type=Path, required=True)
    parser.add_argument("--agc-jfk-wav", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    try:
        items = {"official_jfk_spoken": inspect_wav(args.jfk_wav)}
        for name in ("digital-zero", "stationary-noise-quiet",
                     "stationary-noise-medium"):
            items[name] = inspect_wav(args.noise_dir / (name + ".wav"))
        if args.agc_jfk_wav:
            items["agc_jfk_spoken"] = inspect_wav(args.agc_jfk_wav)
            agc = items["agc_jfk_spoken"]
            if (agc["frame_power_cv"] >= MAX_STATIONARY_CV or
                    agc["frame_power_max_min_ratio"] is None or
                    agc["frame_power_max_min_ratio"] >= MAX_STATIONARY_POWER_RATIO or
                    agc["lag1_abs_correlation"] <= MAX_STATIONARY_LAG1_CORRELATION or
                    agc["stationarity_hypothesis"] != "dynamic_or_unknown"):
                raise ValueError("gain-normalized JFK speech risks false NO_SPEECH")
        if items["official_jfk_spoken"]["stationarity_hypothesis"] != "dynamic_or_unknown":
            raise ValueError("stationarity rule would reject the pinned spoken JFK sample")
        if items["digital-zero"]["stationarity_hypothesis"] != "digital_zero":
            raise ValueError("failed explicit exact-zero gate")
        for name in ("stationary-noise-quiet", "stationary-noise-medium"):
            if items[name]["stationarity_hypothesis"] != "stationary_candidate":
                raise ValueError(f"stationarity hypothesis failed on {name}")
        payload = {
            "model_weights_used": False,
            "acoustic_vad_certified": False,
            "hypothesis_only": True,
            "min_evidence_frames": MIN_FRAMES,
            "max_candidate_cv": MAX_STATIONARY_CV,
            "max_candidate_power_ratio": MAX_STATIONARY_POWER_RATIO,
            "max_uncorrelated_lag1_ratio": MAX_STATIONARY_LAG1_CORRELATION,
            "records": items,
        }
        text = json.dumps(payload, sort_keys=True, indent=2) + "\n"
        args.report.write_text(text, encoding="utf-8")
        sys.stdout.write(text)
        return 0
    except (OSError, EOFError, ValueError, wave.Error) as exc:
        sys.stderr.write(f"stationarity diagnostic rejected: {exc}\n")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
