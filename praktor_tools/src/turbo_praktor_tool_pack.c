#include "turbo_praktor_tool_pack.h"

#include <praktor.h>
#include <json_parser.h>
#include <turbo_runtime_json.h>
#include <turbo_tool_schema.h>
#include <salts_fs.h>
#include <salts/clock.h>
#include <tstr.h>

#include <stdlib.h>
#include <string.h>

#if defined(PRAKTOR_CAPABILITY_WORKFLOW_PLAN)
#define TURBO_PRAKTOR_HAS_WORKFLOW_PLAN 1
#else
#define TURBO_PRAKTOR_HAS_WORKFLOW_PLAN 0
#endif

#if defined(PRAKTOR_CAPABILITY_EXECUTION_EVENTS)
#define TURBO_PRAKTOR_HAS_EXECUTION_EVENTS 1
#else
#define TURBO_PRAKTOR_HAS_EXECUTION_EVENTS 0
#endif

#if defined(PRAKTOR_CAPABILITY_HOST_TOOL) && PRAKTOR_ABI_MINOR >= 5
#define TURBO_PRAKTOR_HAS_HOST_TOOL 1
#else
#define TURBO_PRAKTOR_HAS_HOST_TOOL 0
#endif

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
  tstr workflow_path;
  size_t max_result_bytes;
  int project_agent_output;
  const turbo_tool_registry_t *approved_host_tools;
#if TURBO_PRAKTOR_HAS_WORKFLOW_PLAN
  praktor_workflow_plan *plan;
#endif
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
         api->abi_minor >= 5u &&
         (api->capabilities & PRAKTOR_CAPABILITY_JSON_WORKFLOW) != 0 &&
         (api->capabilities & PRAKTOR_CAPABILITY_EXECUTION_CONTROL) != 0 &&
         (api->capabilities & PRAKTOR_CAPABILITY_WORKFLOW_PLAN) != 0 &&
         (api->capabilities & PRAKTOR_CAPABILITY_HOST_TOOL) != 0 &&
         api->execute_workflow && api->execute_workflow_controlled &&
         api->compile_workflow && api->describe_workflow_plan &&
         api->execute_workflow_plan && api->release_workflow_plan &&
         api->execute_workflow_plan_host_tools && api->release_json;
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
  schema = json_parse(parameters_json, strlen(parameters_json));
  if (!schema || json_type(schema) != JSON_OBJECT) {
    turbo_runtime_json_destroy(schema);
    return 0;
  }
  type = json_get_string(schema, "type");
  valid = !type || strcmp(type, "object") == 0;
  turbo_runtime_json_destroy(schema);
  return valid;
}

static int turbo_praktor_workflow_config_valid(
    const turbo_praktor_workflow_config_t *config) {
  if (!config) return 0;
  if (config->abi_version == TURBO_PRAKTOR_WORKFLOW_CONFIG_ABI_VERSION_V1) {
    return config->struct_size >= TURBO_PRAKTOR_WORKFLOW_CONFIG_V1_SIZE;
  }
  if (config->abi_version == TURBO_PRAKTOR_WORKFLOW_CONFIG_ABI_VERSION_V2) {
    return config->struct_size >= TURBO_PRAKTOR_WORKFLOW_CONFIG_V2_SIZE;
  }
  return config->abi_version == TURBO_PRAKTOR_WORKFLOW_CONFIG_ABI_VERSION &&
         config->struct_size >= sizeof(*config);
}

static int turbo_praktor_require_harness_safe(
    const turbo_praktor_workflow_config_t *config) {
  return config &&
         config->abi_version >= TURBO_PRAKTOR_WORKFLOW_CONFIG_ABI_VERSION_V2 &&
         config->struct_size >= TURBO_PRAKTOR_WORKFLOW_CONFIG_V2_SIZE &&
         config->require_harness_safe != 0;
}

static const turbo_tool_registry_t *turbo_praktor_approved_host_tools(
    const turbo_praktor_workflow_config_t *config) {
  return config &&
         config->abi_version >= TURBO_PRAKTOR_WORKFLOW_CONFIG_ABI_VERSION &&
         config->struct_size >= sizeof(*config)
             ? config->approved_host_tools
             : NULL;
}

typedef struct turbo_praktor_capability_list_s {
  const char **items;
  size_t count;
  size_t capacity;
} turbo_praktor_capability_list_t;

