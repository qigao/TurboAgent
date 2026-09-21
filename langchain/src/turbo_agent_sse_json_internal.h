#ifndef TURBO_AGENT_SSE_JSON_INTERNAL_H
#define TURBO_AGENT_SSE_JSON_INTERNAL_H

#include "turbo_agent_sse_types_internal.h"
#include <json_parser.h>

#ifdef __cplusplus
extern "C" {
#endif

char *turbo_agent_sse_serialize_json_and_free(json_value_t *value);
json_value_t *turbo_agent_sse_build_chat_tool_calls_array(
    const turbo_agent_sse_chat_stream_state_t *state);
json_value_t *turbo_agent_sse_tool_call_input_object(
    const turbo_agent_sse_tool_call_t *tool_call);
json_value_t *turbo_agent_sse_chat_response_shell_create(
    const turbo_agent_sse_chat_stream_state_t *state, json_value_t **out_choice,
    json_value_t **out_message);
json_value_t *turbo_agent_sse_anthropic_response_shell_create(
    const turbo_agent_sse_anthropic_stream_state_t *state, json_value_t **out_content);

#ifdef __cplusplus
}
#endif

#endif
