#ifndef TURBO_LLM_SANDBOX_GUEST_H
#define TURBO_LLM_SANDBOX_GUEST_H

/*
 * Guest-side ABI for sandboxed tool modules.
 *
 * A guest module exports a tool count, descriptor writer, and invoke function.
 * Metadata, arguments, and results use TurboWasm's bounded App I/O capability;
 * guest linear-memory pointers never cross the host API.
 */

#include <turbo_wasm_guest.h>

#if defined(__clang__) || defined(__GNUC__)
  #define TURBO_LLM_SANDBOX_GUEST_EXPORT(name) __attribute__((export_name(name)))
#else
  #define TURBO_LLM_SANDBOX_GUEST_EXPORT(name)
#endif

#define TURBO_LLM_SANDBOX_EXPORT_TOOL_COUNT "turbo_tool_count"
#define TURBO_LLM_SANDBOX_EXPORT_TOOL_DESCRIBE "turbo_tool_describe"
#define TURBO_LLM_SANDBOX_EXPORT_TOOL_INVOKE "turbo_tool_invoke"

#endif
