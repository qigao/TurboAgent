#include "tinytest.h"
#include "turbo_agent_sse.h"
#include <json_parser.h>

#include <string.h>

static json_value_t *parse_sse_response_or_fail(int rc, char **response_json) {
  json_value_t *parsed = NULL;

  check_equal(rc, 0);
  check_not_null(response_json);
  check_not_null(*response_json);
  parsed = json_parse(*response_json, strlen(*response_json));
    check_not_null(parsed);
  check_not_null(parsed);
  json_serialize_free(*response_json);
  *response_json = NULL;
  return parsed;
}

static json_value_t *parse_chat_sse_or_fail(const char *sse) {
  char *response_json = NULL;
  int rc = turbo_agent_chat_sse_to_json(sse, strlen(sse), &response_json);
  return parse_sse_response_or_fail(rc, &response_json);
}

static json_value_t *parse_responses_sse_or_fail(const char *sse) {
  char *response_json = NULL;
  int rc = turbo_agent_responses_sse_to_json(sse, strlen(sse), &response_json);
  return parse_sse_response_or_fail(rc, &response_json);
}

static json_value_t *parse_anthropic_sse_or_fail(const char *sse) {
  char *response_json = NULL;
  int rc = turbo_agent_anthropic_messages_sse_to_json(sse, strlen(sse), &response_json);
  return parse_sse_response_or_fail(rc, &response_json);
}

