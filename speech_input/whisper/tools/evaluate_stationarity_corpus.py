#!/usr/bin/env python3
"""Offline stationarity false-rejection characterization of caller-provided WAVs.

No model/audio downloads, inference, transcripts, or raw audio in reports.
The caller declares source/rights labels; this tool cannot verify them.
Its report is NOT general VAD, ASR quality, or default-ON authorization.
"""
import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
import sys
import wave

from measure_frame_stationarity import inspect_wav_bytes

MAX_MANIFEST_BYTES = 128 * 1024
MAX_SAMPLES = 256
MAX_AUDIO_BYTES = 128 * 1024 * 1024
MAX_SINGLE_WAV_BYTES = 4 * 1024 * 1024
SOURCE_CONDITIONS = {
    "human_voice": {"ordinary", "low_volume", "monotone", "sustained_vowel",
                    "low_snr", "code_switch"},
    "real_noise": {"fan", "hvac", "traffic", "room", "impulse",
                   "intermittent", "music", "silence"},
    "synthetic": {"stationary", "varying", "zero", "equal_power", "other"},
}
LANGUAGES = {"en", "zh", "ja", "mixed"}
RIGHTS_BASIS = {"creator_consent", "public_license", "generated_by_test"}
EXPECTED_FIELDS = {"path", "sha256", "source_kind", "language",
                   "condition", "rights_basis"}


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def rate(positive: int, total: int) -> dict:
    """Wilson 95% confidence interval with explicit unknown zero denominator."""
    if total == 0:
        return {"count": positive, "denominator": 0, "rate": None,
                "wilson95": None}
    p = positive / total
    z = 1.959963984540054
    z2 = z * z
    center = (p + z2 / (2 * total)) / (1 + z2 / total)
    radius = z * math.sqrt(p * (1-p) / total + z2/(4*total*total)) / (1+z2/total)
    return {"count": positive, "denominator": total, "rate": round(p, 6),
            "wilson95": [round(max(0, center-radius), 6),
                         round(min(1, center+radius), 6)]}


def valid_path(root: Path, name: str) -> Path:
    if not isinstance(name, str) or "\\" in name or "\x00" in name:
        raise ValueError("sample path must be a relative POSIX WAV path")
    parts = PurePosixPath(name)
    if (not name or parts.is_absolute() or parts.suffix.lower() != ".wav"
            or any(p in (".", "..", "") for p in name.split("/"))):
        raise ValueError("sample path cannot escape audio-root")
    path = root
    for part in parts.parts:
        path = path / part
        if path.is_symlink():
            raise ValueError("symlinked audio paths are refused")
    resolved = path.resolve(strict=True)
    if not resolved.is_relative_to(root) or not resolved.is_file():
        raise ValueError("sample must be a file inside audio-root")
    return resolved


def inspect_sample(root: Path, item: dict, schema: int) -> tuple:
    if not isinstance(item, dict):
        raise ValueError("sample must be an object")
    source = item.get("source_kind")
    if not isinstance(source, str) or source not in SOURCE_CONDITIONS:
        raise ValueError("unknown source kind")
    expected = EXPECTED_FIELDS | (
        {"speaker_group"} if schema == 2 and source == "human_voice" else set()
    )
    if set(item) != expected:
        raise ValueError("sample must contain exactly required fields")
    lang = item["language"]
    if not isinstance(lang, str) or (
        lang not in LANGUAGES if source == "human_voice" else lang != "none"
    ):
        raise ValueError("invalid language for source kind")
    condition = item["condition"]
    if not isinstance(condition, str) or condition not in SOURCE_CONDITIONS[source]:
        raise ValueError("invalid source condition")
    rights = item["rights_basis"]
    if (not isinstance(rights, str) or rights not in RIGHTS_BASIS
            or (source == "synthetic") != (rights == "generated_by_test")):
        raise ValueError("source rights/consent declaration missing or inconsistent")
    speaker = item.get("speaker_group")
    if schema == 2 and source == "human_voice":
        # Anonymous caller-created grouping token. No actual identity must
        # be inferred, and this token is NEVER emitted in report/log output.
        if (not isinstance(speaker, str)
                or not re.fullmatch(r"[A-Za-z0-9_-]{12,64}", speaker)):
            raise ValueError("invalid private speaker grouping token")
    digest = item["sha256"]
    if not isinstance(digest, str) or not re.fullmatch(r"[a-f0-9]{64}", digest):
        raise ValueError("sample needs exact lowercase SHA-256")
    path = valid_path(root, item["path"])
    if path.stat().st_size > MAX_SINGLE_WAV_BYTES:
        raise ValueError("oversized WAV")
    with path.open("rb") as stream:
        raw = stream.read(MAX_SINGLE_WAV_BYTES + 1)
    if len(raw) > MAX_SINGLE_WAV_BYTES:
        raise ValueError("oversized WAV")
    # Identity and statistics must use this exact SAME immutable snapshot.
    # Re-opening the WAV after validating SHA permits check/use drift.
    if hashlib.sha256(raw).hexdigest() != digest:
        raise ValueError("sample SHA-256 mismatch")
    info = inspect_wav_bytes(raw, include_pcm_digest=True)
    outcome = info["stationarity_hypothesis"]
    duration_frames = info["frame_count"]
    if duration_frames < 50:
        duration = "lt_1s"
    elif duration_frames < 150:
        duration = "1_to_3s"
    elif duration_frames < 500:
        duration = "3_to_10s"
    else:
        duration = "10_to_60s"
    off_rejected = outcome == "digital_zero"
    on_rejected = off_rejected or outcome == "stationary_candidate"
    return (source, lang, condition, len(raw), off_rejected, on_rejected,
            info["_pcm_sha256"], speaker, duration)

