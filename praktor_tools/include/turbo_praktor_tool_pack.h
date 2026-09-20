#ifndef TURBO_PRAKTOR_TOOL_PACK_H
#define TURBO_PRAKTOR_TOOL_PACK_H

#include <platform.h>

#include "turbo_tool_registry.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_PRAKTOR_TOOL_PACK_ABI_VERSION 1u
#define TURBO_PRAKTOR_WORKFLOW_CONFIG_ABI_VERSION 1u

typedef struct turbo_praktor_tool_pack_s turbo_praktor_tool_pack_t;

typedef struct turbo_praktor_tool_pack_config_s {
  uint32_t struct_size;
  uint32_t abi_version;
  /** Hard upper bound for registered workflows. */
  size_t max_workflows;
  /** Maximum canonical Praktor JSON result accepted by the adapter. */
  size_t max_result_bytes;
} turbo_praktor_tool_pack_config_t;

typedef struct turbo_praktor_workflow_config_s {
  uint32_t struct_size;
  uint32_t abi_version;
  /** Stable TurboAgent tool name. The model never receives workflow_path. */
  const char *tool_name;
  /** Human-facing tool description copied by the registry. */
  const char *description;
  /** Absolute, host-registered path to one trusted Praktor YAML workflow. */
  const char *workflow_path;
  /** JSON object schema for workflow inputs. NULL accepts any object. */
  const char *parameters_json;
  int strict;
  turbo_tool_execution_policy_t execution_policy;
  /**
   * Additional host-policy requirements.
   *
   * runtime_tools is always required. Defaults are deliberately conservative:
   * network, shell, patch, and outside_workspace. A host may replace this list
   * only after reviewing the registered workflow's effects.
   */
  const char *const *required_capabilities;
  size_t required_capability_count;
} turbo_praktor_workflow_config_t;

CXX_C_API void
turbo_praktor_tool_pack_config_init(turbo_praktor_tool_pack_config_t *config);

CXX_C_API void
turbo_praktor_workflow_config_init(turbo_praktor_workflow_config_t *config);

/**
 * Create an empty pack bound to the linked Praktor C ABI.
 *
 * Creation fails when the linked ABI major/capability contract is incompatible.
 */
CXX_C_API turbo_praktor_tool_pack_t *
turbo_praktor_tool_pack_create(const turbo_praktor_tool_pack_config_t *config);

CXX_C_API void turbo_praktor_tool_pack_destroy(turbo_praktor_tool_pack_t *pack);

/**
 * Register one trusted workflow as one TurboAgent tool.
 *
 * workflow_path must be absolute and is copied into the pack-owned binding.
 * The model sees only tool_name, description, parameters_json, and later the
 * canonical workflow result. Duplicate names and capacity failures are atomic.
 */
CXX_C_API turbo_tool_status_t turbo_praktor_tool_pack_add_workflow(
    turbo_praktor_tool_pack_t *pack,
    const turbo_praktor_workflow_config_t *config);

/** Borrowed registry. Do not mutate or destroy it; destroy dependent agents/projections before the pack. */
CXX_C_API turbo_tool_registry_t *
turbo_praktor_tool_pack_registry(turbo_praktor_tool_pack_t *pack);

CXX_C_API size_t
turbo_praktor_tool_pack_workflow_count(const turbo_praktor_tool_pack_t *pack);

#ifdef __cplusplus
}
#endif

#endif
