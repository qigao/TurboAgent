#ifndef TURBO_LLM_SANDBOX_GUEST_H
#define TURBO_LLM_SANDBOX_GUEST_H

/*
 * Guest-side ABI for sandboxed TurboAgent tool modules.
 *
 * Tool metadata, invocation arguments and results cross the Wasm boundary only
 * through the three bounded TurboAgent host imports below. The guest exports
 * remain ordinary core-Wasm functions; no TurboWasm-private guest facade is
 * required.
 */

#include <stdint.h>

#if defined(__clang__) || defined(__GNUC__)
  #define TURBO_LLM_SANDBOX_GUEST_EXPORT(name) __attribute__((export_name(name)))
  #define TURBO_LLM_SANDBOX_GUEST_IMPORT(module_name, import_name) \
    __attribute__((import_module(module_name), import_name(import_name)))
#else
  #define TURBO_LLM_SANDBOX_GUEST_EXPORT(name)
  #define TURBO_LLM_SANDBOX_GUEST_IMPORT(module_name, import_name)
#endif

#define TURBO_LLM_SANDBOX_EXPORT_TOOL_COUNT "turbo_tool_count"
#define TURBO_LLM_SANDBOX_EXPORT_TOOL_DESCRIBE "turbo_tool_describe"
#define TURBO_LLM_SANDBOX_EXPORT_TOOL_INVOKE "turbo_tool_invoke"

#define TURBO_LLM_SANDBOX_IMPORT_MODULE "turbo_agent"
#define TURBO_LLM_SANDBOX_IMPORT_INPUT_SIZE "tool_input_size"
#define TURBO_LLM_SANDBOX_IMPORT_INPUT_READ "tool_input_read"
#define TURBO_LLM_SANDBOX_IMPORT_OUTPUT_WRITE "tool_output_write"

TURBO_LLM_SANDBOX_GUEST_IMPORT(
    TURBO_LLM_SANDBOX_IMPORT_MODULE,
    TURBO_LLM_SANDBOX_IMPORT_INPUT_SIZE)
extern int32_t turbo_agent_tool_input_size(void);

TURBO_LLM_SANDBOX_GUEST_IMPORT(
    TURBO_LLM_SANDBOX_IMPORT_MODULE,
    TURBO_LLM_SANDBOX_IMPORT_INPUT_READ)
extern int32_t turbo_agent_tool_input_read_raw(
    int32_t offset, int32_t destination, int32_t capacity);

TURBO_LLM_SANDBOX_GUEST_IMPORT(
    TURBO_LLM_SANDBOX_IMPORT_MODULE,
    TURBO_LLM_SANDBOX_IMPORT_OUTPUT_WRITE)
extern int32_t turbo_agent_tool_output_write_raw(
    int32_t source, int32_t length);

static inline int32_t
turbo_agent_tool_input_read(uint32_t offset, void *destination, uint32_t capacity) {
  return turbo_agent_tool_input_read_raw(
      (int32_t)offset, (int32_t)(uintptr_t)destination, (int32_t)capacity);
}

static inline int32_t
turbo_agent_tool_output_write(const void *source, uint32_t length) {
  return turbo_agent_tool_output_write_raw(
      (int32_t)(uintptr_t)source, (int32_t)length);
}

#endif
