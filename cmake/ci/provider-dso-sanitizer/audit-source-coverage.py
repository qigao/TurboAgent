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
    agent / "cmake/ci/provider-dso-sanitizer/close_fault_hook.c",
    salts / "component-plugin/src/component_plugin.c",
    salts / "component-plugin/src/component_plugin_runtime.c",
    salts / "component/src/component.c",
    salts / "platform/src/thread_posix.c",
    salts / "platform/src/clock.c",
    agent / "runtime/tests/test_turbo_component_dso_lifecycle.c",
    agent / "cmake/ci/provider-dso-sanitizer/test_provider_dso_close_failure.c",
    agent / "cmake/ci/provider-dso-sanitizer/test_provider_registry_destroy_barrier.c",
    agent / "cmake/ci/provider-dso-sanitizer/test_provider_registry_partial_destroy.c",
    agent / "cmake/ci/provider-dso-sanitizer/test_provider_registry_repeated_close_failures.c",
    agent / "cmake/ci/provider-dso-sanitizer/test_provider_generation_drain_close_failure.c",
    agent / "cmake/ci/provider-dso-sanitizer/test_provider_multigeneration_isolated_drain.c",
    agent / "praktor_tools/tests/praktor_dso_provider.c",
    agent / "praktor_tools/tests/praktor_dso_managed_provider.c",
]
commands = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
for source in expected:
    matching = [
        row for row in commands
        if pathlib.Path(row["file"]).resolve() == source
    ]
    minimum = 4 if source.name == "praktor_dso_provider.c" else 1
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
    build / "libprovider_dso_instrumented_managed.so",
    build / "libprovider_dso_instrumented_managed_component.so",
    build / "libprovider_dso_instrumented_managed_component_n1.so",
    build / "test_turbo_component_dso_sanitized",
    build / "test_provider_dso_close_failure",
    build / "test_provider_registry_destroy_barrier",
    build / "test_provider_registry_partial_destroy",
    build / "test_provider_registry_repeated_close_failures",
    build / "test_provider_generation_drain_close_failure",
    build / "test_provider_multigeneration_isolated_drain",
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
    ["nm", "--defined-only", str(build / "test_turbo_component_dso_sanitized")],
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
    "cmeta_plugin_registry_release_real",
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

# Verify both source-owned loader endpoints and the test-only failpoint
# reached the fault-test image; no substituted uninstrumented host loader.
close_test_map = build / "provider_dso_close_failure.map"
if not close_test_map.is_file():
    raise SystemExit("FAIL: missing fault-close test linker map")
close_map_text = close_test_map.read_text(encoding="utf-8", errors="replace")
for item in ("plugin_posix.c.o", "close_fault_hook.c.o"):
    if not re.search(
        r"libprovider_dso_firstparty_runtime\.a\(" + re.escape(item) + r"\)",
        close_map_text,
    ):
        raise SystemExit(f"FAIL: fault-close image lacks instrumented {item}")
    print(f"FAULT-CLOSE LINKED: {item}")
close_symbols = subprocess.run(
    ["nm", "--defined-only", str(build / "test_provider_dso_close_failure")],
    capture_output=True, text=True, check=True,
).stdout
for entry in (
    "cmeta_plugin_platform_close",
    "cmeta_plugin_platform_close_real",
    "provider_close_fault_arm",
):
    if not re.search(r"(?m)^\S+\s+[TW]\s+" + entry + r"$", close_symbols):
        raise SystemExit(f"FAIL: missing source-owned loader endpoint {entry}")
    print(f"FAULT-CLOSE DEFINED: {entry}")


# Integrated source-owned Generation drain failure must call a test shim,
# then the genuine Registry lease-release function; the Managed Component
# Provider DSO must also contain sanitizer instrumentation.
integrated_map = build / "provider_generation_drain_close_failure.map"
if not integrated_map.is_file():
    raise SystemExit("FAIL: no integrated drain+close linker map")
