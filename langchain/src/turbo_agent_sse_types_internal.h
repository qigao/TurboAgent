#ifndef TURBO_AGENT_SSE_TYPES_INTERNAL_H
#define TURBO_AGENT_SSE_TYPES_INTERNAL_H

#include "turbo_agent_sse.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_sse_tool_call_s {
  char *id;
  char *type;
  char *name;
  char *arguments;
} turbo_agent_sse_tool_call_t;

typedef struct turbo_agent_sse_chat_stream_state_s {
  char *id;
  char *role;
  char *content;
  char *finish_reason;
  turbo_agent_sse_tool_call_t *tool_calls;
  size_t tool_call_count;
} turbo_agent_sse_chat_stream_state_t;

typedef struct turbo_agent_sse_anthropic_stream_state_s {
  char *id;
  char *role;
  char *content;
  char *stop_reason;
  turbo_agent_sse_tool_call_t *tool_calls;
  size_t tool_call_count;
} turbo_agent_sse_anthropic_stream_state_t;

#ifdef __cplusplus
}
#endif

#endif
