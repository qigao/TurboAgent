#ifndef TURBO_AGENT_HOOKS_INTERNAL_H
#define TURBO_AGENT_HOOKS_INTERNAL_H

#include "turbo_agent_core_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API void turbo_agent_emit_trace(turbo_agent_t *agent, json_value_t *state,
                                      turbo_agent_trace_event_kind_t kind, const char *name,
                                      const char *detail, const char *payload, int status);
CXX_C_API int turbo_agent_invoke_before_model_middlewares(turbo_agent_t *agent,
                                                          json_value_t *state,
                                                          char **inout_request_json);
CXX_C_API int turbo_agent_invoke_after_model_middlewares(turbo_agent_t *agent,
                                                         json_value_t *state,
                                                         const char *request_json,
                                                         char **inout_response_json,
                                                         int transport_status);
CXX_C_API int turbo_agent_invoke_before_tool_middlewares(turbo_agent_t *agent,
                                                         json_value_t *state,
                                                         const char *call_id,
                                                         const char *tool_name,
                                                         char **inout_arguments_json);
CXX_C_API int turbo_agent_invoke_after_tool_middlewares(turbo_agent_t *agent,
                                                        json_value_t *state,
                                                        const char *call_id,
                                                        const char *tool_name,
                                                        const char *arguments_json,
                                                        char **inout_output,
                                                        turbo_tool_status_t tool_status);
CXX_C_API int turbo_agent_invoke_before_model_guardrails(turbo_agent_t *agent,
                                                         const json_value_t *state,
                                                         const char *request_json,
                                                         char **out_reason);
CXX_C_API int turbo_agent_invoke_after_model_guardrails(turbo_agent_t *agent,
                                                        const json_value_t *state,
                                                        const char *response_json,
                                                        const json_value_t *response,
                                                        char **out_reason);
CXX_C_API int turbo_agent_invoke_before_tool_guardrails(turbo_agent_t *agent,
                                                        const json_value_t *state,
                                                        const char *call_id,
                                                        const char *tool_name,
                                                        const char *arguments_json,
                                                        char **out_reason);
CXX_C_API int turbo_agent_invoke_after_tool_guardrails(turbo_agent_t *agent,
                                                       const json_value_t *state,
                                                       const char *call_id,
                                                       const char *tool_name,
                                                       const char *arguments_json,
                                                       const char *output,
                                                       turbo_tool_status_t tool_status,
                                                       char **out_reason);
CXX_C_API int turbo_agent_tool_approval_review_matches(const json_value_t *state,
                                                       const char *call_id,
                                                       const char *tool_name,
                                                       const char *arguments_json);

#ifdef __cplusplus
}
#endif

#endif
