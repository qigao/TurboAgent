#include "turbo_codex_bridge.h"

#include <stdlib.h>
#include <string.h>

#include <turbo_str.h>

#include "turbo_runtime_json.h"

typedef struct turbo_codex_delegate_binding_s {
  turbo_codex_client_t *client;
  tstr_t model;
  tstr_t cwd;
  tstr_t sandbox;
  tstr_t approval_policy;
  uint64_t timeout_ms;
} turbo_codex_delegate_binding_t;

static const char turbo_codex_delegate_parameters[] =
    "{\"type\":\"object\",\"properties\":{\"task\":{\"type\":\"string\","
    "\"minLength\":1}},\"required\":[\"task\"],\"additionalProperties\":false}";

static void turbo_codex_delegate_binding_destroy(void *user_data) {
  turbo_codex_delegate_binding_t *binding = (turbo_codex_delegate_binding_t *)user_data;
  if (!binding) return;
  tstr_free(binding->model);
  tstr_free(binding->cwd);
  tstr_free(binding->sandbox);
  tstr_free(binding->approval_policy);
  free(binding);
}

static turbo_tool_status_t turbo_codex_tool_status(int rc) {
  if (rc == TURBO_OK) return TURBO_TOOL_OK;
  if (rc == TURBO_ECANCELED) return TURBO_TOOL_CANCELLED;
  if (rc == TURBO_ETIMEDOUT) return TURBO_TOOL_DEADLINE_EXCEEDED;
  if (rc == TURBO_EMSGSIZE) return TURBO_TOOL_OUTPUT_LIMIT;
  if (rc == TURBO_EBUSY) return TURBO_TOOL_BACKPRESSURE;
  return TURBO_TOOL_ERROR;
}

static int turbo_codex_delegate_execute(const json_value_t *arguments,
                                        json_value_t **out_result, void *user_data) {
  turbo_codex_delegate_binding_t *binding = (turbo_codex_delegate_binding_t *)user_data;
  turbo_codex_run_options_t options;
  const char *task;
  char *thread_id = NULL;
  char *turn_id = NULL;
  char *text = NULL;
  json_value_t *turn = NULL;
  json_value_t *result = NULL;
  int rc;
  if (out_result) *out_result = NULL;
  if (!binding || !binding->client || !arguments ||
      json_type(arguments) != JSON_OBJECT || !out_result) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  task = json_get_string(arguments, "task");
  if (!task || !task[0]) return TURBO_TOOL_INVALID_ARGUMENT;
  turbo_codex_run_options_init(&options);
  options.model = binding->model;
  options.cwd = binding->cwd;
  options.sandbox = binding->sandbox;
  options.approval_policy = binding->approval_policy;
  options.timeout_ms = binding->timeout_ms;
  rc = turbo_codex_client_run_text(binding->client, task, &options, &thread_id, &turn_id, &text,
                                   &turn);
  if (rc != TURBO_OK) {
    free(thread_id);
    free(turn_id);
    free(text);
    turbo_runtime_json_destroy(turn);
    return turbo_codex_tool_status(rc);
  }
  result = json_create_object();
  if (!result) {
    rc = TURBO_TOOL_OUT_OF_MEMORY;
    goto cleanup;
  }
  json_object_set_string(result, "threadId", thread_id);
  json_object_set_string(result, "turnId", turn_id);
  json_object_set_string(result, "text", text ? text : "");
  if (!json_object_add_checked(result, "turn", turn)) {
    rc = TURBO_TOOL_OUT_OF_MEMORY;
    goto cleanup;
  }
  turn = NULL;
  *out_result = result;
  result = NULL;
  rc = TURBO_TOOL_OK;

cleanup:
  free(thread_id);
  free(turn_id);
  free(text);
  turbo_runtime_json_destroy(turn);
  turbo_runtime_json_destroy(result);
  return rc;
}

void turbo_codex_delegate_tool_config_init(turbo_codex_delegate_tool_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_CODEX_DELEGATE_TOOL_ABI_VERSION;
  config->tool_name = "codex.delegate";
  config->sandbox = "workspace-write";
  config->approval_policy = "on-request";
  config->timeout_ms = 30U * 60U * 1000U;
}

turbo_tool_status_t turbo_codex_delegate_tool_register(
    turbo_tool_registry_t *registry, const turbo_codex_delegate_tool_config_t *config) {
  static const char *const capabilities[] = {"delegate"};
  turbo_codex_delegate_binding_t *binding;
  turbo_tool_definition_v3_t definition;
  turbo_tool_status_t status;
  if (!registry || !config || config->struct_size < sizeof(*config) ||
      config->abi_version != TURBO_CODEX_DELEGATE_TOOL_ABI_VERSION || !config->client ||
      !config->tool_name || !config->tool_name[0] || !config->timeout_ms ||
      !config->sandbox || !config->sandbox[0] || !config->approval_policy ||
      !config->approval_policy[0]) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  binding = (turbo_codex_delegate_binding_t *)calloc(1, sizeof(*binding));
  if (!binding) return TURBO_TOOL_OUT_OF_MEMORY;
  binding->client = config->client;
  binding->model = config->model ? tstr_dup(config->model) : NULL;
  binding->cwd = config->cwd ? tstr_dup(config->cwd) : NULL;
  binding->sandbox = tstr_dup(config->sandbox);
  binding->approval_policy = tstr_dup(config->approval_policy);
  binding->timeout_ms = config->timeout_ms;
  if ((config->model && !binding->model) || (config->cwd && !binding->cwd) ||
      !binding->sandbox || !binding->approval_policy) {
    turbo_codex_delegate_binding_destroy(binding);
    return TURBO_TOOL_OUT_OF_MEMORY;
  }
  memset(&definition, 0, sizeof(definition));
  definition.struct_size = sizeof(definition);
  definition.abi_version = TURBO_TOOL_DEFINITION_V3_ABI_VERSION;
  definition.definition.name = config->tool_name;
  definition.definition.description =
      "Delegate one bounded coding or repository task to Codex App Server.";
  definition.definition.parameters_json = turbo_codex_delegate_parameters;
  definition.definition.strict = 1;
  definition.definition.json_value_handler = turbo_codex_delegate_execute;
  definition.definition.user_data = binding;
  definition.definition.user_data_free = turbo_codex_delegate_binding_destroy;
  definition.execution_policy.mode = TURBO_TOOL_EXECUTION_EXCLUSIVE;
  definition.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_NONE;
  definition.required_capabilities = capabilities;
  definition.required_capability_count = sizeof(capabilities) / sizeof(capabilities[0]);
  status = turbo_tool_registry_add_v3(registry, &definition);
  if (status != TURBO_TOOL_OK) turbo_codex_delegate_binding_destroy(binding);
  return status;
}
