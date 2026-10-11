#include "tinytest.h"
#include "turbo_agent_session.h"
#include "turbo_agent_state.h"

#include <string.h>

typedef struct memory_capture_s {
  json_value_t *records;
  json_value_t *payload;
  int put_calls;
  int query_calls;
  int list_calls;
  int matched_route;
  int matched_query;
  int put_result;
  int query_result;
} memory_capture_t;

static int memory_capture_put(void *data, const char *ns, const char *key, const char *value) {
  memory_capture_t *capture = data;
  ++capture->put_calls;
  capture->matched_route =
      ns && key && strcmp(ns, "project/demo") == 0 && strcmp(key, "notes") == 0;
  json_free(capture->payload);
  capture->payload = json_parse(value, strlen(value));
  return capture->put_result;
}

static int memory_capture_query(void *data, const char *prefix, const char *kind, const char *key,
                                const char *text, char **out) {
  memory_capture_t *capture = data;
  ++capture->query_calls;
  capture->matched_query = prefix && kind && strcmp(prefix, "project") == 0 &&
                           strcmp(kind, "context") == 0 && !key && !text;
  *out = capture->records ? json_serialize(capture->records, NULL) : NULL;
  return capture->query_result;
}

static int memory_capture_list(void *data, const char *prefix, char **out) {
  memory_capture_t *capture = data;
  (void)prefix;
  ++capture->list_calls;
  *out = NULL;
  return -1;
}

spec("Session memory adapter") {
  static const char records_json[] =
      "[{\"id\":\"context/first\",\"namespace\":\"project/demo\",\"kind\":\"context\","
      "\"key\":\"first\",\"text\":\"remember "
      "first\",\"metadata\":{\"scope\":\"project\",\"path\":null},\"created_at\":null},"
      "{\"id\":\"context/second\",\"namespace\":\"project/demo\",\"kind\":\"context\","
      "\"key\":\"second\",\"text\":\"remember "
      "second\",\"metadata\":{\"scope\":\"user\",\"path\":\"notes.md\"},\"created_at\":null}]";
  static turbo_agent_session_t *session;
  static memory_capture_t capture;
  static json_value_t *state, *factory_state;

  before_each() {
    turbo_agent_session_config_t config = {0};
    memset(&capture, 0, sizeof(capture));
    capture.records = json_parse(records_json, sizeof(records_json) - 1);
    check_not_null(capture.records);
    config.memory_store.put = memory_capture_put;
    config.memory_store.query = memory_capture_query;
    config.memory_store.list = memory_capture_list;
    config.memory_store.user_data = &capture;
    session = turbo_agent_session_create(&config);
    state = turbo_agent_state_create();
    check_not_null(session);
    check_not_null(state);
  }

  after_each() {
    turbo_agent_session_destroy(session);
    session = NULL;
    json_free(capture.records);
    json_free(capture.payload);
    memset(&capture, 0, sizeof(capture));
    json_free(state);
    json_free(factory_state);
    state = factory_state = NULL;
  }

  it("preserves context payload shape and propagates the store write result") {
    static const char text[] = "quoted \"text\"\nnext line";
    capture.put_result = -37;
    check_equal(turbo_agent_session_memory_put_context(session, "project/demo", "notes", "project",
                                                       NULL, text),
                -37);
    check_equal(capture.put_calls, 1);
    check_true(capture.matched_route);
    check_not_null(capture.payload);
    check_equal(json_object_size(capture.payload), 3);
    check_equal(json_get_string(capture.payload, "scope"), "project");
    check_equal(json_get_string(capture.payload, "path"), "");
    check_equal(json_get_string(capture.payload, "text"), text);
    check_equal(
        turbo_agent_session_memory_put_context(session, "project/demo", "notes", "", NULL, text),
        -1);
    check_equal(turbo_agent_session_memory_put_context(session, "project/demo", "notes", "project",
                                                       NULL, ""),
                -1);
    check_equal(capture.put_calls, 1);
  }

  it("copies selected context in order through query without invoking list") {
    check_equal(turbo_agent_state_add_memory_context_layer(state, "system", NULL, "existing"), 0);
    check_equal(turbo_agent_session_load_memory_context(session, state, "project"), 0);
    check_true(capture.matched_query);
    check_equal(capture.query_calls, 1);
    check_equal(capture.list_calls, 0);
    check_equal(turbo_agent_state_memory_layer_count(state), 3);
    turbo_agent_session_destroy(session);
    session = NULL;
    json_free(capture.records);
    capture.records = NULL;
    check_equal(json_get_string(turbo_agent_state_memory_layer_at(state, 0), "text"), "existing");
    check_equal(json_get_string(turbo_agent_state_memory_layer_at(state, 1), "text"),
                "remember first");
    check_equal(json_get_string(turbo_agent_state_memory_layer_at(state, 1), "path"), "");
    check_equal(json_get_string(turbo_agent_state_memory_layer_at(state, 2), "scope"), "user");
    check_equal(json_get_string(turbo_agent_state_memory_layer_at(state, 2), "path"), "notes.md");
  }

  it("retains earlier appended layers on late validation failure while the input factory discards "
     "its state") {
    json_value_t *metadata = json_object_get(json_array_get(capture.records, 1), "metadata");
    check_equal(turbo_runtime_json_object_set_string(metadata, "scope", ""), TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_agent_session_load_memory_context(session, state, "project"), -1);
    check_equal(turbo_agent_state_memory_layer_count(state), 1);
    check_equal(json_get_string(turbo_agent_state_memory_layer_at(state, 0), "text"),
                "remember first");
    factory_state =
        turbo_agent_session_create_input_state_with_memory_json_value(session, "input", "project");
    check_null(factory_state);
    check_equal(capture.query_calls, 2);
    check_equal(capture.list_calls, 0);
  }

  it("does not fall back to list or mutate state when query returns an error") {
    capture.query_result = -37;
    check_equal(turbo_agent_session_load_memory_context(session, state, "project"), -1);
    check_equal(turbo_agent_state_memory_layer_count(state), 0);
    check_equal(capture.query_calls, 1);
    check_equal(capture.list_calls, 0);
  }
}
