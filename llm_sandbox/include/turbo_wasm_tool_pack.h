#ifndef TURBO_WASM_TOOL_PACK_H
#define TURBO_WASM_TOOL_PACK_H

#include <platform.h>

#include "turbo_tool_registry.h"
#include "turbo_tool_runtime_wasm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_WASM_TOOL_PACK_ABI_VERSION 1u

typedef struct turbo_wasm_tool_pack_s turbo_wasm_tool_pack_t;

typedef struct turbo_wasm_tool_pack_config_s {
  uint32_t struct_size;
  uint32_t abi_version;
  /** Hard upper bound for successfully registered modules. */
  size_t max_modules;
  /** Hard upper bound for tools across every registered module. */
  size_t max_tools;
} turbo_wasm_tool_pack_config_t;

typedef struct turbo_wasm_tool_pack_module_config_s {
  uint32_t struct_size;
  uint32_t abi_version;
  turbo_tool_runtime_wasm_config_t runtime;
  turbo_tool_execution_policy_t execution_policy;
} turbo_wasm_tool_pack_module_config_t;

/**
 * Initialize a versioned pack configuration with bounded defaults.
 * @param config Configuration to initialize; ignored when NULL.
 */
CXX_C_API void turbo_wasm_tool_pack_config_init(turbo_wasm_tool_pack_config_t *config);

/**
 * Initialize one module configuration with sequential, non-idempotent execution.
 * @param config Module configuration to initialize; ignored when NULL.
 */
CXX_C_API void
turbo_wasm_tool_pack_module_config_init(turbo_wasm_tool_pack_module_config_t *config);

/**
 * Create an empty pack that owns its unified tool registry.
 * @param config Initialized, versioned pack configuration.
 * @return Owned pack, or NULL for invalid configuration or allocation failure.
 */
CXX_C_API turbo_wasm_tool_pack_t *
turbo_wasm_tool_pack_create(const turbo_wasm_tool_pack_config_t *config);

/**
 * Destroy the registry, all bindings, and their retained TurboWasm runtimes.
 * @param pack Owned pack; may be NULL.
 */
CXX_C_API void turbo_wasm_tool_pack_destroy(turbo_wasm_tool_pack_t *pack);

/**
 * Load and atomically register one TurboWasm module.
 *
 * The module must export at least one tool. Duplicate names and capacity
 * failures leave the prior pack unchanged. The TurboWasm policy is borrowed
 * only during this call and is cloned by the created VM. Current TurboWasm
 * VMs are not reentrant, so PARALLEL_SAFE is rejected.
 *
 * This is a single-owner control-plane operation. Do not call it concurrently
 * with registry projection or tool execution.
 *
 * @param pack Destination pack.
 * @param config Module path, TurboWasm policy, host limits, and tool policy.
 * @return OK on commit; INVALID_ARGUMENT for an invalid ABI or policy,
 * BACKPRESSURE for a pack capacity limit, DUPLICATE for a name collision,
 * OUT_OF_MEMORY for bridge allocation failure, or ERROR for module/guest ABI,
 * schema, policy, or empty-catalog rejection.
 */
CXX_C_API turbo_tool_status_t turbo_wasm_tool_pack_add_module(
    turbo_wasm_tool_pack_t *pack, const turbo_wasm_tool_pack_module_config_t *config);

/**
 * Return the pack-owned registry.
 *
 * The pointer remains valid until pack destruction. Callers must not mutate or
 * destroy it. Any agent or projected registry borrowing callbacks from it must
 * be destroyed before the pack. Direct callers must serialize access; the
 * Agent tool executor honors each registered execution policy.
 *
 * @param pack Pack handle.
 * @return Borrowed registry, or NULL when pack is NULL.
 */
CXX_C_API turbo_tool_registry_t *turbo_wasm_tool_pack_registry(turbo_wasm_tool_pack_t *pack);

/** @return Number of successfully committed modules, or zero for NULL. */
CXX_C_API size_t turbo_wasm_tool_pack_module_count(const turbo_wasm_tool_pack_t *pack);
/** @return Number of registered tools, or zero for NULL. */
CXX_C_API size_t turbo_wasm_tool_pack_tool_count(const turbo_wasm_tool_pack_t *pack);

#ifdef __cplusplus
}
#endif

#endif
