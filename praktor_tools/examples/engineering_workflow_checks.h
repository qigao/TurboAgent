#ifndef TURBO_ENGINEERING_WORKFLOW_EXAMPLE_H
#define TURBO_ENGINEERING_WORKFLOW_EXAMPLE_H

/* Host example, not installed API. Build/test implementations and their
 * resource bounds belong to the embedding Host; these tools only verify data. */
#include "turbo_praktor_tool_pack.h"
#include "turbo_tool_schema.h"
#include <string.h>

static const char engineering_request_schema[] =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"preset\":{\"type\":\"string\",\"enum\":[\"debug\",\"release\"]},"
    "\"source_revision\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64}},"
    "\"required\":[\"preset\",\"source_revision\"]}";
static const char engineering_build_schema[] =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"exit_code\":{\"type\":\"integer\"},"
    "\"expected_revision\":{\"type\":\"string\"},"
    "\"source_revision\":{\"type\":\"string\"},"
    "\"artifact_id\":{\"type\":\"string\"}},"
    "\"required\":[\"exit_code\",\"expected_revision\",\"source_revision\",\"artifact_id\"]}";
static const char engineering_tests_schema[] =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"exit_code\":{\"type\":\"integer\"},"
    "\"expected_revision\":{\"type\":\"string\"},"
    "\"source_revision\":{\"type\":\"string\"},"
    "\"expected_artifact\":{\"type\":\"string\"},"
    "\"artifact_id\":{\"type\":\"string\"},"
    "\"tests_total\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1000000},"
    "\"tests_passed\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1000000},"
    "\"tests_failed\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1000000},"
    "\"tests_skipped\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1000000}},"
    "\"required\":[\"exit_code\",\"expected_revision\",\"source_revision\",\"expected_artifact\","
    "\"artifact_id\",\"tests_total\",\"tests_passed\",\"tests_failed\",\"tests_skipped\"]}";

static const char *engineering_id(const json_value_t *object, const char *key) {
  const json_value_t *value = json_object_get(object, key);
  const char *text;
  size_t size, i;
  if (!value || json_type(value) != JSON_STRING) return NULL;
  text = json_string(value);
  size = json_string_len(value);
  if (!text || size == 0 || size > 64 || strlen(text) != size) return NULL;
  for (i = 0; i < size; ++i) {
    unsigned char c = (unsigned char)text[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) return NULL;
  }
  return text;
}

static int engineering_same_id(const json_value_t *left, const char *left_key,
                                const json_value_t *right, const char *right_key) {
  const char *a = engineering_id(left, left_key);
  const char *b = engineering_id(right, right_key);
  return a && b && strcmp(a, b) == 0;
}

static int engineering_zero(const json_value_t *object, const char *key) {
  return turbo_runtime_json_value_as_int64(json_object_get(object, key), INT64_MIN) == 0;
}

static int engineering_counts_verified(const json_value_t *object) {
  int64_t total = turbo_runtime_json_value_as_int64(json_object_get(object, "tests_total"), -1);
  int64_t passed = turbo_runtime_json_value_as_int64(json_object_get(object, "tests_passed"), -1);
  return engineering_zero(object, "exit_code") && total > 0 && total <= 1000000 &&
         passed == total && engineering_zero(object, "tests_failed") &&
         engineering_zero(object, "tests_skipped");
}

