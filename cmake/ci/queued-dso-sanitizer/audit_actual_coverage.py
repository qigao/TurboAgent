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
    if len(sys.argv) not in (5, 7):
        refuse("usage: audit.py <build> <repo-root> <thread|address> "
               "<sdk-provenance> [<salts-source-build> <salts-source-root>]")
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

    # The integrated phase must prove actual linker extraction of the exact
    # source-built Plugin/ComponentPlugin/Loader archives, not merely that
    # an instrumented Salts archive happened to exist on disk.
    salts_in_process = False
    salts_report = None
    if len(sys.argv) == 7:
        salts_build = Path(sys.argv[5]).resolve()
        salts_source = Path(sys.argv[6]).resolve()
        built_report = Path(os.environ["RUNNER_TEMP"]).resolve() / (
            "queued-salts-source-" + kind + ".json"
        )
        if not built_report.is_file():
            refuse("exact Salts source-build audit is absent")
        salts_report = json.loads(built_report.read_text(encoding="utf-8"))
        installed_source = Path(
            salts_report["installed_source_sdk"]
        ).resolve()
        if installed_source != Path(os.environ["SALTS_ROOT"]).resolve():
            refuse("linked Salts_ROOT does not identify the source-built SDK")
        if salts_report.get("salts_source_commit") != sdk["salts_source_sha"]:
            refuse("source-built Salts commit differs from release source pin")
        if not salts_source.is_dir() or not salts_build.is_dir():
            refuse("missing source-build paths")

        source_maps = [maps]
        extra = build / "praktor_tools/praktor_tools_sanitizer.map"
        if not extra.is_file():
            refuse("source-built Salts integration linker map absent")
        source_maps.append(extra.read_text(
            encoding="utf-8", errors="replace"
        ))
        all_maps = "\n".join(source_maps)
        wanted = (
            ("libcmeta_plugin.a", "plugin_registry.c.o"),
            ("libcmeta_plugin.a", "plugin_posix.c.o"),
            ("libsalts_component_plugin.a", "component_plugin.c.o"),
            ("libsalts_component_plugin.a", "component_plugin_runtime.c.o"),
        )
        for lib, member in wanted:
            pat = re.escape(lib) + r"\(" + re.escape(member) + r"\)"
            if not re.search(pat, all_maps):
                refuse(f"source-built Salts archive member not extracted: {lib}({member})")
            if installed_source.as_posix() not in all_maps:
                refuse("linker map does not contain exact source SDK prefix")
            print(f"INTEGRATED SALTS SOURCE OBJECT: {lib}({member})", flush=True)
        old_sdk = Path(salts_report["original_release_sdk"]).resolve()
        if old_sdk.as_posix() in all_maps:
            refuse("source-integrated executable still links old released Salts SDK")
        salts_in_process = True

    report = {
        "test_target": "test_turbo_praktor_dso_compiled_harness",
        "sanitizer": kind, "expected_flags": list(expected_flags),
        "firstparty_built_count": len(actually_built),
        "firstparty_sources": actually_built,
        "critical_sources": sorted(EXPECTED),
        "instrumented_images": list(INSTRUMENTED_IMAGES),
        "upstream_salts_registry_component_loader_source_in_this_process": salts_in_process,
        "upstream_salts_source_build": salts_report,
        "upstream_dependency_status": (
            "Salts Plugin/ComponentPlugin/Loader linked from exact source-built "
            "instrumented SDK, with extracted archive members in actual maps. "
            "Other released SDKs (SaltsUtils, CHttp, TurboScript, Praktor, "
            "TurboWasm) remain external uninstrumented dependencies."
            if salts_in_process else
            "Salts Plugin/ComponentPlugin linked from exact released SDK; "
            "NOT part of this process's source-sanitizer coverage."
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
    print("UPSTREAM SALTS REGISTRY/LOADER: " +
          ("SOURCE-INSTRUMENTED SAME PROCESS, critical members extracted"
           if salts_in_process else
           "UNINSTRUMENTED RELEASE SDK, GAP OPEN"))
    print(f"COVERAGE EVIDENCE: {path}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, KeyError,
            subprocess.CalledProcessError) as exc:
        print(str(exc), file=sys.stderr)
        raise SystemExit(1)
