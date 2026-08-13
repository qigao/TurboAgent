#include "tinytest.h"
#include "turbo_event_log.h"
#include "turbo_model_provider.h"
#include "turbo_prompt.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  int count;
  char *kind;
  char *response_id;
  char *output_text;
  char *first_tool_arguments;
  size_t tool_call_count;
} model_provider_event_capture_t;

static char *test_strdup_local(const char *text) {
  size_t len;
  char *copy;

  if (!text) {
    return NULL;
  }

  len = strlen(text) + 1;
  copy = (char *)malloc(len);
  if (!copy) {
    return NULL;
  }

  memcpy(copy, text, len);
  return copy;
}

static void capture_model_provider_event(const json_value_t *event,
                                         void *user_data) {
  model_provider_event_capture_t *capture = (model_provider_event_capture_t *)user_data;

  check_not_null(event);
  check_not_null(capture);
  capture->count++;
  free(capture->kind);
  free(capture->response_id);
  free(capture->output_text);
  free(capture->first_tool_arguments);
  capture->kind = test_strdup_local(
      turbo_runtime_json_value_as_string(turbo_json_object_get(event, "kind")));
  capture->response_id = test_strdup_local(turbo_runtime_json_value_as_string(
      turbo_json_object_get(event, "response_id")));
  capture->output_text = test_strdup_local(turbo_runtime_json_value_as_string(
      turbo_json_object_get(event, "output_text")));
  capture->tool_call_count =
      turbo_runtime_json_value_size(turbo_json_object_get(event, "tool_calls"));
  if (capture->tool_call_count > 0) {
    const json_value_t *tool_call = turbo_json_array_get(
        turbo_json_object_get(event, "tool_calls"), 0);
    capture->first_tool_arguments = test_strdup_local(turbo_runtime_json_value_as_string(
        turbo_json_object_get(tool_call, "arguments")));
  } else {
    capture->first_tool_arguments = NULL;
  }
}

