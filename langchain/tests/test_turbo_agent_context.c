#include "tinytest.h"

#include "turbo_agent_context.h"
#include "turbo_agent_session.h"
#include "turbo_agent_state.h"
#include "turbo_event.h"

#include "../src/turbo_agent_event_internal.h"
#include "../src/turbo_agent_runtime_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct context_strategy_s {
  size_t summarize_calls;
  size_t source_event_count;
  char first_kind[32];
  char last_kind[32];
} context_strategy_t;

typedef struct context_transport_s {
  size_t call_count;
  size_t overflow_calls;
  char *last_request;
} context_transport_t;

typedef struct context_failing_store_s {
  turbo_agent_runtime_store_t base;
  int fail_context_head;
} context_failing_store_t;

static char *context_copy_text(const char *text) {
  size_t size = strlen(text) + 1;
  char *copy = (char *)malloc(size);

  if (copy) memcpy(copy, text, size);
  return copy;
}

static size_t context_text_count(const char *text, const char *needle) {
  size_t count = 0;
  size_t needle_size = strlen(needle);

  while (text && needle_size > 0 && (text = strstr(text, needle)) != NULL) {
    ++count;
    text += needle_size;
  }
  return count;
}

static int context_estimate_json(const json_value_t *value, uint64_t *out_tokens, void *user_data) {
  char *serialized;
  size_t length = 0;

  (void)user_data;
  if (!value || !out_tokens) return -1;
  serialized = turbo_json_serialize(value, &length);
  if (!serialized) return -1;
  turbo_json_serialize_free(serialized);
  *out_tokens = (uint64_t)length;
  return 0;
}

static int context_summarize(const json_value_t *source, uint64_t max_summary_tokens,
                             json_value_t **out_summary, void *user_data) {
  context_strategy_t *strategy = (context_strategy_t *)user_data;
  const json_value_t *events;
  const json_value_t *first;
  const json_value_t *last;
  const char *kind;
  json_value_t *summary;

  (void)max_summary_tokens;
  if (!source || !out_summary || !strategy) return -1;
  *out_summary = NULL;
  events = turbo_json_object_get(source, "events");
  if (!events || turbo_json_type(events) != TURBO_JSON_ARRAY ||
      turbo_json_array_size(events) == 0) {
    return -1;
  }
  strategy->summarize_calls++;
  strategy->source_event_count = turbo_json_array_size(events);
  first = turbo_json_array_get(events, 0);
  last = turbo_json_array_get(events, strategy->source_event_count - 1);
  kind = turbo_json_get_string(first, "kind");
  snprintf(strategy->first_kind, sizeof(strategy->first_kind), "%s", kind ? kind : "");
  kind = turbo_json_get_string(last, "kind");
  snprintf(strategy->last_kind, sizeof(strategy->last_kind), "%s", kind ? kind : "");

  summary = turbo_json_create_object();
  if (!summary) return -1;
  turbo_json_object_set_number(summary, "schema_version", 1.0);
  turbo_json_object_set_string(summary, "goal", "summary-old-history");
  turbo_json_object_set_number(summary, "source_event_count", (double)strategy->source_event_count);
  *out_summary = summary;
  return 0;
}

static int context_detect_overflow(int transport_status, const char *response_json,
                                   void *user_data) {
  (void)user_data;
  return transport_status != 0 && response_json &&
                 strstr(response_json, "context_length_exceeded") != NULL
             ? 1
             : 0;
}

static turbo_agent_context_policy_t context_policy(context_strategy_t *strategy,
                                                   uint64_t trigger_tokens) {
  turbo_agent_context_policy_t policy = {0};

  policy.struct_size = sizeof(policy);
  policy.abi_version = TURBO_AGENT_CONTEXT_ABI_VERSION;
  policy.context_window_tokens = 100000;
  policy.reserve_output_tokens = 1000;
  policy.compact_trigger_tokens = trigger_tokens;
  policy.retain_recent_tokens = 1;
  policy.max_summary_input_tokens = 50000;
  policy.max_summary_tokens = 5000;
  policy.max_compactions_per_turn = 2;
  policy.estimate_tokens = context_estimate_json;
  policy.summarize = context_summarize;
  policy.is_context_overflow = context_detect_overflow;
  policy.user_data = strategy;
  return policy;
}

