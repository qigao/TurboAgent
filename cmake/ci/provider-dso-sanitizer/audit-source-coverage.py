#!/usr/bin/env python3
"""Fail closed if the sanitizer binary only instruments the test facade.

Only the listed first-party Salts source files and TurboAgent DSO harness are
claimed covered. Released Salts Core, CMeta and TinyTest are external.
"""
import json
import pathlib
import re
import subprocess
import sys

if len(sys.argv) != 5:
    raise SystemExit("usage: audit-source-coverage.py <build> <salts-source> <agent-source> <thread|address>")
build = pathlib.Path(sys.argv[1]).resolve()
salts = pathlib.Path(sys.argv[2]).resolve()
agent = pathlib.Path(sys.argv[3]).resolve()
kind = sys.argv[4]
if kind not in {"thread", "address"}:
    raise SystemExit("Unsupported sanitizer")
flag = "-fsanitize=thread" if kind == "thread" else "-fsanitize=address,undefined"
expected = [
    salts / "cmeta/src/interface.c",
    salts / "cmeta/src/component.c",
    salts / "cmeta/src/object.c",
    salts / "cmeta/src/data.c",
    salts / "cmeta/src/function.c",
    salts / "utils/src/vstr.c",
    salts / "plugin/src/plugin.c",
    salts / "plugin/src/plugin_registry.c",
    salts / "plugin/src/plugin_posix.c",
    salts / "component-plugin/src/component_plugin.c",
    salts / "component-plugin/src/component_plugin_runtime.c",
    salts / "component/src/component.c",
    salts / "platform/src/thread_posix.c",
    salts / "platform/src/clock.c",
    agent / "runtime/tests/test_turbo_component_dso_lifecycle.c",
    agent / "praktor_tools/tests/praktor_dso_provider.c",
]
commands = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
for source in expected:
    matching = [
        row for row in commands
        if pathlib.Path(row["file"]).resolve() == source
    ]
    minimum = 2 if source.name == "praktor_dso_provider.c" else 1
    if len(matching) != minimum:
        raise SystemExit(f"FAIL: {source} has {len(matching)} objects, expected {minimum}")
    for row in matching:
        command = row.get("command") or " ".join(row.get("arguments", []))
        if flag not in command or "-fno-omit-frame-pointer" not in command:
            raise SystemExit(f"FAIL: uninstrumented first-party object {source}\n{command}")
    print(f"INSTRUMENTED x{len(matching)}: {source}")

libs = [
    build / "libprovider_dso_firstparty_runtime.a",
    build / "libprovider_dso_instrumented_n.so",
    build / "libprovider_dso_instrumented_n1.so",
    build / "test_turbo_component_dso_sanitized",
]
symbol = "__tsan_" if kind == "thread" else "__asan_"
for library in libs:
    if not library.is_file():
        raise SystemExit(f"FAIL: missing output {library}")
    nm = subprocess.run(
        ["nm", "-A", str(library)], capture_output=True, text=True, check=True
    )
    if symbol not in nm.stdout:
        raise SystemExit(f"FAIL: {library} does not contain {symbol} instrumentation")
    print(f"INSTRUMENTED SYMBOLS: {library.name}")

link_map = build / "provider_dso_sanitized.map"
if not link_map.is_file():
    raise SystemExit("FAIL: no linker map for sanitizer executable")
link_data = link_map.read_text(encoding="utf-8", errors="replace")

# GNU ld lists unused command-line archives under LOAD. Only
# libarchive.a(member.o) means a released uninstrumented object was extracted.
for banned in [
    "libcmeta_plugin.a",
    "libsalts_component_plugin.a",
    "libsalts_component.a",
]:
    if re.search(re.escape(banned) + r"\([^)]*\)", link_data):
        raise SystemExit(f"FAIL: extracted uninstrumented host archive member from {banned}")
# CMeta implementation in this slice must be chosen from instrumented
# sources, rather than the uninstrumented released Salts::CMeta archive.
for owner in ("interface", "component", "object", "data", "function"):
    if re.search(r"libcmeta_cmeta\.a\(" + owner + r"\.c\.o\)", link_data):
        raise SystemExit(f"FAIL: pulled released uninstrumented CMeta {owner}.c.o")
if re.search(r"libcmeta_platform\.a\((?:thread_posix|clock)\.c\.o\)", link_data):
    raise SystemExit("FAIL: pulled uninstrumented POSIX thread/clock implementation")

# CMeta's two newly admitted source objects must actually be extracted
# into the native test image. Merely compiling an unused object is insufficient.
for member in ("data.c.o", "function.c.o", "vstr.c.o"):
    pattern = r"libprovider_dso_firstparty_runtime\.a\(" + re.escape(member) + r"\)"
    if not re.search(pattern, link_data):
        raise SystemExit(f"FAIL: instrumented CMeta {member} was not linked")
    print(f"SOURCE LINKED: {member}")

# The linked executable must own Core UTF-8 validation, not just define it
# in an unused archive. The symbol appears in the non-stripped native binary.
binary_symbols = subprocess.run(
    ["nm", "--defined-only", str(libs[-1])],
    capture_output=True, text=True, check=True
).stdout
for symbol_name in ("vstr_utf8_valid", "vstr_utf8_invalid_offset"):
    if not re.search(r"(?m)^\S+\s+[TW]\s+" + symbol_name + r"$", binary_symbols):
        raise SystemExit(f"FAIL: Core UTF-8 {symbol_name} not defined in sanitizer executable")
    print(f"EXECUTABLE DEFINED: {symbol_name}")

# The actual native entrypoints must be defined by the instrumented archive.
archive = subprocess.run(
    ["nm", "--defined-only", str(libs[0])],
    capture_output=True, text=True, check=True
).stdout
for function in [
    "cmeta_plugin_registry_unload",
    "salts_component_plugin_generation_drain",
    "salts_component_plugin_scope_release",
    "cmeta_mutex_lock",
    "cmeta_monotonic_ms",
    "cmeta_interface_desc_equal",
    "cmeta_component_desc_valid",
    "cmeta_object_borrow",
    "cmeta_data_desc_valid",
    "cmeta_data_desc_equal",
    "cmeta_function_desc_valid",
    "cmeta_function_abi_desc_valid",
    "vstr_utf8_valid",
    "vstr_utf8_invalid_offset",
]:
    if not re.search(r"(?m)^\S+\s+[TW]\s+" + re.escape(function) + r"$", archive):
        raise SystemExit(f"FAIL: no source-defined {function} in instrumented archive")
    print(f"SOURCE DEFINED: {function}")
print(f"PASS: {kind} first-party objects, Sanitizer symbols, extracted-archive audit")
