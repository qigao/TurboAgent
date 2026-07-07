#ifndef TURBO_AGENT_EVENT_INTERNAL_H
#define TURBO_AGENT_EVENT_INTERNAL_H

#include "turbo_agent_core_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API int turbo_agent_tool_call_record_fields(const json_value_t *tool_call,
                                                  const char **out_call_id,
                                                  const char **out_name,
                                                  const char **out_arguments);
CXX_C_API int turbo_agent_append_event(json_value_t *state, json_value_t *event);
CXX_C_API int turbo_agent_event_kind_is(const json_value_t *event, const char *kind);
CXX_C_API const char *turbo_agent_event_output_text(const json_value_t *event);
CXX_C_API const json_value_t *turbo_agent_model_event_tool_calls(const json_value_t *event);
CXX_C_API size_t turbo_agent_model_event_tool_call_count(const json_value_t *event);
CXX_C_API const char *turbo_agent_model_event_response_id(const json_value_t *event);
CXX_C_API const json_value_t *turbo_agent_events_last_of_kind(const json_value_t *events,
                                                              const char *kind);
CXX_C_API json_value_t *turbo_agent_tool_result_output_item_create(const char *call_id,
                                                                   const char *output);
CXX_C_API json_value_t *turbo_agent_event_create(const char *kind);
CXX_C_API const json_value_t *turbo_agent_last_model_tool_calls(const json_value_t *state);

#ifdef __cplusplus
}
#endif

#endif