static void turbo_praktor_capability_list_destroy(
    turbo_praktor_capability_list_t *list) {
  if (!list) return;
  free(list->items);
  memset(list, 0, sizeof(*list));
}

static turbo_tool_status_t turbo_praktor_capability_add(
    turbo_praktor_capability_list_t *list, const char *capability) {
  size_t index;
  const char **resized;
  size_t next_capacity;
  if (!list || !capability || !capability[0]) return TURBO_TOOL_INVALID_ARGUMENT;
  for (index = 0; index < list->count; ++index) {
    if (strcmp(list->items[index], capability) == 0) return TURBO_TOOL_OK;
  }
  if (list->count == list->capacity) {
    next_capacity = list->capacity ? list->capacity * 2 : 8;
    resized = (const char **)realloc(
        list->items, next_capacity * sizeof(*resized));
    if (!resized) return TURBO_TOOL_OUT_OF_MEMORY;
    list->items = resized;
    list->capacity = next_capacity;
  }
  list->items[list->count++] = capability;
  return TURBO_TOOL_OK;
}

static turbo_tool_status_t turbo_praktor_add_conservative_capabilities(
    turbo_praktor_capability_list_t *list) {
  static const char *const conservative[] = {
      "network", "shell", "patch", "outside_workspace"};
  size_t index;
  turbo_tool_status_t status;
  for (index = 0; index < sizeof(conservative) / sizeof(conservative[0]); ++index) {
    status = turbo_praktor_capability_add(list, conservative[index]);
    if (status != TURBO_TOOL_OK) return status;
  }
  return TURBO_TOOL_OK;
}

#if TURBO_PRAKTOR_HAS_WORKFLOW_PLAN
static turbo_tool_status_t turbo_praktor_capability_from_effect(
    turbo_praktor_capability_list_t *list, const char *effect) {
  turbo_tool_status_t status;
  if (!effect || !effect[0]) return TURBO_TOOL_INVALID_ARGUMENT;
  if (strcmp(effect, "network") == 0) {
    return turbo_praktor_capability_add(list, "network");
  }
  if (strcmp(effect, "process") == 0 ||
      strcmp(effect, "system_control") == 0) {
    return turbo_praktor_capability_add(list, "shell");
  }
  if (strcmp(effect, "filesystem_write") == 0) {
    return turbo_praktor_capability_add(list, "patch");
  }
  if (strcmp(effect, "outside_workspace") == 0) {
    return turbo_praktor_capability_add(list, "outside_workspace");
  }
  if (strcmp(effect, "plugin") == 0 ||
      strcmp(effect, "native_extension") == 0 ||
      strcmp(effect, "model_api") == 0) {
    return turbo_praktor_capability_add(list, "custom_tools");
  }
  if (strcmp(effect, "filesystem_read") == 0) {
    return TURBO_TOOL_OK;
  }
  status = turbo_praktor_capability_add(list, "custom_tools");
  if (status != TURBO_TOOL_OK) return status;
  return turbo_praktor_add_conservative_capabilities(list);
}

static int turbo_praktor_plan_metadata_valid(
    const json_value_t *description, int require_harness_safe) {
  const json_value_t *schema;
  const json_value_t *profiles;
  const json_value_t *profile;
  if (!description || json_type(description) != JSON_OBJECT) return 0;
  schema = json_object_get(description, "input_schema");
  if (!schema || json_type(schema) != JSON_OBJECT) return 0;
  if (!require_harness_safe) return 1;
  profiles = json_object_get(description, "profiles");
  profile = profiles && json_type(profiles) == JSON_OBJECT
                ? json_object_get(profiles, "harness_safe")
                : NULL;
  return profile && json_type(profile) == JSON_OBJECT &&
         json_get_bool(profile, "qualified", false);
}