spec("turbo model provider helpers") {

  describe("provider lookup") {

    it("should resolve builtin provider aliases") {
      check_ptr_eq(turbo_model_provider_by_name("openai"), turbo_model_provider_openai_responses());
      check_ptr_eq(turbo_model_provider_by_name("responses"),
                   turbo_model_provider_openai_responses());
      check_ptr_eq(turbo_model_provider_by_name("chat_completions"),
                   turbo_model_provider_openai_chat_completions());
      check_ptr_eq(turbo_model_provider_by_name("openai_compatible_chat"),
                   turbo_model_provider_openai_compatible_chat_completions());
      check_ptr_eq(turbo_model_provider_by_name("anthropic"),
                   turbo_model_provider_anthropic_messages());
      check_ptr_eq(turbo_model_provider_by_name("missing"), NULL);
    }
  }

  describe("provider facts") {

    it("should classify legacy chat and anthropic providers") {
      check_false(turbo_model_provider_is_legacy_chat(turbo_model_provider_openai_responses()));
      check_true(
          turbo_model_provider_is_legacy_chat(turbo_model_provider_openai_chat_completions()));
      check_true(turbo_model_provider_is_legacy_chat(
          turbo_model_provider_openai_compatible_chat_completions()));
      check_false(turbo_model_provider_is_legacy_chat(turbo_model_provider_anthropic_messages()));

      check_false(turbo_model_provider_is_anthropic_messages(
          turbo_model_provider_openai_chat_completions()));
      check_true(
          turbo_model_provider_is_anthropic_messages(turbo_model_provider_anthropic_messages()));
    }

    it("should select provider-specific env defaults") {
      check_str_eq(turbo_model_provider_select_api_key(turbo_model_provider_anthropic_messages(),
                                                       "sk-openai", "sk-anthropic"),
                   "sk-anthropic");
      check_str_eq(turbo_model_provider_select_api_key(turbo_model_provider_openai_responses(),
                                                       "sk-openai", "sk-anthropic"),
                   "sk-openai");

      check_str_eq(turbo_model_provider_select_base_url(
                       turbo_model_provider_anthropic_messages(), "https://openai.example/v1",
                       "https://anthropic.example", "https://api.openai.com/v1"),
                   "https://anthropic.example");
      check_str_eq(turbo_model_provider_select_base_url(
                       turbo_model_provider_openai_responses(), "https://openai.example/v1",
                       "https://anthropic.example", "https://api.openai.com/v1"),
                   "https://openai.example/v1");
      check_str_eq(turbo_model_provider_select_base_url(turbo_model_provider_openai_responses(),
                                                        NULL, NULL, "https://api.openai.com/v1"),
                   "https://api.openai.com/v1");
      check_str_eq(turbo_model_provider_select_endpoint_path(
                       turbo_model_provider_anthropic_messages(), NULL),
                   "messages");
      check_str_eq(turbo_model_provider_select_endpoint_path(
                       turbo_model_provider_openai_chat_completions(), "custom/path"),
                   "custom/path");
    }

    it("should build provider-specific wire messages from canonical messages") {
      json_value_t *messages = turbo_prompt_messages_create_json_value();
      json_value_t *chat_messages;
      json_value_t *responses_messages;
      json_value_t *anthropic_messages;
      char *system_text = NULL;

      check_not_null(messages);
      check_int_eq(turbo_runtime_json_array_append(
                       messages, turbo_prompt_message_create_json_value("system", "Rules")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_array_append(
                       messages, turbo_prompt_message_create_json_value("user", "Ping")),
                   TURBO_RUNTIME_JSON_OK);

      chat_messages = turbo_model_provider_messages_to_wire_json(
          turbo_model_provider_openai_chat_completions(), messages, NULL);
      check_not_null(chat_messages);
      check_size_eq(turbo_json_array_size(chat_messages), 2);
      check_str_eq(turbo_json_get_string(turbo_json_array_get(chat_messages, 0), "role"),
                   "system");

      responses_messages = turbo_model_provider_messages_to_wire_json(
          turbo_model_provider_openai_responses(), messages, NULL);
      check_not_null(responses_messages);
      check_size_eq(turbo_json_array_size(responses_messages), 2);
      check_str_eq(turbo_json_get_string(turbo_json_array_get(responses_messages, 1), "content"),
                   "Ping");

      anthropic_messages = turbo_model_provider_messages_to_wire_json(
          turbo_model_provider_anthropic_messages(), messages, &system_text);
      check_not_null(anthropic_messages);
      check_str_eq(system_text, "Rules");
      check_size_eq(turbo_json_array_size(anthropic_messages), 1);
      check_str_eq(turbo_json_get_string(turbo_json_array_get(anthropic_messages, 0), "role"),
                   "user");

      free(system_text);
      turbo_free_json(&anthropic_messages);
      turbo_free_json(&responses_messages);
      turbo_free_json(&chat_messages);
      turbo_runtime_json_destroy(messages);
    }

    it("should normalize provider responses into canonical model events") {
      json_value_t *responses_response = turbo_json_create_object();
      json_value_t *responses_output = turbo_json_create_array();
      json_value_t *responses_text_item = turbo_json_create_object();
      json_value_t *responses_content = turbo_json_create_array();
      json_value_t *responses_text_part = turbo_json_create_object();
      json_value_t *responses_tool_call = turbo_json_create_object();
      json_value_t *chat_response = turbo_json_create_object();
      json_value_t *chat_choices = turbo_json_create_array();
      json_value_t *chat_choice = turbo_json_create_object();
      json_value_t *chat_message = turbo_json_create_object();
      json_value_t *chat_tool_calls = turbo_json_create_array();
      json_value_t *chat_tool_call = turbo_json_create_object();
      json_value_t *chat_function = turbo_json_create_object();
      json_value_t *anthropic_response = turbo_json_create_object();
      json_value_t *anthropic_content = turbo_json_create_array();
      json_value_t *anthropic_text = turbo_json_create_object();
      json_value_t *anthropic_tool_use = turbo_json_create_object();
      json_value_t *anthropic_input = turbo_json_create_object();
      json_value_t *responses_event;
      json_value_t *chat_event;
      json_value_t *anthropic_event;

      check_not_null(responses_response);
      check_not_null(responses_output);
      check_not_null(responses_text_item);
      check_not_null(responses_content);
      check_not_null(responses_text_part);
      check_not_null(responses_tool_call);
      check_not_null(chat_response);
      check_not_null(chat_choices);
      check_not_null(chat_choice);
      check_not_null(chat_message);
      check_not_null(chat_tool_calls);
      check_not_null(chat_tool_call);
      check_not_null(chat_function);
      check_not_null(anthropic_response);
      check_not_null(anthropic_content);
      check_not_null(anthropic_text);
      check_not_null(anthropic_tool_use);
      check_not_null(anthropic_input);

      turbo_json_object_set_string(responses_response, "id", "resp_1");
      turbo_json_object_set_string(responses_text_part, "type", "output_text");
      turbo_json_object_set_string(responses_text_part, "text", "hello");
      turbo_json_array_add(responses_content, responses_text_part);
      turbo_json_object_set_string(responses_text_item, "type", "message");
      turbo_json_object_add(responses_text_item, "content", responses_content);
      turbo_json_array_add(responses_output, responses_text_item);
      turbo_json_object_set_string(responses_tool_call, "type", "function_call");
      turbo_json_object_set_string(responses_tool_call, "call_id", "call_1");
      turbo_json_object_set_string(responses_tool_call, "name", "sum");
      turbo_json_object_set_string(responses_tool_call, "arguments", "{\"a\":1}");
      turbo_json_array_add(responses_output, responses_tool_call);
      turbo_json_object_add(responses_response, "output", responses_output);

      turbo_json_object_set_string(chat_response, "id", "chat_1");
      turbo_json_object_set_string(chat_message, "role", "assistant");
      turbo_json_object_set_string(chat_message, "content", "world");
      turbo_json_object_set_string(chat_tool_call, "id", "call_2");
      turbo_json_object_set_string(chat_tool_call, "type", "function");
      turbo_json_object_set_string(chat_function, "name", "echo");
      turbo_json_object_set_string(chat_function, "arguments", "{\"x\":2}");
      turbo_json_object_add(chat_tool_call, "function", chat_function);
      turbo_json_array_add(chat_tool_calls, chat_tool_call);
      turbo_json_object_add(chat_message, "tool_calls", chat_tool_calls);
      turbo_json_object_add(chat_choice, "message", chat_message);
      turbo_json_array_add(chat_choices, chat_choice);
      turbo_json_object_add(chat_response, "choices", chat_choices);

      turbo_json_object_set_string(anthropic_response, "id", "anth_1");
      turbo_json_object_set_string(anthropic_text, "type", "text");
      turbo_json_object_set_string(anthropic_text, "text", "anth");
      turbo_json_array_add(anthropic_content, anthropic_text);
      turbo_json_object_set_string(anthropic_tool_use, "type", "tool_use");
      turbo_json_object_set_string(anthropic_tool_use, "id", "call_3");
      turbo_json_object_set_string(anthropic_tool_use, "name", "grep");
      turbo_json_object_set_string(anthropic_input, "pattern", "x");
      turbo_json_object_add(anthropic_tool_use, "input", anthropic_input);
      turbo_json_array_add(anthropic_content, anthropic_tool_use);
      turbo_json_object_add(anthropic_response, "content", anthropic_content);

      responses_event = turbo_model_provider_response_to_event_json(
          turbo_model_provider_openai_responses(), responses_response);
      chat_event = turbo_model_provider_response_to_event_json(
          turbo_model_provider_openai_chat_completions(), chat_response);
      anthropic_event = turbo_model_provider_response_to_event_json(
          turbo_model_provider_anthropic_messages(), anthropic_response);

      check_not_null(responses_event);
      check_not_null(chat_event);
      check_not_null(anthropic_event);
      check_str_eq(turbo_json_get_string(responses_event, "kind"), "model");
      check_str_eq(turbo_json_get_string(responses_event, "output_text"), "hello");
      check_size_eq(turbo_json_array_size(turbo_json_object_get(responses_event, "tool_calls")), 1);
      check_str_eq(turbo_json_get_string(chat_event, "output_text"), "world");
      check_size_eq(turbo_json_array_size(turbo_json_object_get(chat_event, "tool_calls")), 1);
      check_str_eq(turbo_json_get_string(anthropic_event, "output_text"), "anth");
      check_size_eq(turbo_json_array_size(turbo_json_object_get(anthropic_event, "tool_calls")), 1);

      turbo_free_json(&anthropic_event);
      turbo_free_json(&chat_event);
      turbo_free_json(&responses_event);
      turbo_free_json(&anthropic_response);
      turbo_free_json(&chat_response);
      turbo_free_json(&responses_response);
    }

    it("should normalize provider sse payloads into canonical model events") {
      const char *chat_sse =
          "data: {\"id\":\"chat_1\",\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"hi\"}}]}\n\n"
          "data: {\"id\":\"chat_1\",\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
          "data: [DONE]\n\n";
      const char *responses_sse =
          "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"yo\"}]}}\n\n"
          "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\"}}\n\n"
          "data: [DONE]\n\n";
      const char *anthropic_sse =
          "data: {\"type\":\"message_start\",\"message\":{\"id\":\"anth_1\",\"role\":\"assistant\"}}\n\n"
          "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"an\"}}\n\n"
          "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"th\"}}\n\n"
          "data: {\"type\":\"message_stop\"}\n\n";
      json_value_t *chat_event = turbo_model_provider_sse_to_event_json(
          turbo_model_provider_openai_chat_completions(), chat_sse, strlen(chat_sse));
      json_value_t *responses_event = turbo_model_provider_sse_to_event_json(
          turbo_model_provider_openai_responses(), responses_sse, strlen(responses_sse));
      json_value_t *anthropic_event = turbo_model_provider_sse_to_event_json(
          turbo_model_provider_anthropic_messages(), anthropic_sse, strlen(anthropic_sse));

      check_not_null(chat_event);
      check_not_null(responses_event);
      check_not_null(anthropic_event);
      check_str_eq(turbo_json_get_string(chat_event, "output_text"), "hi");
      check_str_eq(turbo_json_get_string(responses_event, "output_text"), "yo");
      check_str_eq(turbo_json_get_string(anthropic_event, "output_text"), "anth");

      turbo_free_json(&anthropic_event);
      turbo_free_json(&responses_event);
      turbo_free_json(&chat_event);
    }

    it("should emit TurboParser JSON-native events from provider sse payloads") {
      const char *chat_sse =
          "data: {\"id\":\"chat_1\",\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
          "data: [DONE]\n\n";
      model_provider_event_capture_t capture = {0};

      check_int_eq(turbo_model_provider_sse_emit_json_value(
                       turbo_model_provider_openai_chat_completions(), chat_sse, strlen(chat_sse),
                       capture_model_provider_event, &capture),
                    0);
      check_int_eq(capture.count, 1);
      check_str_eq(capture.kind, "model");
      check_str_eq(capture.response_id, "chat_1");
      check_str_eq(capture.output_text, "hi");
      free(capture.first_tool_arguments);
      free(capture.output_text);
      free(capture.response_id);
      free(capture.kind);
    }

    it("should capture provider sse events into an event log") {
      const char *responses_sse =
          "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_log\",\"output\":[]}}\n\n"
          "data: {\"type\":\"response.output_text.delta\",\"delta\":\"4\"}\n\n"
          "data: {\"type\":\"response.output_text.delta\",\"delta\":\"2\"}\n\n"
          "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_log\",\"output\":[]}}\n\n"
          "data: [DONE]\n\n";
      turbo_event_log_t *log = turbo_event_log_create();
      const json_value_t *last_event;

      check_not_null(log);
      check_int_eq(turbo_model_provider_sse_emit_json_value(turbo_model_provider_openai_responses(),
                                                      responses_sse, strlen(responses_sse),
                                                      turbo_event_log_capture_json_value, log),
                   0);
      check_int_eq(turbo_event_log_status(log), TURBO_EVENT_LOG_OK);
      check_size_eq(turbo_event_log_size(log), 2);

      last_event = turbo_event_log_get(log, turbo_event_log_size(log) - 1);
      check_not_null(last_event);
      check_str_eq(turbo_event_kind_json_value(last_event), "model");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(last_event, "response_id")),
                   "resp_log");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(last_event, "output_text")),
                   "42");

      turbo_event_log_destroy(log);
    }

    it("should emit incremental TurboParser JSON-native events from multi-frame provider sse payloads") {
      const char *responses_sse =
          "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_stream\",\"output\":[]}}\n\n"
          "data: {\"type\":\"response.output_text.delta\",\"delta\":\"4\"}\n\n"
          "data: {\"type\":\"response.output_text.delta\",\"delta\":\"2\"}\n\n"
          "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_stream\",\"output\":[]}}\n\n"
          "data: [DONE]\n\n";
      const char *anthropic_sse =
          "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_text\",\"role\":\"assistant\",\"content\":[]}}\n\n"
          "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"o\"}}\n\n"
          "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"k\"}}\n\n"
          "data: {\"type\":\"message_stop\"}\n\n";
      model_provider_event_capture_t responses_capture = {0};
      model_provider_event_capture_t anthropic_capture = {0};

      check_int_eq(turbo_model_provider_sse_emit_json_value(turbo_model_provider_openai_responses(),
                                                      responses_sse, strlen(responses_sse),
                                                      capture_model_provider_event,
                                                      &responses_capture),
                   0);
      check_int_eq(responses_capture.count, 2);
      check_str_eq(responses_capture.kind, "model");
      check_str_eq(responses_capture.response_id, "resp_stream");
      check_str_eq(responses_capture.output_text, "42");

      check_int_eq(turbo_model_provider_sse_emit_json_value(turbo_model_provider_anthropic_messages(),
                                                      anthropic_sse, strlen(anthropic_sse),
                                                      capture_model_provider_event,
                                                      &anthropic_capture),
                   0);
      check_int_eq(anthropic_capture.count, 2);
      check_str_eq(anthropic_capture.kind, "model");
      check_str_eq(anthropic_capture.response_id, "msg_text");
      check_str_eq(anthropic_capture.output_text, "ok");

      free(anthropic_capture.output_text);
      free(anthropic_capture.response_id);
      free(anthropic_capture.first_tool_arguments);
      free(anthropic_capture.kind);
      free(responses_capture.output_text);
      free(responses_capture.response_id);
      free(responses_capture.first_tool_arguments);
      free(responses_capture.kind);
    }

    it("should emit incremental TurboParser JSON-native tool call events from provider sse payloads") {
      const char *responses_sse =
          "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_tool_stream\",\"output\":[]}}\n\n"
          "data: {\"type\":\"response.output_item.added\",\"item\":{\"type\":\"function_call\",\"call_id\":\"call_1\",\"name\":\"sum\",\"arguments\":\"\"}}\n\n"
          "data: {\"type\":\"response.function_call_arguments.delta\",\"delta\":\"{\\\"a\\\":2\"}\n\n"
          "data: {\"type\":\"response.function_call_arguments.delta\",\"delta\":\",\\\"b\\\":3}\"}\n\n"
          "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"function_call\",\"call_id\":\"call_1\",\"name\":\"sum\",\"arguments\":\"{\\\"a\\\":2,\\\"b\\\":3}\"}}\n\n"
          "data: [DONE]\n\n";
      const char *anthropic_sse =
          "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_tool\",\"role\":\"assistant\",\"content\":[]}}\n\n"
          "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"call_1\",\"name\":\"sum\",\"input\":{}}}\n\n"
          "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"a\\\":2\"}}\n\n"
          "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\",\\\"b\\\":3}\"}}\n\n"
          "data: {\"type\":\"message_stop\"}\n\n";
      model_provider_event_capture_t responses_capture = {0};
      model_provider_event_capture_t anthropic_capture = {0};

      check_int_eq(turbo_model_provider_sse_emit_json_value(turbo_model_provider_openai_responses(),
                                                      responses_sse, strlen(responses_sse),
                                                      capture_model_provider_event,
                                                      &responses_capture),
                   0);
      check_int_eq(responses_capture.count, 3);
      check_size_eq(responses_capture.tool_call_count, 1);
      check_str_eq(responses_capture.response_id, "resp_tool_stream");
      check_str_eq(responses_capture.first_tool_arguments, "{\"a\":2,\"b\":3}");

      check_int_eq(turbo_model_provider_sse_emit_json_value(turbo_model_provider_anthropic_messages(),
                                                      anthropic_sse, strlen(anthropic_sse),
                                                      capture_model_provider_event,
                                                      &anthropic_capture),
                   0);
      check_int_eq(anthropic_capture.count, 3);
      check_size_eq(anthropic_capture.tool_call_count, 1);
      check_str_eq(anthropic_capture.response_id, "msg_tool");
      check_str_eq(anthropic_capture.first_tool_arguments, "{\"a\":2,\"b\":3}");

      free(anthropic_capture.output_text);
      free(anthropic_capture.response_id);
      free(anthropic_capture.first_tool_arguments);
      free(anthropic_capture.kind);
      free(responses_capture.output_text);
      free(responses_capture.response_id);
      free(responses_capture.first_tool_arguments);
      free(responses_capture.kind);
    }

    it("should emit TurboParser JSON-native events from provider responses") {
      json_value_t *chat_response = turbo_json_create_object();
      json_value_t *choices = turbo_json_create_array();
      json_value_t *choice = turbo_json_create_object();
      json_value_t *message = turbo_json_create_object();
      model_provider_event_capture_t capture = {0};

      turbo_json_object_set_string(chat_response, "id", "chat_1");
      turbo_json_object_set_string(message, "content", "world");
      turbo_json_object_add(choice, "message", message);
      turbo_json_array_add(choices, choice);
      turbo_json_object_add(chat_response, "choices", choices);

      check_int_eq(turbo_model_provider_response_emit_json_value(
                       turbo_model_provider_openai_chat_completions(), chat_response,
                       capture_model_provider_event, &capture),
                   0);
      check_int_eq(capture.count, 1);
      check_str_eq(capture.kind, "model");

      free(capture.output_text);
      free(capture.response_id);
      free(capture.first_tool_arguments);
      free(capture.kind);
      turbo_free_json(&chat_response);
    }

    it("should normalize provider responses directly into TurboParser JSON-native model events") {
      json_value_t *responses_response = turbo_json_create_object();
      json_value_t *responses_output = turbo_json_create_array();
      json_value_t *responses_text_item = turbo_json_create_object();
      json_value_t *responses_content = turbo_json_create_array();
      json_value_t *responses_text_part = turbo_json_create_object();
      json_value_t *responses_tool_call = turbo_json_create_object();
      json_value_t *event = NULL;
      const json_value_t *tool_calls;
      const json_value_t *first_call;

      check_not_null(responses_response);
      check_not_null(responses_output);
      check_not_null(responses_text_item);
      check_not_null(responses_content);
      check_not_null(responses_text_part);
      check_not_null(responses_tool_call);

      turbo_json_object_set_string(responses_response, "id", "resp_json_value");
      turbo_json_object_set_string(responses_text_part, "type", "output_text");
      turbo_json_object_set_string(responses_text_part, "text", "hello");
      turbo_json_array_add(responses_content, responses_text_part);
      turbo_json_object_set_string(responses_text_item, "type", "message");
      turbo_json_object_add(responses_text_item, "content", responses_content);
      turbo_json_array_add(responses_output, responses_text_item);
      turbo_json_object_set_string(responses_tool_call, "type", "function_call");
      turbo_json_object_set_string(responses_tool_call, "call_id", "call_json_value");
      turbo_json_object_set_string(responses_tool_call, "name", "sum");
      turbo_json_object_set_string(responses_tool_call, "arguments", "{\"a\":1}");
      turbo_json_array_add(responses_output, responses_tool_call);
      turbo_json_object_add(responses_response, "output", responses_output);

      event = turbo_model_provider_response_to_event_json_value(
          turbo_model_provider_openai_responses(), responses_response);
      check_not_null(event);
      check_str_eq(turbo_event_kind_json_value(event), "model");
      check_str_eq(
          turbo_runtime_json_value_as_string(
              turbo_json_object_get(event, "response_id")),
          "resp_json_value");
      check_str_eq(
          turbo_runtime_json_value_as_string(
              turbo_json_object_get(event, "output_text")),
          "hello");

      tool_calls = turbo_json_object_get(event, "tool_calls");
      check_not_null(tool_calls);
      check_size_eq(turbo_runtime_json_value_size(tool_calls), 1);
      first_call = turbo_json_array_get(tool_calls, 0);
      check_str_eq(
          turbo_runtime_json_value_as_string(
              turbo_json_object_get(first_call, "name")),
          "sum");

      turbo_runtime_json_destroy(event);
      turbo_free_json(&responses_response);
    }
  }
}
