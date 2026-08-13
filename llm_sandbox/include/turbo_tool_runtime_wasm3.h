#ifndef TURBO_TOOL_RUNTIME_WASM3_H
#define TURBO_TOOL_RUNTIME_WASM3_H

#include <platform.h>

#include "turbo_tool_runtime_wasm.h"

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
 * @brief Create a TurboWasm-backed tool runtime using the legacy config.
 *
 * This source-compatibility entry derives a least-privilege TurboWasm policy
 * from module_path. Broad legacy TurboNet/HTTP/Redis host switches are rejected;
 * use `turbo_tool_runtime_wasm_create()` with an explicit TurboWasm policy.
 * Guest modules must implement the pointer-free ABI documented there.
 *
 * @param config Legacy runtime configuration.
 * @return Runtime handle or NULL on load/validation failure.
 */
CXX_C_API turbo_tool_runtime_t *
turbo_tool_runtime_wasm3_create(const turbo_tool_runtime_wasm3_config_t *config);

/**
 * @brief Create the default sandboxed tool runtime backend.
 *
 * The current default backend is TurboWasm.
 *
 * @param config Legacy runtime configuration.
 * @return Runtime handle or NULL on allocation or module-load failure.
 */
CXX_C_API turbo_tool_runtime_t *
turbo_tool_runtime_default_create(const turbo_tool_runtime_wasm3_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
