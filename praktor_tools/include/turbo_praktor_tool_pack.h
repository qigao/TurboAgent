#ifndef TURBO_PRAKTOR_TOOL_PACK_H
#define TURBO_PRAKTOR_TOOL_PACK_H

#include <turbo_agent_api.h>

#include "turbo_tool_registry.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_PRAKTOR_TOOL_PACK_ABI_VERSION 1u
#define TURBO_PRAKTOR_WORKFLOW_CONFIG_ABI_VERSION_V1 1u
#define TURBO_PRAKTOR_WORKFLOW_CONFIG_ABI_VERSION 2u

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
  /** Absolute path to one existing regular, non-symlink Praktor YAML workflow. */
  const char *workflow_path;
  /** JSON object schema for workflow inputs. NULL accepts any object. */
  const char *parameters_json;
  int strict;
  turbo_tool_execution_policy_t execution_policy;
  /**
   * Additional host-policy requirements.
   *
   * runtime_tools is always required. With WorkflowPlan support these entries
   * are additional requirements unioned with capabilities derived from the
   * plan effect manifest; they can never narrow discovered effects. Without
   * WorkflowPlan support NULL + zero falls back to the conservative legacy
   * network/shell/patch/outside_workspace set.
   */
  const char *const *required_capabilities;
  size_t required_capability_count;

  /**
   * Require WorkflowPlan metadata to qualify for profiles.harness_safe.
   *
   * v2 init enables this by default. v1 callers retain legacy path behavior.
   * When linked against a Praktor SDK without WorkflowPlan support the adapter
   * falls back to the legacy reviewed-path contract.
   */
  int require_harness_safe;
} turbo_praktor_workflow_config_t;

#define TURBO_PRAKTOR_WORKFLOW_CONFIG_V1_SIZE \
  offsetof(turbo_praktor_workflow_config_t, require_harness_safe)

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
 * workflow_path must be an existing regular non-symlink file. When the linked
 * Praktor exposes WorkflowPlan, registration compiles and owns an immutable
 * plan, consumes its generated input schema/effect manifest/profile metadata,
 * and later executes the bound plan. Legacy SDKs retain path-based execution.
 * The model never receives workflow_path. Duplicate names and capacity failures
 * are atomic.
 */
CXX_C_API turbo_tool_status_t turbo_praktor_tool_pack_add_workflow(
    turbo_praktor_tool_pack_t *pack,
    const turbo_praktor_workflow_config_t *config);

/** Borrowed registry. Do not mutate or destroy it; destroy dependent agents/projections before the pack. */
CXX_C_API turbo_tool_registry_t *
turbo_praktor_tool_pack_registry(turbo_praktor_tool_pack_t *pack);

CXX_C_API size_t
turbo_praktor_tool_pack_workflow_count(const turbo_praktor_tool_pack_t *pack);


/** Return whether the linked Praktor exposes immutable WorkflowPlan contracts. */
CXX_C_API int
turbo_praktor_tool_pack_supports_workflow_plan(
    const turbo_praktor_tool_pack_t *pack);

/** Return whether the linked Praktor exposes lifecycle event callbacks. */
CXX_C_API int
turbo_praktor_tool_pack_supports_execution_events(
    const turbo_praktor_tool_pack_t *pack);

#ifdef __cplusplus
}
#endif

#endif