integrated_map_text = integrated_map.read_text(
    encoding="utf-8", errors="replace"
)
for member in ("plugin_registry.c.o", "component_plugin.c.o",
               "plugin_posix.c.o", "close_fault_hook.c.o"):
    if not re.search(
        r"libprovider_dso_firstparty_runtime\.a\(" + re.escape(member) + r"\)",
        integrated_map_text,
    ):
        raise SystemExit(f"FAIL: integrated image lacks instrumented {member}")
    print(f"INTEGRATED SOURCE LINKED: {member}")
integrated_symbols = subprocess.run(
    ["nm", "--defined-only",
     str(build / "test_provider_generation_drain_close_failure")],
    capture_output=True, text=True, check=True,
).stdout
for endpoint in (
    "cmeta_plugin_registry_release",
    "cmeta_plugin_registry_release_real",
    "provider_release_fault_arm",
    "salts_component_plugin_generation_drain",
    "cmeta_plugin_platform_close",
    "cmeta_plugin_platform_close_real",
):
    if not re.search(r"(?m)^\S+\s+[TW]\s+" + endpoint + r"$",
                     integrated_symbols):
        raise SystemExit(f"FAIL: integrated image missing {endpoint}")
    print(f"INTEGRATED SOURCE DEFINED: {endpoint}")


# Parallel drain must select the exact pinned Salts ComponentPlugin and Plugin
# implementations plus the scoped-release fault shim, not SDK static fallbacks.
multi_map = build / "provider_multigeneration_isolated_drain.map"
if not multi_map.is_file():
    raise SystemExit("FAIL: missing multi-Generation drain linker map")
multi_text = multi_map.read_text(encoding="utf-8", errors="replace")
for member in ("component_plugin.c.o", "plugin_registry.c.o",
               "plugin_posix.c.o", "close_fault_hook.c.o"):
    if not re.search(
        r"libprovider_dso_firstparty_runtime\.a\(" + re.escape(member) + r"\)",
        multi_text,
    ):
        raise SystemExit(f"FAIL: multi-Generation image lacks instrumented {member}")
    print(f"MULTI-GENERATION SOURCE LINKED: {member}")
multi_symbols = subprocess.run(
    ["nm", "--defined-only",
     str(build / "test_provider_multigeneration_isolated_drain")],
    capture_output=True, text=True, check=True,
).stdout
for endpoint in ("salts_component_plugin_generation_drain",
                 "cmeta_plugin_registry_release",
                 "cmeta_plugin_registry_release_real",
                 "provider_release_fault_arm_plugin",
                 "cmeta_plugin_platform_close",
                 "cmeta_plugin_platform_close_real"):
    if not re.search(r"(?m)^\S+\s+[TW]\s+" + endpoint + r"$",
                     multi_symbols):
        raise SystemExit(f"FAIL: multi-Generation image missing {endpoint}")
    print(f"MULTI-GENERATION SOURCE DEFINED: {endpoint}")


# The dedicated Registry destruction barrier must use the SAME first-party
# Registry, real native loader and test-only platform close fault implementation.
destroy_map = build / "provider_registry_destroy_barrier.map"
if not destroy_map.is_file():
    raise SystemExit("FAIL: missing Registry destruction barrier linker map")
destroy_text = destroy_map.read_text(encoding="utf-8", errors="replace")
for member in ("plugin_registry.c.o", "plugin_posix.c.o", "close_fault_hook.c.o"):
    if not re.search(
        r"libprovider_dso_firstparty_runtime\.a\(" + re.escape(member) + r"\)",
        destroy_text,
    ):
        raise SystemExit(f"FAIL: Registry destroy executable lacks {member}")
    print(f"REGISTRY DESTROY LINKED: {member}")
