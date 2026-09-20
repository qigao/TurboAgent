#include "turbo_praktor_tool_pack.h"

#include <praktor.h>
#include <turbo_fs.h>
#include <turbo_str.h>

#include <stdlib.h>
#include <string.h>

enum {
  TURBO_PRAKTOR_DEFAULT_MAX_WORKFLOWS = 64,
  TURBO_PRAKTOR_DEFAULT_MAX_RESULT_BYTES = 1024 * 1024,
  TURBO_PRAKTOR_MAX_TOOL_NAME_BYTES = 128
};

static const char turbo_praktor_default_parameters[] =
    "{\"type\":\"object\",\"additionalProperties\":true}";
static const char *const turbo_praktor_default_capabilities[] = {
    "network", "shell", "patch", "outside_workspace"};

typedef struct turbo_praktor_binding_s {
  const praktor_api *api;
  tstr_t workflow_path;
  size_t max_result_bytes;
} turbo_praktor_binding_t;

struct turbo_praktor_tool_pack_s {
  const praktor_api *api;
  turbo_tool_registry_t *registry;
  size_t workflow_count;
  size_t max_workflows;
  size_t max_result_bytes;
};

static int turbo_praktor_api_valid(const praktor_api *api) {
  return api && api->struct_size >= sizeof(*api) &&
         api->abi_major == PRAKTOR_ABI_MAJOR &&
         (api->capabilities & PRAKTOR_CAPABILITY_JSON_WORKFLOW) != 0 &&
         api->execute_workflow && api->release_json;
}

static int turbo_praktor_execution_policy_valid(
    const turbo_tool_execution_policy_t *policy) {
  if (!policy || policy->mode < TURBO_TOOL_EXECUTION_SEQUENTIAL ||
      policy->mode > TURBO_TOOL_EXECUTION_EXCLUSIVE) {
    return 0;
  }
  return policy->idempotency >= TURBO_TOOL_IDEMPOTENCY_NONE &&
         policy->idempotency <= TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
}

static int turbo_praktor_tool_name_valid(const char *name) {
  size_t index;
  size_t length;
  if (!name || !(length = strlen(name)) ||
      length > TURBO_PRAKTOR_MAX_TOOL_NAME_BYTES) {
    return 0;
  }
  for (index = 0; index < length; ++index) {
    const unsigned char ch = (unsigned char)name[index];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
      return 0;
    }
  }
  return 1;
}

static int turbo_praktor_capabilities_valid(const char *const *capabilities,
                                             size_t capability_count) {
  size_t index;
  if (capability_count && !capabilities) return 0;
  for (index = 0; index < capability_count; ++index) {
    size_t previous;
    if (!capabilities[index] || !capabilities[index][0]) return 0;
    for (previous = 0; previous < index; ++previous) {
      if (strcmp(capabilities[previous], capabilities[index]) == 0) return 0;
    }
  }
  return 1;
}

static int turbo_praktor_schema_valid(const char *parameters_json) {
  json_value_t *schema = NULL;
  const char *type;
  int valid;
  if (!parameters_json) return 1;
  if (turbo_parse_json((const uint8_t *)parameters_json, strlen(parameters_json),
                       &schema) != 0 ||
      !schema || turbo_json_type(schema) != TURBO_JSON_OBJECT) {
    turbo_free_json(&schema);
    return 0;
  }
  type = turbo_json_get_string(schema, "type");
  valid = !type || strcmp(type, "object") == 0;
  turbo_free_json(&schema);
  return valid;
}

static void turbo_praktor_binding_destroy(void *user_data) {
  turbo_praktor_binding_t *binding = (turbo_praktor_binding_t *)user_data;
  if (!binding) return;
  tstr_free(binding->workflow_path);
  free(binding);
}

static const char *turbo_praktor_error_phase_name(praktor_error_phase phase) {
  switch (phase) {
    case PRAKTOR_ERROR_PHASE_REQUEST:
      return "request";
    case PRAKTOR_ERROR_PHASE_INPUT_JSON:
      return "input_json";
    case PRAKTOR_ERROR_PHASE_EXECUTION:
      return "execution";
    case PRAKTOR_ERROR_PHASE_RESULT_JSON:
      return "result_json";
    case PRAKTOR_ERROR_PHASE_NONE:
    default:
      return "none";
  }
}

