#ifndef TURBO_AGENT_CORE_INTERNAL_H
#define TURBO_AGENT_CORE_INTERNAL_H

#include "turbo_agent.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*turbo_agent_owned_resource_free_fn)(void *resource);
typedef struct coro_context_s coro_context_t;

struct turbo_agent_s {
  char *api_key;
  char *model;
  char *base_url;
  char *endpoint_path;
  char *instructions;
  char *structured_output_name;
  char *structured_output_schema_json;
  int structured_output_strict;
  size_t structured_output_max_retries;
  turbo_agent_api_mode_t api_mode;
  int stream_response;
  int parallel_tool_calls;
  int owns_http_client;
  int owns_http_context;
  int owns_tool_registry;
  http_client_t *http_client;
  coro_context_t *http_context;
  turbo_tool_registry_t *tool_registry;
  turbo_agent_transport_fn transport_fn;
  void *transport_user_data;
  void *owned_resource;
  turbo_agent_owned_resource_free_fn owned_resource_free;
  const turbo_model_provider_t *provider;
  turbo_agent_middleware_t *middlewares;
  size_t middleware_count;
  size_t middleware_capacity;
  turbo_agent_guardrail_t *guardrails;
  size_t guardrail_count;
  size_t guardrail_capacity;
  turbo_agent_trace_sink_t *trace_sinks;
  size_t trace_sink_count;
  size_t trace_sink_capacity;
  turbo_agent_trace_bind_sink_t *trace_bind_sinks;
  size_t trace_bind_sink_count;
  size_t trace_bind_sink_capacity;
  int capture_trace_history;
  char *last_stream_sse;
  size_t last_stream_sse_len;
  turbo_agent_store_t store;
  int has_store;
};

#ifdef __cplusplus
}
#endif

#endif
