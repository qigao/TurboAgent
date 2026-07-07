#include "tinytest.h"
#include "turbo_action_tool.h"

#include <stdlib.h>
#include <string.h>

static int ok_handler(const json_value_t *args, json_value_t **out_result, void *user_data) {
  json_value_t *result;

  (void)args;
  (void)user_data;
  result = turbo_action_result_create(1, "ok");
  if (!result) {
    return -1;
  }

  turbo_json_object_set_string(result, "note", "worked");
  *out_result = result;
  return 0;
}

static int invalid_handler(const json_value_t *args, json_value_t **out_result, void *user_data) {
  (void)args;
  (void)user_data;
  *out_result = turbo_json_create_object();
  return *out_result ? 0 : -1;
}

static int echo_handler(const json_value_t *args, json_value_t **out_result, void *user_data) {
  json_value_t *result;

  (void)user_data;
  result = turbo_action_result_create(1, "ok");
  if (!result) {
    return -1;
  }

  turbo_json_object_set_string(result, "echo", turbo_json_get_string(args, "message"));
  *out_result = result;
  return 0;
}

spec("turbo action tool") {

  it("should register and execute an action tool") {
    turbo_action_tool_registry_t *registry = turbo_action_tool_registry_create();
    turbo_action_tool_definition_t definition = {
        .name = "read_file",
        .description = "Read a file",
        .parameters_json =
            "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}",
        .kind = TURBO_ACTION_OBSERVE,
        .handler = ok_handler,
    };
    json_value_t *args = turbo_json_create_object();
    json_value_t *result = NULL;

    check_not_null(registry);
    check_not_null(args);
    check_int_eq(turbo_action_tool_registry_add(registry, &definition), TURBO_ACTION_TOOL_OK);
    check_size_eq(turbo_action_tool_registry_count(registry), 1);
    check_not_null(turbo_action_tool_registry_find(registry, "read_file"));
    check_int_eq(turbo_action_tool_registry_execute(registry, "read_file", args, &result),
                 TURBO_ACTION_TOOL_OK);
    check_not_null(result);
    check_true(turbo_json_get_bool(result, "ok", false));
    check_str_eq(turbo_json_get_string(result, "summary"), "ok");
    check_str_eq(turbo_json_get_string(result, "note"), "worked");

    turbo_free_json(&result);
    turbo_free_json(&args);
    turbo_action_tool_registry_destroy(registry);
  }

  it("should reject malformed action results") {
    turbo_action_tool_registry_t *registry = turbo_action_tool_registry_create();
    turbo_action_tool_definition_t definition = {
        .name = "bad",
        .description = "Bad tool",
        .parameters_json = "{\"type\":\"object\"}",
        .kind = TURBO_ACTION_OBSERVE,
        .handler = invalid_handler,
    };
    json_value_t *result = NULL;

    check_not_null(registry);
    check_int_eq(turbo_action_tool_registry_add(registry, &definition), TURBO_ACTION_TOOL_OK);
    check_int_eq(turbo_action_tool_registry_execute(registry, "bad", NULL, &result),
                 TURBO_ACTION_TOOL_ERROR);
    check_null(result);

    turbo_action_tool_registry_destroy(registry);
  }

  it("should serialize canonical failure envelopes with escaped text") {
    json_value_t *result = turbo_action_result_create(0, "quote: \"bad\"\npath");
    char *serialized;
    json_value_t *parsed = NULL;

    check_not_null(result);
    check_int_eq(turbo_action_result_set_command_fields(result, 17, "",
                                                        "line 1\\line 2\n\"oops\""),
                 0);

    serialized = turbo_json_serialize(result, NULL);
    check_not_null(serialized);
    check_int_eq(turbo_parse_json((const uint8_t *)serialized, strlen(serialized), &parsed), 0);
    check_not_null(parsed);
    check_false(turbo_json_get_bool(parsed, "ok", true));
    check_str_eq(turbo_json_get_string(parsed, "summary"), "quote: \"bad\"\npath");
    check_str_eq(turbo_json_get_string(parsed, "stderr"), "line 1\\line 2\n\"oops\"");

    turbo_free_json(&parsed);
    free(serialized);
    turbo_free_json(&result);
  }

  it("should serialize action tools into chat function shape") {
    turbo_action_tool_registry_t *registry = turbo_action_tool_registry_create();
    turbo_action_tool_definition_t definition = {
        .name = "run_command",
        .description = "Run a command",
        .parameters_json =
            "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},"
            "\"required\":[\"command\"]}",
        .kind = TURBO_ACTION_DANGEROUS,
        .handler = ok_handler,
    };
    json_value_t *tools;
    const json_value_t *tool;
    const json_value_t *function_object;

    check_not_null(registry);
    check_int_eq(turbo_action_tool_registry_add(registry, &definition), TURBO_ACTION_TOOL_OK);
    tools = turbo_action_tool_registry_build_openai_chat_tools(registry);
    check_not_null(tools);
    check_size_eq(turbo_json_array_size(tools), 1);
    tool = turbo_json_array_get(tools, 0);
    function_object = turbo_json_object_get(tool, "function");
    check_str_eq(turbo_json_get_string(tool, "type"), "function");
    check_str_eq(turbo_json_get_string(function_object, "name"), "run_command");
    check_true(turbo_json_get_bool(function_object, "strict", false));

    turbo_free_json(&tools);
    turbo_action_tool_registry_destroy(registry);
  }

  it("should bridge action tools into legacy tool registry") {
    turbo_action_tool_registry_t *action_registry = turbo_action_tool_registry_create();
    turbo_action_tool_definition_t definition = {
        .name = "echo",
        .description = "Echo a message",
        .parameters_json =
            "{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\"}},"
            "\"required\":[\"message\"]}",
        .kind = TURBO_ACTION_OBSERVE,
        .handler = echo_handler,
    };
    turbo_tool_registry_t *tool_registry;
    char *output = NULL;
    json_value_t *result = NULL;

    check_not_null(action_registry);
    check_int_eq(turbo_action_tool_registry_add(action_registry, &definition),
                 TURBO_ACTION_TOOL_OK);

    tool_registry = turbo_action_tool_registry_build_tool_registry_bridge(action_registry);
    check_not_null(tool_registry);
    check_size_eq(turbo_tool_registry_count(tool_registry), 1);
    check_int_eq(turbo_tool_registry_execute(tool_registry, "echo", "{\"message\":\"hi\"}", &output),
                 TURBO_TOOL_OK);
    check_not_null(output);
    check_int_eq(turbo_parse_json((const uint8_t *)output, strlen(output), &result), 0);
    check_true(turbo_json_get_bool(result, "ok", false));
    check_str_eq(turbo_json_get_string(result, "echo"), "hi");

    turbo_free_json(&result);
    free(output);
    turbo_tool_registry_destroy(tool_registry);
    turbo_action_tool_registry_destroy(action_registry);
  }

  it("should keep bridged tools usable after the source registry is destroyed") {
    turbo_action_tool_registry_t *action_registry = turbo_action_tool_registry_create();
    turbo_action_tool_definition_t definition = {
        .name = "echo",
        .description = "Echo a message",
        .parameters_json =
            "{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\"}},"
            "\"required\":[\"message\"]}",
        .kind = TURBO_ACTION_OBSERVE,
        .handler = echo_handler,
    };
    turbo_tool_registry_t *tool_registry;
    char *output = NULL;
    json_value_t *result = NULL;

    check_not_null(action_registry);
    check_int_eq(turbo_action_tool_registry_add(action_registry, &definition),
                 TURBO_ACTION_TOOL_OK);

    tool_registry = turbo_action_tool_registry_build_tool_registry_bridge(action_registry);
    check_not_null(tool_registry);
    turbo_action_tool_registry_destroy(action_registry);
    action_registry = NULL;

    check_int_eq(turbo_tool_registry_execute(tool_registry, "echo", "{\"message\":\"hi\"}", &output),
                 TURBO_TOOL_OK);
    check_not_null(output);
    check_int_eq(turbo_parse_json((const uint8_t *)output, strlen(output), &result), 0);
    check_true(turbo_json_get_bool(result, "ok", false));
    check_str_eq(turbo_json_get_string(result, "echo"), "hi");

    turbo_free_json(&result);
    free(output);
    turbo_tool_registry_destroy(tool_registry);
  }

  it("should bridge action tools into bind-native tool registry execution") {
    turbo_action_tool_registry_t *action_registry = turbo_action_tool_registry_create();
    turbo_action_tool_definition_t definition = {
        .name = "echo",
        .description = "Echo a message",
        .parameters_json =
            "{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\"}},"
            "\"required\":[\"message\"]}",
        .kind = TURBO_ACTION_OBSERVE,
        .handler = echo_handler,
    };
    turbo_tool_registry_t *tool_registry;
    turbo_runtime_data_bind_value_t *args = turbo_runtime_data_bind_value_create_object();
    turbo_runtime_data_bind_value_t *message =
        turbo_runtime_data_bind_value_create_string("hi");
    turbo_runtime_data_bind_value_t *result = NULL;

    check_not_null(action_registry);
    check_not_null(args);
    check_not_null(message);
    check_int_eq(turbo_runtime_data_bind_object_set(args, "message", message),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_action_tool_registry_add(action_registry, &definition),
                 TURBO_ACTION_TOOL_OK);

    tool_registry = turbo_action_tool_registry_build_tool_registry_bridge(action_registry);
    check_not_null(tool_registry);
    check_int_eq(turbo_tool_registry_execute_bind(tool_registry, "echo", args, &result),
                 TURBO_TOOL_OK);
    check_not_null(result);
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(result, "ok"), 0));
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(result, "echo")),
                 "hi");

    turbo_runtime_data_bind_value_destroy(result);
    turbo_runtime_data_bind_value_destroy(args);
    turbo_tool_registry_destroy(tool_registry);
    turbo_action_tool_registry_destroy(action_registry);
  }
}
