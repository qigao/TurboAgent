#include "turbo_agent_policy.h"

#include "turbo_tool_registry.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int turbo_policy_is_path_separator(char ch) { return ch == '/' || ch == '\\'; }

static char turbo_policy_normalize_path_char(char ch) {
  if (turbo_policy_is_path_separator(ch)) {
    return '/';
  }

#ifdef _WIN32
  return (char)tolower((unsigned char)ch);
#else
  return ch;
#endif
}

static int turbo_policy_component_eq(const char *text, size_t len, const char *value) {
  return strlen(value) == len && strncmp(text, value, len) == 0;
}

static int turbo_policy_last_component_is_parent(const char *path, size_t out, size_t root_len) {
  size_t start;

  if (!path || out <= root_len) {
    return 0;
  }
  start = out;
  while (start > root_len && path[start - 1] != '/') {
    start--;
  }
  return out - start == 2 && path[start] == '.' && path[start + 1] == '.';
}

static char *turbo_policy_normalize_path(const char *path) {
  size_t len;
  size_t pos = 0;
  size_t out = 0;
  size_t root_len = 0;
  char *normalized;
  int absolute = 0;

  if (!path) {
    return NULL;
  }

  len = strlen(path);
  normalized = (char *)malloc(len + 1);
  if (!normalized) {
    return NULL;
  }

#ifdef _WIN32
  if (len >= 2 && isalpha((unsigned char)path[0]) && path[1] == ':') {
    normalized[out++] = (char)tolower((unsigned char)path[0]);
    normalized[out++] = ':';
    pos = 2;
    root_len = out;
    if (pos < len && turbo_policy_is_path_separator(path[pos])) {
      normalized[out++] = '/';
      while (pos < len && turbo_policy_is_path_separator(path[pos])) {
        pos++;
      }
      root_len = out;
      absolute = 1;
    }
  } else if (len > 0 && turbo_policy_is_path_separator(path[0])) {
    normalized[out++] = '/';
    while (pos < len && turbo_policy_is_path_separator(path[pos])) {
      pos++;
    }
    root_len = out;
    absolute = 1;
  }
#else
  if (len > 0 && turbo_policy_is_path_separator(path[0])) {
    normalized[out++] = '/';
    while (pos < len && turbo_policy_is_path_separator(path[pos])) {
      pos++;
    }
    root_len = out;
    absolute = 1;
  }
#endif

  while (pos < len) {
    size_t component_start;
    size_t component_len;
    size_t i;

    while (pos < len && turbo_policy_is_path_separator(path[pos])) {
      pos++;
    }
    if (pos >= len) {
      break;
    }
    component_start = pos;
    while (pos < len && !turbo_policy_is_path_separator(path[pos])) {
      pos++;
    }
    component_len = pos - component_start;

    if (turbo_policy_component_eq(path + component_start, component_len, ".")) {
      continue;
    }
    if (turbo_policy_component_eq(path + component_start, component_len, "..")) {
      if (out > root_len &&
          (absolute || !turbo_policy_last_component_is_parent(normalized, out, root_len))) {
        size_t end = out;
        while (end > root_len && normalized[end - 1] != '/') {
          end--;
        }
        out = end > root_len ? end - 1 : root_len;
      } else if (!absolute) {
        if (out > 0 && normalized[out - 1] != '/') {
          normalized[out++] = '/';
        }
        normalized[out++] = '.';
        normalized[out++] = '.';
      }
      continue;
    }

    if (out > 0 && normalized[out - 1] != '/') {
      normalized[out++] = '/';
    }
    for (i = 0; i < component_len; ++i) {
      normalized[out++] = turbo_policy_normalize_path_char(path[component_start + i]);
    }
  }

  while (out > root_len && out > 0 && normalized[out - 1] == '/') {
    out--;
  }

  normalized[out] = '\0';
  return normalized;
}