static json_value_t *turbo_praktor_error_result(praktor_result status,
                                                const praktor_error *error) {
  json_value_t *result = turbo_json_create_object();
  if (!result) return NULL;
  turbo_json_object_set_string(result, "workflow_status", "error");
  turbo_json_object_set_number(result, "result_code", (double)status);
  turbo_json_object_set_string(
      result, "error_phase",
      turbo_praktor_error_phase_name(error ? error->phase : PRAKTOR_ERROR_PHASE_NONE));
  turbo_json_object_set_string(
      result, "error",
      error && error->message[0] ? error->message : "Praktor workflow invocation failed");
  return result;
}

static int turbo_praktor_copy_text(const char *data, size_t size,
                                    char **out_text) {
  char *copy;
  if (!data || !size || !out_text || size == SIZE_MAX) return -1;
  copy = (char *)malloc(size + 1);
  if (!copy) return -1;
  memcpy(copy, data, size);
  copy[size] = '\0';
  *out_text = copy;
  return 0;
}

static int turbo_praktor_execute_text(turbo_praktor_binding_t *binding,
                                      const char *input_json, size_t input_size,
                                      char **out_output) {
  praktor_execute_request request = PRAKTOR_EXECUTE_REQUEST_INIT;
  praktor_owned_json output = PRAKTOR_OWNED_JSON_INIT;
  praktor_error error = PRAKTOR_ERROR_INIT;
  praktor_result status;
  json_value_t *parsed = NULL;
  char *serialized = NULL;
  size_t serialized_size = 0;
  int rc = -1;

  if (out_output) *out_output = NULL;
  if (!binding || !binding->api || !input_json || !input_size || !out_output) return -1;

  request.workflow_path = binding->workflow_path;
  request.input_json = input_json;
  request.input_json_size = input_size;
  status = (praktor_result)binding->api->execute_workflow(&request, &output, &error);

  if (output.size > binding->max_result_bytes) goto cleanup;

  if (status == PRAKTOR_RESULT_SUCCESS ||
      status == PRAKTOR_RESULT_EXECUTION_FAILED) {
    if (!output.data || !output.size ||
        turbo_parse_json((const uint8_t *)output.data, output.size, &parsed) != 0 ||
        !parsed || turbo_json_type(parsed) != TURBO_JSON_OBJECT) {
      goto cleanup;
    }
    rc = turbo_praktor_copy_text(output.data, output.size, out_output);
    goto cleanup;
  }

  parsed = turbo_praktor_error_result(status, &error);
  if (!parsed) goto cleanup;
  serialized = turbo_json_serialize(parsed, &serialized_size);
  if (!serialized || serialized_size > binding->max_result_bytes) goto cleanup;
  rc = turbo_praktor_copy_text(serialized, serialized_size, out_output);

cleanup:
  if (serialized) turbo_json_serialize_free(serialized);
  turbo_free_json(&parsed);
  binding->api->release_json(&output);
  return rc;
}

static int turbo_praktor_execute_string(const char *arguments_json,
                                        char **out_output, void *user_data) {
  turbo_praktor_binding_t *binding = (turbo_praktor_binding_t *)user_data;
  const char *effective_arguments = arguments_json ? arguments_json : "{}";
  return turbo_praktor_execute_text(binding, effective_arguments,
                                    strlen(effective_arguments), out_output);
}