spec("turbo agent sse api") {

  it("should aggregate chat completions frames into one response object") {
    const char *sse =
        "data: {\"id\":\"chat_1\",\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"he\"}}]}\n\n"
        "data: {\"id\":\"chat_1\",\"choices\":[{\"delta\":{\"content\":\"llo\"},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    json_value_t *response;
    const json_value_t *choices;
    const json_value_t *message;

    response = parse_chat_sse_or_fail(sse);
    check_equal(json_get_string(response, "id"), "chat_1");
    choices = json_object_get(response, "choices");
    check_not_null(choices);
    check_equal(json_array_size(choices), 1);
    message = json_object_get(json_array_get(choices, 0), "message");
    check_not_null(message);
    check_equal(json_get_string(message, "role"), "assistant");
    check_equal(json_get_string(message, "content"), "hello");

    json_free(response); response = NULL;
  }

  it("should aggregate responses frames into one response object") {
    const char *sse =
        "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"yo\"}]}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\"}}\n\n"
        "data: [DONE]\n\n";
    json_value_t *response;
    const json_value_t *output;
    const json_value_t *content;

    response = parse_responses_sse_or_fail(sse);
    check_equal(json_get_string(response, "id"), "resp_1");
    output = json_object_get(response, "output");
    check_not_null(output);
    check_equal(json_array_size(output), 1);
    content = json_object_get(json_array_get(output, 0), "content");
    check_not_null(content);
    check_equal(json_get_string(json_array_get(content, 0), "text"), "yo");

    json_free(response); response = NULL;
  }

  it("should rebuild responses output items when completion omits output") {
    const char *sse =
        "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"function_call_output\",\"call_id\":\"call_1\",\"output\":\"{\\\"ok\\\":true}\"}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_rebuilt\"}}\n\n"
        "data: [DONE]\n\n";
    json_value_t *response;
    const json_value_t *output;

    response = parse_responses_sse_or_fail(sse);
    check_equal(json_get_string(response, "id"), "resp_rebuilt");
    output = json_object_get(response, "output");
    check_not_null(output);
    check_equal(json_array_size(output), 1);
    check_equal(json_get_string(json_array_get(output, 0), "type"),
                 "function_call_output");
    check_equal(json_get_string(json_array_get(output, 0), "call_id"), "call_1");

    json_free(response); response = NULL;
  }

  it("should fail responses aggregation without a completed response") {
    const char *sse =
        "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"yo\"}]}}\n\n"
        "data: [DONE]\n\n";
    char *response_json = NULL;

    check_not_equal(turbo_agent_responses_sse_to_json(sse, strlen(sse), &response_json), 0);
    check_true((response_json) == (NULL));
  }

  it("should fail responses aggregation on malformed json frame") {
    const char *sse =
        "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"yo\"}]}}\n\n"
        "data: {not-json}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\"}}\n\n"
        "data: [DONE]\n\n";
    char *response_json = NULL;

    check_not_equal(turbo_agent_responses_sse_to_json(sse, strlen(sse), &response_json), 0);
    check_true((response_json) == (NULL));
  }

  it("should fail responses aggregation when completed response payload is malformed") {
    const char *sse =
        "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"yo\"}]}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":\"bad\"}\n\n"
        "data: [DONE]\n\n";
    char *response_json = NULL;

    check_not_equal(turbo_agent_responses_sse_to_json(sse, strlen(sse), &response_json), 0);
    check_true((response_json) == (NULL));
  }

  it("should fail responses aggregation when completed response id is missing") {
    const char *sse =
        "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"yo\"}]}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"output\":[]}}\n\n"
        "data: [DONE]\n\n";
    char *response_json = NULL;

    check_not_equal(turbo_agent_responses_sse_to_json(sse, strlen(sse), &response_json), 0);
    check_true((response_json) == (NULL));
  }

  it("should fail responses aggregation when output item payload is malformed") {
    const char *sse =
        "data: {\"type\":\"response.output_item.done\",\"item\":\"bad\"}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\"}}\n\n"
        "data: [DONE]\n\n";
    char *response_json = NULL;

    check_not_equal(turbo_agent_responses_sse_to_json(sse, strlen(sse), &response_json), 0);
    check_true((response_json) == (NULL));
  }

  it("should aggregate anthropic frames into one response object") {
    const char *sse =
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"role\":\"assistant\"}}\n\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"an\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"th\"}}\n\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    json_value_t *response;
    const json_value_t *content;

    response = parse_anthropic_sse_or_fail(sse);
    check_equal(json_get_string(response, "id"), "msg_1");
    check_equal(json_get_string(response, "stop_reason"), "end_turn");
    content = json_object_get(response, "content");
    check_not_null(content);
    check_equal(json_array_size(content), 1);
    check_equal(json_get_string(json_array_get(content, 0), "type"), "text");
    check_equal(json_get_string(json_array_get(content, 0), "text"), "anth");

    json_free(response); response = NULL;
  }

  it("should aggregate anthropic partial tool input json into one tool_use block") {
    const char *sse =
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_tool\",\"role\":\"assistant\"}}\n\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"call_1\",\"name\":\"sum\",\"input\":{}}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"a\\\":2\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\",\\\"b\\\":3}\"}}\n\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    json_value_t *response;
    const json_value_t *content;
    const json_value_t *tool_use;
    const json_value_t *input;

    response = parse_anthropic_sse_or_fail(sse);
    content = json_object_get(response, "content");
    check_not_null(content);
    check_equal(json_array_size(content), 1);
    tool_use = json_array_get(content, 0);
    check_not_null(tool_use);
    check_equal(json_get_string(tool_use, "type"), "tool_use");
    check_equal(json_get_string(tool_use, "id"), "call_1");
    input = json_object_get(tool_use, "input");
    check_not_null(input);
    check_equal(json_get_int(input, "a", 0), 2);
    check_equal(json_get_int(input, "b", 0), 3);

    json_free(response); response = NULL;
  }

  it("should fail anthropic aggregation on malformed partial tool input json") {
    const char *sse =
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_bad_tool\",\"role\":\"assistant\"}}\n\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"call_1\",\"name\":\"sum\",\"input\":{}}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\"}}\n\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    char *response_json = NULL;

    check_not_equal(
        turbo_agent_anthropic_messages_sse_to_json(sse, strlen(sse), &response_json), 0);
    check_true((response_json) == (NULL));
  }

  it("should fail anthropic aggregation when message_start is missing an id") {
    const char *sse =
        "data: {\"type\":\"message_start\",\"message\":{\"role\":\"assistant\"}}\n\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"hi\"}}\n\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    char *response_json = NULL;

    check_not_equal(
        turbo_agent_anthropic_messages_sse_to_json(sse, strlen(sse), &response_json), 0);
    check_true((response_json) == (NULL));
  }

  it("should fail anthropic aggregation when message_start is missing a role") {
    const char *sse =
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_no_role\"}}\n\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"hi\"}}\n\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    char *response_json = NULL;

    check_not_equal(
        turbo_agent_anthropic_messages_sse_to_json(sse, strlen(sse), &response_json), 0);
    check_true((response_json) == (NULL));
  }

  it("should fail anthropic aggregation on malformed json frame") {
    const char *sse =
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_bad\",\"role\":\"assistant\"}}\n\n"
        "data: {not-json}\n\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    char *response_json = NULL;

    check_not_equal(
        turbo_agent_anthropic_messages_sse_to_json(sse, strlen(sse), &response_json), 0);
    check_true((response_json) == (NULL));
  }

  it("should fail anthropic aggregation when tool_use block is missing required fields") {
    const char *sse =
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_bad_tool_use\",\"role\":\"assistant\"}}\n\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"input\":{}}}\n\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    char *response_json = NULL;

    check_not_equal(
        turbo_agent_anthropic_messages_sse_to_json(sse, strlen(sse), &response_json), 0);
    check_true((response_json) == (NULL));
  }

  it("should aggregate streamed chat tool call arguments into one tool call") {
    const char *sse =
        "data: {\"id\":\"chat_tool\",\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"sum\",\"arguments\":\"{\\\"a\\\":2\"}}]}}]}\n\n"
        "data: {\"id\":\"chat_tool\",\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\",\\\"b\\\":3}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    json_value_t *response;
    const json_value_t *choices;
    const json_value_t *message;
    const json_value_t *tool_calls;
    const json_value_t *function;

    response = parse_chat_sse_or_fail(sse);
    choices = json_object_get(response, "choices");
    check_not_null(choices);
    message = json_object_get(json_array_get(choices, 0), "message");
    check_not_null(message);
    tool_calls = json_object_get(message, "tool_calls");
    check_not_null(tool_calls);
    check_equal(json_array_size(tool_calls), 1);
    function = json_object_get(json_array_get(tool_calls, 0), "function");
    check_not_null(function);
    check_equal(json_get_string(function, "name"), "sum");
    check_equal(json_get_string(function, "arguments"), "{\"a\":2,\"b\":3}");

    json_free(response); response = NULL;
  }

  it("should fail chat aggregation on malformed json frame") {
    const char *sse =
        "data: {\"id\":\"chat_bad\",\"choices\":[{\"delta\":{\"content\":\"oops\"}}]}\n\n"
        "data: {not-json}\n\n"
        "data: [DONE]\n\n";
    char *response_json = NULL;

    check_not_equal(turbo_agent_chat_sse_to_json(sse, strlen(sse), &response_json), 0);
    check_true((response_json) == (NULL));
  }

  it("should fail chat aggregation when choices are missing") {
    const char *sse =
        "data: {\"id\":\"chat_bad\"}\n\n"
        "data: [DONE]\n\n";
    char *response_json = NULL;

    check_not_equal(turbo_agent_chat_sse_to_json(sse, strlen(sse), &response_json), 0);
    check_true((response_json) == (NULL));
  }

  it("should fail chat aggregation when id is missing") {
    const char *sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"oops\"}}]}\n\n"
        "data: [DONE]\n\n";
    char *response_json = NULL;

    check_not_equal(turbo_agent_chat_sse_to_json(sse, strlen(sse), &response_json), 0);
    check_true((response_json) == (NULL));
  }

  it("should fail chat aggregation when tool call entry is malformed") {
    const char *sse =
        "data: {\"id\":\"chat_bad_tool\",\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"type\":\"function\"}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    char *response_json = NULL;

    check_not_equal(turbo_agent_chat_sse_to_json(sse, strlen(sse), &response_json), 0);
    check_true((response_json) == (NULL));
  }
}
