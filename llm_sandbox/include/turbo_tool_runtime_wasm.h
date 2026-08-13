#ifndef TURBO_TOOL_RUNTIME_WASM_H
#define TURBO_TOOL_RUNTIME_WASM_H

#include <platform.h>
#include <turbo_wasm.h>

#include "turbo_tool_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_TOOL_RUNTIME_WASM_ABI_VERSION 1u

typedef struct turbo_tool_runtime_wasm_config_s {
  uint32_t struct_size;
  uint32_t abi_version;
  /** Relative module path resolved beneath the module root in policy. */
  const char *module_path;
  /** Borrowed during create; TurboWasm clones and freezes it for the VM. */
  const turbo_wasm_policy_t *policy;
  size_t max_tools;
  size_t max_metadata_bytes;
  size_t max_input_bytes;
  size_t max_output_bytes;
} turbo_tool_runtime_wasm_config_t;

/** Initialize a versioned configuration with bounded host-side defaults. */
CXX_C_API void turbo_tool_runtime_wasm_config_init(turbo_tool_runtime_wasm_config_t *config);

/**
 * Create a TurboWasm-backed sandboxed tool runtime.
 *
 * `policy` is the sole capability and VM quota source. It must grant
 * `TURBO_WASM_CAP_APP`, configure a module root, and remain valid only for the
 * duration of this call. TurboWasm clones it before returning.
 *
 * Guest ABI v1 exports:
 * - `turbo_tool_count() -> i32`
 * - `turbo_tool_describe(index: i32) -> i32`
 * - `turbo_tool_invoke(index: i32) -> i32`
 *
 * Describe writes one UTF-8 JSON descriptor to TurboWasm stdout. Invoke reads
 * JSON arguments through TurboWasm app input and writes one JSON result to
 * stdout. Both return zero on success. No guest pointer crosses this API.
 */
CXX_C_API turbo_tool_runtime_t *
turbo_tool_runtime_wasm_create(const turbo_tool_runtime_wasm_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