static turbo_tool_status_t turbo_praktor_effect_capabilities(
    const json_value_t *description,
    turbo_praktor_capability_list_t *list) {
  const json_value_t *manifest;
  const json_value_t *effects;
  size_t index;
  turbo_tool_status_t status;
  manifest = description ? json_object_get(description, "effect_manifest") : NULL;
  if (!manifest || json_type(manifest) != JSON_OBJECT) {
    return turbo_praktor_add_conservative_capabilities(list);
  }
  if (json_get_bool(manifest, "unknown_effects", false)) {
    status = turbo_praktor_capability_add(list, "custom_tools");
    if (status != TURBO_TOOL_OK) return status;
    return turbo_praktor_add_conservative_capabilities(list);
  }
  effects = json_object_get(manifest, "effects");
  if (!effects || json_type(effects) != JSON_ARRAY) {
    return turbo_praktor_add_conservative_capabilities(list);
  }
  for (index = 0; index < json_array_size(effects); ++index) {
    const json_value_t *value = json_array_get(effects, index);
    const char *effect = value && json_type(value) == JSON_STRING
                             ? json_string(value)
                             : NULL;
    if (!effect) return turbo_praktor_add_conservative_capabilities(list);
    status = turbo_praktor_capability_from_effect(list, effect);
    if (status != TURBO_TOOL_OK) return status;
  }
  return TURBO_TOOL_OK;
}

static turbo_tool_status_t turbo_praktor_compile_plan(
    const praktor_api *api, const char *workflow_path,
    praktor_workflow_plan **out_plan, json_value_t **out_description) {
  praktor_compile_request request = PRAKTOR_COMPILE_REQUEST_INIT;
  praktor_owned_json description = PRAKTOR_OWNED_JSON_INIT;
  praktor_error error = PRAKTOR_ERROR_INIT;
  praktor_result status;
  if (!api || !workflow_path || !out_plan || !out_description ||
      (api->capabilities & PRAKTOR_CAPABILITY_WORKFLOW_PLAN) == 0 ||
      !api->compile_workflow || !api->describe_workflow_plan ||
      !api->release_workflow_plan) {
    return TURBO_TOOL_NOT_FOUND;
  }
  *out_plan = NULL;
  *out_description = NULL;
  request.workflow_path = workflow_path;
  status = (praktor_result)api->compile_workflow(&request, out_plan, &error);
  if (status != PRAKTOR_RESULT_SUCCESS || !*out_plan) {
    return TURBO_TOOL_ERROR;
  }
  status = (praktor_result)api->describe_workflow_plan(
      *out_plan, &description, &error);
  if (status != PRAKTOR_RESULT_SUCCESS || !description.data || !description.size) {
    api->release_workflow_plan(*out_plan);
    *out_plan = NULL;
    api->release_json(&description);
    return TURBO_TOOL_ERROR;
  }
  *out_description = json_parse(description.data, description.size);
  api->release_json(&description);
  if (!*out_description || json_type(*out_description) != JSON_OBJECT) {
    turbo_runtime_json_destroy(*out_description);
    *out_description = NULL;
    api->release_workflow_plan(*out_plan);
    *out_plan = NULL;
    return TURBO_TOOL_ERROR;
  }
  return TURBO_TOOL_OK;
}
#endif

static void turbo_praktor_binding_destroy(void *user_data) {
  turbo_praktor_binding_t *binding = (turbo_praktor_binding_t *)user_data;
  if (!binding) return;
#if TURBO_PRAKTOR_HAS_WORKFLOW_PLAN
  if (binding->plan && binding->api && binding->api->release_workflow_plan) {
    binding->api->release_workflow_plan(binding->plan);
  }
#endif
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
#if PRAKTOR_ABI_MINOR >= 2
    case PRAKTOR_ERROR_PHASE_PLAN:
      return "plan";
#endif
#if PRAKTOR_ABI_MINOR >= 3
    case PRAKTOR_ERROR_PHASE_INPUT_CONTRACT:
      return "input_contract";
#endif
    case PRAKTOR_ERROR_PHASE_NONE:
    default:
      return "none";
  }
}

