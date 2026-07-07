#include "tinytest.h"
#include "turbo_tool_registry.h"
#include "turbo_tool_runtime.h"

#include <stdlib.h>
#include <string.h>

static int test_echo_tool(const char *arguments_json, char **out_output,
                          void *user_data) {
  size_t len;
  char *copy;
  (void)user_data;

  if (!out_output) {
    return -1;
  }

  len = strlen(arguments_json ? arguments_json : "{}") + 1;
  copy = (char *)malloc(len);
  if (!copy) {
    return -1;
  }

  memcpy(copy, arguments_json ? arguments_json : "{}", len);
  *out_output = copy;
  return 0;
}

static int test_echo_tool_bind(const turbo_runtime_data_bind_value_t *arguments,
                               turbo_runtime_data_bind_value_t **out_result,
                               void *user_data) {
  json_value_t *json_value;
  turbo_runtime_data_bind_value_t *result;
  (void)user_data;

  if (!arguments || !out_result) {
    return -1;
  }

  json_value = turbo_runtime_data_bind_value_to_json(arguments);
  if (!json_value) {
    return -1;
  }
  result = turbo_runtime_data_bind_value_from_json(json_value);
  turbo_free_json(&json_value);
  if (!result) {
    return -1;
  }

  *out_result = result;
  return 0;
}

spec("turbo tool runtime") {

  it("should bridge native callback tools through a runtime") {
    turbo_tool_runtime_t *runtime = NULL;
    turbo_tool_registry_t *registry = NULL;
    turbo_tool_runtime_tool_t tool = {0};
    turbo_tool_definition_t definition = {
        "echo_json",
        "Echo JSON back to the caller.",
        "{\"type\":\"object\"}",
        NULL,
        1,
        test_echo_tool,
        test_echo_tool_bind,
        NULL,
        NULL};
    char *output = NULL;
    turbo_runtime_data_bind_value_t *bind_args = NULL;
    turbo_runtime_data_bind_value_t *bind_result = NULL;

    runtime = turbo_tool_runtime_native_create();
    check_not_null(runtime);
    check_int_eq(turbo_tool_runtime_native_add_tool(runtime, &definition),
                 TURBO_TOOL_OK);
    check_size_eq(turbo_tool_runtime_count(runtime), 1);
    check_int_eq(turbo_tool_runtime_get_tool(runtime, 0, &tool), TURBO_TOOL_OK);
    check_str_eq(tool.name, "echo_json");
    check_int_eq(
        turbo_tool_runtime_invoke(runtime, "echo_json", "{\"ok\":true}", &output),
        TURBO_TOOL_OK);
    check_str_eq(output, "{\"ok\":true}");
    free(output);
    output = NULL;

    bind_args = turbo_runtime_data_bind_value_create_object();
    check_not_null(bind_args);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     bind_args, "bridge",
                     turbo_runtime_data_bind_value_create_int64(1)),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(
        turbo_tool_runtime_invoke_bind(runtime, "echo_json", bind_args, &bind_result),
        TURBO_TOOL_OK);
    check_not_null(bind_result);
    check_int_eq((int)turbo_runtime_data_bind_value_as_int64(
                     turbo_runtime_data_bind_object_get(bind_result, "bridge"), 0),
                 1);
    turbo_runtime_data_bind_value_destroy(bind_result);
    bind_result = NULL;

    registry = turbo_tool_runtime_build_registry_bridge(runtime);
    check_not_null(registry);
    check_int_eq(turbo_tool_registry_execute(registry, "echo_json",
                                             "{\"bridge\":1}", &output),
                 TURBO_TOOL_OK);
    check_str_eq(output, "{\"bridge\":1}");

    check_int_eq(turbo_tool_registry_execute_bind(registry, "echo_json", bind_args, &bind_result),
                 TURBO_TOOL_OK);
    check_not_null(bind_result);
    check_int_eq((int)turbo_runtime_data_bind_value_as_int64(
                     turbo_runtime_data_bind_object_get(bind_result, "bridge"), 0),
                 1);

    turbo_runtime_data_bind_value_destroy(bind_result);
    turbo_runtime_data_bind_value_destroy(bind_args);
    free(output);
    turbo_tool_registry_destroy(registry);
    turbo_tool_runtime_destroy(runtime);
  }
}
