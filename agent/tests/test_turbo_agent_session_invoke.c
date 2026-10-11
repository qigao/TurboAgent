#include "tinytest.h"
#include "turbo_agent_session.h"
#include "turbo_agent_state.h"
#include "turbo_prompt.h"

#include <stdlib.h>
#include <string.h>

typedef struct invoke_capture_s {
  const char *response;
  int calls;
} invoke_capture_t;

static int invoke_transport(const char *request, char **out_response, void *data) {
  invoke_capture_t *capture = data;
  size_t size;
  ++capture->calls;
  if (strstr(request, "force-failure")) return -1;
  size = strlen(capture->response) + 1;
  *out_response = malloc(size);
  if (!*out_response) return -1;
  memcpy(*out_response, capture->response, size);
  return 0;
}

spec("Session invocation results") {
  static const char json_response[] =
      "{\"id\":\"response\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"{\\\"value\\\":7}\"}]}]}";
  static const char text_response[] =
      "{\"id\":\"response\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"not JSON\"}]}]}";
  static turbo_agent_session_t *session;
  static invoke_capture_t capture;
  static json_value_t *state, *messages, *result, *second, *summary;
  static char *text;

  before_each() {
    turbo_agent_session_config_t config = {0};
    capture.response = json_response;
    capture.calls = 0;
    config.agent_config.model = "test-model";
    config.agent_config.transport_fn = invoke_transport;
    config.agent_config.transport_user_data = &capture;
    session = turbo_agent_session_create(&config);
    messages = turbo_prompt_messages_create_json_value();
    check_not_null(session);
    check_not_null(messages);
    check_equal(turbo_prompt_messages_append(messages, "user", "hello"), TURBO_PROMPT_OK);
  }

  after_each() {
    turbo_agent_session_destroy(session);
    session = NULL;
    json_free(state);
    json_free(messages);
    json_free(result);
    json_free(second);
    json_free(summary);
    state = messages = result = second = summary = NULL;
    free(text);
    text = NULL;
  }

  it("copies final text independently of the result state") {
    state = turbo_agent_state_create();
    check_not_null(state);
    check_null(turbo_agent_session_result_text(state));
    check_null(turbo_agent_session_result_text(NULL));
    check_equal(turbo_agent_state_set_final_answer(state, "original answer"), 0);
    text = turbo_agent_session_result_text(state);
    check_not_null(text);
    check_equal(turbo_agent_state_set_final_answer(state, "new answer"), 0);
    json_free(state);
    state = NULL;
    check_equal(text, "original answer");
  }

  it("returns parsed JSON with independent ownership for text and canonical-message input") {
    check_equal(turbo_agent_session_invoke_json(session, "hello", NULL, &result, &summary), 0);
    check_equal(json_get_string(summary, "status"), "completed");
    json_free(summary);
    summary = NULL;
    check_equal(
        turbo_agent_session_invoke_messages_json(session, messages, NULL, &second, &summary), 0);
    check_equal(capture.calls, 2);
    check_equal(turbo_runtime_json_object_set_int64(result, "value", 99), TURBO_RUNTIME_JSON_OK);
    turbo_agent_session_destroy(session);
    session = NULL;
    json_free(messages);
    messages = NULL;
    check_equal(json_get_int(result, "value", 0), 99);
    check_equal(json_get_int(second, "value", 0), 7);
    check_equal(json_get_string(summary, "status"), "completed");
  }

  it("clears result and summary on JSON conversion failure for both input forms") {
    capture.response = text_response;
    check_equal(turbo_agent_session_invoke_json(session, "hello", NULL, &result, &summary), -1);
    check_null(result);
    check_null(summary);
    check_equal(
        turbo_agent_session_invoke_messages_json(session, messages, NULL, &result, &summary), -1);
    check_null(result);
    check_null(summary);
    check_equal(capture.calls, 2);
    check_not_null(turbo_agent_session_last_run_id(session));
  }

  it("discards an interrupted summary when no final text can be returned") {
    static const char *before[] = {"model"};
    turbo_graph_run_options_t options = {0};
    options.interrupt_before_nodes = before;
    options.interrupt_before_count = 1;
    check_equal(turbo_agent_session_invoke_text(session, "hello", &options, &text, &summary), -1);
    check_null(text);
    check_null(summary);
    check_equal(
        turbo_agent_session_invoke_messages_text(session, messages, &options, &text, &summary), -1);
    check_null(text);
    check_null(summary);
    check_equal(capture.calls, 0);
    check_not_null(turbo_agent_session_last_checkpoint_id(session));
  }

  it("continues a sequential batch after invalid input and invocation failure") {
    const char *inputs[] = {"first", NULL, "force-failure", "last"};
    const json_value_t *item;
    check_equal(turbo_agent_session_batch_text(session, inputs, 4, NULL, &result), 0);
    check_equal(capture.calls, 3);
    check_equal(json_array_size(result), 4);
    item = json_array_get(result, 1);
    check_equal(json_get_int(item, "index", -1), 1);
    check_false(json_get_bool(item, "ok", true));
    check_equal(json_get_string(item, "error"), "invalid input");
    check_true(json_is_null(json_object_get(item, "summary")));
    item = json_array_get(result, 2);
    check_equal(json_get_int(item, "index", -1), 2);
    check_false(json_get_bool(item, "ok", true));
    check_equal(json_get_string(item, "error"), "invoke failed");
    check_true(json_is_null(json_object_get(item, "output_text")));
    check_true(json_is_null(json_object_get(item, "summary")));
    turbo_agent_session_destroy(session);
    session = NULL;
    item = json_array_get(result, 3);
    check_true(json_get_bool(item, "ok", false));
    check_equal(json_get_int(item, "index", -1), 3);
    check_equal(json_get_string(item, "output_text"), "{\"value\":7}");
    check_equal(json_get_string(json_object_get(item, "summary"), "status"), "completed");
    check_true(json_is_null(json_object_get(item, "error")));
  }
}