static json_value_t *turbo_praktor_error_result(praktor_result status,
                                                const praktor_error *error) {
  json_value_t *result = json_create_object();
  if (!result) return NULL;
  json_object_set_string(result, "workflow_status", "error");
  json_object_set_number(result, "result_code", (double)status);
  json_object_set_string(
      result, "error_phase",
      turbo_praktor_error_phase_name(error ? error->phase : PRAKTOR_ERROR_PHASE_NONE));
  json_object_set_string(
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

static int32_t PRAKTOR_CALL turbo_praktor_cancel_probe(void *user_data) {
  const turbo_cancel_token_t *token = (const turbo_cancel_token_t *)user_data;
  if (!token || turbo_cancel_token_check(token) == SALTS_OK) return 0;
  /*
   * Deadline expiry is represented separately by Praktor timeout_ms so the
   * backend reports TIMED_OUT instead of collapsing it into CANCELLED.
   */
  return turbo_cancel_token_reason(token) == TURBO_CANCEL_DEADLINE ? 0 : 1;
}

static turbo_tool_status_t turbo_praktor_context_status(
    const turbo_tool_execution_context_t *context) {
  int rc;
  if (!context) return TURBO_TOOL_OK;
  if (context->cancel_token) {
    rc = turbo_cancel_token_check(context->cancel_token);
    if (rc != 0) {
      return turbo_cancel_token_reason(context->cancel_token) == TURBO_CANCEL_DEADLINE
                 ? TURBO_TOOL_DEADLINE_EXCEEDED
                 : TURBO_TOOL_CANCELLED;
    }
  }
  if (context->deadline_mono_ms && salts_monotonic_ms() >= context->deadline_mono_ms) {
    return TURBO_TOOL_DEADLINE_EXCEEDED;
  }
  return TURBO_TOOL_OK;
}

static void turbo_praktor_control_from_context(
    const turbo_tool_execution_context_t *context,
    praktor_execution_control *control) {
  uint64_t now;
  uint64_t deadline;
  if (!context || !control) return;
  if (context->cancel_token) {
    control->is_cancelled = turbo_praktor_cancel_probe;
    control->user_data = (void *)context->cancel_token;
  }
  deadline = context->deadline_mono_ms;
  if (!deadline && context->cancel_token) {
    deadline = turbo_cancel_token_deadline_mono_ms(context->cancel_token);
  }
  if (!deadline) return;
  now = salts_monotonic_ms();
  control->timeout_ms = deadline > now ? deadline - now : 1u;
}

static int turbo_praktor_context_has_observation(
    const turbo_tool_execution_context_t *context) {
  return context &&
         context->abi_version >= TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION &&
         context->struct_size >= sizeof(*context);
}

#if TURBO_PRAKTOR_HAS_EXECUTION_EVENTS
static const char *turbo_praktor_event_name(praktor_event_type type) {
  switch (type) {
    case PRAKTOR_EVENT_WORKFLOW_STARTED:
      return "praktor.workflow.started";
    case PRAKTOR_EVENT_TASK_STARTED:
      return "praktor.task.started";
    case PRAKTOR_EVENT_TASK_PROGRESS:
      return "praktor.task.progress";
    case PRAKTOR_EVENT_TASK_COMPLETED:
      return "praktor.task.completed";
    case PRAKTOR_EVENT_TASK_FAILED:
      return "praktor.task.failed";
    case PRAKTOR_EVENT_WORKFLOW_COMPLETED:
      return "praktor.workflow.completed";
    default:
      return "praktor.event";
  }
}

static void PRAKTOR_CALL turbo_praktor_event_bridge(
    const praktor_execution_event *event, void *user_data) {
  const turbo_tool_execution_context_t *context =
      (const turbo_tool_execution_context_t *)user_data;
  json_value_t *payload = NULL;
  json_value_t *trace = NULL;
  char *payload_text = NULL;
  int status = 0;
  if (!event || !turbo_praktor_context_has_observation(context) ||
      !context->event_sink) {
    return;
  }

  payload = json_create_object();
  trace = json_create_object();
  if (!payload || !trace) goto cleanup;

  json_object_set_number(payload, "sequence", (double)event->sequence);
  if (event->task_name) json_object_set_string(payload, "task_name", event->task_name);
  if (event->status) json_object_set_string(payload, "status", event->status);
  if (event->message) json_object_set_string(payload, "message", event->message);
  if (event->thread_id) json_object_set_string(payload, "thread_id", event->thread_id);
  if (event->run_id) json_object_set_string(payload, "run_id", event->run_id);
  if (event->turn_id) json_object_set_string(payload, "turn_id", event->turn_id);
  if (event->tool_call_id) {
    json_object_set_string(payload, "tool_call_id", event->tool_call_id);
  }

  payload_text = json_serialize(payload, NULL);
  if (!payload_text) goto cleanup;

  if (event->type == PRAKTOR_EVENT_TASK_FAILED ||
      (event->type == PRAKTOR_EVENT_WORKFLOW_COMPLETED &&
       event->status && strcmp(event->status, "success") != 0)) {
    status = -1;
  }
  json_object_set_string(trace, "kind", "trace");
  json_object_set_string(trace, "name", turbo_praktor_event_name(event->type));
  json_object_set_string(trace, "detail", event->status ? event->status : "");
  json_object_set_string(trace, "payload", payload_text);
  json_object_set_number(trace, "status", (double)status);

  context->event_sink(trace, context->event_sink_user_data);

cleanup:
  if (payload_text) json_serialize_free(payload_text);
  turbo_runtime_json_destroy(payload);
  turbo_runtime_json_destroy(trace);
}
#endif

static turbo_tool_status_t turbo_praktor_execute_text(
    turbo_praktor_binding_t *binding, const char *input_json, size_t input_size,
    const turbo_tool_execution_context_t *context, char **out_output) {
  praktor_execution_control control = PRAKTOR_EXECUTION_CONTROL_INIT;
  praktor_owned_json output = PRAKTOR_OWNED_JSON_INIT;
  praktor_error error = PRAKTOR_ERROR_INIT;
  praktor_result status;
  turbo_tool_status_t tool_status;
  json_value_t *parsed = NULL;
  const json_value_t *projection = NULL;
  char *serialized = NULL;
  size_t serialized_size = 0;

  if (out_output) *out_output = NULL;
  if (!binding || !binding->api || !input_json || !input_size || !out_output) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  tool_status = turbo_praktor_context_status(context);
  if (tool_status != TURBO_TOOL_OK) return tool_status;
  if (context) turbo_praktor_control_from_context(context, &control);

#if TURBO_PRAKTOR_HAS_WORKFLOW_PLAN
  if (binding->plan && binding->api->execute_workflow_plan) {
    praktor_plan_execute_request request = PRAKTOR_PLAN_EXECUTE_REQUEST_INIT;
    request.plan = binding->plan;
    request.input_json = input_json;
    request.input_json_size = input_size;
#if TURBO_PRAKTOR_HAS_EXECUTION_EVENTS
    if (context &&
        (binding->api->capabilities & PRAKTOR_CAPABILITY_EXECUTION_EVENTS) != 0 &&
        binding->api->execute_workflow_plan_observed) {
      praktor_execution_observer observer = PRAKTOR_EXECUTION_OBSERVER_INIT;
      observer.on_event = turbo_praktor_event_bridge;
      observer.user_data = (void *)context;
      observer.thread_id = context->thread_id;
      observer.run_id = context->run_id;
      observer.turn_id = context->turn_id;
      observer.tool_call_id = context->tool_call_id;
      status = (praktor_result)binding->api->execute_workflow_plan_observed(
          &request, &control, &observer, &output, &error);
    } else
#endif
    {
      status = (praktor_result)binding->api->execute_workflow_plan(
          &request, context ? &control : NULL, &output, &error);
    }
  } else
#endif
  {
    praktor_execute_request request = PRAKTOR_EXECUTE_REQUEST_INIT;
    request.workflow_path = binding->workflow_path;
    request.input_json = input_json;
    request.input_json_size = input_size;
    if (context) {
      status = (praktor_result)binding->api->execute_workflow_controlled(
          &request, &control, &output, &error);
    } else {
      status = (praktor_result)binding->api->execute_workflow(
          &request, &output, &error);
    }
  }

  if (status == PRAKTOR_RESULT_CANCELLED) {
    tool_status = TURBO_TOOL_CANCELLED;
    goto cleanup;
  }
  if (status == PRAKTOR_RESULT_TIMED_OUT) {
    tool_status = TURBO_TOOL_DEADLINE_EXCEEDED;
    goto cleanup;
  }
  if (output.size > binding->max_result_bytes) {
    tool_status = TURBO_TOOL_ERROR;
    goto cleanup;
  }

  if (status == PRAKTOR_RESULT_SUCCESS ||
      status == PRAKTOR_RESULT_EXECUTION_FAILED) {
    parsed = output.data && output.size ? json_parse(output.data, output.size) : NULL;
    if (!parsed || json_type(parsed) != JSON_OBJECT) {
      tool_status = TURBO_TOOL_ERROR;
      goto cleanup;
    }

    if (turbo_praktor_context_has_observation(context) &&
        context->detail_sink) {
      context->detail_sink(parsed, context->detail_sink_user_data);
    }

    projection = binding->project_agent_output
                     ? json_object_get(parsed, "agent_output")
                     : NULL;
    if (projection && json_type(projection) == JSON_OBJECT) {
      serialized = json_serialize(projection, &serialized_size);
      if (!serialized || serialized_size > binding->max_result_bytes ||
          turbo_praktor_copy_text(serialized, serialized_size, out_output) != 0) {
        tool_status = TURBO_TOOL_ERROR;
        goto cleanup;
      }
    } else if (turbo_praktor_copy_text(
                   output.data, output.size, out_output) != 0) {
      tool_status = TURBO_TOOL_ERROR;
      goto cleanup;
    }

    tool_status = TURBO_TOOL_OK;
    goto cleanup;
  }

  parsed = turbo_praktor_error_result(status, &error);
  if (!parsed) {
    tool_status = TURBO_TOOL_ERROR;
    goto cleanup;
  }
  serialized = json_serialize(parsed, &serialized_size);
  if (!serialized || serialized_size > binding->max_result_bytes ||
      turbo_praktor_copy_text(serialized, serialized_size, out_output) != 0) {
    tool_status = TURBO_TOOL_ERROR;
    goto cleanup;
  }
  tool_status = TURBO_TOOL_OK;

cleanup:
  if (serialized) json_serialize_free(serialized);
  turbo_runtime_json_destroy(parsed);
  binding->api->release_json(&output);
  return tool_status;
}

static int turbo_praktor_execute_string(const char *arguments_json,
                                        char **out_output, void *user_data) {
  turbo_praktor_binding_t *binding = (turbo_praktor_binding_t *)user_data;
  const char *effective_arguments = arguments_json ? arguments_json : "{}";
  return turbo_praktor_execute_text(binding, effective_arguments,
                                    strlen(effective_arguments), NULL, out_output) ==
                 TURBO_TOOL_OK
             ? 0
             : -1;
}

static turbo_tool_status_t turbo_praktor_execute_string_with_context(
    const char *arguments_json, const turbo_tool_execution_context_t *context,
    char **out_output, void *user_data) {
  turbo_praktor_binding_t *binding = (turbo_praktor_binding_t *)user_data;
  const char *effective_arguments = arguments_json ? arguments_json : "{}";
  return turbo_praktor_execute_text(binding, effective_arguments,
                                    strlen(effective_arguments), context, out_output);
}

static turbo_tool_status_t turbo_praktor_execute_json_common(
    const json_value_t *arguments, const turbo_tool_execution_context_t *context,
    json_value_t **out_result, void *user_data) {
  turbo_praktor_binding_t *binding = (turbo_praktor_binding_t *)user_data;
  json_value_t *empty_arguments = NULL;
  const json_value_t *effective_arguments = arguments;
  char *input_json = NULL;
  size_t input_size = 0;
  char *output_json = NULL;
  json_value_t *parsed = NULL;
  turbo_tool_status_t status;

  if (out_result) *out_result = NULL;
  if (!binding || !binding->api || !out_result ||
      (arguments && json_type(arguments) != JSON_OBJECT)) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  if (!effective_arguments) {
    empty_arguments = json_create_object();
    if (!empty_arguments) return TURBO_TOOL_OUT_OF_MEMORY;
    effective_arguments = empty_arguments;
  }
  input_json = json_serialize(effective_arguments, &input_size);
  turbo_runtime_json_destroy(empty_arguments);
  if (!input_json) return TURBO_TOOL_OUT_OF_MEMORY;

  status = turbo_praktor_execute_text(binding, input_json, input_size, context, &output_json);
  json_serialize_free(input_json);
  if (status != TURBO_TOOL_OK) {
    free(output_json);
    return status;
  }
  parsed = output_json ? json_parse(output_json, strlen(output_json)) : NULL;
  if (!parsed || json_type(parsed) != JSON_OBJECT) {
    free(output_json);
    turbo_runtime_json_destroy(parsed);
    return TURBO_TOOL_ERROR;
  }
  free(output_json);
  *out_result = parsed;
  return TURBO_TOOL_OK;
}

static int turbo_praktor_execute(const json_value_t *arguments,
                                 json_value_t **out_result, void *user_data) {
  return turbo_praktor_execute_json_common(arguments, NULL, out_result, user_data) ==
                 TURBO_TOOL_OK
             ? 0
             : -1;
}

static turbo_tool_status_t turbo_praktor_execute_with_context(
    const json_value_t *arguments, const turbo_tool_execution_context_t *context,
    json_value_t **out_result, void *user_data) {
  return turbo_praktor_execute_json_common(arguments, context, out_result, user_data);
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
  config->required_capabilities = NULL;
  config->required_capability_count = 0;
  config->require_harness_safe = 1;
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
    const json_value_t *plan_description, int plan_bound,
    turbo_praktor_capability_list_t *out) {
  const char *const *requested;
  size_t requested_count;
  size_t index;
  turbo_tool_status_t status;

  if (!config || !out) return TURBO_TOOL_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));

  status = turbo_praktor_capability_add(out, "runtime_tools");
  if (status != TURBO_TOOL_OK) return status;

#if TURBO_PRAKTOR_HAS_WORKFLOW_PLAN
  if (plan_bound) {
    status = turbo_praktor_effect_capabilities(plan_description, out);
    if (status != TURBO_TOOL_OK) goto fail;
  } else
#endif
  {
    status = turbo_praktor_add_conservative_capabilities(out);
    if (status != TURBO_TOOL_OK) goto fail;
  }

  requested = config->required_capabilities;
  requested_count = config->required_capability_count;

  /*
   * Compatibility mode preserves the pre-WorkflowPlan conservative default
   * unless the host explicitly supplies its reviewed requirements. Harness-safe
   * mode may rely on the plan-derived manifest because profile qualification is
   * mandatory.
   */
  if (plan_bound && !turbo_praktor_require_harness_safe(config) &&
      requested_count == 0) {
    status = turbo_praktor_add_conservative_capabilities(out);
    if (status != TURBO_TOOL_OK) goto fail;
  }

  if (!turbo_praktor_capabilities_valid(requested, requested_count)) {
    status = TURBO_TOOL_INVALID_ARGUMENT;
    goto fail;
  }
  for (index = 0; index < requested_count; ++index) {
    status = turbo_praktor_capability_add(out, requested[index]);
    if (status != TURBO_TOOL_OK) goto fail;
  }
  return TURBO_TOOL_OK;

fail:
  turbo_praktor_capability_list_destroy(out);
  return status;
}

