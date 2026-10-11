#include "tinytest.h"
#include "turbo_agent_session.h"
#include "turbo_agent_state.h"
#include "turbo_model_provider.h"
#include "../src/turbo_agent_runtime_internal.h"

#include <stdlib.h>
#include <string.h>

static int history_transport(const char *request, char **out, void *data) {
  (void)request;
  *out = NULL;
  ++*(int *)data;
  return -1;
}

spec("conversation history validation") {
  static turbo_agent_session_t *session;
  static json_value_t *state, *event, *follow_up;
  static char *request, *before, *after;
  static int transport_calls;

  before_each() {
    static const char initial[] =
        "{\"state_version\":1,\"input\":[{\"role\":\"user\",\"content\":\"first task\"}],"
        "\"events\":[{\"kind\":\"model\",\"output_text\":\"first reply\",\"tool_calls\":[]}]}";
    state = json_parse(initial, sizeof(initial) - 1);
    check_not_null(state);
    transport_calls = 0;
  }

  after_each() {
    json_free(state); json_free(event); json_free(follow_up);
    state = event = follow_up = NULL;
    json_serialize_free(before); json_serialize_free(after); free(request);
    before = after = request = NULL;
    turbo_agent_session_destroy(session); session = NULL;
  }

  const struct {
    const char *name, *value;
    int container;
  } invalid[] = {
      {"object events", "{}", 1},
      {"string events", "\"lost history\"", 1},
      {"null events", "null", 1},
      {"non-object event", "\"lost event\"", 0},
      {"missing event kind", "{\"output_text\":\"lost reply\"}", 0},
      {"non-string event kind", "{\"kind\":7,\"output_text\":\"lost reply\"}", 0},
      {"string tool calls", "{\"kind\":\"model\",\"output_text\":\"reply\",\"tool_calls\":\"lost calls\"}", 0},
      {"object tool calls", "{\"kind\":\"model\",\"output_text\":\"reply\",\"tool_calls\":{}}", 0},
      {"null tool calls", "{\"kind\":\"model\",\"output_text\":\"reply\",\"tool_calls\":null}", 0},
      {"non-string model text", "{\"kind\":\"model\",\"output_text\":7,\"tool_calls\":[]}", 0}};

  for (int anthropic = 0; anthropic < 2; ++anthropic) {
    for (size_t scenario = 0; scenario < sizeof(invalid) / sizeof(invalid[0]); ++scenario) {
      it("rejects %s for follow-up and %s requests without changing source state",
         invalid[scenario].name, anthropic ? "Anthropic" : "Chat Completions") {
        turbo_agent_session_config_t config = {0};
        config.agent_config.model = "history-test";
        config.agent_config.provider = anthropic ? turbo_model_provider_anthropic_messages()
                                                : turbo_model_provider_openai_chat_completions();
        config.agent_config.transport_fn = history_transport;
        config.agent_config.transport_user_data = &transport_calls;
        session = turbo_agent_session_create(&config);
        check_not_null(session);
        event = json_parse(invalid[scenario].value, strlen(invalid[scenario].value));
        check_not_null(event);
        if (invalid[scenario].container)
          check_equal(turbo_runtime_json_object_set(state, "events", event), TURBO_RUNTIME_JSON_OK);
        else
          check_equal(turbo_runtime_json_array_append(json_object_get(state, "events"), event),
                      TURBO_RUNTIME_JSON_OK);
        event = NULL;
        before = json_serialize(state, NULL);
        check_not_null(before);
        follow_up = turbo_agent_session_create_follow_up_state_json_value(session, state, "next");
        check_null(follow_up);
        check_not_equal(turbo_agent_build_turn_request(turbo_agent_session_agent(session), state,
                                                       &request), 0);
        check_null(request);
        after = json_serialize(state, NULL);
        check_not_null(after);
        check_equal(after, before);
        check_equal(transport_calls, 0);
      }
    }

    it("preserves valid optional model fields and failed tool history for %s",
       anthropic ? "Anthropic" : "Chat Completions") {
      static const char sequence[] =
          "[{\"kind\":\"trace\",\"tool_calls\":false},"
          "{\"kind\":\"model\",\"output_text\":\"second reply\"},"
          "{\"kind\":\"model\",\"output_text\":null,\"tool_calls\":["
            "{\"call_id\":\"call_1\",\"name\":\"fs.edit\",\"arguments\":\"{}\"}]},"
          "{\"kind\":\"tool_results\",\"outputs\":[{\"call_id\":\"call_1\","
            "\"output\":\"{\\\"ok\\\":false,\\\"status\\\":\\\"conflict\\\"}\","
            "\"detail\":{\"host_only\":true}}]},"
          "{\"kind\":\"model\",\"output_text\":\"done\",\"tool_calls\":[]}]";
      turbo_agent_session_config_t config = {0};
      const json_value_t *messages, *call;
      config.agent_config.model = "history-test";
      config.agent_config.provider = anthropic ? turbo_model_provider_anthropic_messages()
                                              : turbo_model_provider_openai_chat_completions();
      config.agent_config.transport_fn = history_transport;
      config.agent_config.transport_user_data = &transport_calls;
      session = turbo_agent_session_create(&config);
      check_not_null(session);
      event = json_parse(sequence, sizeof(sequence) - 1);
      check_not_null(event);
      for (size_t i = 0; i < json_array_size(event); ++i)
        check_equal(turbo_runtime_json_array_append(json_object_get(state, "events"),
                                                    json_clone(json_array_get(event, i))),
                    TURBO_RUNTIME_JSON_OK);
      before = json_serialize(state, NULL);
      check_not_null(before);
      follow_up = turbo_agent_session_create_follow_up_state_json_value(session, state, "next");
      check_not_null(follow_up);
      check_equal(turbo_agent_build_turn_request(turbo_agent_session_agent(session), state,
                                                 &request), 0);
      check_not_null(request);
      after = json_serialize(state, NULL);
      check_not_null(after);
      check_equal(after, before);
      check_equal(transport_calls, 0);
      json_free(state); state = NULL;
      turbo_agent_session_destroy(session); session = NULL;

      messages = json_object_get(follow_up, "input");
      check_equal(json_array_size(messages), 7);
      check_equal(json_get_string(json_array_get(messages, 0), "content"), "first task");
      check_equal(json_get_string(json_array_get(messages, 1), "content"), "first reply");
      check_equal(json_get_string(json_array_get(messages, 2), "content"), "second reply");
      check_equal(json_type(json_object_get(json_array_get(messages, 3), "content")), JSON_NULL);
      call = json_array_get(json_object_get(json_array_get(messages, 3), "tool_calls"), 0);
      check_equal(json_get_string(call, "id"), "call_1");
      check_equal(json_get_string(json_object_get(call, "function"), "name"), "fs.edit");
      check_equal(json_get_string(json_array_get(messages, 4), "tool_call_id"), "call_1");
      check_equal(json_get_string(json_array_get(messages, 4), "content"),
                  "{\"ok\":false,\"status\":\"conflict\"}");
      check_null(json_object_get(json_array_get(messages, 4), "detail"));
      check_equal(json_get_string(json_array_get(messages, 5), "content"), "done");
      check_equal(json_get_string(json_array_get(messages, 6), "content"), "next");
      check_equal(turbo_agent_state_event_count(follow_up), 0);
    }

    it("keeps input-only states valid without an events member for %s",
       anthropic ? "Anthropic" : "Chat Completions") {
      static const char initial[] = "{\"input\":[{\"role\":\"user\",\"content\":\"first task\"}]}";
      turbo_agent_session_config_t config = {0};
      config.agent_config.model = "history-test";
      config.agent_config.provider = anthropic ? turbo_model_provider_anthropic_messages()
                                              : turbo_model_provider_openai_chat_completions();
      config.agent_config.transport_fn = history_transport;
      config.agent_config.transport_user_data = &transport_calls;
      session = turbo_agent_session_create(&config);
      check_not_null(session);
      json_free(state);
      state = json_parse(initial, sizeof(initial) - 1);
      check_not_null(state);
      follow_up = turbo_agent_session_create_follow_up_state_json_value(session, state, "next");
      check_not_null(follow_up);
      check_equal(json_array_size(json_object_get(follow_up, "input")), 2);
      check_equal(turbo_agent_build_turn_request(turbo_agent_session_agent(session), state,
                                                 &request), 0);
      check_not_null(request);
      check_null(json_object_get(state, "events"));
      check_equal(transport_calls, 0);
    }
  }
}