static int engineering_check(int stage, const json_value_t *arguments, json_value_t **out) {
  static const char *const schemas[] = {
      engineering_request_schema, engineering_build_schema, engineering_tests_schema};
  turbo_tool_definition_t definition = {0};
  const char *preset;
  int valid = 0;
  if (!out) return TURBO_TOOL_INVALID_ARGUMENT;
  *out = NULL;
  if (stage < 0 || stage > 2) return TURBO_TOOL_INVALID_ARGUMENT;
  definition.parameters_json = schemas[stage];
  definition.strict = 1;
  if (turbo_tool_schema_validate_arguments_json_value(&definition, arguments, NULL, 0) !=
      TURBO_TOOL_SCHEMA_VALID) return TURBO_TOOL_INVALID_ARGUMENT;
  if (stage == 0) {
    preset = engineering_id(arguments, "preset");
    valid = preset && (strcmp(preset, "debug") == 0 || strcmp(preset, "release") == 0) &&
            engineering_id(arguments, "source_revision");
  } else if (stage == 1) {
    valid = engineering_zero(arguments, "exit_code") && engineering_id(arguments, "artifact_id") &&
            engineering_same_id(arguments, "expected_revision", arguments, "source_revision");
  } else {
    valid = engineering_counts_verified(arguments) &&
            engineering_same_id(arguments, "expected_revision", arguments, "source_revision") &&
            engineering_same_id(arguments, "expected_artifact", arguments, "artifact_id");
  }
  if (!valid) return TURBO_TOOL_ERROR;
  *out = json_create_bool(1);
  return *out ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static int engineering_request_check(const json_value_t *args, json_value_t **out, void *user) {
  (void)user;
  return engineering_check(0, args, out);
}
static int engineering_build_check(const json_value_t *args, json_value_t **out, void *user) {
  (void)user;
  return engineering_check(1, args, out);
}
static int engineering_tests_check(const json_value_t *args, json_value_t **out, void *user) {
  (void)user;
  return engineering_check(2, args, out);
}

/* Registration is quiescent. Roll back only definitions added by this call. */
static turbo_tool_status_t engineering_register_checks(turbo_tool_registry_t *registry) {
  static const char *const names[] = {
      "engineering.check_request", "engineering.check_build", "engineering.check_tests"};
  static const char *const schemas[] = {
      engineering_request_schema, engineering_build_schema, engineering_tests_schema};
  static const turbo_tool_json_value_handler_fn handlers[] = {
      engineering_request_check, engineering_build_check, engineering_tests_check};
  static const char *const capabilities[] = {"runtime_tools"};
  size_t i;
  for (i = 0; i < 3; ++i) {
    turbo_tool_definition_v6_t definition = {0};
    turbo_tool_status_t status;
    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
    definition.definition.name = names[i];
    definition.definition.description = "Validate the reviewed engineering workflow contract.";
    definition.definition.parameters_json = schemas[i];
    definition.definition.strict = 1;
    definition.definition.json_value_handler = handlers[i];
    definition.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
    definition.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
    definition.required_capabilities = capabilities;
    definition.required_capability_count = 1;
    definition.result_schema_json = "{\"type\":\"boolean\",\"const\":true}";
    definition.strict_result = 1;
    definition.effect_flags = TURBO_TOOL_EFFECT_PURE;
    status = turbo_tool_registry_add_v6(registry, &definition);
    if (status != TURBO_TOOL_OK) {
      while (i) turbo_tool_registry_remove(registry, names[--i]);
      return status;
    }
  }
  return TURBO_TOOL_OK;
}

/* The Host supplies the reviewed path and exact approved tool projection.
 * Both remain outside model input; all borrowed callback dependencies outlive
 * the pack and its selections/agents. No legacy SDK profile is admitted. */
static turbo_tool_status_t engineering_register_workflow(
    turbo_praktor_tool_pack_t *pack, const char *path, const turbo_tool_registry_t *tools) {
  static const char *const capabilities[] = {"shell", "patch"};
  turbo_praktor_workflow_config_t config;
  if (!pack || !tools || !turbo_praktor_tool_pack_supports_workflow_plan(pack) ||
      !turbo_praktor_tool_pack_supports_host_tools(pack)) return TURBO_TOOL_INVALID_ARGUMENT;
  turbo_praktor_workflow_config_init(&config);
  config.tool_name = "engineering_build_test";
  config.description = "Build and test one Host-selected source revision with explicit verification.";
  config.workflow_path = path;
  config.strict = 1;
  config.approved_host_tools = tools;
  config.require_harness_safe = 1;
  config.required_capabilities = capabilities;
  config.required_capability_count = 2;
  config.execution_policy.mode = TURBO_TOOL_EXECUTION_EXCLUSIVE;
  config.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_NONE;
  return turbo_praktor_tool_pack_add_workflow(pack, &config);
}

/* TURBO_TOOL_OK alone is not task completion: Praktor can return structured
 * failures successfully. Consume these reviewed outputs, with the bound request. */
static int engineering_workflow_verified(const json_value_t *result, const json_value_t *request) {
  const json_value_t *outputs = json_object_get(result, "outputs");
  const json_value_t *verified = json_object_get(outputs, "verified");
  const char *status = engineering_id(result, "workflow_status");
  return status && strcmp(status, "success") == 0 &&
         verified && json_type(verified) == JSON_BOOL && json_bool(verified) &&
         engineering_id(outputs, "artifact_id") && engineering_zero(outputs, "build_exit_code") &&
         engineering_same_id(outputs, "preset", request, "preset") &&
         engineering_same_id(outputs, "source_revision", request, "source_revision") &&
         engineering_counts_verified(outputs);
}

#endif