def evaluate(audio_root: Path, manifest_path: Path) -> dict:
    root = audio_root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("audio-root must be a directory")
    with manifest_path.open("rb") as stream:
        manifest_bytes = stream.read(MAX_MANIFEST_BYTES + 1)
    if len(manifest_bytes) > MAX_MANIFEST_BYTES:
        raise ValueError("oversized manifest")
    manifest = json.loads(manifest_bytes.decode("utf-8"),
                          object_pairs_hook=unique_pairs)
    if (not isinstance(manifest, dict) or set(manifest) != {"schema", "samples"}
            or type(manifest["schema"]) is not int or manifest["schema"] not in (1, 2)
            or not isinstance(manifest["samples"], list)
            or not 1 <= len(manifest["samples"]) <= MAX_SAMPLES):
        raise ValueError("unsupported or empty corpus manifest")
    schema = manifest["schema"]
    groups = defaultdict(lambda: [0, 0, 0])  # count / OFF reject / ON reject
    langs = defaultdict(lambda: [0, 0])
    conditions = defaultdict(lambda: [0, 0])
    real_noise_conditions = defaultdict(lambda: [0, 0])
    durations = {k: [0, 0] for k in
                 ("lt_1s", "1_to_3s", "3_to_10s", "10_to_60s")}
    speaker_groups = defaultdict(lambda: [0, 0])
    total_bytes = 0
    seen_digests = set()
    seen_pcm_digests = set()
    for item in manifest["samples"]:
        # Exact container duplicates and identical raw PCM under modified
        # WAV headers must never inflate the observed sample denominator.
        digest = item.get("sha256") if isinstance(item, dict) else None
        if isinstance(digest, str) and digest in seen_digests:
            raise ValueError("duplicate WAV bytes in corpus")
        source, lang, condition, size, off, on, pcm_digest, speaker, duration = (
            inspect_sample(root, item, schema))
        if pcm_digest in seen_pcm_digests:
            raise ValueError("duplicate PCM samples in corpus")
        seen_digests.add(digest)
        seen_pcm_digests.add(pcm_digest)
        total_bytes += size
        if total_bytes > MAX_AUDIO_BYTES:
            raise ValueError("bounded corpus byte budget exceeded")
        g = groups[source]
        g[0] += 1
        g[1] += int(off)
        g[2] += int(on)
        if source == "human_voice":
            langs[lang][0] += 1
            langs[lang][1] += int(on)
            conditions[condition][0] += 1
            conditions[condition][1] += int(on)
            durations[duration][0] += 1
            durations[duration][1] += int(on)
            if schema == 2:
                s = speaker_groups[speaker]
                s[0] += 1
                s[1] += int(on)
        elif source == "real_noise":
            n = real_noise_conditions[condition]
            n[0] += 1
            n[1] += int(on)
    human, noise, synthetic = (groups[k] for k in
                               ("human_voice", "real_noise", "synthetic"))
    sp_count = len(speaker_groups)
    sp_any = sum(1 for clips, rejected in speaker_groups.values() if rejected > 0)
    sp_macro = (round(sum(rej / clips for clips, rej in speaker_groups.values())
                      / sp_count, 6) if sp_count else None)
    # Do not omit missing stratification categories: a null 0/0 cohort
    # is a visible evidence gap, NOT an observed zero false-refusal rate.
    empty_languages = sorted(k for k in LANGUAGES if langs[k][0] == 0)
    empty_voice_conditions = sorted(
        k for k in SOURCE_CONDITIONS["human_voice"] if conditions[k][0] == 0)
    empty_noise_conditions = sorted(
        k for k in SOURCE_CONDITIONS["real_noise"]
        if real_noise_conditions[k][0] == 0)
    return {
        "schema": schema,
        "coverage_gaps": {
            "missing_declared_voice_languages": empty_languages,
            "missing_declared_voice_conditions": empty_voice_conditions,
            "missing_declared_real_noise_conditions": empty_noise_conditions,
            "any_independently_verified_human_voice": False,
            "any_independently_verified_real_noise": False,
            "all_required_strata_independently_qualified": False,
            "no_data_is_not_zero_error_rate": True,
        },
        "method": "preinference_20ms_frame_power_lag1_correlation_guard_abi2",
        "guard_eligibility": {
            "min_full_20ms_frames": 50,
            "max_frame_power_cv": 0.12,
            "max_frame_power_ratio": 2.0,
            "max_absolute_lag1_correlation_exclusive": 0.12,
            "applies_only_to_nonzero_pcm": True,
            "matches_default_off_c11_provider_abi": 2,
            "not_general_voice_activity_detection": True,
        },
        "guard_default_changed": False,
        "model_inference_used": False,
        "model_weights_downloaded": False,
        "human_labels_and_rights_independently_verified": False,
        "speaker_independence_verified": False,
        "wilson95_assumes_independent_recordings": True,
        "acoustic_vad_or_recognition_quality_certified": False,
        "source_label_scope": "caller_declared_not_verified",
        "sample_count": len(manifest["samples"]),
        "human_voice": {
            "guard_off_rejections": human[1],
            "guard_on_rejections": human[2],
            "incremental_guard_rejections": human[2]-human[1],
            "guard_on_false_rejection": rate(human[2], human[0]),
            "by_language": {
                k: rate(langs[k][1], langs[k][0]) for k in sorted(LANGUAGES)},
            "by_condition": {
                k: rate(conditions[k][1], conditions[k][0])
                for k in sorted(SOURCE_CONDITIONS["human_voice"])},
            "by_duration": {k: rate(v[1], v[0]) for k, v in sorted(durations.items())},
            "speaker_groups": {
                "schema2_declared_groups": schema == 2,
                "unique_group_count": sp_count if schema == 2 else None,
                "speaker_any_guard_rejection": (
                    rate(sp_any, sp_count) if schema == 2 else None),
                "equal_weight_per_speaker_rejection_rate": (
                    sp_macro if schema == 2 else None),
                "group_labels_or_identity_verified": False,
            },
        },
        "real_noise": {
            "guard_off_predecode_rejections": noise[1],
            "guard_on_predecode_rejections": rate(noise[2], noise[0]),
            "not_screened_by_guard": noise[0]-noise[2],
            "by_condition": {
                k: rate(real_noise_conditions[k][1], real_noise_conditions[k][0])
                for k in sorted(SOURCE_CONDITIONS["real_noise"])},
            "hallucination_rate_measured": False,
        },
        "synthetic_controls": {
            "count": synthetic[0],
            "guard_off_predecode_rejections": synthetic[1],
            "guard_on_predecode_rejections": synthetic[2],
            "not_human_speech_evidence": True,
        },
    }

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--audio-root", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    try:
        payload = json.dumps(evaluate(args.audio_root, args.manifest),
                             sort_keys=True, indent=2) + "\n"
        args.report.write_text(payload, encoding="utf-8")
        sys.stdout.write(payload)  # aggregates only, never sample paths/bytes
        return 0
    except (OSError, ValueError, TypeError, UnicodeError, EOFError, wave.Error) as exc:
        # Avoid leaking private source paths in CI error logs.
        sys.stderr.write(f"speech corpus stationarity evaluation rejected: {type(exc).__name__}\n")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
