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

from measure_frame_stationarity import inspect_wav

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


def inspect_sample(root: Path, item: dict) -> tuple:
    if not isinstance(item, dict) or set(item) != EXPECTED_FIELDS:
        raise ValueError("sample must contain exactly required fields")
    source = item["source_kind"]
    if not isinstance(source, str) or source not in SOURCE_CONDITIONS:
        raise ValueError("unknown source kind")
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
    digest = item["sha256"]
    if not isinstance(digest, str) or not re.fullmatch(r"[a-f0-9]{64}", digest):
        raise ValueError("sample needs exact lowercase SHA-256")
    path = valid_path(root, item["path"])
    size = path.stat().st_size
    if size > MAX_SINGLE_WAV_BYTES:
        raise ValueError("oversized WAV")
    h = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(65536)
            if not chunk:
                break
            h.update(chunk)
    if h.hexdigest() != digest:
        raise ValueError("sample SHA-256 mismatch")
    outcome = inspect_wav(path)["stationarity_hypothesis"]
    off_rejected = outcome == "digital_zero"
    on_rejected = off_rejected or outcome == "stationary_candidate"
    return source, lang, condition, size, off_rejected, on_rejected


def evaluate(audio_root: Path, manifest_path: Path) -> dict:
    root = audio_root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("audio-root must be a directory")
    if manifest_path.stat().st_size > MAX_MANIFEST_BYTES:
        raise ValueError("oversized manifest")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"),
                          object_pairs_hook=unique_pairs)
    if (not isinstance(manifest, dict) or set(manifest) != {"schema", "samples"}
            or type(manifest["schema"]) is not int or manifest["schema"] != 1
            or not isinstance(manifest["samples"], list)
            or not 1 <= len(manifest["samples"]) <= MAX_SAMPLES):
        raise ValueError("unsupported or empty corpus manifest")
    groups = defaultdict(lambda: [0, 0, 0])  # count / OFF reject / ON reject
    langs = defaultdict(lambda: [0, 0])
    conditions = defaultdict(lambda: [0, 0])
    total_bytes = 0
    for item in manifest["samples"]:
        source, lang, condition, size, off, on = inspect_sample(root, item)
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
    human, noise, synthetic = (groups[k] for k in
                               ("human_voice", "real_noise", "synthetic"))
    return {
        "schema": 1,
        "method": "preinference_20ms_frame_power_stationarity_guard_abi2",
        "guard_default_changed": False,
        "model_inference_used": False,
        "model_weights_downloaded": False,
        "human_labels_and_rights_independently_verified": False,
        "acoustic_vad_or_recognition_quality_certified": False,
        "source_label_scope": "caller_declared_not_verified",
        "sample_count": len(manifest["samples"]),
        "human_voice": {
            "guard_off_rejections": human[1],
            "guard_on_rejections": human[2],
            "incremental_guard_rejections": human[2]-human[1],
            "guard_on_false_rejection": rate(human[2], human[0]),
            "by_language": {k: rate(v[1], v[0]) for k, v in sorted(langs.items())},
            "by_condition": {k: rate(v[1], v[0]) for k, v in sorted(conditions.items())},
        },
        "real_noise": {
            "guard_off_predecode_rejections": noise[1],
            "guard_on_predecode_rejections": rate(noise[2], noise[0]),
            "not_screened_by_guard": noise[0]-noise[2],
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
    except (OSError, ValueError, TypeError, EOFError, wave.Error) as exc:
        # Avoid leaking private source paths in CI error logs.
        sys.stderr.write(f"speech corpus stationarity evaluation rejected: {type(exc).__name__}\n")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