turbo_tool_status_t turbo_praktor_tool_pack_add_workflow(
    turbo_praktor_tool_pack_t *pack,
    const turbo_praktor_workflow_config_t *config) {
  turbo_praktor_binding_t *binding = NULL;
  turbo_tool_definition_v4_t definition;
  salts_fs_stat_t metadata;
  turbo_praktor_capability_list_t capabilities;
  turbo_tool_status_t status;
  const char *parameters_json;
  char *generated_schema = NULL;
  json_value_t *plan_description = NULL;
  int plan_bound = 0;
#if TURBO_PRAKTOR_HAS_WORKFLOW_PLAN
  praktor_workflow_plan *plan = NULL;
#endif

  if (!pack || !turbo_praktor_workflow_config_valid(config) ||
      !turbo_praktor_tool_name_valid(config->tool_name) ||
      !config->description || !config->description[0] ||
      !config->workflow_path || !config->workflow_path[0] ||
      !salts_fs_path_is_absolute(config->workflow_path) ||
      salts_fs_lstat(config->workflow_path, &metadata) != 0 ||
      metadata.is_symlink || !metadata.is_file ||
      !turbo_praktor_schema_valid(config->parameters_json) ||
      (config->strict != 0 && config->strict != 1) ||
      !turbo_praktor_execution_policy_valid(&config->execution_policy)) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  if (pack->workflow_count >= pack->max_workflows) {
    return TURBO_TOOL_BACKPRESSURE;
  }

#if TURBO_PRAKTOR_HAS_WORKFLOW_PLAN
  if (config->abi_version >= TURBO_PRAKTOR_WORKFLOW_CONFIG_ABI_VERSION &&
      (pack->api->capabilities & PRAKTOR_CAPABILITY_WORKFLOW_PLAN) != 0 &&
      pack->api->compile_workflow && pack->api->describe_workflow_plan &&
      pack->api->execute_workflow_plan && pack->api->release_workflow_plan) {
    status = turbo_praktor_compile_plan(
        pack->api, config->workflow_path, &plan, &plan_description);
    if (status != TURBO_TOOL_OK) return status;
    plan_bound = 1;
    if (!turbo_praktor_plan_metadata_valid(
            plan_description, turbo_praktor_require_harness_safe(config))) {
      turbo_runtime_json_destroy(plan_description);
      pack->api->release_workflow_plan(plan);
      return TURBO_TOOL_UNKNOWN_SIDE_EFFECT;
    }
  }
#endif

  status = turbo_praktor_effective_capabilities(
      config, plan_description, plan_bound, &capabilities);
  if (status != TURBO_TOOL_OK) goto cleanup;

  parameters_json = config->parameters_json
                        ? config->parameters_json
                        : turbo_praktor_default_parameters;
#if TURBO_PRAKTOR_HAS_WORKFLOW_PLAN
  if (plan_bound &&
      (turbo_praktor_require_harness_safe(config) ||
       !config->parameters_json)) {
    const json_value_t *schema =
        json_object_get(plan_description, "input_schema");
    generated_schema = schema ? json_serialize(schema, NULL) : NULL;
    if (!generated_schema) {
      status = TURBO_TOOL_OUT_OF_MEMORY;
      goto cleanup;
    }
    parameters_json = generated_schema;
  }
#endif

  binding = (turbo_praktor_binding_t *)calloc(1, sizeof(*binding));
  if (!binding) {
    status = TURBO_TOOL_OUT_OF_MEMORY;
    goto cleanup;
  }
  binding->api = pack->api;
  binding->workflow_path = tstr_dup(config->workflow_path);
  binding->max_result_bytes = pack->max_result_bytes;
  binding->project_agent_output =
      plan_bound && turbo_praktor_require_harness_safe(config);
#if TURBO_PRAKTOR_HAS_WORKFLOW_PLAN
  binding->plan = plan;
  plan = NULL;
#endif
  if (!binding->workflow_path) {
    status = TURBO_TOOL_OUT_OF_MEMORY;
    goto cleanup;
  }

  memset(&definition, 0, sizeof(definition));
  definition.struct_size = sizeof(definition);
  definition.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
  definition.definition.name = config->tool_name;
  definition.definition.description = config->description;
  definition.definition.parameters_json = parameters_json;
  definition.definition.strict = config->strict;
  definition.definition.handler = turbo_praktor_execute_string;
  definition.definition.json_value_handler = turbo_praktor_execute;
  definition.context_handler = turbo_praktor_execute_string_with_context;
  definition.json_value_context_handler = turbo_praktor_execute_with_context;
  definition.definition.user_data = binding;
  definition.definition.user_data_free = turbo_praktor_binding_destroy;
  definition.execution_policy = config->execution_policy;
  definition.required_capabilities = capabilities.items;
  definition.required_capability_count = capabilities.count;

  status = turbo_tool_registry_add_v4(pack->registry, &definition);
  if (status != TURBO_TOOL_OK) goto cleanup;

  binding = NULL;
  ++pack->workflow_count;

cleanup:
  if (binding) turbo_praktor_binding_destroy(binding);
#if TURBO_PRAKTOR_HAS_WORKFLOW_PLAN
  if (plan) pack->api->release_workflow_plan(plan);
#endif
  turbo_runtime_json_destroy(plan_description);
  if (generated_schema) json_serialize_free(generated_schema);
  turbo_praktor_capability_list_destroy(&capabilities);
  return status;
}