destroy_symbols = subprocess.run(
    ["nm", "--defined-only", str(build / "test_provider_registry_destroy_barrier")],
    capture_output=True, text=True, check=True,
).stdout
for entry in (
    "cmeta_plugin_registry_destroy",
    "cmeta_plugin_registry_acquire",
    "cmeta_plugin_registry_release",
    "cmeta_plugin_registry_release_real",
    "cmeta_plugin_platform_close",
    "cmeta_plugin_platform_close_real",
    "provider_close_fault_arm",
):
    if not re.search(r"(?m)^\S+\s+[TW]\s+" + entry + r"$", destroy_symbols):
        raise SystemExit(f"FAIL: Registry destroy executable missing {entry}")
    print(f"REGISTRY DESTROY DEFINED: {entry}")


# Partial Registry teardown must link the exact instrumented first-party
# Registry and POSIX loader plus the controlled close failure boundary.
partial_map = build / "provider_registry_partial_destroy.map"
if not partial_map.is_file():
    raise SystemExit("FAIL: partial Registry destroy linker map missing")
partial_link = partial_map.read_text(encoding="utf-8", errors="replace")
for member in ("plugin_registry.c.o", "plugin_posix.c.o", "close_fault_hook.c.o"):
    if not re.search(
        r"libprovider_dso_firstparty_runtime\.a\(" + re.escape(member) + r"\)",
        partial_link
    ):
        raise SystemExit(f"FAIL: partial Registry destroy lacks instrumented {member}")
    print(f"PARTIAL DESTROY LINKED: {member}")
partial_symbols = subprocess.run(
    ["nm", "--defined-only", str(build / "test_provider_registry_partial_destroy")],
    capture_output=True, text=True, check=True,
).stdout
for endpoint in (
    "cmeta_plugin_registry_destroy",
    "cmeta_plugin_registry_load",
    "cmeta_plugin_registry_acquire",
    "cmeta_plugin_platform_close",
    "cmeta_plugin_platform_close_real",
    "provider_close_fault_arm_after",
):
    if not re.search(r"(?m)^\S+\s+[TW]\s+" + endpoint + r"$",
                     partial_symbols):
        raise SystemExit(f"FAIL: partial destroy image lacks {endpoint}")
    print(f"PARTIAL DESTROY DEFINED: {endpoint}")


# Repeated close failures must exercise the source-backed Registry and
# authentic native loader through the test-only deterministic schedule shim.
repeated_map = build / "provider_registry_repeated_close_failures.map"
if not repeated_map.is_file():
    raise SystemExit("FAIL: repeated Registry close linker map missing")
repeated_link = repeated_map.read_text(encoding="utf-8", errors="replace")
for member in ("plugin_registry.c.o", "plugin_posix.c.o", "close_fault_hook.c.o"):
    if not re.search(
        r"libprovider_dso_firstparty_runtime\.a\(" + re.escape(member) + r"\)",
        repeated_link,
    ):
        raise SystemExit(f"FAIL: repeated destroy image lacks instrumented {member}")
    print(f"REPEATED CLOSE SOURCE LINKED: {member}")
repeated_symbols = subprocess.run(
    ["nm", "--defined-only",
     str(build / "test_provider_registry_repeated_close_failures")],
    capture_output=True, text=True, check=True,
).stdout
for endpoint in ("cmeta_plugin_registry_destroy", "cmeta_plugin_registry_load",
                 "cmeta_plugin_registry_acquire", "cmeta_plugin_platform_close",
                 "cmeta_plugin_platform_close_real",
                 "provider_close_fault_arm_sequence",
                 "provider_close_fault_sequence_calls"):
    if not re.search(r"(?m)^\S+\s+[TW]\s+" + endpoint + r"$",
                     repeated_symbols):
        raise SystemExit(f"FAIL: repeated close image lacks {endpoint}")
    print(f"REPEATED CLOSE SOURCE DEFINED: {endpoint}")

print(f"PASS: {kind} first-party objects, Sanitizer symbols, extracted-archive audit")
