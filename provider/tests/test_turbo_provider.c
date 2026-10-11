#include "tinytest.h"
#include "turbo_provider.h"

#include <string.h>

spec("standalone provider wire contracts") {
  it("preserves Responses request framing and borrows the projected arrays") {
    turbo_provider_request_t config = {0};
    json_value_t *input = json_create_array();
    json_value_t *tools = json_create_array();
    char *text = NULL;
    config.format = TURBO_PROVIDER_RESPONSES;
    config.model = "test-model";
    config.stream = 1;
    config.parallel_tool_calls = 1;
    config.input = input;
    config.tools = tools;
    config.instructions = "be concise";
    config.previous_response_id = "response-1";
    check_equal(turbo_provider_build_request(&config, &text), 0);
    check_equal(json_array_size(input), (size_t)0);
    check_equal(json_array_size(tools), (size_t)0);
    json_free(input);
    json_free(tools);
    check_equal(text, "{\"model\":\"test-model\",\"stream\":true,"
                      "\"previous_response_id\":\"response-1\",\"input\":[],"
                      "\"instructions\":\"be concise\",\"parallel_tool_calls\":true}");
    json_serialize_free(text);
  }

  it("keeps Chat and compatible Chat tool and structured-output differences") {
    static const char tool_json[] =
        "[{\"type\":\"function\",\"function\":{\"name\":\"probe\","
        "\"parameters\":{\"type\":\"object\"}}}]";
    turbo_provider_request_t config = {0};
    json_value_t *input = json_create_array();
    json_value_t *tools = json_parse(tool_json, strlen(tool_json));
    json_value_t *request;
    const json_value_t *format;
    char *text = NULL;
    config.format = TURBO_PROVIDER_CHAT;
    config.model = "test-model";
    config.input = input;
    config.tools = tools;
    config.parallel_tool_calls = 1;
    config.structured_output_schema_json = "{\"type\":\"object\"}";
    config.structured_output_strict = 1;
    check_equal(turbo_provider_build_request(&config, &text), 0);
    request = json_parse(text, strlen(text));
    check_not_null(request);
    check_equal(json_get_string(request, "tool_choice"), "auto");
    check_not_null(json_object_get(request, "parallel_tool_calls"));
    format = json_object_get(request, "response_format");
    check_equal(json_get_string(format, "type"), "json_schema");
    check_equal(json_get_string(json_object_get(format, "json_schema"), "name"), "structured_output");
    json_free(request);
    json_serialize_free(text);

    config.format = TURBO_PROVIDER_COMPATIBLE_CHAT;
    check_equal(turbo_provider_build_request(&config, &text), 0);
    request = json_parse(text, strlen(text));
    check_not_null(request);
    check_true(json_object_get(request, "tool_choice") == NULL);
    check_true(json_object_get(request, "parallel_tool_calls") == NULL);
    check_equal(json_array_size(json_object_get(request, "tools")), (size_t)1);
    check_not_null(json_object_get(request, "response_format"));
    check_equal(json_array_size(tools), (size_t)1);
    json_free(request);
    json_serialize_free(text);
    json_free(input);
    json_free(tools);
  }

  it("preserves Anthropic system and structured-output omission") {
    turbo_provider_request_t config = {0};
    json_value_t *input = json_create_array();
    json_value_t *tools = json_create_array();
    char *text = NULL;
    config.format = TURBO_PROVIDER_ANTHROPIC;
    config.model = "test-model";
    config.input = input;
    config.tools = tools;
    config.instructions = "be concise";
    config.previous_response_id = "ignored";
    config.parallel_tool_calls = 1;
    config.structured_output_schema_json = "invalid but not used by this protocol";
    check_equal(turbo_provider_build_request(&config, &text), 0);
    check_equal(text, "{\"model\":\"test-model\",\"messages\":[],\"system\":\"be concise\"}");
    json_serialize_free(text);
    json_free(input);
    json_free(tools);
  }

  it("returns no partial request on invalid format or structured schema") {
    turbo_provider_request_t config = {0};
    json_value_t *input = json_create_array();
    json_value_t *tools = json_create_array();
    char marker;
    char *text = &marker;
    config.model = "test-model";
    config.input = input;
    config.tools = tools;
    check_equal(turbo_provider_build_request(&config, &text), -1);
    check_true(text == NULL);
    config.format = TURBO_PROVIDER_RESPONSES;
    config.structured_output_schema_json = "[]";
    text = &marker;
    check_equal(turbo_provider_build_request(&config, &text), -1);
    check_true(text == NULL);
    check_equal(json_array_size(input), (size_t)0);
    check_equal(json_array_size(tools), (size_t)0);
    json_free(input);
    json_free(tools);
  }

  it("decodes responses and SSE without an Agent or transport") {
    static const char response_text[] =
        "{\"id\":\"response-1\",\"choices\":[{\"message\":{\"content\":\"hello\"}}]}";
    static const char sse[] =
        "data: {\"id\":\"response-2\",\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n\n"
        "data: [DONE]\n\n";
    json_value_t *response = json_parse(response_text, strlen(response_text));
    json_value_t *event = turbo_provider_response_to_event_json_value(TURBO_PROVIDER_CHAT, response);
    check_not_null(event);
    json_free(response);
    check_equal(json_get_string(event, "output_text"), "hello");
    json_free(event);
    event = turbo_provider_sse_to_event_json_value(TURBO_PROVIDER_CHAT, sse, strlen(sse));
    check_not_null(event);
    check_equal(json_get_string(event, "output_text"), "hello");
    json_free(event);
  }
}
