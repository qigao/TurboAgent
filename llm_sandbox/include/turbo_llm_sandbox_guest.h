#ifndef TURBO_LLM_SANDBOX_GUEST_H
#define TURBO_LLM_SANDBOX_GUEST_H

/*
 * Guest-side ABI for sandboxed tool modules.
 *
 * A guest module exports metadata functions for each tool plus one invoke
 * function. Pointer-returning functions return guest-memory offsets to
 * NUL-terminated UTF-8 strings or buffers.
 */

#if defined(__clang__) || defined(__GNUC__)
#define TURBO_LLM_SANDBOX_GUEST_EXPORT(name) __attribute__((export_name(name)))
#else
#define TURBO_LLM_SANDBOX_GUEST_EXPORT(name)
#endif

#define TURBO_LLM_SANDBOX_EXPORT_TOOL_COUNT "turbo_tool_count"
#define TURBO_LLM_SANDBOX_EXPORT_TOOL_NAME "turbo_tool_name"
#define TURBO_LLM_SANDBOX_EXPORT_TOOL_DESCRIPTION "turbo_tool_description"
#define TURBO_LLM_SANDBOX_EXPORT_TOOL_PARAMETERS "turbo_tool_parameters"
#define TURBO_LLM_SANDBOX_EXPORT_TOOL_STRICT "turbo_tool_strict"
#define TURBO_LLM_SANDBOX_EXPORT_TOOL_INPUT_PTR "turbo_tool_input_ptr"
#define TURBO_LLM_SANDBOX_EXPORT_TOOL_INPUT_CAPACITY "turbo_tool_input_capacity"
#define TURBO_LLM_SANDBOX_EXPORT_TOOL_OUTPUT_PTR "turbo_tool_output_ptr"
#define TURBO_LLM_SANDBOX_EXPORT_TOOL_OUTPUT_CAPACITY "turbo_tool_output_capacity"
#define TURBO_LLM_SANDBOX_EXPORT_TOOL_INVOKE "turbo_tool_invoke"

#endif
