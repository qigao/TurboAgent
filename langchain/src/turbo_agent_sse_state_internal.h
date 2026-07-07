#ifndef TURBO_AGENT_SSE_STATE_INTERNAL_H
#define TURBO_AGENT_SSE_STATE_INTERNAL_H

#include "turbo_agent_sse_types_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

int turbo_agent_sse_replace_text(char **target, const char *text);
int turbo_agent_sse_set_if_nonempty(char **target, const char *text);
int turbo_agent_sse_append_dynamic_text(char **buffer, const char *text);
void turbo_agent_sse_free_tool_calls(turbo_agent_sse_tool_call_t *tool_calls,
                                     size_t tool_call_count);
turbo_agent_sse_tool_call_t *turbo_agent_sse_tool_call_slot(
    turbo_agent_sse_tool_call_t **tool_calls, size_t *tool_call_count, size_t index);
void turbo_agent_sse_free_chat_state(turbo_agent_sse_chat_stream_state_t *state);
void turbo_agent_sse_free_anthropic_state(turbo_agent_sse_anthropic_stream_state_t *state);
char *turbo_agent_sse_normalize_newlines(const char *data, size_t len);
int turbo_agent_sse_collect_event_data(const char *begin, const char *end, char **out_data);
int turbo_agent_responses_sse_to_json(const char *sse_data, size_t sse_len,
                                      char **out_response_json);

#ifdef __cplusplus
}
#endif

#endif
