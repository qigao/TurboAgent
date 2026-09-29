#ifndef TURBO_AGENT_RUNTIME_INTERNAL_H
#define TURBO_AGENT_RUNTIME_INTERNAL_H

#include "turbo_graph.h"
#include "turbo_agent_core_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_execution_context_s {
  const char *thread_id;
  const char *run_id;
  const char *tool_call_id;
  const char *tool_name;
  turbo_agent_runtime_t *runtime;
  const turbo_cancel_token_t *cancel_token;
} turbo_agent_execution_context_t;

CXX_C_API const char *turbo_agent_provider_name(const turbo_agent_t *agent);
CXX_C_API turbo_graph_exec_status_t turbo_agent_clone_json(const json_value_t *value,
                                                           json_value_t **out_value);
CXX_C_API char *turbo_agent_format_tool_error(const char *name, const char *message);
CXX_C_API int turbo_agent_build_turn_request(turbo_agent_t *agent, json_value_t *state,
                                             char **out_request_json);
CXX_C_API int turbo_agent_append_model_event(turbo_agent_t *agent, json_value_t *state,
                                             const json_value_t *response);
CXX_C_API int turbo_agent_copy_model_error(json_value_t *dst_state,
                                           const json_value_t *src_state);
CXX_C_API int turbo_agent_copy_guardrail_rejection(json_value_t *dst_state,
                                                   const json_value_t *src_state);
CXX_C_API int turbo_agent_contains_failure_marker(const char *text);
CXX_C_API void turbo_agent_execution_context_get(
    turbo_agent_execution_context_t *out_context);
CXX_C_API void turbo_agent_execution_context_set(
    const turbo_agent_execution_context_t *context);
CXX_C_API void turbo_agent_current_tool_approval_set(int granted);

#ifdef __cplusplus
}
#endif

#endif
