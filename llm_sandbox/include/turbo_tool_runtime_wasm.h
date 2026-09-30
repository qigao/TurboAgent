#ifndef TURBO_TOOL_RUNTIME_WASM_H
#define TURBO_TOOL_RUNTIME_WASM_H

#include <turbo_agent_api.h>
#include <turbowasm/turbowasm.h>

#include "turbo_tool_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_TOOL_RUNTIME_WASM_ABI_VERSION 2u

typedef struct turbo_tool_runtime_wasm_config_s {
  uint32_t struct_size;
  uint32_t abi_version;

  /**
   * Host-selected Wasm module file.
   *
   * The runtime reads and owns the module bytes during create. Model-originated
   * data never selects this path.
   */
  const char *module_path;

  size_t max_tools;
  size_t max_metadata_bytes;
  size_t max_input_bytes;
  size_t max_output_bytes;

  /** TurboWasm Runtime resource limits. All are enforced before/while execution. */
  size_t max_module_bytes;
  size_t max_allocation_bytes;
  size_t max_linear_memory_bytes;
  uint32_t max_table_elements;

  /** Per-export invocation fuel budget. Zero disables the fuel limit. */
  uint64_t fuel_per_call;
} turbo_tool_runtime_wasm_config_t;

/** Initialize a versioned configuration with bounded sandbox defaults. */
CXX_C_API void turbo_tool_runtime_wasm_config_init(turbo_tool_runtime_wasm_config_t *config);

/**
 * Create a RuntimeTools backend over the current public TurboWasm Runtime API.
 *
 * The guest is a core Wasm module. It receives only the explicit TurboAgent
 * host-import namespace below; no filesystem/network/WASI capability is linked
 * implicitly.
 *
 * Required guest exports:
 * - turbo_tool_count() -> i32
 * - turbo_tool_describe(index: i32) -> i32
 * - turbo_tool_invoke(index: i32) -> i32
 *
 * Allowed host imports from module "turbo_agent":
 * - tool_input_size() -> i32
 * - tool_input_read(offset: i32, destination: i32, capacity: i32) -> i32
 * - tool_output_write(source: i32, length: i32) -> i32
 *
 * Describe writes one UTF-8 JSON descriptor through tool_output_write. Invoke
 * reads JSON arguments through the bounded input imports and writes one JSON
 * result through tool_output_write. Both return zero on success.
 *
 * RuntimeTools v2 cancellation/deadline is projected into
 * turbowasm_execution_options.should_interrupt. TurboWasm Runtime owns Wasm
 * validation, traps, fuel and resource enforcement.
 */
CXX_C_API turbo_tool_runtime_t *
turbo_tool_runtime_wasm_create(const turbo_tool_runtime_wasm_config_t *config);

/**
 * Create the same runtime and optionally return caller-owned descriptive
 * execution metadata derived from the exact module bytes retained by the
 * runtime. The metadata contains no filesystem path and grants no authority.
 */
CXX_C_API turbo_tool_runtime_t *
turbo_tool_runtime_wasm_create_with_metadata(
    const turbo_tool_runtime_wasm_config_t *config,
    json_value_t **out_execution_metadata);

#ifdef __cplusplus
}
#endif

#endif