turbo_tool_registry_t *
turbo_praktor_tool_pack_registry(turbo_praktor_tool_pack_t *pack) {
  return pack ? pack->registry : NULL;
}

size_t turbo_praktor_tool_pack_workflow_count(
    const turbo_praktor_tool_pack_t *pack) {
  return pack ? pack->workflow_count : 0;
}


int turbo_praktor_tool_pack_supports_workflow_plan(
    const turbo_praktor_tool_pack_t *pack) {
#if TURBO_PRAKTOR_HAS_WORKFLOW_PLAN
  const praktor_api *api = pack ? pack->api : NULL;
  return api &&
         (api->capabilities & PRAKTOR_CAPABILITY_WORKFLOW_PLAN) != 0 &&
         api->compile_workflow && api->describe_workflow_plan &&
         api->execute_workflow_plan && api->release_workflow_plan;
#else
  (void)pack;
  return 0;
#endif
}

int turbo_praktor_tool_pack_supports_execution_events(
    const turbo_praktor_tool_pack_t *pack) {
#if TURBO_PRAKTOR_HAS_EXECUTION_EVENTS
  const praktor_api *api = pack ? pack->api : NULL;
  return api &&
         (api->capabilities & PRAKTOR_CAPABILITY_EXECUTION_EVENTS) != 0 &&
         api->execute_workflow_plan_observed;
#else
  (void)pack;
  return 0;
#endif
}
