#!/usr/bin/env python3
"""Verify the actually built, first-party TurboAgent queued-DSO sanitizer slice.

IMPORTANT: Salts Plugin/ComponentPlugin come from pinned *released* SDKs in
this phase.  Thus this audit MUST NOT claim all Salts objects were instrumented
inside the same test process.  See #119 and full-source next-phase gate.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys


EXPECTED = {
    "harness/src/turbo_agent_harness.c",
    "agent_compiler/src/turbo_agent_compiler.c",
    "runtime_tools/src/turbo_tool_registry.c",
    "runtime_tools/src/turbo_tool_runtime.c",
    "agent/src/turbo_agent_execution.c",
    "agent/src/turbo_agent_session_execution.c",
    "agent/src/turbo_agent_session.c",
    "agent/src/turbo_agent_app.c",
    "praktor_tools/tests/test_turbo_praktor_dso_compiled_harness.c",
    "praktor_tools/tests/praktor_dso_provider.c",
}
INSTRUMENTED_IMAGES = (
    "bin/test_turbo_praktor_dso_compiled_harness",
    "bin/libpraktor_dso_fixture_async_managed.so",
    "bin/libturbo_agent_harness.so",
    "bin/libruntime_tools.so",
    "bin/libagent_compiler.so",
)


def refuse(message: str) -> None:
    raise RuntimeError("SOURCE COVERAGE FAIL: " + message)


def object_path(row: dict[str, str]) -> Path | None:
    cmd = row.get("arguments") or shlex.split(row["command"])
    if "-o" not in cmd:
        return None
    pos = cmd.index("-o")
    if pos + 1 >= len(cmd):
        refuse(f"missing output for {row['file']}")
    return (Path(row["directory"]) / cmd[pos + 1]).resolve()


def main() -> None:
    if len(sys.argv) != 5:
        refuse("usage: audit.py <build> <repo-root> <thread|address> <sdk-provenance>")
    build, root = (Path(sys.argv[i]).resolve() for i in (1, 2))
    kind = sys.argv[3]
    provenance = Path(sys.argv[4]).resolve()
    if kind not in {"thread", "address"}:
        refuse("sanitizer kind is not thread/address")
    expected_flags = (
        ("-fsanitize=thread",) if kind == "thread"
        else ("-fsanitize=address", "-fsanitize=undefined")
    )
    if not (build / "compile_commands.json").is_file():
        refuse("missing compile_commands.json; no source coverage evidence")
    if not provenance.is_file():
        refuse("no frozen exact SDK/source provenance")
    sdk = json.loads(provenance.read_text(encoding="utf-8"))
    if len(sdk.get("packages", [])) != 6 or not sdk.get("salts_source_sha"):
        refuse("incomplete six-SDK source pin record")
    commands = json.loads((build / "compile_commands.json").read_text(
        encoding="utf-8"))
    observed: dict[str, int] = {}
    actually_built = []
    for row in commands:
        path = Path(row["file"]).resolve()
        if not path.is_relative_to(root):
            continue
        rel = path.relative_to(root).as_posix()
        if rel.startswith(("build/", "vcpkg_installed/", "salts-source/")):
            continue
        output = object_path(row)
        if output is None or not output.is_file():
            continue  # Configured-only targets are NOT evidence of instrumentation.
        cmd = row.get("arguments") or shlex.split(row["command"])
        if any(flag not in cmd for flag in expected_flags):
            refuse(f"built uninstrumented first-party unit: {rel} => {output}")
        if "-fno-omit-frame-pointer" not in cmd:
            refuse(f"missing stack frames in built first-party unit: {rel}")
        observed[rel] = observed.get(rel, 0) + 1
        actually_built.append({"file": rel,
                               "object": str(output.relative_to(build))
                               if output.is_relative_to(build) else str(output)})
    for rel in EXPECTED:
        if not observed.get(rel):
            refuse(f"required execution/DSO source was never compiled: {rel}")
    if len(actually_built) < 25:
        refuse(f"expected an actual native target closure, built only {len(actually_built)} units")
    # Ensure the managed fixture is compiled, not merely a different fixture
    # incarnation pulled into the build from the main test graph.
    fixture = [
        item for item in actually_built
        if item["file"] == "praktor_tools/tests/praktor_dso_provider.c"
        and "praktor_dso_fixture_async_managed.dir" in item["object"]
    ]
    if len(fixture) != 1:
        refuse("real Managed queued callback DSO fixture absent from object graph")

    symbols = "__tsan_" if kind == "thread" else "__asan_"
    ub = "__ubsan_" if kind == "address" else None
    for rel in INSTRUMENTED_IMAGES:
        binary = build / rel
        if not binary.is_file():
            refuse(f"missing linked first-party image: {binary}")
        result = subprocess.run(
            ["nm", "-a", str(binary)], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, check=True,
        ).stdout
        if symbols not in result or (ub and ub not in result):
            refuse(f"missing sanitizer symbols in actually linked {binary}")
        print(f"SANITIZER IMAGE: {rel}: {kind}", flush=True)

    linkmap = build / "praktor_tools" / "queued_dso_sanitizer.map"
    if not linkmap.is_file():
        refuse("missing GNU ld map for real queued DSO test")
    maps = linkmap.read_text(encoding="utf-8", errors="replace")
    if "test_turbo_praktor_dso_compiled_harness.c.o" not in maps:
        refuse("no linked test C object in linker map")
    if not any(x in maps for x in ("libturbo_agent_harness.so", "libpraktor_tools.so")):
        refuse("linked test map lacks actual TurboAgent runtime shared libraries")

    report = {
        "test_target": "test_turbo_praktor_dso_compiled_harness",
        "sanitizer": kind, "expected_flags": list(expected_flags),
        "firstparty_built_count": len(actually_built),
        "firstparty_sources": actually_built,
        "critical_sources": sorted(EXPECTED),
        "instrumented_images": list(INSTRUMENTED_IMAGES),
        "upstream_salts_registry_component_loader_source_in_this_process": False,
        "upstream_dependency_status": (
            "Salts Plugin/ComponentPlugin linked from exact released SDK; "
            "NOT part of this process's source-sanitizer coverage. "
            "Do not call #119 full-source-qualified yet."
        ),
        "sdk_provenance": sdk,
        "tested_commit": os.environ.get("GITHUB_SHA", "unknown"),
        "source_head": os.environ.get("SOURCE_HEAD_SHA", "unknown"),
        "run_id": os.environ.get("GITHUB_RUN_ID", "local"),
    }
    path = build / "queued_dso_coverage.json"
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")
    print(f"SANITIZER SOURCE COVERAGE: {len(actually_built)} actual objects, {kind}")
    print("UPSTREAM SALTS REGISTRY/LOADER: UNINSTRUMENTED RELEASE SDK, GAP OPEN")
    print(f"COVERAGE EVIDENCE: {path}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, KeyError,
            subprocess.CalledProcessError) as exc:
        print(str(exc), file=sys.stderr)
        raise SystemExit(1)
