#include "tinytest.h"
#include "turbo_agent_session.h"
#include "turbo_agent_state.h"
#include "turbo_prompt.h"
#include <stdlib.h>
#include <string.h>

static int input_transport(const char *request, char **out, void *data) {
  (void)request; (void)out; (void)data;
  return -1;
}

static int input_failed_memory_query(void *data, const char *prefix, const char *kind,
                                     const char *key, const char *text, char **out) {
  (void)data; (void)prefix; (void)kind; (void)key; (void)text;
  *out = NULL;
  return -1;
}

spec("session input ownership") {
  static turbo_agent_session_t *session;
  static json_value_t *messages, *state, *second;
  after_each() {
    json_free(messages); json_free(state); json_free(second);
    messages = state = second = NULL;
    turbo_agent_session_destroy(session); session = NULL;
  }

  it("should own nested canonical content independently of the caller") {
    static const char source[] =
        "[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"original\"}]}]";
    json_value_t *part;
    messages = json_parse(source, sizeof(source) - 1);
    check_not_null(messages);
    state = turbo_agent_session_create_input_messages_state_json_value(messages);
    check_not_null(state);
    part = json_array_get(json_object_get(json_array_get(messages, 0), "content"), 0);
    check_equal(turbo_runtime_json_object_set_string(part, "text", "changed"), TURBO_RUNTIME_JSON_OK);
    json_free(messages); messages = NULL;
    part = json_array_get(json_object_get(json_array_get(json_object_get(state, "input"), 0), "content"), 0);
    check_equal(json_get_string(part, "text"), "original");
    check_equal(turbo_agent_state_version(state), 1);
    check_equal(turbo_agent_state_event_count(state), 0);
  }

  it("should preserve empty text and empty canonical array input semantics") {
    state = turbo_agent_session_create_input_state_json_value(NULL);
    second = turbo_agent_session_create_input_state_json_value("");
    check_not_null(state); check_not_null(second);
    check_equal(json_array_size(json_object_get(state, "input")), 0);
    check_equal(json_array_size(json_object_get(second, "input")), 0);
    json_free(second);
    messages = json_create_array();
    second = turbo_agent_session_create_input_messages_state_json_value(messages);
    check_not_null(second);
    check_equal(json_array_size(json_object_get(second, "input")), 0);
  }

  it("should reject a late invalid message without changing the borrowed array") {
    messages = json_create_array();
    check_equal(turbo_prompt_messages_append(messages, "user", "preserved"), TURBO_PROMPT_OK);
    check_equal(turbo_runtime_json_array_append(messages, json_create_object()), TURBO_RUNTIME_JSON_OK);
    state = turbo_agent_session_create_input_messages_state_json_value(messages);
    check_null(state);
    check_equal(json_array_size(messages), 2);
    check_equal(json_get_string(json_array_get(messages, 0), "content"), "preserved");
    check_null(turbo_agent_session_create_input_messages_state_json_value(NULL));
    check_null(turbo_agent_session_create_input_messages_state_json_value(json_array_get(messages, 0)));
  }

  it("should retain copied input and memory after the session is destroyed") {
    turbo_agent_session_config_t config = {0};
    config.memory_store = turbo_agent_memory_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_equal(turbo_agent_session_memory_put_context(session, "project/demo", "notes",
                                                        "project", "AGENTS.md", "retained memory"), 0);
    messages = json_create_array();
    check_equal(turbo_prompt_messages_append(messages, "user", "retained input"), TURBO_PROMPT_OK);
    state = turbo_agent_session_create_input_messages_state_with_memory_json_value(session, messages, "project");
    second = turbo_agent_session_create_input_state_with_memory_json_value(session, "text input", "project");
    check_not_null(state); check_not_null(second);
    json_free(messages); messages = NULL;
    turbo_agent_session_destroy(session); session = NULL;
    check_equal(json_get_string(json_array_get(json_object_get(state, "input"), 0), "content"), "retained input");
    check_equal(json_get_string(turbo_agent_state_memory_layer_at(state, 0), "text"), "retained memory");
    check_equal(json_get_string(json_array_get(json_object_get(second, "input"), 0), "content"), "text input");
    check_equal(json_get_string(turbo_agent_state_memory_layer_at(second, 0), "text"), "retained memory");
  }

  it("should propagate memory query failure without returning a partial state") {
    turbo_agent_session_config_t config = {0};
    config.memory_store.query = input_failed_memory_query;
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    messages = json_create_array();
    check_equal(turbo_prompt_messages_append(messages, "user", "preserved"), TURBO_PROMPT_OK);
    state = turbo_agent_session_create_input_messages_state_with_memory_json_value(session, messages, "project");
    second = turbo_agent_session_create_input_state_with_memory_json_value(session, "text", "project");
    check_null(state); check_null(second);
    check_equal(json_array_size(messages), 1);
    check_equal(json_get_string(json_array_get(messages, 0), "content"), "preserved");
  }

  it("should discard partial history when a tool event is malformed") {
    static const char source[] =
        "{\"state_version\":1,\"input\":[{\"role\":\"user\",\"content\":\"preserved\"}],"
        "\"events\":[{\"kind\":\"model\",\"output_text\":\"answer\",\"tool_calls\":[]},"
        "{\"kind\":\"tool_results\",\"outputs\":[{\"output\":\"missing call id\"}]}]}";
    turbo_agent_session_config_t config = {0};
    config.agent_config.model = "input-test"; config.agent_config.transport_fn = input_transport;
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    state = json_parse(source, sizeof(source) - 1);
    check_not_null(state);
    second = turbo_agent_session_create_follow_up_state_json_value(session, state, "next");
    check_null(second);
    check_equal(turbo_agent_state_event_count(state), 2);
    check_equal(json_get_string(json_array_get(json_object_get(state, "input"), 0), "content"), "preserved");
    check_null(turbo_agent_session_create_follow_up_state_json_value(NULL, state, "next"));
    check_null(turbo_agent_session_create_follow_up_state_json_value(session, state, ""));
  }
}
