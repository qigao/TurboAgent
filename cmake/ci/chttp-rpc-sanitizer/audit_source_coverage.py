#!/usr/bin/env python3
"""Audit real external C11/DSO sanitizer coverage; upstream binaries are NOT covered."""
import json
from pathlib import Path
import subprocess
import sys


def require(ok, message):
    if not ok:
        raise ValueError(message)


def main():
    require(len(sys.argv) == 3, "usage: audit_source_coverage.py build thread|address")
    build = Path(sys.argv[1]).resolve()
    kind = sys.argv[2]
    require(kind in ("thread", "address"), "invalid sanitizer")
    flags = ("-fsanitize=thread",) if kind == "thread" else (
        "-fsanitize=address", "-fsanitize=undefined"
    )
    raw = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    targets = (
        "agent/tests/rpc_generated/test_turboagent_typed_rpc.c",
        "agent/tests/rpc_generated/turboagent_rpc_provider_dso.c",
    )
    recorded = {}
    for path in targets:
        records = [cmd for cmd in raw
                   if cmd.get("file", "").replace("\\", "/").endswith("/" + path)]
        require(len(records) == 1, path + ": expected one actual compiler command")
        command = records[0].get("command") or " ".join(records[0].get("arguments", []))
        for flag in flags:
            require(flag in command, path + ": missing " + flag)
        recorded[path] = {"compiled_with": list(flags)}
    ninja = subprocess.check_output(
        ["ninja", "-C", str(build), "-t", "commands", "test_turboagent_typed_rpc"],
        text=True,
    )
    lines = ninja.splitlines()
    executables = [
        cmd for cmd in lines if
        "-o bin/test_turboagent_typed_rpc" in cmd
        or "-o " + str(build / "bin/test_turboagent_typed_rpc") in cmd
    ]
    require(any(all(flag in cmd for flag in flags) for cmd in executables),
            "linked first-party typed RPC test is NOT sanitizer instrumented")
    dynamic_provider = [cmd for cmd in lines if
                        "-shared" in cmd and "turboagent_rpc_provider_dso" in cmd]
    require(any(all(flag in cmd for flag in flags) for cmd in dynamic_provider),
            "real dlopen/LoadLibrary Provider DSO link lacks sanitizer flags")
    print(json.dumps({
        "audit": "PASS",
        "kind": kind,
        "first_party_source": recorded,
        "test_executable_link": "instrumented",
        "real_provider_dso_link": "instrumented",
        "published_chttp_salts_saltsutils_binary_sources": "UNINSTRUMENTED",
        "sanitizer_coverage": "PARTIAL_NOT_FULL_SOURCE_CLOSURE",
    }, indent=2, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as exc:
        print("SOURCE INSTRUMENTATION GATE FAILED:", exc, file=sys.stderr)
        sys.exit(1)
