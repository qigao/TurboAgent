# TurboAgent-owned source protection inventory.
#
# This file enumerates real library/executable targets in the normal product
# graph, not synthetic consumers, tests or imported vcpkg/SDK dependencies.
# It is included after all the add_subdirectory() calls in root CMakeLists.
#
# It deliberately has no compiler/version/ABI/IR probe, no new build target,
# no fallback and no code scanners. We classify the code owner, not the final
# DLL/executable that links its precompiled machine code.
#
# Turn on with -DTURBOAGENT_PROTECTED_RELEASE=ON only when the downstream
# product Release workflow selects native Clang / Windows llvm-mingw and
# supplies the obfuscator-llvm plugin via its existing setup action.
if(NOT DEFINED ENV{QIGAO_OBFUSCATION_POLICY_CMAKE} OR
   "$ENV{QIGAO_OBFUSCATION_POLICY_CMAKE}" STREQUAL "")
  message(FATAL_ERROR
    "TurboAgent protected Release requires vcpkg-cache setup-llvm-native "
    "or setup-llvm-mingw to export QIGAO_OBFUSCATION_POLICY_CMAKE")
endif()
include("$ENV{QIGAO_OBFUSCATION_POLICY_CMAKE}")

# Proprietary behavior, tool authority, execution admission, graph state and
# local CLI knowledge/approval code: protect where these functions compile.
set(_turboagent_sensitive_targets
  agent_compiler
  runtime_tools
  turbo_runtime
  turbo_agent
  turbo_agent_harness
  turbo_cli_driver
)

# Generic runtime primitives, data containers, transport bridges and the thin
# executable entrypoint do not need global obfuscation. Their dependencies
# stay unaffected because the shared policy uses PRIVATE compile options.
set(_turboagent_ordinary_targets
  runtime_core
  runtime_data
  turbo_provider
  turboagent
)

if(ENABLE_MCP_TOOLS)
  list(APPEND _turboagent_ordinary_targets mcp_tools)
endif()
if(ENABLE_PRAKTOR_TOOLS)
  list(APPEND _turboagent_sensitive_targets praktor_tools)
endif()
if(ENABLE_CODEX_BRIDGE)
  list(APPEND _turboagent_ordinary_targets codex_bridge)
endif()
if(ENABLE_CODING_TOOLS)
  list(APPEND _turboagent_sensitive_targets coding_tools)
endif()
if(ENABLE_WASM_TOOLS)
  list(APPEND _turboagent_sensitive_targets wasm_tools)
endif()
if(ENABLE_SPEECH_INPUT)
  list(APPEND _turboagent_ordinary_targets turbo_agent_speech_input)
endif()
if(ENABLE_SPEECH_OUTPUT)
  list(APPEND _turboagent_ordinary_targets turbo_agent_speech_output)
endif()

qigao_apply_obfuscation_policy(
  SENSITIVE_TARGETS ${_turboagent_sensitive_targets}
  ORDINARY_TARGETS ${_turboagent_ordinary_targets}
)

unset(_turboagent_sensitive_targets)
unset(_turboagent_ordinary_targets)
