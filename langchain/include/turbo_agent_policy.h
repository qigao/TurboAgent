#ifndef TURBO_AGENT_POLICY_H
#define TURBO_AGENT_POLICY_H

#include <turbo_agent_api.h>

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
 * operation; NULL restores bounded defaults.
 */
CXX_C_API int turbo_agent_set_tool_policy(turbo_agent_t *agent, const turbo_agent_policy_t *policy);
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
