#ifndef TURBO_AGENT_SESSION_DIAGNOSTICS_INTERNAL_H
#define TURBO_AGENT_SESSION_DIAGNOSTICS_INTERNAL_H

#include "turbo_agent_session.h"

/* Borrowed only for the synchronous diagnostic call. The caller must keep the
 * Session and its registry alive and exclude concurrent mutation/destruction.
 * This view owns nothing, caches nothing, and carries no execution handles. */
typedef struct turbo_agent_session_diagnostic_view_s {
  int has_agent;
  int has_runtime;
  int has_memory_store;
  const turbo_tool_registry_t *registry;
  turbo_agent_policy_t tool_policy;
  size_t tool_count;
  turbo_agent_session_workflow_kind_t workflow_kind;
  const char *memory_namespace;
  const char *model;
  const char *base_url;
  const char *provider_name;
  int has_api_key;
  int has_knowledge_store;
  int has_retriever;
  const char *structured_output_name;
  const char *structured_output_schema_json;
  int structured_output_strict;
} turbo_agent_session_diagnostic_view_t;

int turbo_agent_session_diagnostic_view_internal(const turbo_agent_session_t *session,
                                                 turbo_agent_session_diagnostic_view_t *out_view);

#endif