static int turbo_praktor_execute(const json_value_t *arguments,
                                 json_value_t **out_result, void *user_data) {
  turbo_praktor_binding_t *binding = (turbo_praktor_binding_t *)user_data;
  json_value_t *empty_arguments = NULL;
  const json_value_t *effective_arguments = arguments;
  char *input_json = NULL;
  size_t input_size = 0;
  char *output_json = NULL;
  json_value_t *parsed = NULL;
  int rc;

  if (out_result) *out_result = NULL;
  if (!binding || !binding->api || !out_result ||
      (arguments && turbo_json_type(arguments) != TURBO_JSON_OBJECT)) {
    return -1;
  }

  if (!effective_arguments) {
    empty_arguments = turbo_json_create_object();
    if (!empty_arguments) return -1;
    effective_arguments = empty_arguments;
  }
  input_json = turbo_json_serialize(effective_arguments, &input_size);
  turbo_runtime_json_destroy(empty_arguments);
  if (!input_json) return -1;

  rc = turbo_praktor_execute_text(binding, input_json, input_size, &output_json);
  turbo_json_serialize_free(input_json);
  if (rc != 0 || !output_json) {
    free(output_json);
    return -1;
  }
  if (turbo_parse_json((const uint8_t *)output_json, strlen(output_json), &parsed) != 0 ||
      !parsed || turbo_json_type(parsed) != TURBO_JSON_OBJECT) {
    free(output_json);
    turbo_free_json(&parsed);
    return -1;
  }
  free(output_json);
  *out_result = parsed;
  return 0;
}

void turbo_praktor_tool_pack_config_init(
    turbo_praktor_tool_pack_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_PRAKTOR_TOOL_PACK_ABI_VERSION;
  config->max_workflows = TURBO_PRAKTOR_DEFAULT_MAX_WORKFLOWS;
  config->max_result_bytes = TURBO_PRAKTOR_DEFAULT_MAX_RESULT_BYTES;
}

void turbo_praktor_workflow_config_init(
    turbo_praktor_workflow_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_PRAKTOR_WORKFLOW_CONFIG_ABI_VERSION;
  config->execution_policy.mode = TURBO_TOOL_EXECUTION_EXCLUSIVE;
  config->execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_NONE;
  config->required_capabilities = turbo_praktor_default_capabilities;
  config->required_capability_count =
      sizeof(turbo_praktor_default_capabilities) /
      sizeof(turbo_praktor_default_capabilities[0]);
}

turbo_praktor_tool_pack_t *
turbo_praktor_tool_pack_create(const turbo_praktor_tool_pack_config_t *config) {
  turbo_praktor_tool_pack_t *pack;
  const praktor_api *api;
  if (!config || config->struct_size < sizeof(*config) ||
      config->abi_version != TURBO_PRAKTOR_TOOL_PACK_ABI_VERSION ||
      !config->max_workflows || !config->max_result_bytes) {
    return NULL;
  }
  api = praktor_get_api();
  if (!turbo_praktor_api_valid(api)) return NULL;

  pack = (turbo_praktor_tool_pack_t *)calloc(1, sizeof(*pack));
  if (!pack) return NULL;
  pack->registry = turbo_tool_registry_create();
  if (!pack->registry) {
    free(pack);
    return NULL;
  }
  pack->api = api;
  pack->max_workflows = config->max_workflows;
  pack->max_result_bytes = config->max_result_bytes;
  return pack;
}

void turbo_praktor_tool_pack_destroy(turbo_praktor_tool_pack_t *pack) {
  if (!pack) return;
  turbo_tool_registry_destroy(pack->registry);
  free(pack);
}

static turbo_tool_status_t turbo_praktor_effective_capabilities(
    const turbo_praktor_workflow_config_t *config,
    const char ***out_capabilities, size_t *out_count) {
  const char *const *requested;
  size_t requested_count;
  const char **capabilities;
  size_t index;
  size_t count = 1;

  if (!config || !out_capabilities || !out_count) return TURBO_TOOL_INVALID_ARGUMENT;
  requested = config->required_capabilities;
  requested_count = config->required_capability_count;
  if (!requested && requested_count == 0) {
    requested = turbo_praktor_default_capabilities;
    requested_count = sizeof(turbo_praktor_default_capabilities) /
                      sizeof(turbo_praktor_default_capabilities[0]);
  }
  if (!turbo_praktor_capabilities_valid(requested, requested_count)) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  for (index = 0; index < requested_count; ++index) {
    if (strcmp(requested[index], "runtime_tools") != 0) ++count;
  }
  capabilities = (const char **)calloc(count, sizeof(*capabilities));
  if (!capabilities) return TURBO_TOOL_OUT_OF_MEMORY;
  capabilities[0] = "runtime_tools";
  count = 1;
  for (index = 0; index < requested_count; ++index) {
    if (strcmp(requested[index], "runtime_tools") == 0) continue;
    capabilities[count++] = requested[index];
  }
  *out_capabilities = capabilities;
  *out_count = count;
  return TURBO_TOOL_OK;
}