static int context_transport(const char *request_json, char **out_response_json, void *user_data) {
  static const char success[] = "{\"id\":\"resp_context\",\"output\":[{\"type\":\"message\","
                                "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
                                "\"text\":\"done\"}]}]}";
  static const char overflow[] = "{\"error\":{\"code\":\"context_length_exceeded\"}}";
  context_transport_t *transport = (context_transport_t *)user_data;

  if (!request_json || !out_response_json || !transport) return -1;
  free(transport->last_request);
  transport->last_request = context_copy_text(request_json);
  if (!transport->last_request) return -1;
  ++transport->call_count;
  if (transport->call_count <= transport->overflow_calls) {
    *out_response_json = context_copy_text(overflow);
    return -1;
  }
  *out_response_json = context_copy_text(success);
  return *out_response_json ? 0 : -1;
}

static turbo_agent_session_t *context_create_session(const char *thread_id,
                                                     turbo_agent_runtime_store_t store,
                                                     context_transport_t *transport) {
  turbo_agent_session_config_t config = {0};
  turbo_agent_session_t *session;

  config.thread_id = thread_id;
  config.runtime_store = store;
  config.agent_config.model = "gpt-5.4";
  config.agent_config.instructions = "Be concise.";
  config.agent_config.transport_fn = context_transport;
  config.agent_config.transport_user_data = transport;
  session = turbo_agent_session_create(&config);
  check_not_null(session);
  return session;
}

static json_value_t *context_state_with_model_events(void) {
  json_value_t *state = turbo_agent_state_create();
  json_value_t *tool_calls = turbo_json_create_array();
  json_value_t *event;

  check_not_null(state);
  check_not_null(tool_calls);
  if (!state || !tool_calls) return state;
  check_int_eq(turbo_agent_state_add_user_message(state, "current question"), 0);
  event = turbo_event_model_create_json_value("resp-old", "old answer", tool_calls);
  check_not_null(event);
  check_int_eq(turbo_agent_append_event(state, event), 0);
  event = turbo_event_model_create_json_value("resp-recent", "recent answer", tool_calls);
  check_not_null(event);
  check_int_eq(turbo_agent_append_event(state, event), 0);
  turbo_runtime_json_destroy(tool_calls);
  return state;
}

static json_value_t *context_state_with_tool_pair(void) {
  json_value_t *state = turbo_agent_state_create();
  json_value_t *tool_calls = turbo_json_create_array();
  json_value_t *recent_tool_calls = turbo_json_create_array();
  json_value_t *tool_call = turbo_json_create_object();
  json_value_t *event;
  json_value_t *outputs;
  json_value_t *output;

  check_not_null(state);
  check_not_null(tool_calls);
  check_not_null(recent_tool_calls);
  check_not_null(tool_call);
  if (!state || !tool_calls || !recent_tool_calls || !tool_call) return state;
  check_int_eq(turbo_agent_state_add_user_message(state, "current question"), 0);
  turbo_json_object_set_string(tool_call, "call_id", "call-old");
  turbo_json_object_set_string(tool_call, "name", "read_file");
  turbo_json_object_set_string(tool_call, "arguments", "{\"path\":\"old.txt\"}");
  turbo_json_array_add(tool_calls, tool_call);
  event = turbo_event_model_create_json_value("resp-tool", "", tool_calls);
  check_not_null(event);
  check_int_eq(turbo_agent_append_event(state, event), 0);

  event = turbo_agent_event_create("tool_results");
  outputs = turbo_json_create_array();
  output = turbo_agent_tool_result_output_item_create("call-old", "old tool output");
  check_not_null(event);
  check_not_null(outputs);
  check_not_null(output);
  turbo_json_array_add(outputs, output);
  turbo_json_object_add(event, "outputs", outputs);
  check_int_eq(turbo_agent_append_event(state, event), 0);

  event = turbo_event_model_create_json_value("resp-recent", "recent answer", recent_tool_calls);
  check_not_null(event);
  check_int_eq(turbo_agent_append_event(state, event), 0);
  turbo_runtime_json_destroy(tool_calls);
  turbo_runtime_json_destroy(recent_tool_calls);
  return state;
}

static int context_failing_store_put(void *user_data, const char *collection, const char *id,
                                     const char *record_json) {
  context_failing_store_t *store = (context_failing_store_t *)user_data;

  if (store->fail_context_head && strcmp(collection, "agent_context_heads") == 0) {
    store->fail_context_head = 0;
    return -1;
  }
  return store->base.put(store->base.user_data, collection, id, record_json);
}

