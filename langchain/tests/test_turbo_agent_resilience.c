#include "tinytest.h"
#include "turbo_agent_graph.h"
#include "turbo_agent_resilience.h"
#include "turbo_agent_state.h"

#include "../src/turbo_agent_runtime_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct resilience_transport_s {
  int calls;
  int failures_before_success;
  int retryable;
  turbo_cancel_source_t *cancel_source;
} resilience_transport_t;

static char *resilience_strdup(const char *text) {
  size_t length = strlen(text) + 1;
  char *copy = (char *)malloc(length);
  if (copy) memcpy(copy, text, length);
  return copy;
}

static int resilience_v1_transport(const char *request_json, char **out_response_json,
                                   void *user_data) {
  (void)request_json;
  (void)out_response_json;
  (void)user_data;
  return -1;
}

static int resilience_v2_transport(const char *request_json,
                                   const turbo_cancel_token_t *cancel_token,
                                   unsigned int request_timeout_ms, unsigned int connect_timeout_ms,
                                   turbo_agent_transport_response_t *out_response,
                                   void *user_data) {
  resilience_transport_t *transport = (resilience_transport_t *)user_data;
  (void)request_json;
  (void)cancel_token;
  check_true(request_timeout_ms > 0);
  check_true(connect_timeout_ms > 0);
  ++transport->calls;
  out_response->struct_size = sizeof(*out_response);
  out_response->abi_version = TURBO_AGENT_TRANSPORT_V2_ABI_VERSION;
  if (transport->calls <= transport->failures_before_success) {
    out_response->transport_status = -77;
    out_response->http_status = 429;
    out_response->retryable = transport->retryable;
    out_response->retry_after_ms = 1;
    out_response->body = resilience_strdup("transient");
    if (transport->cancel_source)
      (void)turbo_cancel_source_cancel(transport->cancel_source, TURBO_CANCEL_USER);
    return -77;
  }
  out_response->transport_status = 0;
  out_response->http_status = 200;
  out_response->provider_request_id = "request-1";
  out_response->body = resilience_strdup(
      "{\"id\":\"response-1\",\"output\":[{\"type\":\"message\",\"content\":[{"
      "\"type\":\"output_text\",\"text\":\"ok\"}]}],\"usage\":{\"input_tokens\":10,"
      "\"output_tokens\":2,\"total_tokens\":12}}");
  return out_response->body ? 0 : -1;
}

static turbo_agent_t *resilience_agent(resilience_transport_t *transport,
                                       unsigned int max_attempts) {
  turbo_agent_config_t config = {0};
  turbo_agent_retry_policy_t policy;
  turbo_agent_t *agent;
  config.model = "gpt-5.4";
  config.transport_fn = resilience_v1_transport;
  agent = turbo_agent_create(&config);
  if (!agent) return NULL;
  turbo_agent_retry_policy_init(&policy);
  policy.max_attempts = max_attempts;
  policy.base_delay_ms = 1;
  policy.max_delay_ms = 2;
  policy.max_elapsed_ms = 100;
  if (turbo_agent_retry_configure(agent, &policy) != TURBO_OK ||
      turbo_agent_transport_v2_set(agent, resilience_v2_transport, transport, NULL) != TURBO_OK) {
    turbo_agent_destroy(agent);
    return NULL;
  }
  return agent;
}

spec("turbo agent resilience") {

  it("should retry structured transient failures and record exact usage") {
    resilience_transport_t transport = {0, 2, 1, NULL};
    turbo_agent_t *agent = resilience_agent(&transport, 3);
    json_value_t *state = turbo_agent_state_create();
    turbo_graph_exec_ctx_t ctx = {0};
    const json_value_t *attempts;
    const json_value_t *usage_records;
    const json_value_t *usage;
    const json_value_t *cost;

    check_not_null(agent);
    check_int_eq(turbo_agent_state_add_user_message(state, "hello"), 0);
    ctx.state = state;
    check_int_eq(turbo_agent_model_node(&ctx, agent), 0);
    check_int_eq(transport.calls, 3);
    attempts = turbo_json_object_get(state, "provider_attempts");
    check_size_eq(turbo_json_array_size(attempts), 3);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(attempts, 0), "outcome"),
                 "retry_scheduled");
    usage_records = turbo_json_object_get(state, "usage_records");
    check_size_eq(turbo_json_array_size(usage_records), 1);
    usage = turbo_json_array_get(usage_records, 0);
    check_int_eq((int)turbo_json_get_double(usage, "input_tokens", -1), 10);
    check_int_eq((int)turbo_json_get_double(usage, "output_tokens", -1), 2);
    check_int_eq((int)turbo_json_get_double(usage, "total_tokens", -1), 12);
    cost = turbo_json_object_get(usage, "cost");
    check_str_eq(turbo_json_get_string(cost, "status"), "unknown");

    turbo_runtime_json_destroy(state);
    turbo_agent_destroy(agent);
  }

  it("should not retry a failure the transport marks non-retryable") {
    resilience_transport_t transport = {0, 3, 0, NULL};
    turbo_agent_t *agent = resilience_agent(&transport, 3);
    json_value_t *state = turbo_agent_state_create();
    turbo_graph_exec_ctx_t ctx = {0};

    turbo_agent_state_add_user_message(state, "hello");
    ctx.state = state;
    check_int_ne(turbo_agent_model_node(&ctx, agent), 0);
    check_int_eq(transport.calls, 1);
    check_str_eq(turbo_agent_state_model_error_phase(state), "transport");

    turbo_runtime_json_destroy(state);
    turbo_agent_destroy(agent);
  }

  it("should cancel a retry wait without issuing another request") {
    resilience_transport_t transport = {0, 3, 1, NULL};
    turbo_cancel_source_t *source = NULL;
    turbo_cancel_token_t *token = NULL;
    turbo_agent_execution_context_t saved = {0};
    turbo_agent_execution_context_t current = {0};
    turbo_agent_t *agent;
    json_value_t *state = turbo_agent_state_create();
    turbo_graph_exec_ctx_t ctx = {0};

    check_int_eq(turbo_cancel_source_create(NULL, &source), TURBO_OK);
    check_int_eq(turbo_cancel_source_token(source, &token), TURBO_OK);
    transport.cancel_source = source;
    agent = resilience_agent(&transport, 3);
    turbo_agent_execution_context_get(&saved);
    current.cancel_token = token;
    turbo_agent_execution_context_set(&current);
    turbo_agent_state_add_user_message(state, "hello");
    ctx.state = state;
    check_int_ne(turbo_agent_model_node(&ctx, agent), 0);
    check_int_eq(transport.calls, 1);
    turbo_agent_execution_context_set(&saved);

    turbo_runtime_json_destroy(state);
    turbo_agent_destroy(agent);
    turbo_cancel_token_release(token);
    turbo_cancel_source_destroy(source);
  }
}