turbo_tool_status_t turbo_praktor_tool_pack_add_workflow(
    turbo_praktor_tool_pack_t *pack,
    const turbo_praktor_workflow_config_t *config) {
  turbo_praktor_binding_t *binding;
  turbo_tool_definition_v3_t definition;
  turbo_fs_stat_t metadata;
  const char **capabilities = NULL;
  size_t capability_count = 0;
  turbo_tool_status_t status;

  if (!pack || !config || config->struct_size < sizeof(*config) ||
      config->abi_version != TURBO_PRAKTOR_WORKFLOW_CONFIG_ABI_VERSION ||
      !turbo_praktor_tool_name_valid(config->tool_name) ||
      !config->description || !config->description[0] ||
      !config->workflow_path || !config->workflow_path[0] ||
      !turbo_fs_path_is_absolute(config->workflow_path) ||
      turbo_fs_lstat(config->workflow_path, &metadata) != 0 ||
      metadata.is_symlink || !metadata.is_file ||
      !turbo_praktor_schema_valid(config->parameters_json) ||
      (config->strict != 0 && config->strict != 1) ||
      !turbo_praktor_execution_policy_valid(&config->execution_policy)) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  if (pack->workflow_count >= pack->max_workflows) {
    return TURBO_TOOL_BACKPRESSURE;
  }

  status = turbo_praktor_effective_capabilities(config, &capabilities,
                                                 &capability_count);
  if (status != TURBO_TOOL_OK) return status;

  binding = (turbo_praktor_binding_t *)calloc(1, sizeof(*binding));
  if (!binding) {
    free(capabilities);
    return TURBO_TOOL_OUT_OF_MEMORY;
  }
  binding->api = pack->api;
  binding->workflow_path = tstr_dup(config->workflow_path);
  binding->max_result_bytes = pack->max_result_bytes;
  if (!binding->workflow_path) {
    free(capabilities);
    turbo_praktor_binding_destroy(binding);
    return TURBO_TOOL_OUT_OF_MEMORY;
  }

  memset(&definition, 0, sizeof(definition));
  definition.struct_size = sizeof(definition);
  definition.abi_version = TURBO_TOOL_DEFINITION_V3_ABI_VERSION;
  definition.definition.name = config->tool_name;
  definition.definition.description = config->description;
  definition.definition.parameters_json =
      config->parameters_json ? config->parameters_json
                              : turbo_praktor_default_parameters;
  definition.definition.strict = config->strict;
  definition.definition.handler = turbo_praktor_execute_string;
  definition.definition.json_value_handler = turbo_praktor_execute;
  definition.definition.user_data = binding;
  definition.definition.user_data_free = turbo_praktor_binding_destroy;
  definition.execution_policy = config->execution_policy;
  definition.required_capabilities = capabilities;
  definition.required_capability_count = capability_count;

  status = turbo_tool_registry_add_v3(pack->registry, &definition);
  free(capabilities);
  if (status != TURBO_TOOL_OK) {
    turbo_praktor_binding_destroy(binding);
    return status;
  }
  ++pack->workflow_count;
  return TURBO_TOOL_OK;
}

turbo_tool_registry_t *
turbo_praktor_tool_pack_registry(turbo_praktor_tool_pack_t *pack) {
  return pack ? pack->registry : NULL;
}

size_t turbo_praktor_tool_pack_workflow_count(
    const turbo_praktor_tool_pack_t *pack) {
  return pack ? pack->workflow_count : 0;
}