static int context_failing_store_get(void *user_data, const char *collection, const char *id,
                                     char **out_record_json) {
  context_failing_store_t *store = (context_failing_store_t *)user_data;
  return store->base.get(store->base.user_data, collection, id, out_record_json);
}

static int context_failing_store_list(void *user_data, const char *collection,
                                      const char *filter_key, const char *filter_value,
                                      char **out_records_json) {
  context_failing_store_t *store = (context_failing_store_t *)user_data;
  return store->base.list(store->base.user_data, collection, filter_key, filter_value,
                          out_records_json);
}

spec("turbo agent context") {
  it("compacts old events while preserving full durable state") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    context_transport_t transport = {0};
    context_strategy_t strategy = {0};
    turbo_agent_context_policy_t policy = context_policy(&strategy, 2);
    turbo_agent_session_t *session = context_create_session("context-auto", store, &transport);
    json_value_t *state = context_state_with_model_events();
    json_value_t *summary = NULL;
    json_value_t *result_state = NULL;
    json_value_t *status = NULL;

    check_int_eq(turbo_agent_session_context_configure(session, &policy), TURBO_OK);
    check_int_eq(turbo_agent_session_start_preset(session, TURBO_AGENT_SESSION_WORKFLOW_LOOP, state,
                                                  NULL, NULL, NULL, &summary, &result_state),
                 TURBO_OK);
    check_size_eq(strategy.summarize_calls, 1);
    check_size_eq(strategy.source_event_count, 1);
    check_size_eq(turbo_agent_state_event_count(result_state), 3);
    check_not_null(strstr(transport.last_request, "summary-old-history"));
    check_null(strstr(transport.last_request, "old answer"));
    check_not_null(strstr(transport.last_request, "recent answer"));
    check_size_eq(context_text_count(transport.last_request, "Be concise."), 1);
    check_int_eq(turbo_agent_session_context_status(session, &status), TURBO_OK);
    check_str_eq(turbo_json_get_string(status, "status"), "committed");
    check_size_eq((size_t)turbo_json_get_double(status, "source_event_end", 0), 1);
    check_size_eq(turbo_agent_state_event_count(state), 2);

    free(transport.last_request);
    turbo_runtime_json_destroy(status);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(summary);
    turbo_runtime_json_destroy(state);
    turbo_agent_session_destroy(session);
  }

  it("keeps assistant tool calls and matching results in one compacted segment") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    context_transport_t transport = {0};
    context_strategy_t strategy = {0};
    turbo_agent_context_policy_t policy = context_policy(&strategy, 2);
    turbo_agent_session_t *session = context_create_session("context-tool-pair", store, &transport);
    json_value_t *state = context_state_with_tool_pair();

    check_int_eq(turbo_agent_session_context_configure(session, &policy), TURBO_OK);
    check_int_eq(turbo_agent_session_context_compact(session, state), TURBO_OK);
    check_size_eq(strategy.source_event_count, 2);
    check_str_eq(strategy.first_kind, "model");
    check_str_eq(strategy.last_kind, "tool_results");
    check_size_eq(turbo_agent_state_event_count(state), 3);

    turbo_runtime_json_destroy(state);
    turbo_agent_session_destroy(session);
  }

  it("recovers only a committed context head") {
    turbo_agent_runtime_store_t owned_store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_store_t shared_store = owned_store;
    context_transport_t first_transport = {0};
    context_transport_t second_transport = {0};
    context_strategy_t strategy = {0};
    turbo_agent_context_policy_t policy = context_policy(&strategy, 2);
    turbo_agent_session_t *first;
    turbo_agent_session_t *second;
    json_value_t *state = context_state_with_model_events();
    json_value_t *status = NULL;
    char *request_json = NULL;

    shared_store.user_data_free = NULL;
    first = context_create_session("context-recovery", shared_store, &first_transport);
    check_int_eq(turbo_agent_session_context_configure(first, &policy), TURBO_OK);
    check_int_eq(turbo_agent_session_context_compact(first, state), TURBO_OK);
    turbo_agent_session_destroy(first);

    second = context_create_session("context-recovery", shared_store, &second_transport);
    check_int_eq(turbo_agent_session_context_configure(second, &policy), TURBO_OK);
    check_int_eq(turbo_agent_session_context_status(second, &status), TURBO_OK);
    check_int_eq(
        turbo_agent_build_turn_request(turbo_agent_session_agent(second), state, &request_json), 0);
    check_not_null(strstr(request_json, "summary-old-history"));
    check_null(strstr(request_json, "old answer"));
    check_not_null(strstr(request_json, "recent answer"));

    free(request_json);
    turbo_runtime_json_destroy(status);
    turbo_runtime_json_destroy(state);
    turbo_agent_session_destroy(second);
    owned_store.user_data_free(owned_store.user_data);
  }

  it("does not expose a prepared compaction when the commit marker fails") {
    context_failing_store_t failing = {0};
    turbo_agent_runtime_store_t shared = {0};
    context_transport_t first_transport = {0};
    context_transport_t second_transport = {0};
    context_strategy_t strategy = {0};
    turbo_agent_context_policy_t policy = context_policy(&strategy, 2);
    turbo_agent_session_t *first;
    turbo_agent_session_t *second;
    json_value_t *state = context_state_with_model_events();
    json_value_t *status = NULL;
    char *request_json = NULL;

    failing.base = turbo_agent_runtime_store_memory_create();
    failing.fail_context_head = 1;
    shared.put = context_failing_store_put;
    shared.get = context_failing_store_get;
    shared.list = context_failing_store_list;
    shared.user_data = &failing;
    first = context_create_session("context-prepared-only", shared, &first_transport);
    check_int_eq(turbo_agent_session_context_configure(first, &policy), TURBO_OK);
    check_int_eq(turbo_agent_session_context_compact(first, state), TURBO_EIO);
    check_int_eq(turbo_agent_session_context_status(first, &status), TURBO_ENOENT);
    check_null(status);
    turbo_agent_session_destroy(first);

    second = context_create_session("context-prepared-only", shared, &second_transport);
    check_int_eq(turbo_agent_session_context_configure(second, &policy), TURBO_OK);
    check_int_eq(turbo_agent_session_context_status(second, &status), TURBO_ENOENT);
    check_int_eq(
        turbo_agent_build_turn_request(turbo_agent_session_agent(second), state, &request_json), 0);
    check_not_null(strstr(request_json, "current question"));
    check_null(strstr(request_json, "summary-old-history"));

    free(request_json);
    turbo_runtime_json_destroy(state);
    turbo_agent_session_destroy(second);
    failing.base.user_data_free(failing.base.user_data);
  }

  it("compacts and retries one explicit provider overflow") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    context_transport_t transport = {0};
    context_strategy_t strategy = {0};
    turbo_agent_context_policy_t policy = context_policy(&strategy, 90000);
    turbo_agent_session_t *session = context_create_session("context-overflow", store, &transport);
    json_value_t *state = context_state_with_model_events();
    json_value_t *summary = NULL;
    json_value_t *result_state = NULL;

    transport.overflow_calls = 1;
    check_int_eq(turbo_agent_session_context_configure(session, &policy), TURBO_OK);
    check_int_eq(turbo_agent_session_start_preset(session, TURBO_AGENT_SESSION_WORKFLOW_LOOP, state,
                                                  NULL, NULL, NULL, &summary, &result_state),
                 TURBO_OK);
    check_size_eq(transport.call_count, 2);
    check_size_eq(strategy.summarize_calls, 1);
    check_not_null(strstr(transport.last_request, "summary-old-history"));
    check_null(strstr(transport.last_request, "old answer"));
    check_size_eq(turbo_agent_state_event_count(result_state), 3);

    free(transport.last_request);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(summary);
    turbo_runtime_json_destroy(state);
    turbo_agent_session_destroy(session);
  }

  it("retries repeated provider overflow at most once") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    context_transport_t transport = {0};
    context_strategy_t strategy = {0};
    turbo_agent_context_policy_t policy = context_policy(&strategy, 90000);
    turbo_agent_session_t *session =
        context_create_session("context-overflow-once", store, &transport);
    json_value_t *state = context_state_with_model_events();
    json_value_t *summary = NULL;
    json_value_t *result_state = NULL;

    transport.overflow_calls = 2;
    check_int_eq(turbo_agent_session_context_configure(session, &policy), TURBO_OK);
    check_true(turbo_agent_session_start_preset(session, TURBO_AGENT_SESSION_WORKFLOW_LOOP, state,
                                                NULL, NULL, NULL, &summary,
                                                &result_state) != TURBO_OK);
    check_size_eq(transport.call_count, 2);
    check_size_eq(strategy.summarize_calls, 1);

    free(transport.last_request);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(summary);
    turbo_runtime_json_destroy(state);
    turbo_agent_session_destroy(session);
  }
}
