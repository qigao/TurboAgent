#include "tinytest.h"
#include "turbo_agent_extensions.h"
#include "turbo_agent.h"
#include <string.h>

typedef struct {
  int count;
  char last_name[64];
} trace_bind_capture_t;

static int dummy_transport(const char *request_json, char **out_response_json, void *user_data) {
  (void)request_json;
  (void)out_response_json;
  (void)user_data;
  return -1;
}

static void capture_trace_bind(turbo_agent_t *agent,
                               const turbo_runtime_data_bind_value_t *event, void *user_data) {
  trace_bind_capture_t *capture = (trace_bind_capture_t *)user_data;
  const char *name;

  (void)agent;
  check_not_null(event);
  check_not_null(capture);
  capture->count++;
  name = turbo_runtime_data_bind_value_as_string(turbo_runtime_data_bind_object_get(event, "name"));
  if (name) {
    strncpy(capture->last_name, name, sizeof(capture->last_name) - 1);
    capture->last_name[sizeof(capture->last_name) - 1] = '\0';
  }
}

spec("turbo agent extensions api") {

  it("should expose optional runtime extension helpers through the extensions header") {
    void *middleware = (void *)turbo_agent_add_middleware;
    void *trace_sink = (void *)turbo_agent_add_trace_sink;
    void *trace_bind_sink = (void *)turbo_agent_add_trace_bind_sink;
    void *store = (void *)turbo_agent_set_store;
    void *memory_store = (void *)turbo_agent_store_memory_create;
    void *guardrail = (void *)turbo_agent_add_guardrail;
    void *policy_guardrail = (void *)turbo_agent_add_action_policy_guardrail;
    void *runnable = (void *)turbo_agent_runnable_create;

    check_not_null(middleware);
    check_not_null(trace_sink);
    check_not_null(trace_bind_sink);
    check_not_null(store);
    check_not_null(memory_store);
    check_not_null(guardrail);
    check_not_null(policy_guardrail);
    check_not_null(runnable);
  }

  it("should emit bind-native trace events through the extensions api") {
    turbo_agent_config_t config = {0};
    turbo_agent_t *agent;
    turbo_agent_store_t store;
    turbo_agent_trace_bind_sink_t sink = {0};
    trace_bind_capture_t capture = {0};
    json_value_t *state;

    config.model = "gpt-5.4";
    config.transport_fn = dummy_transport;
    agent = turbo_agent_create(&config);
    check_not_null(agent);

    store = turbo_agent_store_memory_create();
    check_int_eq(turbo_agent_set_store(agent, &store), 0);

    sink.callback = capture_trace_bind;
    sink.user_data = &capture;
    check_int_eq(turbo_agent_add_trace_bind_sink(agent, &sink), 0);

    state = turbo_agent_state_create();
    check_not_null(state);
    check_int_eq(turbo_agent_state_set_memory_json(state, "session", "{\"ok\":true}"), 0);
    check_int_eq(turbo_agent_state_save_memory(agent, state, "session"), 0);
    check_int_eq(turbo_agent_state_load_memory(agent, state, "session"), 0);
    check_true(capture.count >= 2);
    check_str_eq(capture.last_name, "memory_load");

    turbo_free_json(&state);
    turbo_agent_destroy(agent);
  }
}


