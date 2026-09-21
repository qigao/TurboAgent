#ifndef TURBO_AGENT_CORE_INTERNAL_H
#define TURBO_AGENT_CORE_INTERNAL_H

#include "turbo_agent.h"
#include "turbo_agent_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*turbo_agent_owned_resource_free_fn)(void *resource);
typedef int (*turbo_agent_before_turn_fn)(turbo_agent_t *agent, json_value_t *state,
                                          void *user_data);
typedef int (*turbo_agent_context_overflow_fn)(turbo_agent_t *agent, json_value_t *state,
                                               int transport_status, const char *response_json,
                                               void *user_data);
typedef struct turbo_agent_tool_executor_s turbo_agent_tool_executor_t;

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
  int owns_tool_registry;
  chttp_client *http_client;
  turbo_tool_registry_t *tool_registry;
  turbo_agent_tool_executor_t *tool_executor;
  turbo_agent_policy_t tool_policy;
  char *tool_policy_workspace_root;
  turbo_agent_transport_fn transport_fn;
  void *transport_user_data;
  turbo_agent_transport_v2_fn transport_v2;
  void *transport_v2_user_data;
  turbo_agent_transport_v2_user_data_free_fn transport_v2_user_data_free;
  turbo_agent_retry_policy_t retry_policy;
  char *last_provider_request_id;
  void *owned_resource;
  turbo_agent_owned_resource_free_fn owned_resource_free;
  turbo_agent_before_turn_fn before_turn;
  void *before_turn_user_data;
  turbo_agent_context_overflow_fn context_overflow;
  void *context_overflow_user_data;
  json_value_t *context_summary;
  size_t context_event_start;
  int context_projection_active;
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
  turbo_agent_trace_json_value_sink_t *trace_json_value_sinks;
  size_t trace_json_value_sink_count;
  size_t trace_json_value_sink_capacity;
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
