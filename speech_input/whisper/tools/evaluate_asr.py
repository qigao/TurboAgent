#!/usr/bin/env python3
"""Offline, dependency-free Unicode-aware ASR quality metrics for CI.

This is evaluation-only tooling. No inference, audio/model download, persistent
recording, shell/Agent interaction or external Python package dependency.
"""
import argparse
import json
from pathlib import Path
import re
import sys
import unicodedata

MAX_TEXT_BYTES = 4097

def _read_transcript(path: Path) -> str:
    data = path.read_bytes()
    if not data or len(data) > MAX_TEXT_BYTES:
        raise ValueError("transcript must contain 1..4097 bytes")
    text = data.decode("utf-8", errors="strict").rstrip("\r\n")
    if not text:
        raise ValueError("transcript is empty")
    if "\x00" in text or any(unicodedata.category(ch) == "Cc"
                            and ch not in "\t\n" for ch in text):
        raise ValueError("transcript contains unexpected controls")
    return text

def _normal_form(value: str) -> str:
    return unicodedata.normalize("NFKC", value).casefold()

def _content_characters(text: str) -> list[str]:
    return [ch for ch in _normal_form(text)
            if not (ch.isspace()
                    or unicodedata.category(ch).startswith("P")
                    or unicodedata.category(ch).startswith("C"))]

def _words(text: str) -> list[str]:
    chars = _normal_form(text)
    chars = "".join(" " if (unicodedata.category(c).startswith("P")
                            or unicodedata.category(c).startswith("C"))
                    else c for c in chars)
    return re.findall(r"[^\W_]+", chars)

def edit_distance(reference: list[str], hypothesis: list[str]) -> int:
    row = list(range(len(hypothesis) + 1))
    for i, x in enumerate(reference, 1):
        next_row = [i] + [0] * len(hypothesis)
        for j, y in enumerate(hypothesis, 1):
            next_row[j] = min(next_row[j-1] + 1,
                              row[j] + 1,
                              row[j-1] + (x != y))
        row = next_row
    return row[-1]

def score(reference: str, hypothesis: str, language: str) -> dict:
    if language not in ("en", "zh", "ja"):
        raise ValueError("language must be en, zh or ja")
    if not isinstance(reference, str) or not isinstance(hypothesis, str):
        raise TypeError("reference and hypothesis must be UTF-8 text")
    source_chars = _content_characters(reference)
    decoded_chars = _content_characters(hypothesis)
    source_words = _words(reference)
    decoded_words = _words(hypothesis)
    if not source_chars or not decoded_chars or not source_words:
        raise ValueError("reference and hypothesis must contain content")
    return {
        "language": language,
        "reference_codepoints": len(source_chars),
        "hypothesis_codepoints": len(decoded_chars),
        "reference_words": len(source_words),
        "hypothesis_words": len(decoded_words),
        "character_error_rate": round(
            edit_distance(source_chars, decoded_chars) / len(source_chars), 6),
        "word_error_rate": round(
            edit_distance(source_words, decoded_words) / len(source_words), 6),
        "quality_certification": False,  # one clip is smoke, not a benchmark
    }

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", required=True)
    parser.add_argument("--transcript-file", type=Path, required=True)
    parser.add_argument("--language", choices=("en", "zh", "ja"), required=True)
    parser.add_argument("--max-wer", type=float)
    parser.add_argument("--max-cer", type=float)
    parser.add_argument("--require-word", action="append", default=[])
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    try:
        reference = args.reference
        if not reference or len(reference.encode("utf-8")) > MAX_TEXT_BYTES:
            raise ValueError("reference text too long or empty")
        hypothesis = _read_transcript(args.transcript_file)
        scored = score(reference, hypothesis, args.language)
        for name, value in (("max-wer", args.max_wer),
                            ("max-cer", args.max_cer)):
            if value is not None and not 0.0 <= value <= 1.0:
                raise ValueError(f"{name} must be between 0 and 1")
        words = set(_words(hypothesis))
        for required in args.require_word:
            pieces = _words(required)
            if len(pieces) != 1 or pieces[0] not in words:
                raise ValueError(f"missing required spoken word: {required!r}")
        if args.max_wer is not None and scored["word_error_rate"] > args.max_wer:
            raise ValueError(f"WER {scored['word_error_rate']} > {args.max_wer}")
        if args.max_cer is not None and scored["character_error_rate"] > args.max_cer:
            raise ValueError(f"CER {scored['character_error_rate']} > {args.max_cer}")
        # Avoid echoing user-supplied transcript in logs. JSON contains numeric
        # measurements and source language only, never speech content.
        payload = json.dumps(scored, ensure_ascii=False, indent=2) + "\n"
        args.report.write_text(payload, encoding="utf-8")
        sys.stdout.write(payload)
        return 0
    except (OSError, UnicodeError, ValueError, TypeError) as exc:
        sys.stderr.write(f"ASR quality admission rejected: {exc}\n")
        return 2

if __name__ == "__main__":
    raise SystemExit(main())
