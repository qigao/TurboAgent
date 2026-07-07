#include "tinytest.h"
#include "turbo_prompt.h"

#include <stdlib.h>

spec("turbo prompt helpers") {

  describe("template rendering") {

    it("should render placeholders from an input object") {
      json_value_t *input = turbo_json_create_object();
      char *rendered;

      check_not_null(input);
      turbo_json_object_set_string(input, "task", "inspect code");
      turbo_json_object_set_number(input, "count", 2);

      rendered = turbo_prompt_render_template("Do {{task}} in {{count}} steps.", input);
      check_not_null(rendered);
      check_str_eq(rendered, "Do inspect code in 2 steps.");

      free(rendered);
      turbo_free_json(&input);
    }

    it("should leave unknown placeholders empty") {
      char *rendered = turbo_prompt_render_template("Hello {{name}}.", NULL);

      check_not_null(rendered);
      check_str_eq(rendered, "Hello .");

      free(rendered);
    }

    it("should render placeholders from a runtime data-bind object") {
      turbo_runtime_data_bind_value_t *input = turbo_runtime_data_bind_value_create_object();
      turbo_runtime_data_bind_value_t *task =
          turbo_runtime_data_bind_value_create_string("inspect code");
      turbo_runtime_data_bind_value_t *count = turbo_runtime_data_bind_value_create_int64(2);
      char *rendered;

      check_not_null(input);
      check_not_null(task);
      check_not_null(count);
      check_int_eq(turbo_runtime_data_bind_object_set(input, "task", task),
                   TURBO_RUNTIME_DATA_BIND_OK);
      check_int_eq(turbo_runtime_data_bind_object_set(input, "count", count),
                   TURBO_RUNTIME_DATA_BIND_OK);

      rendered = turbo_prompt_render_template_bind("Do {{task}} in {{count}} steps.", input);
      check_not_null(rendered);
      check_str_eq(rendered, "Do inspect code in 2 steps.");

      free(rendered);
      turbo_runtime_data_bind_value_destroy(input);
    }
  }

  describe("message creation") {

    it("should append role-content messages into an array") {
      json_value_t *messages = turbo_json_create_array();
      json_value_t *message;

      check_not_null(messages);
      check_int_eq(turbo_prompt_messages_append(messages, "user", "Ping"), TURBO_PROMPT_OK);
      check_size_eq(turbo_json_array_size(messages), 1);

      message = turbo_json_array_get(messages, 0);
      check_str_eq(turbo_json_get_string(message, "role"), "user");
      check_str_eq(turbo_json_get_string(message, "content"), "Ping");

      turbo_free_json(&messages);
    }

    it("should keep arbitrary content values when building a message") {
      json_value_t *content = turbo_json_create_string("plain text");
      json_value_t *message = turbo_prompt_message_with_content_create("user", content);

      check_not_null(message);
      check_str_eq(turbo_json_get_string(message, "role"), "user");
      check_str_eq(turbo_json_get_string(message, "content"), "plain text");

      turbo_free_json(&message);
    }

    it("should append role-content messages into a runtime data-bind array") {
      turbo_runtime_data_bind_value_t *messages = turbo_prompt_messages_create_bind();
      const turbo_runtime_data_bind_value_t *message;

      check_not_null(messages);
      check_int_eq(turbo_prompt_messages_append_bind(messages, "user", "Ping"),
                   TURBO_PROMPT_OK);
      check_size_eq(turbo_runtime_data_bind_value_size(messages), 1);

      message = turbo_runtime_data_bind_array_get(messages, 0);
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(message, "role")),
                   "user");
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(message, "content")),
                   "Ping");

      turbo_runtime_data_bind_value_destroy(messages);
    }

    it("should expose a canonical bind-native message schema") {
      turbo_runtime_data_bind_value_t *schema = turbo_prompt_message_schema_bind();
      const turbo_runtime_data_bind_value_t *properties;

      check_not_null(schema);
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(schema, "type")),
                   "object");
      properties = turbo_runtime_data_bind_object_get(schema, "properties");
      check_not_null(properties);
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(
                           turbo_runtime_data_bind_object_get(properties, "role"), "type")),
                   "string");
      check_false(turbo_runtime_data_bind_value_as_bool(
          turbo_runtime_data_bind_object_get(schema, "additionalProperties"), 1));

      turbo_runtime_data_bind_value_destroy(schema);
    }

    it("should expose a canonical bind-native tool-call schema") {
      turbo_runtime_data_bind_value_t *schema = turbo_prompt_tool_call_schema_bind();
      const turbo_runtime_data_bind_value_t *properties;

      check_not_null(schema);
      properties = turbo_runtime_data_bind_object_get(schema, "properties");
      check_not_null(properties);
      check_not_null(turbo_runtime_data_bind_object_get(properties, "id"));
      check_not_null(turbo_runtime_data_bind_object_get(properties, "function"));

      turbo_runtime_data_bind_value_destroy(schema);
    }

    it("should expose a canonical bind-native content-part schema") {
      turbo_runtime_data_bind_value_t *schema = turbo_prompt_content_part_schema_bind();
      const turbo_runtime_data_bind_value_t *properties;

      check_not_null(schema);
      properties = turbo_runtime_data_bind_object_get(schema, "properties");
      check_not_null(properties);
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(
                           turbo_runtime_data_bind_object_get(properties, "type"), "type")),
                   "string");
      check_not_null(turbo_runtime_data_bind_object_get(properties, "text"));

      turbo_runtime_data_bind_value_destroy(schema);
    }

    it("should validate bind-native prompt messages") {
      turbo_runtime_data_bind_value_t *message =
          turbo_prompt_message_create_bind("user", "Ping");
      turbo_runtime_data_bind_value_t *invalid =
          turbo_runtime_data_bind_value_create_object();

      check_not_null(message);
      check_not_null(invalid);
      check_int_eq(turbo_prompt_message_validate_bind(message), TURBO_PROMPT_OK);
      check_int_eq(turbo_prompt_message_validate_bind(invalid),
                   TURBO_PROMPT_INVALID_ARGUMENT);

      turbo_runtime_data_bind_value_destroy(invalid);
      turbo_runtime_data_bind_value_destroy(message);
    }

    it("should validate assistant messages with tool calls") {
      turbo_runtime_data_bind_value_t *tool_calls = turbo_runtime_data_bind_value_create_array();
      turbo_runtime_data_bind_value_t *tool_call =
          turbo_prompt_chat_tool_call_create_bind("call_1", "function", "sum", "{\"a\":1}");
      turbo_runtime_data_bind_value_t *message;

      check_not_null(tool_calls);
      check_not_null(tool_call);
      check_int_eq(turbo_runtime_data_bind_array_append(tool_calls, tool_call),
                   TURBO_RUNTIME_DATA_BIND_OK);
      message = turbo_prompt_chat_assistant_message_create_bind("", tool_calls);
      check_not_null(message);
      check_int_eq(turbo_prompt_message_validate_bind(message), TURBO_PROMPT_OK);

      turbo_runtime_data_bind_value_destroy(message);
    }

    it("should validate tool messages with tool_call_id") {
      turbo_runtime_data_bind_value_t *message =
          turbo_prompt_chat_tool_message_create_bind("call_1", "42");

      check_not_null(message);
      check_int_eq(turbo_prompt_message_validate_bind(message), TURBO_PROMPT_OK);

      turbo_runtime_data_bind_value_destroy(message);
    }

    it("should validate messages with rich content parts") {
      turbo_runtime_data_bind_value_t *parts = turbo_runtime_data_bind_value_create_array();
      turbo_runtime_data_bind_value_t *text_part =
          turbo_prompt_content_text_part_create_bind("Ping");
      turbo_runtime_data_bind_value_t *tool_result =
          turbo_prompt_tool_result_part_create_bind("call_1", "42");
      turbo_runtime_data_bind_value_t *message = NULL;

      check_not_null(parts);
      check_not_null(text_part);
      check_not_null(tool_result);
      check_int_eq(turbo_runtime_data_bind_array_append(parts, text_part),
                   TURBO_RUNTIME_DATA_BIND_OK);
      check_int_eq(turbo_runtime_data_bind_array_append(parts, tool_result),
                   TURBO_RUNTIME_DATA_BIND_OK);
      message = turbo_prompt_message_with_content_create_bind("user", parts);
      check_not_null(message);
      check_int_eq(turbo_prompt_message_validate_bind(message), TURBO_PROMPT_OK);

      turbo_runtime_data_bind_value_destroy(message);
    }

    it("should reject malformed tool calls") {
      turbo_runtime_data_bind_value_t *message =
          turbo_prompt_message_create_bind("assistant", "");
      turbo_runtime_data_bind_value_t *tool_calls =
          turbo_runtime_data_bind_value_create_array();
      turbo_runtime_data_bind_value_t *tool_call =
          turbo_runtime_data_bind_value_create_object();
      turbo_runtime_data_bind_value_t *function =
          turbo_runtime_data_bind_value_create_object();

      check_not_null(message);
      check_not_null(tool_calls);
      check_not_null(tool_call);
      check_not_null(function);
      check_int_eq(
          turbo_runtime_data_bind_object_set(tool_call, "id",
                                             turbo_runtime_data_bind_value_create_string("call_1")),
          TURBO_RUNTIME_DATA_BIND_OK);
      check_int_eq(
          turbo_runtime_data_bind_object_set(tool_call, "type",
                                             turbo_runtime_data_bind_value_create_string("function")),
          TURBO_RUNTIME_DATA_BIND_OK);
      check_int_eq(turbo_runtime_data_bind_object_set(tool_call, "function", function),
                   TURBO_RUNTIME_DATA_BIND_OK);
      check_int_eq(turbo_runtime_data_bind_array_append(tool_calls, tool_call),
                   TURBO_RUNTIME_DATA_BIND_OK);
      check_int_eq(turbo_runtime_data_bind_object_set(message, "tool_calls", tool_calls),
                   TURBO_RUNTIME_DATA_BIND_OK);

      check_int_eq(turbo_prompt_message_validate_bind(message),
                   TURBO_PROMPT_INVALID_ARGUMENT);

      turbo_runtime_data_bind_value_destroy(message);
    }

    it("should convert canonical messages into openai chat json") {
      turbo_runtime_data_bind_value_t *messages = turbo_prompt_messages_create_bind();
      turbo_runtime_data_bind_value_t *assistant = NULL;
      turbo_runtime_data_bind_value_t *tool_calls = turbo_runtime_data_bind_value_create_array();
      turbo_runtime_data_bind_value_t *tool_call =
          turbo_prompt_chat_tool_call_create_bind("call_1", "function", "sum", "{\"a\":1}");
      json_value_t *json_messages;

      check_not_null(messages);
      check_int_eq(turbo_runtime_data_bind_array_append(
                       messages, turbo_prompt_message_create_bind("user", "Ping")),
                   TURBO_RUNTIME_DATA_BIND_OK);
      check_not_null(tool_calls);
      check_not_null(tool_call);
      check_int_eq(turbo_runtime_data_bind_array_append(tool_calls, tool_call),
                   TURBO_RUNTIME_DATA_BIND_OK);
      assistant = turbo_prompt_chat_assistant_message_create_bind("", tool_calls);
      check_not_null(assistant);
      check_int_eq(turbo_runtime_data_bind_array_append(messages, assistant),
                   TURBO_RUNTIME_DATA_BIND_OK);

      json_messages = turbo_prompt_messages_to_openai_chat_json(messages);
      check_not_null(json_messages);
      check_size_eq(turbo_json_array_size(json_messages), 2);
      check_str_eq(turbo_json_get_string(turbo_json_array_get(json_messages, 0), "role"), "user");
      check_ptr_eq(turbo_json_object_get(turbo_json_array_get(json_messages, 1), "tool_calls") != NULL
                       ? (void *)1
                       : NULL,
                   (void *)1);

      turbo_free_json(&json_messages);
      turbo_runtime_data_bind_value_destroy(messages);
    }

    it("should convert canonical messages into openai responses input json") {
      turbo_runtime_data_bind_value_t *messages = turbo_prompt_messages_create_bind();
      json_value_t *json_messages;

      check_not_null(messages);
      check_int_eq(turbo_runtime_data_bind_array_append(
                       messages, turbo_prompt_message_create_bind("user", "Ping")),
                   TURBO_RUNTIME_DATA_BIND_OK);

      json_messages = turbo_prompt_messages_to_openai_responses_json(messages);
      check_not_null(json_messages);
      check_size_eq(turbo_json_array_size(json_messages), 1);
      check_str_eq(turbo_json_get_string(turbo_json_array_get(json_messages, 0), "role"), "user");
      check_str_eq(turbo_json_get_string(turbo_json_array_get(json_messages, 0), "content"),
                   "Ping");

      turbo_free_json(&json_messages);
      turbo_runtime_data_bind_value_destroy(messages);
    }

    it("should convert canonical messages into anthropic json") {
      turbo_runtime_data_bind_value_t *messages = turbo_prompt_messages_create_bind();
      turbo_runtime_data_bind_value_t *assistant = NULL;
      turbo_runtime_data_bind_value_t *tool_calls = turbo_runtime_data_bind_value_create_array();
      turbo_runtime_data_bind_value_t *tool_call =
          turbo_prompt_chat_tool_call_create_bind("call_1", "function", "sum", "{\"a\":1}");
      turbo_runtime_data_bind_value_t *tool_message = NULL;
      json_value_t *json_messages = NULL;
      char *system_text = NULL;

      check_not_null(messages);
      check_int_eq(turbo_runtime_data_bind_array_append(
                       messages, turbo_prompt_message_create_bind("system", "Rules")),
                   TURBO_RUNTIME_DATA_BIND_OK);

      check_not_null(tool_calls);
      check_not_null(tool_call);
      check_int_eq(turbo_runtime_data_bind_array_append(tool_calls, tool_call),
                   TURBO_RUNTIME_DATA_BIND_OK);
      assistant = turbo_prompt_chat_assistant_message_create_bind("", tool_calls);
      check_not_null(assistant);
      check_int_eq(turbo_runtime_data_bind_array_append(messages, assistant),
                   TURBO_RUNTIME_DATA_BIND_OK);

      tool_message = turbo_prompt_chat_tool_message_create_bind("call_1", "42");
      check_not_null(tool_message);
      check_int_eq(turbo_runtime_data_bind_array_append(messages, tool_message),
                   TURBO_RUNTIME_DATA_BIND_OK);

      check_int_eq(
          turbo_prompt_messages_to_anthropic_json(messages, &json_messages, &system_text),
          TURBO_PROMPT_OK);
      check_not_null(json_messages);
      check_str_eq(system_text, "Rules");
      check_size_eq(turbo_json_array_size(json_messages), 2);
      check_str_eq(turbo_json_get_string(turbo_json_array_get(json_messages, 0), "role"),
                   "assistant");
      check_str_eq(turbo_json_get_string(turbo_json_array_get(json_messages, 1), "role"),
                   "user");

      free(system_text);
      turbo_free_json(&json_messages);
      turbo_runtime_data_bind_value_destroy(messages);
    }
  }
}
