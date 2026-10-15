#!/usr/bin/env python3
"""Private known-noise/known-clean S16 A/B. Not speech recognition acceptance."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import random
import stat
import struct
import sys

SAMPLES_FRAME = 320
PRE_ROLL_FRAMES = 50
SEED = 20261015
NOISE_PEAK = 400
ALLOWED_NOISE_PEAKS = (100, 200, 400)
ALLOWED_NOISE_PROFILES = ("white", "lowpass")
MAX_RAW_BYTES = 1920000


def read_pinned(path: Path, digest: str):
    if (not path.is_absolute() or path.is_symlink() or not path.is_file()
            or not isinstance(digest, str) or len(digest) != 64
            or any(ch not in "0123456789abcdef" for ch in digest)):
        raise ValueError("invalid pinned private PCM")
    with path.open("rb") as f:
        raw = f.read(MAX_RAW_BYTES + 1)
    if (not raw or len(raw)>MAX_RAW_BYTES or len(raw)%640
            or hashlib.sha256(raw).hexdigest() != digest):
        raise ValueError("unaligned or unverified PCM")
    return raw


def samples(raw: bytes):
    return struct.unpack("<%dh" % (len(raw)//2),raw)


def new_private(path: Path, contents: bytes):
    if not path.is_absolute() or path.is_symlink() or path.exists():
        raise ValueError("private destination already exists or not absolute")
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd,"wb") as output:
            output.write(contents)
    except BaseException:
        path.unlink(missing_ok=True)
        raise


def prepare(clean_source: bytes, private_root: Path,
            noise_peak: int = NOISE_PEAK, noise_profile: str = "white") -> dict:
    if (not isinstance(clean_source, bytes) or not clean_source
            or len(clean_source)%640 or len(clean_source)>MAX_RAW_BYTES-32000):
        raise ValueError("source must be bounded S16 complete frames")
    if (not private_root.is_absolute() or private_root.is_symlink()
            or not private_root.is_dir()
            or stat.S_IMODE(private_root.stat().st_mode) != 0o700):
        raise ValueError("require private 0700 temp root")
    if type(noise_peak) is not int or noise_peak not in ALLOWED_NOISE_PEAKS:
        raise ValueError("noise peak must be one of three approved levels")
    if noise_profile not in ALLOWED_NOISE_PROFILES:
        raise ValueError("invalid explicit synthetic noise profile")
    source = list(samples(clean_source))
    clean = [0] * (PRE_ROLL_FRAMES * SAMPLES_FRAME) + source
    rng = random.Random(SEED)
    mixed = []
    prev_noise = 0
    for x in clean:
        white_noise = rng.randint(-noise_peak, noise_peak)
        if noise_profile == "lowpass":
            # Explicit fixed-coefficient first-order correlated noise.
            # This is synthesized noise, NOT recorded HVAC/room noise.
            prev_noise = int((3 * prev_noise + white_noise) / 4)
            noise = prev_noise
        else:
            noise = white_noise
        value = x + noise
        if value < -32768 or value > 32767:
            raise ValueError("noise mix clipping disallowed")
        mixed.append(value)
    fmt = "<%dh" % len(clean)
    original = struct.pack(fmt, *clean)
    contaminated = struct.pack(fmt, *mixed)
    before = private_root/"clean-reference.raw"
    noisy = private_root/"mixed-known-noise.raw"
    new_private(before, original)
    try:
        new_private(noisy, contaminated)
    except BaseException:
        before.unlink(missing_ok=True)
        raise
    return {
        "schema": 1,
        "method": "original_human_voice_plus_deterministic_synthetic_noise",
        "total_frames": len(clean) // SAMPLES_FRAME,
        "speech_frames": len(source) // SAMPLES_FRAME,
        "noise_only_preroll_frames": PRE_ROLL_FRAMES,
        "noise_seed": SEED,
        "noise_max_amplitude": noise_peak,
        "noise_profile": noise_profile,
        "noise_profile_is_synthetic": True,
        "no_raw_audio_in_report": True,
        "no_model_weights_or_transcripts": True,
    }


def score(clean_raw: bytes, mixed_raw: bytes, after_raw: bytes,
          clean_filtered_raw: bytes = None, *,
          noise_peak: int = NOISE_PEAK, noise_profile: str = "white") -> dict:
    if (not clean_raw or len(clean_raw)!=len(mixed_raw)
            or len(clean_raw)!=len(after_raw)
            or len(clean_raw)>MAX_RAW_BYTES or len(clean_raw)%640):
        raise ValueError("paired recordings are not complete matching frames")
    if type(noise_peak) is not int or noise_peak not in ALLOWED_NOISE_PEAKS:
        raise ValueError("unsupported noise peak")
    if noise_profile not in ALLOWED_NOISE_PROFILES:
        raise ValueError("unsupported noise profile")
    if clean_filtered_raw is not None and len(clean_filtered_raw) != len(clean_raw):
        raise ValueError("clean filtered reference length mismatch")
    clean, mixed, after = map(samples, (clean_raw,mixed_raw,after_raw))
    start = PRE_ROLL_FRAMES * SAMPLES_FRAME
    if start >= len(clean):
        raise ValueError("source contains no complete human speech")
    speech_power = sum(int(x)*int(x) for x in clean[start:])
    before_error = sum((x-y)**2 for x,y in zip(clean[start:],mixed[start:]))
    after_error = sum((x-y)**2 for x,y in zip(clean[start:],after[start:]))
    if speech_power==0 or before_error==0:
        raise ValueError("insufficient nonzero clean signal or injected noise")
    pre = 10.0*math.log10(speech_power/before_error)
    post = 10.0*math.log10(speech_power/after_error) if after_error else None
    clean_only_error = None
    clean_only_ser = None
    clean_only_zeroed_frames = None
    if clean_filtered_raw is not None:
        clean_filtered = samples(clean_filtered_raw)
        clean_only_error = sum((x-y)**2 for x,y in
                               zip(clean[start:], clean_filtered[start:]))
        clean_only_ser = (10.0*math.log10(speech_power/clean_only_error)
                          if clean_only_error else None)
        original_frames = [clean[i:i+SAMPLES_FRAME]
                           for i in range(start,len(clean),SAMPLES_FRAME)]
        filtered_frames = [clean_filtered[i:i+SAMPLES_FRAME]
                           for i in range(start,len(clean_filtered),SAMPLES_FRAME)]
        clean_only_zeroed_frames = sum(any(x != 0 for x in original)
                                       and all(y == 0 for y in filtered)
                                       for original, filtered in
                                       zip(original_frames, filtered_frames))
    return {
        "schema":1,
        "signal":"licensed_whisper_plus_generated_noise",
        "noise_profile":noise_profile,
        "noise_max_amplitude":noise_peak,
        "profiles_are_simulated_not_real_room_recordings":True,
        "frames":len(clean)//SAMPLES_FRAME,
        "speech_frames":(len(clean)-start)//SAMPLES_FRAME,
        "noise_preroll_frames":PRE_ROLL_FRAMES,
        "speech_reference_power":speech_power,
        "known_input_noise_power":before_error,
        "post_filter_reference_error_power":after_error,
        "clean_only_filter_error_power":clean_only_error,
        "clean_only_filter_ser_db":round(clean_only_ser,4) if clean_only_ser is not None else None,
        "clean_only_source_nonzero_frames_zeroed":clean_only_zeroed_frames,
        "pre_filter_reference_ser_db":round(pre,4),
        "post_filter_reference_ser_db":round(post,4) if post is not None else None,
        "reference_ser_delta_db":round(post-pre,4) if post is not None else None,
        "includes_ns_speech_distortion_in_post_error":True,
        "model_wer_cer_or_phoneme_intelligibility_measured":False,
        "real_room_noise_or_microphone_transfer_measured":False,
        "no_pcm_or_transcripts_in_report":True,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="task",required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--source-raw",type=Path,required=True)
    p.add_argument("--source-sha256",required=True)
    p.add_argument("--private-root",type=Path,required=True)
    p.add_argument("--noise-peak",type=int,choices=ALLOWED_NOISE_PEAKS,default=NOISE_PEAK)
    p.add_argument("--noise-profile",choices=ALLOWED_NOISE_PROFILES,default="white")
    e = sub.add_parser("score")
    for name in ("clean","mixed","filtered"):
        e.add_argument("--"+name,type=Path,required=True)
        e.add_argument("--"+name+"-sha256",required=True)
    e.add_argument("--clean-filtered",type=Path)
    e.add_argument("--clean-filtered-sha256")
    e.add_argument("--noise-peak",type=int,choices=ALLOWED_NOISE_PEAKS,default=NOISE_PEAK)
    e.add_argument("--noise-profile",choices=ALLOWED_NOISE_PROFILES,default="white")
    options = parser.parse_args()
    try:
        if options.task=="prepare":
            payload=prepare(read_pinned(options.source_raw,options.source_sha256),
                            options.private_root, options.noise_peak, options.noise_profile)
        else:
            if (options.clean_filtered is None) != (
                    options.clean_filtered_sha256 is None):
                raise ValueError("clean reference output requires SHA pair")
            controlled = (read_pinned(options.clean_filtered,
                                     options.clean_filtered_sha256)
                          if options.clean_filtered is not None else None)
            payload=score(
                read_pinned(options.clean, options.clean_sha256),
                read_pinned(options.mixed, options.mixed_sha256),
                read_pinned(options.filtered, options.filtered_sha256),
                controlled, noise_peak=options.noise_peak,
                noise_profile=options.noise_profile)
        print(json.dumps(payload,sort_keys=True))
        return 0
    except (ValueError,OSError,OverflowError,struct.error):
        print("private known-noise pair refused",file=sys.stderr)
        return 2


if __name__=="__main__":
    raise SystemExit(main())
