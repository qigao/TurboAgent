#ifndef TURBO_AGENT_POLICY_H
#define TURBO_AGENT_POLICY_H

#include <turbo_agent_api.h>
#include <stdint.h>

#include "turbo_action_tool.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_s turbo_agent_t;
typedef struct turbo_tool_registry_s turbo_tool_registry_t;

typedef struct turbo_agent_policy_s {
  const char *workspace_root;
  int auto_approve_safe;
  int allow_shell;
  int allow_patch;
  int allow_custom_tools;
  int allow_runtime_tools;
  int allow_delegate;
  int allow_network;
  int allow_outside_workspace;
  unsigned max_action_retries;
  unsigned max_steps;
  unsigned max_replans;
} turbo_agent_policy_t;

typedef enum {
  TURBO_AGENT_POLICY_ALLOW = 0,
  TURBO_AGENT_POLICY_REQUIRE_APPROVAL = 1,
  TURBO_AGENT_POLICY_DENY = 2
} turbo_agent_policy_decision_t;

typedef enum {
  TURBO_AGENT_POLICY_CAPABILITY_CUSTOM_TOOLS = 0,
  TURBO_AGENT_POLICY_CAPABILITY_RUNTIME_TOOLS = 1,
  TURBO_AGENT_POLICY_CAPABILITY_DELEGATE = 2,
  TURBO_AGENT_POLICY_CAPABILITY_NETWORK = 3,
  TURBO_AGENT_POLICY_CAPABILITY_SHELL = 4,
  TURBO_AGENT_POLICY_CAPABILITY_PATCH = 5,
  TURBO_AGENT_POLICY_CAPABILITY_OUTSIDE_WORKSPACE = 6
} turbo_agent_policy_capability_t;

#define TURBO_AGENT_TOOL_AUTHORITY_SNAPSHOT_ABI_VERSION 1u
#define TURBO_AGENT_TOOL_AUTHORITY_CAPABILITY_BIT(capability) (UINT32_C(1) << (capability))

typedef struct turbo_agent_tool_authority_snapshot_s {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t generation;
  uint32_t capability_mask;
  uint32_t reserved;
} turbo_agent_tool_authority_snapshot_t;

CXX_C_API turbo_agent_policy_t turbo_agent_policy_default(void);
CXX_C_API int turbo_agent_policy_validate_path(const turbo_agent_policy_t *policy,
                                               const char *path);
CXX_C_API int turbo_agent_policy_is_dangerous_command(const char *command);
CXX_C_API int turbo_agent_policy_allows_capability(const turbo_agent_policy_t *policy,
                                                   turbo_agent_policy_capability_t capability,
                                                   const char **out_reason);
CXX_C_API const char *
turbo_agent_policy_capability_name(turbo_agent_policy_capability_t capability);
CXX_C_API int
turbo_agent_policy_capability_from_name(const char *name,
                                        turbo_agent_policy_capability_t *out_capability);

/**
 * Check the capability metadata attached to one registered tool.
 * Legacy tools with no explicit metadata require `custom_tools`. Unknown
 * capability names fail closed with `unknown_tool_capability`.
 */
CXX_C_API turbo_agent_policy_decision_t turbo_agent_policy_check_tool(
    const turbo_agent_policy_t *policy, const turbo_tool_registry_t *registry,
    const char *tool_name, const char **out_reason);

/**
 * Replace the policy enforced by the Agent tool executor.
 * The policy and workspace root are copied. This is a quiescent control-plane
 * operation; NULL restores bounded defaults. The Host must serialize it with
 * all Agent/Harness executions and policy/snapshot reads, and wait for active
 * Runs and callbacks to finish. Calling it from a tool, store or event callback
 * during execution is unsupported. The atomic generation is an invalidation
 * token, not synchronization for the policy fields or workspace-root lifetime.
 */
CXX_C_API int turbo_agent_set_tool_policy(turbo_agent_t *agent, const turbo_agent_policy_t *policy);

/**
 * Capture the Agent's Host-granted tool capabilities and policy generation.
 * This contains no registry-derived authority: tool identity, required
 * capabilities, effects and result contracts must still be frozen by the
 * execution admission layer. Policy updates are quiescent control-plane
 * operations; a captured generation must be rechecked immediately before a
 * not-yet-started tool callback.
 */
CXX_C_API int turbo_agent_tool_authority_snapshot_get(
    const turbo_agent_t *agent, turbo_agent_tool_authority_snapshot_t *out_snapshot);

/**
 * Return 1 when current, 0 when revoked/stale, and -1 for invalid input.
 * This check does not acquire a policy lease or permit concurrent replacement.
 */
CXX_C_API int turbo_agent_tool_authority_snapshot_is_current(
    const turbo_agent_t *agent, const turbo_agent_tool_authority_snapshot_t *snapshot);
CXX_C_API turbo_agent_policy_decision_t turbo_agent_policy_check_action(
    const turbo_agent_policy_t *policy, const turbo_action_tool_definition_t *definition,
    const json_value_t *args, const char **out_reason);
CXX_C_API int turbo_agent_policy_requires_approval(const turbo_agent_policy_t *policy,
                                                   const turbo_action_tool_definition_t *definition,
                                                   const json_value_t *args);

#ifdef __cplusplus
}
#endif

#endif