static const char *turbo_policy_get_path_arg(const json_value_t *args) {
  if (!args || turbo_json_type(args) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  if (turbo_json_get_string(args, "path")) {
    return turbo_json_get_string(args, "path");
  }

  if (turbo_json_get_string(args, "workdir")) {
    return turbo_json_get_string(args, "workdir");
  }

  if (turbo_json_get_string(args, "root_dir")) {
    return turbo_json_get_string(args, "root_dir");
  }

  return turbo_json_get_string(args, "cwd");
}

turbo_agent_policy_t turbo_agent_policy_default(void) {
  turbo_agent_policy_t policy;

  memset(&policy, 0, sizeof(policy));
  policy.auto_approve_safe = 1;
  policy.allow_shell = 1;
  policy.allow_patch = 1;
  policy.allow_custom_tools = 1;
  policy.allow_runtime_tools = 1;
  policy.allow_delegate = 1;
  policy.allow_network = 1;
  policy.max_action_retries = 1;
  policy.max_steps = 32;
  policy.max_replans = 3;
  return policy;
}

int turbo_agent_policy_validate_path(const turbo_agent_policy_t *policy, const char *path) {
  size_t root_len;
  char *normalized_root;
  char *normalized_path;
  int rc = -1;

  if (!path || path[0] == '\0') {
    return -1;
  }

  if (!policy || !policy->workspace_root || policy->allow_outside_workspace) {
    return 0;
  }

  normalized_root = turbo_policy_normalize_path(policy->workspace_root);
  normalized_path = turbo_policy_normalize_path(path);
  if (!normalized_root || !normalized_path) {
    free(normalized_root);
    free(normalized_path);
    return -1;
  }

  root_len = strlen(normalized_root);
  if (root_len == 0) {
    free(normalized_root);
    free(normalized_path);
    return 0;
  }

  if (strlen(normalized_path) >= root_len &&
      strncmp(normalized_path, normalized_root, root_len) == 0 &&
      (normalized_path[root_len] == '\0' || normalized_path[root_len] == '/')) {
    rc = 0;
  }

  free(normalized_root);
  free(normalized_path);
  return rc;
}

static int turbo_policy_ascii_contains_case_insensitive(const char *text, const char *pattern) {
  size_t pattern_len;
  size_t i;

  if (!text || !pattern) {
    return 0;
  }
  pattern_len = strlen(pattern);
  if (pattern_len == 0) {
    return 1;
  }

  for (i = 0; text[i] != '\0'; ++i) {
    size_t j = 0;
    while (j < pattern_len && text[i + j] != '\0' &&
           tolower((unsigned char)text[i + j]) == tolower((unsigned char)pattern[j])) {
      j++;
    }
    if (j == pattern_len) {
      return 1;
    }
  }

  return 0;
}

int turbo_agent_policy_is_dangerous_command(const char *command) {
  static const char *patterns[] = {"rm -rf",           "del /s",        "format",
                                   "git reset --hard", "git clean -fd", NULL};
  size_t i;

  if (!command) {
    return 0;
  }

  for (i = 0; patterns[i]; ++i) {
    if (turbo_policy_ascii_contains_case_insensitive(command, patterns[i])) {
      return 1;
    }
  }

  return 0;
}

int turbo_agent_policy_allows_capability(const turbo_agent_policy_t *policy,
                                         turbo_agent_policy_capability_t capability,
                                         const char **out_reason) {
  turbo_agent_policy_t default_policy;

  if (out_reason) {
    *out_reason = NULL;
  }
  if (!policy) {
    default_policy = turbo_agent_policy_default();
    policy = &default_policy;
  }

  switch (capability) {
  case TURBO_AGENT_POLICY_CAPABILITY_CUSTOM_TOOLS:
    if (policy->allow_custom_tools) {
      return 1;
    }
    if (out_reason) {
      *out_reason = "custom_tools_disabled";
    }
    return 0;
  case TURBO_AGENT_POLICY_CAPABILITY_RUNTIME_TOOLS:
    if (policy->allow_runtime_tools) {
      return 1;
    }
    if (out_reason) {
      *out_reason = "runtime_tools_disabled";
    }
    return 0;
  case TURBO_AGENT_POLICY_CAPABILITY_DELEGATE:
    if (policy->allow_delegate) {
      return 1;
    }
    if (out_reason) {
      *out_reason = "delegate_disabled";
    }
    return 0;
  case TURBO_AGENT_POLICY_CAPABILITY_NETWORK:
    if (policy->allow_network) {
      return 1;
    }
    if (out_reason) {
      *out_reason = "network_disabled";
    }
    return 0;
  case TURBO_AGENT_POLICY_CAPABILITY_SHELL:
    if (policy->allow_shell) {
      return 1;
    }
    if (out_reason) {
      *out_reason = "shell_disabled";
    }
    return 0;
  case TURBO_AGENT_POLICY_CAPABILITY_PATCH:
    if (policy->allow_patch) {
      return 1;
    }
    if (out_reason) {
      *out_reason = "patch_disabled";
    }
    return 0;
  case TURBO_AGENT_POLICY_CAPABILITY_OUTSIDE_WORKSPACE:
    if (policy->allow_outside_workspace) {
      return 1;
    }
    if (out_reason) {
      *out_reason = "outside_workspace_disabled";
    }
    return 0;
  default:
    if (out_reason) {
      *out_reason = "unknown_capability";
    }
    return 0;
  }
}

const char *turbo_agent_policy_capability_name(turbo_agent_policy_capability_t capability) {
  switch (capability) {
  case TURBO_AGENT_POLICY_CAPABILITY_CUSTOM_TOOLS:
    return "custom_tools";
  case TURBO_AGENT_POLICY_CAPABILITY_RUNTIME_TOOLS:
    return "runtime_tools";
  case TURBO_AGENT_POLICY_CAPABILITY_DELEGATE:
    return "delegate";
  case TURBO_AGENT_POLICY_CAPABILITY_NETWORK:
    return "network";
  case TURBO_AGENT_POLICY_CAPABILITY_SHELL:
    return "shell";
  case TURBO_AGENT_POLICY_CAPABILITY_PATCH:
    return "patch";
  case TURBO_AGENT_POLICY_CAPABILITY_OUTSIDE_WORKSPACE:
    return "outside_workspace";
  default:
    return NULL;
  }
}

int turbo_agent_policy_capability_from_name(const char *name,
                                            turbo_agent_policy_capability_t *out_capability) {
  turbo_agent_policy_capability_t capability;
  if (!name || !out_capability) return -1;
  for (capability = TURBO_AGENT_POLICY_CAPABILITY_CUSTOM_TOOLS;
       capability <= TURBO_AGENT_POLICY_CAPABILITY_OUTSIDE_WORKSPACE;
       capability = (turbo_agent_policy_capability_t)(capability + 1)) {
    const char *candidate = turbo_agent_policy_capability_name(capability);
    if (candidate && strcmp(candidate, name) == 0) {
      *out_capability = capability;
      return 0;
    }
  }
  if (strcmp(name, "wasm") == 0) {
    *out_capability = TURBO_AGENT_POLICY_CAPABILITY_RUNTIME_TOOLS;
    return 0;
  }
  return -1;
}

turbo_agent_policy_decision_t turbo_agent_policy_check_tool(const turbo_agent_policy_t *policy,
                                                            const turbo_tool_registry_t *registry,
                                                            const char *tool_name,
                                                            const char **out_reason) {
  const char *const *required_capabilities = NULL;
  size_t required_capability_count = 0;
  size_t index;
  if (out_reason) *out_reason = NULL;
  if (!registry || !tool_name ||
      turbo_tool_registry_get_required_capabilities(registry, tool_name, &required_capabilities,
                                                    &required_capability_count) != TURBO_TOOL_OK) {
    if (out_reason) *out_reason = "tool_not_found";
    return TURBO_AGENT_POLICY_DENY;
  }
  if (required_capability_count == 0) {
    const char *reason = NULL;
    if (turbo_agent_policy_allows_capability(policy, TURBO_AGENT_POLICY_CAPABILITY_CUSTOM_TOOLS,
                                             &reason)) {
      return TURBO_AGENT_POLICY_ALLOW;
    }
    if (out_reason) *out_reason = reason;
    return TURBO_AGENT_POLICY_DENY;
  }
  for (index = 0; index < required_capability_count; ++index) {
    turbo_agent_policy_capability_t capability;
    const char *reason = NULL;
    if (turbo_agent_policy_capability_from_name(required_capabilities[index], &capability) != 0) {
      if (out_reason) *out_reason = "unknown_tool_capability";
      return TURBO_AGENT_POLICY_DENY;
    }
    if (!turbo_agent_policy_allows_capability(policy, capability, &reason)) {
      if (out_reason) *out_reason = reason;
      return TURBO_AGENT_POLICY_DENY;
    }
  }
  return TURBO_AGENT_POLICY_ALLOW;
}

turbo_agent_policy_decision_t
turbo_agent_policy_check_action(const turbo_agent_policy_t *policy,
                                const turbo_action_tool_definition_t *definition,
                                const json_value_t *args, const char **out_reason) {
  const char *path;
  const char *command;

  if (out_reason) {
    *out_reason = NULL;
  }

  if (!definition) {
    if (out_reason) {
      *out_reason = "missing_definition";
    }
    return TURBO_AGENT_POLICY_DENY;
  }

  path = turbo_policy_get_path_arg(args);
  if (path && turbo_agent_policy_validate_path(policy, path) != 0) {
    if (out_reason) {
      *out_reason = "path_outside_workspace";
    }
    return TURBO_AGENT_POLICY_DENY;
  }

  if (definition->kind == TURBO_ACTION_MUTATE && policy && !policy->allow_patch) {
    if (out_reason) {
      *out_reason = "patch_disabled";
    }
    return TURBO_AGENT_POLICY_DENY;
  }

  if (definition->kind == TURBO_ACTION_DANGEROUS && policy && !policy->allow_shell) {
    if (out_reason) {
      *out_reason = "shell_disabled";
    }
    return TURBO_AGENT_POLICY_DENY;
  }

  command = args && turbo_json_type(args) == TURBO_JSON_OBJECT
                ? turbo_json_get_string(args, "command")
                : NULL;
  if (command && turbo_agent_policy_is_dangerous_command(command)) {
    if (out_reason) {
      *out_reason = "dangerous_command";
    }
    return TURBO_AGENT_POLICY_REQUIRE_APPROVAL;
  }

  if (definition->requires_approval) {
    if (out_reason) {
      *out_reason = "tool_requires_approval";
    }
    return TURBO_AGENT_POLICY_REQUIRE_APPROVAL;
  }

  if (policy && policy->auto_approve_safe && definition->kind == TURBO_ACTION_OBSERVE) {
    return TURBO_AGENT_POLICY_ALLOW;
  }

  if (definition->kind == TURBO_ACTION_DANGEROUS) {
    if (out_reason) {
      *out_reason = "dangerous_action";
    }
    return TURBO_AGENT_POLICY_REQUIRE_APPROVAL;
  }

  return TURBO_AGENT_POLICY_ALLOW;
}

int turbo_agent_policy_requires_approval(const turbo_agent_policy_t *policy,
                                         const turbo_action_tool_definition_t *definition,
                                         const json_value_t *args) {
  return turbo_agent_policy_check_action(policy, definition, args, NULL) ==
                 TURBO_AGENT_POLICY_REQUIRE_APPROVAL
             ? 1
             : 0;
}
