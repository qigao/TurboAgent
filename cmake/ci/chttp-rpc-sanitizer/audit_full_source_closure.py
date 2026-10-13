#!/usr/bin/env python3
"""Fail-closed source-graph sanitizer audit for TurboAgent #123.

Separate from the published NuGet consumer audit. Every compiled first-party
unit in four isolated source graphs must carry the same sanitizer flags; the
actual native test and unloadable Provider DSO must be linked with them too.
"""
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys


REQUIRED_SOURCES = {
    "salts": (
        "cnet/src/cnet_module.c",
        "platform/src/thread_posix.c",
        "cmeta/src/function.c",
        "plugin/src/plugin.c",
    ),
    "salts-utils": (
        "databind/runtime/data_bind_binding_plan.c",
        "databind/runtime/data_bind_method_plan.c",
        "databind/runtime/data_bind_native.c",
    ),
    "chttp": (
        "http_client/src/chttp_client.c",
        "http_server/src/chttp_server.c",
        "http_server/rpc/chttp_server_rpc_service.c",
        "http_server/rpc/crpc_server.c",
    ),
    "turboagent": (
        "agent/tests/rpc_generated/test_turboagent_typed_rpc.c",
        "agent/tests/rpc_generated/turboagent_rpc_provider_dso.c",
    ),
}


def require(condition, explanation):
    if not condition:
        raise ValueError(explanation)


def command(record):
    return record.get("command") or " ".join(record.get("arguments", ()))


def object_path(build, row):
    # A compile database also contains targets that were CONFIGURED but never
    # actually built. Only count existing compiler outputs as tested source.
    argv = row.get("arguments") or shlex.split(command(row))
    require("-o" in argv, f"missing -o output in {row.get('file')}")
    index = argv.index("-o")
    require(index + 1 < len(argv), "incomplete -o compiler argument")
    output = Path(argv[index + 1])
    return output if output.is_absolute() else (
        Path(row.get("directory") or str(build)) / output)


def audited_compiles(build, component, flags):
    path = build / "compile_commands.json"
    require(path.is_file(), f"{component}: missing compile_commands.json")
    rows = json.loads(path.read_text(encoding="utf-8"))
    require(isinstance(rows, list) and rows, f"{component}: zero configured source units")
    missing = [row.get("file", "<unknown>") for row in rows
               if not all(flag in command(row) for flag in flags)]
    require(not missing, f"{component}: uninstrumented source units: {missing[:8]}")
    built = [r for r in rows if object_path(build, r).is_file()]
    require(built, f"{component}: zero actually built instrumented objects")
    for source in REQUIRED_SOURCES[component]:
        hits = [r for r in rows if
                r.get("file", "").replace("\\", "/").endswith("/" + source)]
        require(len(hits) == 1,
                f"{component}: expected exactly one configured unit {source}; got {len(hits)}")
        require(object_path(build, hits[0]).is_file(),
                f"{component}: required source not actually compiled {source}")
    if component in ("salts", "salts-utils"):
        # Independent PRIVATE Monocypher archives must have separate
        # visibility domains: no exported crypto_argon2_no_extras collision.
        # ASan ODR detection stays on; confirm the ACTUAL compiled TU flag.
        vendor = [row for row in rows if row.get("file", "").replace("\\", "/")
                  .endswith("/vendor/monocypher/monocypher.c")]
        require(len(vendor) == 1 and object_path(build, vendor[0]).is_file(),
                f"{component}: expected one built Monocypher vendor object")
        require("-fvisibility=hidden" in command(vendor[0]),
                f"{component}: Monocypher vendor symbol isolation missing")
    return {
        "configured_compile_commands": len(rows),
        "actually_built_object_units": len(built),
        "instrumented_built_units": len(built),
        "required_built_sources": list(REQUIRED_SOURCES[component]),
        "flags": list(flags),
    }


def cache_value(path, key):
    require(path.is_file(), f"missing CMake cache {path}")
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith(key + ":"):
            return line.partition("=")[2]
    raise ValueError(f"{path}: missing required {key}")


