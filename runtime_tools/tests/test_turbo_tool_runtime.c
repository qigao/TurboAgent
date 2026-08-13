#include "tinytest.h"
#include "turbo_tool_registry.h"
#include "turbo_tool_runtime.h"

#include <stdlib.h>
#include <string.h>

static int test_echo_tool(const char *arguments_json, char **out_output, void *user_data) {
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

static int test_echo_tool_json_value(const json_value_t *arguments, json_value_t **out_result,
                                     void *user_data) {
  json_value_t *json_value;
  json_value_t *result;
  (void)user_data;

  if (!arguments || !out_result) {
    return -1;
  }

  json_value = turbo_json_clone(arguments);
  if (!json_value) {
    return -1;
  }
  result = turbo_json_clone(json_value);
  turbo_free_json(&json_value);
  if (!result) {
    return -1;
  }

  *out_result = result;
  return 0;
}

spec("turbo tool runtime") {

  it("should preserve legacy policy and expose validated v2 policy") {
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_execution_policy_t policy = {0};
    turbo_tool_definition_t legacy = {"legacy", "Legacy",       "{\"type\":\"object\"}",   NULL,
                                      1,        test_echo_tool, test_echo_tool_json_value, NULL,
                                      NULL};
    turbo_tool_definition_v2_t v2 = {
        sizeof(turbo_tool_definition_v2_t),
        TURBO_TOOL_DEFINITION_V2_ABI_VERSION,
        {"parallel", "Parallel", "{\"type\":\"object\"}", NULL, 1, test_echo_tool,
         test_echo_tool_json_value, NULL, NULL},
        {TURBO_TOOL_EXECUTION_PARALLEL_SAFE, TURBO_TOOL_IDEMPOTENCY_READ_ONLY}};

    check_not_null(registry);
    check_int_eq(turbo_tool_registry_add(registry, &legacy), TURBO_TOOL_OK);
    check_int_eq(turbo_tool_registry_get_execution_policy(registry, "legacy", &policy),
                 TURBO_TOOL_OK);
    check_int_eq(policy.mode, TURBO_TOOL_EXECUTION_SEQUENTIAL);
    check_int_eq(policy.idempotency, TURBO_TOOL_IDEMPOTENCY_NONE);
    check_int_eq(turbo_tool_registry_add_v2(registry, &v2), TURBO_TOOL_OK);
    check_int_eq(turbo_tool_registry_get_execution_policy(registry, "parallel", &policy),
                 TURBO_TOOL_OK);
    check_int_eq(policy.mode, TURBO_TOOL_EXECUTION_PARALLEL_SAFE);
    check_int_eq(policy.idempotency, TURBO_TOOL_IDEMPOTENCY_READ_ONLY);
    v2.abi_version++;
    check_int_eq(turbo_tool_registry_add_v2(registry, &v2), TURBO_TOOL_INVALID_ARGUMENT);
    turbo_tool_registry_destroy(registry);
  }

  it("should build an ordered non-owning registry projection") {
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_registry_t *projection = NULL;
    turbo_tool_definition_t first = {"first", "First",        "{\"type\":\"object\"}",   NULL,
                                     1,       test_echo_tool, test_echo_tool_json_value, NULL,
                                     NULL};
    turbo_tool_definition_t second = {"second", "Second",       "{\"type\":\"object\"}",   NULL,
                                      1,        test_echo_tool, test_echo_tool_json_value, NULL,
                                      NULL};
    turbo_tool_definition_t view = {0};
    const char *names[] = {"second"};
    const char *missing[] = {"missing"};

    check_int_eq(turbo_tool_registry_add(registry, &first), TURBO_TOOL_OK);
    check_int_eq(turbo_tool_registry_add(registry, &second), TURBO_TOOL_OK);
    check_int_eq(turbo_tool_registry_project(registry, names, 1, &projection), TURBO_TOOL_OK);
    check_not_null(projection);
    check_size_eq(turbo_tool_registry_count(projection), 1);
    check_int_eq(turbo_tool_registry_get_definition(projection, 0, &view), TURBO_TOOL_OK);
    check_str_eq(view.name, "second");
    turbo_tool_registry_destroy(projection);
    projection = NULL;
    check_int_eq(turbo_tool_registry_project(registry, missing, 1, &projection),
                 TURBO_TOOL_NOT_FOUND);
    check_null(projection);
    turbo_tool_registry_destroy(registry);
  }

  it("should bridge native callback tools through a runtime") {
    turbo_tool_runtime_t *runtime = NULL;
    turbo_tool_registry_t *registry = NULL;
    turbo_tool_runtime_tool_t tool = {0};
    turbo_tool_definition_t definition = {"echo_json",
                                          "Echo JSON back to the caller.",
                                          "{\"type\":\"object\"}",
                                          NULL,
                                          1,
                                          test_echo_tool,
                                          test_echo_tool_json_value,
                                          NULL,
                                          NULL};
    char *output = NULL;
    json_value_t *json_value_args = NULL;
    json_value_t *json_value_result = NULL;

    runtime = turbo_tool_runtime_native_create();
    check_not_null(runtime);
    check_int_eq(turbo_tool_runtime_native_add_tool(runtime, &definition), TURBO_TOOL_OK);
    check_size_eq(turbo_tool_runtime_count(runtime), 1);
    check_int_eq(turbo_tool_runtime_get_tool(runtime, 0, &tool), TURBO_TOOL_OK);
    check_str_eq(tool.name, "echo_json");
    check_int_eq(turbo_tool_runtime_invoke(runtime, "echo_json", "{\"ok\":true}", &output),
                 TURBO_TOOL_OK);
    check_str_eq(output, "{\"ok\":true}");
    free(output);
    output = NULL;

    json_value_args = turbo_json_create_object();
    check_not_null(json_value_args);
    check_int_eq(
        turbo_runtime_json_object_set(json_value_args, "bridge", turbo_json_create_int64(1)),
        TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_tool_runtime_invoke_json_value(runtime, "echo_json", json_value_args,
                                                      &json_value_result),
                 TURBO_TOOL_OK);
    check_not_null(json_value_result);
    check_int_eq((int)turbo_runtime_json_value_as_int64(
                     turbo_json_object_get(json_value_result, "bridge"), 0),
                 1);
    turbo_runtime_json_destroy(json_value_result);
    json_value_result = NULL;

    registry = turbo_tool_runtime_build_registry_bridge(runtime);
    check_not_null(registry);
    check_int_eq(turbo_tool_registry_execute(registry, "echo_json", "{\"bridge\":1}", &output),
                 TURBO_TOOL_OK);
    check_str_eq(output, "{\"bridge\":1}");

    check_int_eq(turbo_tool_registry_execute_json_value(registry, "echo_json", json_value_args,
                                                        &json_value_result),
                 TURBO_TOOL_OK);
    check_not_null(json_value_result);
    check_int_eq((int)turbo_runtime_json_value_as_int64(
                     turbo_json_object_get(json_value_result, "bridge"), 0),
                 1);

    turbo_runtime_json_destroy(json_value_result);
    turbo_runtime_json_destroy(json_value_args);
    free(output);
    turbo_tool_registry_destroy(registry);
    turbo_tool_runtime_destroy(runtime);
  }

  it("should append runtimes with v2 policy and retain their lifetime") {
    turbo_tool_runtime_t *first_runtime = turbo_tool_runtime_native_create();
    turbo_tool_runtime_t *second_runtime = turbo_tool_runtime_native_create();
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t first = {"first_runtime_tool",
                                     "First runtime tool",
                                     "{\"type\":\"object\"}",
                                     NULL,
                                     1,
                                     test_echo_tool,
                                     test_echo_tool_json_value,
                                     NULL,
                                     NULL};
    turbo_tool_definition_t second = {"second_runtime_tool",
                                      "Second runtime tool",
                                      "{\"type\":\"object\"}",
                                      NULL,
                                      1,
                                      test_echo_tool,
                                      test_echo_tool_json_value,
                                      NULL,
                                      NULL};
    turbo_tool_execution_policy_t first_policy = {TURBO_TOOL_EXECUTION_EXCLUSIVE,
                                                  TURBO_TOOL_IDEMPOTENCY_KEYED};
    turbo_tool_execution_policy_t second_policy = {TURBO_TOOL_EXECUTION_SEQUENTIAL,
                                                   TURBO_TOOL_IDEMPOTENCY_READ_ONLY};
    turbo_tool_execution_policy_t observed = {0};
    char *output = NULL;

    check_not_null(first_runtime);
    check_not_null(second_runtime);
    check_not_null(registry);
    check_int_eq(turbo_tool_runtime_native_add_tool(first_runtime, &first), TURBO_TOOL_OK);
    check_int_eq(turbo_tool_runtime_native_add_tool(second_runtime, &second), TURBO_TOOL_OK);
    check_int_eq(turbo_tool_runtime_add_to_registry(first_runtime, registry, &first_policy),
                 TURBO_TOOL_OK);
    check_int_eq(turbo_tool_runtime_add_to_registry(second_runtime, registry, &second_policy),
                 TURBO_TOOL_OK);
    check_size_eq(turbo_tool_registry_count(registry), 2);
    turbo_tool_runtime_destroy(first_runtime);
    turbo_tool_runtime_destroy(second_runtime);

    check_int_eq(
        turbo_tool_registry_get_execution_policy(registry, "first_runtime_tool", &observed),
        TURBO_TOOL_OK);
    check_int_eq(observed.mode, TURBO_TOOL_EXECUTION_EXCLUSIVE);
    check_int_eq(observed.idempotency, TURBO_TOOL_IDEMPOTENCY_KEYED);
    check_int_eq(
        turbo_tool_registry_execute(registry, "second_runtime_tool", "{\"ok\":2}", &output),
        TURBO_TOOL_OK);
    check_str_eq(output, "{\"ok\":2}");

    free(output);
    turbo_tool_registry_destroy(registry);
  }

  it("should leave the destination unchanged when a runtime name conflicts") {
    turbo_tool_runtime_t *runtime = turbo_tool_runtime_native_create();
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t existing = {
        "conflict", "Existing tool", "{\"type\":\"object\"}",   NULL,
        1,          test_echo_tool,  test_echo_tool_json_value, NULL,
        NULL};
    turbo_tool_definition_t first = {"would_be_added",
                                     "First runtime tool",
                                     "{\"type\":\"object\"}",
                                     NULL,
                                     1,
                                     test_echo_tool,
                                     test_echo_tool_json_value,
                                     NULL,
                                     NULL};
    turbo_tool_definition_t conflict = {"conflict",
                                        "Conflicting runtime tool",
                                        "{\"type\":\"object\"}",
                                        NULL,
                                        1,
                                        test_echo_tool,
                                        test_echo_tool_json_value,
                                        NULL,
                                        NULL};
    turbo_tool_execution_policy_t policy = {TURBO_TOOL_EXECUTION_SEQUENTIAL,
                                            TURBO_TOOL_IDEMPOTENCY_NONE};

    check_not_null(runtime);
    check_not_null(registry);
    check_int_eq(turbo_tool_registry_add(registry, &existing), TURBO_TOOL_OK);
    check_int_eq(turbo_tool_runtime_native_add_tool(runtime, &first), TURBO_TOOL_OK);
    check_int_eq(turbo_tool_runtime_native_add_tool(runtime, &conflict), TURBO_TOOL_OK);
    check_int_eq(turbo_tool_runtime_add_to_registry(runtime, registry, &policy),
                 TURBO_TOOL_DUPLICATE);
    check_size_eq(turbo_tool_registry_count(registry), 1);
    check_int_eq(turbo_tool_registry_get_execution_policy(registry, "would_be_added", &policy),
                 TURBO_TOOL_NOT_FOUND);

    turbo_tool_registry_destroy(registry);
    turbo_tool_runtime_destroy(runtime);
  }
}
