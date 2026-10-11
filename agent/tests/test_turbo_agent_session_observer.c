#include "tinytest.h"
#include "turbo_agent_session.h"
#include "turbo_agent_state.h"

#include <stdlib.h>
#include <string.h>

typedef struct observer_capture_s {
  size_t calls;
  int releases;
  json_value_t *last;
} observer_capture_t;

static void observer_capture(const json_value_t *event, void *data) {
  observer_capture_t *capture = data;
  ++capture->calls;
  json_free(capture->last);
  capture->last = json_clone(event);
}

static void observer_trace_capture(turbo_agent_t *agent, const json_value_t *event, void *data) {
  (void)agent;
  observer_capture(event, data);
}

static void observer_release(void *data) {
  observer_capture_t *capture = data;
  ++capture->releases;
}

static int observer_transport(const char *request, char **out_response, void *data) {
  static const char response[] =
      "{\"id\":\"response\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"ok\"}]}]}";
  int status = *(int *)data;
  (void)request;
  *out_response = NULL;
  if (status != 0) return status;
  *out_response = malloc(sizeof(response));
  if (!*out_response) return -1;
  memcpy(*out_response, response, sizeof(response));
  return 0;
}

spec("Session live observers") {
  static turbo_agent_session_t *session;
  static observer_capture_t trace, first, second;
  static turbo_agent_trace_json_value_sink_t trace_sink;
  static turbo_agent_observer_json_value_sink_t first_sink, second_sink;
  static int transport_status;
  static json_value_t *state, *next_state, *summary;

  before_each() {
    turbo_agent_session_config_t config = {0};
    transport_status = 0;
    config.agent_config.model = "test-model";
    config.agent_config.transport_fn = observer_transport;
    config.agent_config.transport_user_data = &transport_status;
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    trace_sink =
        (turbo_agent_trace_json_value_sink_t){observer_trace_capture, &trace, observer_release};
    first_sink =
        (turbo_agent_observer_json_value_sink_t){observer_capture, &first, observer_release};
    second_sink =
        (turbo_agent_observer_json_value_sink_t){observer_capture, &second, observer_release};
  }

  after_each() {
    turbo_agent_session_destroy(session);
    session = NULL;
    json_free(trace.last);
    json_free(first.last);
    json_free(second.last);
    trace = (observer_capture_t){0};
    first = (observer_capture_t){0};
    second = (observer_capture_t){0};
    json_free(state);
    json_free(next_state);
    json_free(summary);
    state = next_state = summary = NULL;
  }

  it("copies registrations and releases each accepted sink exactly once at destruction") {
    check_equal(turbo_agent_session_add_trace_json_value_sink(session, &trace_sink), 0);
    check_equal(turbo_agent_session_add_observer_json_value_sink(session, &first_sink), 0);
    check_equal(turbo_agent_session_add_observer_json_value_sink(session, &second_sink), 0);
    /* Stack registration descriptors may change after success. */
    trace_sink = (turbo_agent_trace_json_value_sink_t){0};
    first_sink = second_sink = (turbo_agent_observer_json_value_sink_t){0};
    check_equal(turbo_agent_session_start_text(session, "hello", NULL, &summary, &state), 0);
    check_true(trace.calls >= 2);
    check_equal(first.calls, trace.calls);
    check_equal(second.calls, first.calls);
    check_equal(first.releases, 0);
    check_equal(second.releases, 0);
    check_equal(trace.releases, 0);
    check_not_null(first.last);
    check_not_null(second.last);
    check_not_null(trace.last);
    check_equal(json_get_string(first.last, "kind"), "observer");
    check_equal(json_get_string(first.last, "type"), "model_delta");
    check_equal(json_get_string(first.last, "name"), "model_response");
    check_equal(json_get_string(first.last, "payload"), json_get_string(trace.last, "payload"));
    check_equal(turbo_runtime_json_object_set_string(json_object_get(first.last, "event"),
                                                     "payload", "caller-change"),
                TURBO_RUNTIME_JSON_OK);
    check_equal(json_get_string(json_object_get(second.last, "event"), "payload"),
                json_get_string(trace.last, "payload"));
    turbo_agent_session_destroy(session);
    session = NULL;
    check_equal(first.releases, 1);
    check_equal(second.releases, 1);
    check_equal(trace.releases, 1);
    check_equal(json_get_string(first.last, "name"), "model_response");
    check_equal(json_get_string(json_object_get(first.last, "event"), "payload"), "caller-change");
    check_equal(json_get_string(second.last, "type"), "model_delta");
  }

  it("leaves rejected sink user data with the caller") {
    turbo_agent_session_config_t empty_config = {0};
    first_sink.callback = NULL;
    trace_sink.callback = NULL;
    check_equal(turbo_agent_session_add_observer_json_value_sink(session, &first_sink), -1);
    check_equal(turbo_agent_session_add_trace_json_value_sink(session, &trace_sink), -1);
    check_equal(turbo_agent_session_add_observer_json_value_sink(NULL, &second_sink), -1);
    turbo_agent_session_destroy(session);
    session = turbo_agent_session_create(&empty_config);
    check_not_null(session);
    trace_sink.callback = observer_trace_capture;
    check_equal(turbo_agent_session_add_trace_json_value_sink(session, &trace_sink), -1);
    check_equal(turbo_agent_session_add_observer_json_value_sink(session, &second_sink), -1);
    check_equal(turbo_agent_session_set_trace_history_enabled(session, 1), -1);
    turbo_agent_session_destroy(session);
    session = NULL;
    check_equal(first.releases, 0);
    check_equal(second.releases, 0);
    check_equal(trace.releases, 0);
    check_equal(first.calls + second.calls + trace.calls, 0);
  }

  it("toggles retained trace history independently of live delivery") {
    size_t first_calls;
    check_equal(turbo_agent_session_add_trace_json_value_sink(session, &trace_sink), 0);
    check_equal(turbo_agent_session_add_observer_json_value_sink(session, &first_sink), 0);
    check_equal(turbo_agent_session_set_trace_history_enabled(session, 0), 0);
    check_equal(turbo_agent_session_start_text(session, "first", NULL, NULL, &state), 0);
    check_equal(turbo_agent_state_trace_event_count(state), 0);
    check_true(first.calls >= 2);
    check_equal(trace.calls, first.calls);
    first_calls = first.calls;
    check_equal(turbo_agent_session_set_trace_history_enabled(session, 1), 0);
    check_equal(turbo_agent_session_start_text(session, "second", NULL, NULL, &next_state), 0);
    check_true(first.calls > first_calls);
    check_equal(turbo_agent_state_trace_event_count(next_state), first.calls - first_calls);
    check_equal(turbo_agent_state_trace_event_count(state), 0);
    check_equal(turbo_agent_session_set_trace_history_enabled(session, 0), 0);
    check_equal(turbo_agent_state_trace_event_count(next_state), first.calls - first_calls);
    check_equal(first.releases, 0);
  }

  it("delivers transport failure status without releasing registered sinks early") {
    transport_status = -17;
    check_equal(turbo_agent_session_add_trace_json_value_sink(session, &trace_sink), 0);
    check_equal(turbo_agent_session_add_observer_json_value_sink(session, &first_sink), 0);
    check_equal(turbo_agent_session_start_text(session, "failure", NULL, &summary, &state), -1);
    check_true(first.calls >= 2);
    check_equal(first.calls, trace.calls);
    check_not_null(first.last);
    check_equal(json_get_string(first.last, "name"), "model_response");
    check_equal(json_get_int(first.last, "status", 0), -17);
    check_equal(json_get_int(json_object_get(first.last, "event"), "status", 0), -17);
    check_equal(first.releases, 0);
    check_equal(trace.releases, 0);
    turbo_agent_session_destroy(session);
    session = NULL;
    check_equal(first.releases, 1);
    check_equal(trace.releases, 1);
  }
}