def audit_prefixes(root, builds):
    prefix = root / "prefix"
    roots = {
        "salts": prefix / "salts",
        "salts-utils": prefix / "salts-utils",
        "chttp": prefix / "chttp",
    }
    for name, installed in roots.items():
        require(installed.is_dir(), f"{name}: missing source-built install")
    bindings = (
        ("salts-utils", "Salts_DIR", roots["salts"]),
        ("chttp", "Salts_DIR", roots["salts"]),
        ("chttp", "SaltsUtils_DIR", roots["salts-utils"]),
        ("turboagent", "Salts_DIR", roots["salts"]),
        ("turboagent", "SaltsUtils_DIR", roots["salts-utils"]),
        ("turboagent", "Chttp_DIR", roots["chttp"]),
    )
    for component, key, installed in bindings:
        val = cache_value(builds[component] / "CMakeCache.txt", key)
        require(val.startswith(str(installed) + "/"),
                f"{component} {key}: imported wrong SDK root {val}")
    return {name: str(location) for name, location in roots.items()}


def audit_links(build, flags, prefixes):
    result = subprocess.run(
        ["ninja", "-C", str(build), "-t", "commands",
         "test_turboagent_typed_rpc"],
        check=True, capture_output=True, text=True,
    )
    lines = result.stdout.splitlines()
    exe = [line for line in lines if re.search(
        r"-o (?:[^ ]*/)?bin/test_turboagent_typed_rpc(?:\s|$)", line)]
    dylib = [line for line in lines
             if "-shared" in line and "turboagent_rpc_provider_dso" in line]
    require(len(exe) == 1 and all(f in exe[0] for f in flags),
            "typed RPC executable final link lacks sanitizer flags")
    require(len(dylib) == 1 and all(f in dylib[0] for f in flags),
            "real dynamically loaded Provider DSO link lacks sanitizer flags")
    # Reject an installed binary from a published SDK or unintended prefix:
    # imported Salts, SaltsUtils and CHttp native libraries must resolve to
    # THIS job's source-built prefixes, not merely an instrumented test host.
    exe_path = build / "bin/test_turboagent_typed_rpc"
    require(exe_path.is_file(), f"missing built test executable: {exe_path}")
    ld = subprocess.run(["ldd", str(exe_path)], check=True,
                        capture_output=True, text=True).stdout
    for name, prefix in prefixes.items():
        evidence = exe[0] + "\n" + ld
        require(prefix in evidence, f"{name}: test not linked to source-built prefix")
    require("not found" not in ld, "test executable has unresolved shared libraries")
    return {
        "typed_rpc_executable": str(exe_path),
        "executable_instrumented": True,
        "real_provider_dso_instrumented": True,
        "source_prefixes_resolved": list(prefixes),
        "ldd": ld,
    }


def main():
    require(len(sys.argv) == 3,
            "usage: audit_full_source_closure.py <graph-root> thread|address")
    root = Path(sys.argv[1]).resolve()
    kind = sys.argv[2]
    require(kind in ("thread", "address"), f"unknown sanitizer {kind}")
    flags = ("-fsanitize=thread",) if kind == "thread" else (
        "-fsanitize=address", "-fsanitize=undefined")
    builds = {name: root / ("build-" + name)
              for name in REQUIRED_SOURCES}
    source_report = {
        name: audited_compiles(directory, name, flags)
        for name, directory in builds.items()
    }
    prefixes = audit_prefixes(root, builds)
    links = audit_links(builds["turboagent"], flags, prefixes)
    report = {
        "status": "PASS",
        "coverage": "FULL_FIRST_PARTY_NATIVE_SOURCE_GRAPH",
        "sanitizer": kind,
        "configured_compiler_commands": sum(v["configured_compile_commands"]
                                             for v in source_report.values()),
        "actually_built_object_units": sum(v["actually_built_object_units"]
                                           for v in source_report.values()),
        "components": source_report,
        "links": links,
        "upstream_inputs": "exact_source_tags_not_released_nupkg",
    }
    print(json.dumps(report, indent=2, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as exc:
        print("FULL SOURCE SANITIZER GATE FAILED:", exc, file=sys.stderr)
        sys.exit(1)
