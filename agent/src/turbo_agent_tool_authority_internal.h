#ifndef TURBO_AGENT_TOOL_AUTHORITY_INTERNAL_H
#define TURBO_AGENT_TOOL_AUTHORITY_INTERNAL_H

#include "turbo_agent_policy.h"
#include "turbo_tool_registry.h"

#include <stdlib.h>

/*
 * Model-visible tool schemas must use the same Host capability decision as
 * execution. The returned projection borrows callback user data from source;
 * callers must destroy it before the source registry and its providers.
 */
static turbo_tool_status_t turbo_agent_tool_authority_project(
    const turbo_agent_policy_t *policy, const turbo_tool_registry_t *source,
    turbo_tool_registry_t **out_projection) {
  const char **names = NULL;
  size_t count;
  size_t allowed = 0;
  size_t i;
  turbo_tool_status_t status = TURBO_TOOL_OK;

  if (!out_projection) return TURBO_TOOL_INVALID_ARGUMENT;
  *out_projection = NULL;
  if (!source) {
    *out_projection = turbo_tool_registry_create();
    return *out_projection ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
  }

  count = turbo_tool_registry_count(source);
  if (count) {
    names = (const char **)calloc(count, sizeof(*names));
    if (!names) return TURBO_TOOL_ERROR;
  }
  for (i = 0; i < count; ++i) {
    turbo_tool_definition_t definition;
    const char *reason = NULL;
    if (turbo_tool_registry_get_definition(source, i, &definition) != TURBO_TOOL_OK ||
        !definition.name) {
      status = TURBO_TOOL_ERROR;
      goto done;
    }
    if (turbo_agent_policy_check_tool(policy, source, definition.name, &reason) ==
        TURBO_AGENT_POLICY_ALLOW) {
      names[allowed++] = definition.name;
    }
  }
  status = turbo_tool_registry_project(source, names, allowed, out_projection);

done:
  free(names);
  return status;
}

#endif
