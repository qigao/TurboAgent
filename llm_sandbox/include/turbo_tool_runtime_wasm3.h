#ifndef TURBO_TOOL_RUNTIME_WASM3_H
#define TURBO_TOOL_RUNTIME_WASM3_H

#include <platform.h>

#include "turbo_tool_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_tool_runtime_wasm3_config_s {
  const char *module_path;
  const char *module_name;
  uint32_t stack_size;
  size_t socket_capacity;
  int enable_turbonet_host;
  int enable_http_host;
  int enable_redis_host;
} turbo_tool_runtime_wasm3_config_t;

/**
 * @brief Create a wasm3-backed tool runtime from a guest module file.
 *
 * Guest modules can include `turbo_llm_sandbox_guest.h` to use the canonical
 * export names and export attribute macro.
 *
 * Expected guest exports:
 * - `turbo_tool_count() -> i32`
 * - `turbo_tool_name(index) -> i32`
 * - `turbo_tool_description(index) -> i32`
 * - `turbo_tool_parameters(index) -> i32`
 * - `turbo_tool_strict(index) -> i32`
 * - `turbo_tool_input_ptr() -> i32`
 * - `turbo_tool_input_capacity() -> i32`
 * - `turbo_tool_output_ptr() -> i32`
 * - `turbo_tool_output_capacity() -> i32`
 * - `turbo_tool_invoke(index, input_len) -> i32`
 *
 * Pointer-returning metadata functions must return guest-memory offsets to
 * NUL-terminated UTF-8 strings. `turbo_tool_invoke(...)` should write output
 * bytes into the exported output buffer and return the output length, or a
 * negative value on failure.
 *
 * @param config Wasm runtime configuration.
 * @return Runtime handle or NULL on load/validation failure.
 */
CXX_C_API turbo_tool_runtime_t *
turbo_tool_runtime_wasm3_create(const turbo_tool_runtime_wasm3_config_t *config);

/**
 * @brief Create the default sandboxed tool runtime backend.
 *
 * The current default backend is wasm3.
 *
 * @param config Wasm3 runtime configuration.
 * @return Runtime handle or NULL on allocation or module-load failure.
 */
CXX_C_API turbo_tool_runtime_t *
turbo_tool_runtime_default_create(const turbo_tool_runtime_wasm3_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
