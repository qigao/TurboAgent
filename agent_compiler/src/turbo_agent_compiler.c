#include "turbo_agent_compiler.h"

#include "turbo_runtime_json.h"
#include "turbo_tool_schema.h"

#include <data_bind.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TURBO_AGENT_PLAN_FNV_OFFSET_BASIS UINT64_C(14695981039346656037)
#define TURBO_AGENT_PLAN_FNV_PRIME UINT64_C(1099511628211)

enum {
  TURBO_AGENT_COMPILER_DEFAULT_MAX_SOURCE_BYTES = 1024 * 1024,
  TURBO_AGENT_COMPILER_DEFAULT_MAX_ARGUMENT_BYTES = 1024 * 1024,
  TURBO_AGENT_COMPILER_DEFAULT_MAX_ARGUMENT_NODES = 16384,
  TURBO_AGENT_COMPILER_DEFAULT_MAX_ARGUMENT_DEPTH = 64
};

struct turbo_agent_executable_plan_s {
  const turbo_agent_template_descriptor_t *template_descriptor;
  turbo_agent_template_kind_t template_kind;
  char *step_id;
  char *tool_name;
  json_value_t *arguments;
  turbo_tool_registry_t *projection;
  turbo_tool_execution_policy_t execution_policy;
  char **capabilities;
  size_t capability_count;
  json_value_t *execution_metadata;
  uint64_t plan_hash;
};

static const turbo_agent_template_descriptor_t inspect_template = {
    sizeof(turbo_agent_template_descriptor_t),
    TURBO_AGENT_TEMPLATE_DESCRIPTOR_ABI_VERSION,
    TURBO_AGENT_TEMPLATE_INSPECT,
    1u,
    "inspect",
    "TurboAgent.Inspect.v1",
    TURBO_AGENT_TEMPLATE_PROPERTY_READ_ONLY};

const turbo_agent_template_descriptor_t *
turbo_agent_template_descriptor(turbo_agent_template_kind_t kind) {
  switch (kind) {
    case TURBO_AGENT_TEMPLATE_INSPECT:
      return &inspect_template;
    default:
      return NULL;
  }
}

static char *agent_compiler_strdup(const char *value) {
  size_t size;
  char *copy;
  if (!value) return NULL;
  size = strlen(value) + 1u;
  copy = (char *)malloc(size);
  if (!copy) return NULL;
  memcpy(copy, value, size);
  return copy;
}

static void diagnostic_set(turbo_agent_compile_diagnostic_t *diagnostic,
                           turbo_agent_compile_status_t status,
                           const char *message) {
  if (!diagnostic) return;
  memset(diagnostic, 0, sizeof(*diagnostic));
  diagnostic->status = status;
  if (message) {
    snprintf(diagnostic->message, sizeof(diagnostic->message), "%s", message);
  }
}

static turbo_agent_compile_status_t compile_fail(
    turbo_agent_compile_diagnostic_t *diagnostic,
    turbo_agent_compile_status_t status,
    const char *message) {
  diagnostic_set(diagnostic, status, message);
  return status;
}

static int capability_compare(const void *lhs, const void *rhs) {
  const char *const *a = (const char *const *)lhs;
  const char *const *b = (const char *const *)rhs;
  return strcmp(*a, *b);
}

static int capability_allowed(const turbo_agent_compiler_config_t *config,
                              const char *capability) {
  size_t i;
  if (!config || !capability) return 0;
  if (!config->deny_unlisted_capabilities) return 1;
  for (i = 0; i < config->allowed_capability_count; ++i) {
    const char *allowed = config->allowed_capabilities
                              ? config->allowed_capabilities[i]
                              : NULL;
    if (allowed && strcmp(allowed, capability) == 0) return 1;
  }
  return 0;
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size) {
  const unsigned char *bytes = (const unsigned char *)data;
  size_t i;
  for (i = 0; i < size; ++i) {
    hash ^= (uint64_t)bytes[i];
    hash *= TURBO_AGENT_PLAN_FNV_PRIME;
  }
  return hash;
}

static uint64_t hash_u32_le(uint64_t hash, uint32_t value) {
  const unsigned char bytes[4] = {
      (unsigned char)(value & UINT32_C(0xff)),
      (unsigned char)((value >> 8) & UINT32_C(0xff)),
      (unsigned char)((value >> 16) & UINT32_C(0xff)),
      (unsigned char)((value >> 24) & UINT32_C(0xff))};
  return hash_bytes(hash, bytes, sizeof(bytes));
}

static uint64_t hash_cstring(uint64_t hash, const char *value) {
  static const unsigned char separator = 0xffu;
  if (value) hash = hash_bytes(hash, value, strlen(value));
  return hash_bytes(hash, &separator, 1u);
}

static uint64_t hash_json_value(uint64_t hash, const json_value_t *value);

static uint64_t hash_json_object(uint64_t hash, const json_value_t *value) {
  size_t count = json_object_size(value);
  const char **keys = NULL;
  size_t i;

  hash = hash_cstring(hash, "object");
  if (count == 0) return hash;
  keys = (const char **)calloc(count, sizeof(*keys));
  if (!keys) return 0;
  for (i = 0; i < count; ++i) {
    keys[i] = json_object_key(value, i);
    if (!keys[i]) {
      free(keys);
      return 0;
    }
  }
  qsort(keys, count, sizeof(*keys), capability_compare);
  for (i = 0; i < count; ++i) {
    const json_value_t *field = json_object_get(value, keys[i]);
    hash = hash_cstring(hash, keys[i]);
    hash = hash_json_value(hash, field);
    if (!hash) break;
  }
  free(keys);
  return hash;
}

static uint64_t hash_json_value(uint64_t hash, const json_value_t *value) {
  size_t i;
  size_t len = 0;
  const char *number_text;

  if (!value) return hash_cstring(hash, "null-pointer");

  switch (json_type(value)) {
    case JSON_NULL:
      return hash_cstring(hash, "null");
    case JSON_BOOL:
      hash = hash_cstring(hash, "bool");
      return hash_cstring(hash, json_bool(value) ? "true" : "false");
    case JSON_NUMBER:
      hash = hash_cstring(hash, "number");
      number_text = json_number_text(value, &len);
      return number_text ? hash_bytes(hash, number_text, len) : 0;
    case JSON_STRING:
      hash = hash_cstring(hash, "string");
      return hash_cstring(hash, json_string(value));
    case JSON_ARRAY:
      hash = hash_cstring(hash, "array");
      for (i = 0; i < json_array_size(value); ++i) {
        hash = hash_json_value(hash, json_array_get(value, i));
        if (!hash) return 0;
      }
      return hash;
    case JSON_OBJECT:
      return hash_json_object(hash, value);
    default:
      return 0;
  }
}

static uint64_t executable_plan_hash(
    const turbo_agent_template_descriptor_t *template_descriptor,
    const char *step_id,
    const char *tool_name,
    const json_value_t *arguments,
    const turbo_tool_execution_policy_t *policy,
    char *const *capabilities,
    size_t capability_count,
    const json_value_t *execution_metadata) {
  uint64_t hash = TURBO_AGENT_PLAN_FNV_OFFSET_BASIS;
  size_t i;

  hash = hash_cstring(hash, "TurboAgent.ExecutablePlan.v1");
  hash = hash_cstring(hash, template_descriptor->name);
  hash = hash_u32_le(hash, template_descriptor->version);
  hash = hash_cstring(hash, template_descriptor->input_contract);
  hash = hash_cstring(hash, step_id);
  hash = hash_cstring(hash, tool_name);
  hash = hash_json_value(hash, arguments);
  if (!hash) return 0;
  {
    uint32_t execution_mode = (uint32_t)policy->mode;
    uint32_t idempotency = (uint32_t)policy->idempotency;
    hash = hash_u32_le(hash, execution_mode);
    hash = hash_u32_le(hash, idempotency);
  }
  for (i = 0; i < capability_count; ++i) {
    hash = hash_cstring(hash, capabilities[i]);
  }
  hash = hash_cstring(hash, "execution_metadata");
  if (execution_metadata) {
    hash = hash_json_value(hash, execution_metadata);
  } else {
    hash = hash_cstring(hash, "none");
  }
  return hash;
}

static void executable_plan_clear(turbo_agent_executable_plan_t *plan) {
  size_t i;
  if (!plan) return;
  turbo_tool_registry_destroy(plan->projection);
  turbo_runtime_json_destroy(plan->arguments);
  turbo_runtime_json_destroy(plan->execution_metadata);
  free(plan->step_id);
  free(plan->tool_name);
  for (i = 0; i < plan->capability_count; ++i) free(plan->capabilities[i]);
  free(plan->capabilities);
  memset(plan, 0, sizeof(*plan));
}

static int argument_budget_add(size_t amount, size_t limit, size_t *total) {
  if (!total || *total > limit || amount > limit - *total) return 0;
  *total += amount;
  return 1;
}

static int argument_tree_within_limits(
    const json_value_t *value,
    size_t depth,
    const turbo_agent_compiler_config_t *config,
    size_t *node_count,
    size_t *byte_count) {
  size_t i;
  if (!value || !config || !node_count || !byte_count ||
      depth > config->max_argument_depth ||
      *node_count >= config->max_argument_nodes) {
    return 0;
  }
  ++*node_count;

  switch (json_type(value)) {
    case JSON_NULL:
    case JSON_BOOL:
      return 1;
    case JSON_NUMBER: {
      size_t number_size = 0;
      const char *number = json_number_text(value, &number_size);
      return number &&
             argument_budget_add(number_size, config->max_argument_bytes,
                                 byte_count);
    }
    case JSON_STRING:
      return argument_budget_add(strlen(json_string(value)),
                                 config->max_argument_bytes, byte_count);
    case JSON_ARRAY:
      for (i = 0; i < json_array_size(value); ++i) {
        if (!argument_tree_within_limits(
                json_array_get(value, i), depth + 1u, config,
                node_count, byte_count)) {
          return 0;
        }
      }
      return 1;
    case JSON_OBJECT:
      for (i = 0; i < json_object_size(value); ++i) {
        const char *key = json_object_key(value, i);
        const json_value_t *field = json_object_value(value, i);
        if (!key || !field ||
            !argument_budget_add(strlen(key), config->max_argument_bytes,
                                 byte_count) ||
            !argument_tree_within_limits(field, depth + 1u, config,
                                         node_count, byte_count)) {
          return 0;
        }
      }
      return 1;
    default:
      return 0;
  }
}

static int compiler_config_valid(
    const turbo_agent_compiler_config_t *config) {
  return config &&
         config->struct_size == sizeof(*config) &&
         config->abi_version == TURBO_AGENT_COMPILER_CONFIG_ABI_VERSION &&
         (config->allowed_capability_count == 0 ||
          config->allowed_capabilities != NULL) &&
         config->max_source_bytes != 0 &&
         config->max_argument_bytes != 0 &&
         config->max_argument_nodes != 0 &&
         config->max_argument_depth != 0;
}

void turbo_agent_typed_plan_init(turbo_agent_typed_plan_t *plan) {
  if (!plan) return;
  memset(plan, 0, sizeof(*plan));
  plan->struct_size = sizeof(*plan);
  plan->abi_version = TURBO_AGENT_TYPED_PLAN_ABI_VERSION;
}

void turbo_agent_compiler_config_init(turbo_agent_compiler_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_AGENT_COMPILER_CONFIG_ABI_VERSION;
  config->deny_unlisted_capabilities = 1;
  config->max_source_bytes = TURBO_AGENT_COMPILER_DEFAULT_MAX_SOURCE_BYTES;
  config->max_argument_bytes = TURBO_AGENT_COMPILER_DEFAULT_MAX_ARGUMENT_BYTES;
  config->max_argument_nodes = TURBO_AGENT_COMPILER_DEFAULT_MAX_ARGUMENT_NODES;
  config->max_argument_depth = TURBO_AGENT_COMPILER_DEFAULT_MAX_ARGUMENT_DEPTH;
}

turbo_agent_compile_status_t turbo_agent_compile_plan(
    const turbo_agent_compiler_config_t *config,
    const turbo_tool_registry_t *source_registry,
    const turbo_agent_typed_plan_t *source,
    turbo_agent_executable_plan_t **out_plan,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  turbo_tool_definition_t definition = {0};
  turbo_tool_execution_policy_t policy = {0};
  const char *canonical_tool_name = NULL;
  const char *const *required = NULL;
  size_t required_count = 0;
  const json_value_t *execution_metadata = NULL;
  size_t i;
  turbo_agent_executable_plan_t *plan = NULL;
  const char *projection_names[1];
  const turbo_agent_template_descriptor_t *template_descriptor = NULL;

  if (out_plan) *out_plan = NULL;
  diagnostic_set(diagnostic, TURBO_AGENT_COMPILE_OK, "");

  if (!compiler_config_valid(config) ||
      !source_registry || !source || !out_plan ||
      source->struct_size != sizeof(*source) ||
      source->abi_version != TURBO_AGENT_TYPED_PLAN_ABI_VERSION ||
      !source->step_id || !source->step_id[0] ||
      !source->tool_name || !source->tool_name[0]) {
    return compile_fail(diagnostic, TURBO_AGENT_COMPILE_INVALID_ARGUMENT,
                        "invalid compiler or typed-plan arguments");
  }

  template_descriptor = turbo_agent_template_descriptor(source->template_kind);
  if (!template_descriptor) {
    return compile_fail(diagnostic, TURBO_AGENT_COMPILE_UNSUPPORTED_TEMPLATE,
                        "Phase 1 supports only registered template descriptors");
  }
  if (!source->arguments || json_type(source->arguments) != JSON_OBJECT) {
    return compile_fail(diagnostic, TURBO_AGENT_COMPILE_INVALID_ARGUMENT,
                        "Inspect arguments must be an explicit JSON object");
  }
  {
    size_t argument_nodes = 0;
    size_t argument_bytes = 0;
    if (!argument_tree_within_limits(
            source->arguments, 0u, config, &argument_nodes, &argument_bytes)) {
      return compile_fail(diagnostic, TURBO_AGENT_COMPILE_PLAN_LIMIT,
                          "tool arguments exceed compiler resource limits");
    }
  }
  if (turbo_tool_registry_resolve_name(
          source_registry, source->tool_name, &canonical_tool_name) !=
          TURBO_TOOL_OK ||
      !canonical_tool_name) {
    return compile_fail(diagnostic, TURBO_AGENT_COMPILE_UNRESOLVED_TOOL,
                        "tool cannot be resolved during compilation");
  }
  {
    size_t definition_index;
    int definition_found = 0;
    for (definition_index = 0;
         definition_index < turbo_tool_registry_count(source_registry);
         ++definition_index) {
      turbo_tool_definition_t candidate = {0};
      if (turbo_tool_registry_get_definition(
              source_registry, definition_index, &candidate) != TURBO_TOOL_OK) {
        return compile_fail(diagnostic, TURBO_AGENT_COMPILE_UNRESOLVED_TOOL,
                            "tool definition cannot be read during compilation");
      }
      if (candidate.name &&
          strcmp(candidate.name, canonical_tool_name) == 0) {
        definition = candidate;
        definition_found = 1;
        break;
      }
    }
    if (!definition_found) {
      return compile_fail(diagnostic, TURBO_AGENT_COMPILE_UNRESOLVED_TOOL,
                          "canonical tool definition is unavailable");
    }
  }
  {
    char schema_diagnostic[256] = {0};
    turbo_tool_schema_validation_status_t schema_status =
        turbo_tool_schema_validate_arguments_json_value(
            &definition, source->arguments, schema_diagnostic,
            sizeof(schema_diagnostic));
    if (schema_status == TURBO_TOOL_SCHEMA_VALUE_INVALID) {
      return compile_fail(
          diagnostic, TURBO_AGENT_COMPILE_TOOL_ARGUMENTS_INVALID,
          schema_diagnostic[0] ? schema_diagnostic
                               : "tool arguments do not satisfy parameter schema");
    }
    if (schema_status != TURBO_TOOL_SCHEMA_VALID) {
      return compile_fail(
          diagnostic, TURBO_AGENT_COMPILE_TOOL_SCHEMA_INVALID,
          schema_diagnostic[0] ? schema_diagnostic
                               : "tool parameter schema is invalid or unsupported");
    }
  }
  if (turbo_tool_registry_get_execution_policy(
          source_registry, canonical_tool_name, &policy) != TURBO_TOOL_OK) {
    return compile_fail(diagnostic, TURBO_AGENT_COMPILE_UNRESOLVED_TOOL,
                        "tool execution policy is unavailable");
  }

  /*
   * Inspect is read-only by contract. Phase 1 deliberately uses the existing
   * RuntimeTools idempotency fact rather than inventing a second effect system.
   */
  if ((template_descriptor->properties & TURBO_AGENT_TEMPLATE_PROPERTY_READ_ONLY) != 0u &&
      policy.idempotency != TURBO_TOOL_IDEMPOTENCY_READ_ONLY) {
    return compile_fail(diagnostic, TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION,
                        "read-only template requires a READ_ONLY tool");
  }

  if (turbo_tool_registry_get_required_capabilities(
          source_registry, canonical_tool_name, &required,
          &required_count) != TURBO_TOOL_OK) {
    return compile_fail(diagnostic, TURBO_AGENT_COMPILE_UNRESOLVED_TOOL,
                        "tool capability metadata is unavailable");
  }
  for (i = 0; i < required_count; ++i) {
    if (!required[i] || !required[i][0] ||
        !capability_allowed(config, required[i])) {
      return compile_fail(diagnostic, TURBO_AGENT_COMPILE_CAPABILITY_DENIED,
                          "tool requires a capability not admitted by the compiler");
    }
  }
  if (turbo_tool_registry_get_execution_metadata(
          source_registry, canonical_tool_name, &execution_metadata) !=
      TURBO_TOOL_OK) {
    return compile_fail(diagnostic, TURBO_AGENT_COMPILE_UNRESOLVED_TOOL,
                        "tool execution metadata is unavailable");
  }

  plan = (turbo_agent_executable_plan_t *)calloc(1, sizeof(*plan));
  if (!plan) {
    return compile_fail(diagnostic, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
                        "could not allocate executable plan");
  }
  plan->template_descriptor = template_descriptor;
  plan->template_kind = source->template_kind;
  plan->step_id = agent_compiler_strdup(source->step_id);
  plan->tool_name = agent_compiler_strdup(canonical_tool_name);
  plan->arguments = source->arguments ? json_clone(source->arguments)
                                     : json_create_object();
  plan->execution_policy = policy;
  plan->execution_metadata =
      execution_metadata ? json_clone(execution_metadata) : NULL;

  if (!plan->step_id || !plan->tool_name || !plan->arguments ||
      (execution_metadata && !plan->execution_metadata))
    goto oom;

  {
    static const char *const implicit_custom_tools[] = {"custom_tools"};
    const char *const *effective_required =
        required_count ? required : implicit_custom_tools;
    size_t effective_count = required_count ? required_count : 1u;

    if (!required_count &&
        !capability_allowed(config, implicit_custom_tools[0])) {
      executable_plan_clear(plan);
      free(plan);
      return compile_fail(
          diagnostic, TURBO_AGENT_COMPILE_CAPABILITY_DENIED,
          "legacy tool requires implicit custom_tools capability");
    }

    plan->capabilities =
        (char **)calloc(effective_count, sizeof(*plan->capabilities));
    if (!plan->capabilities) goto oom;
    plan->capability_count = effective_count;
    for (i = 0; i < effective_count; ++i) {
      plan->capabilities[i] = agent_compiler_strdup(effective_required[i]);
      if (!plan->capabilities[i]) goto oom;
    }
    qsort(plan->capabilities, plan->capability_count,
          sizeof(*plan->capabilities), capability_compare);
  }

  projection_names[0] = canonical_tool_name;
  if (turbo_tool_registry_project(source_registry, projection_names, 1,
                                  &plan->projection) != TURBO_TOOL_OK ||
      !plan->projection) {
    executable_plan_clear(plan);
    free(plan);
    return compile_fail(diagnostic, TURBO_AGENT_COMPILE_UNRESOLVED_TOOL,
                        "could not freeze the resolved tool projection");
  }

  plan->plan_hash = executable_plan_hash(
      plan->template_descriptor, plan->step_id, plan->tool_name, plan->arguments,
      &plan->execution_policy, plan->capabilities, plan->capability_count,
      plan->execution_metadata);
  if (!plan->plan_hash) goto oom;

  *out_plan = plan;
  diagnostic_set(diagnostic, TURBO_AGENT_COMPILE_OK, "ok");
  return TURBO_AGENT_COMPILE_OK;

oom:
  executable_plan_clear(plan);
  free(plan);
  return compile_fail(diagnostic, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
                      "could not freeze executable-plan semantics");
}


static int databind_view_equals(const DataBindStringView *view,
                                const char *literal) {
  size_t literal_size;
  if (!view || !literal || !view->data) return 0;
  literal_size = strlen(literal);
  return view->length == literal_size &&
         memcmp(view->data, literal, literal_size) == 0;
}

static char *databind_view_copy(const DataBindStringView *view) {
  char *copy;
  if (!view || (!view->data && view->length != 0)) return NULL;
  copy = (char *)malloc(view->length + 1u);
  if (!copy) return NULL;
  if (view->length) memcpy(copy, view->data, view->length);
  copy[view->length] = '\0';
  return copy;
}

turbo_agent_compile_status_t turbo_agent_compile_plan_json(
    const turbo_agent_compiler_config_t *config,
    const turbo_tool_registry_t *source_registry,
    const char *source_json,
    size_t source_json_size,
    turbo_agent_executable_plan_t **out_plan,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  static const char schema[] =
      "message AgentInspectPlan { "
      "string template_id; "
      "string step_id; "
      "string tool; "
      "string arguments_json; "
      "}";
  DataBind *codec = NULL;
  DataBindRecord *record = NULL;
  DataBindJsonOptions bind_options = DATA_BIND_JSON_OPTIONS_INIT;
  DataBindError bind_error = DATA_BIND_ERROR_INIT;
  DataBindStringView template_id = DATA_BIND_STRING_VIEW_INIT;
  DataBindStringView step_id = DATA_BIND_STRING_VIEW_INIT;
  DataBindStringView tool = DATA_BIND_STRING_VIEW_INIT;
  DataBindStringView arguments_json = DATA_BIND_STRING_VIEW_INIT;
  turbo_agent_typed_plan_t source;
  json_value_t *arguments = NULL;
  char *step_copy = NULL;
  char *tool_copy = NULL;
  turbo_agent_compile_status_t status;

  if (out_plan) *out_plan = NULL;
  if (!compiler_config_valid(config) || !source_registry || !source_json ||
      source_json_size == 0 || !out_plan) {
    return compile_fail(diagnostic, TURBO_AGENT_COMPILE_INVALID_ARGUMENT,
                        "invalid JSON compile arguments");
  }
  if (!config->max_source_bytes || source_json_size > config->max_source_bytes) {
    return compile_fail(diagnostic, TURBO_AGENT_COMPILE_PLAN_LIMIT,
                        "model plan source exceeds compiler byte limit");
  }

  if (data_bind_create_from_text(schema, sizeof(schema) - 1u, &codec,
                                 &bind_error) != DATA_BIND_OK ||
      !codec) {
    return compile_fail(diagnostic, TURBO_AGENT_COMPILE_SOURCE_INVALID,
                        bind_error.message[0] ? bind_error.message
                                              : "could not create AgentPlan DataBind codec");
  }

  bind_options.flags =
      DATA_BIND_JSON_BIND_EXACT_SCALAR_TOKENS |
      DATA_BIND_JSON_BIND_REJECT_UNKNOWN_FIELDS;
  if (data_bind_record_from_json_ex(
          codec, "AgentInspectPlan", source_json, source_json_size,
          &bind_options, &record, &bind_error) != DATA_BIND_OK ||
      !record) {
    status = compile_fail(diagnostic, TURBO_AGENT_COMPILE_SOURCE_INVALID,
                          bind_error.message[0] ? bind_error.message
                                                : "AgentPlan source failed DataBind validation");
    goto cleanup;
  }

  if (data_bind_record_get_string(record, "template_id", &template_id,
                                  &bind_error) != DATA_BIND_OK ||
      data_bind_record_get_string(record, "step_id", &step_id,
                                  &bind_error) != DATA_BIND_OK ||
      data_bind_record_get_string(record, "tool", &tool,
                                  &bind_error) != DATA_BIND_OK ||
      data_bind_record_get_string(record, "arguments_json", &arguments_json,
                                  &bind_error) != DATA_BIND_OK) {
    status = compile_fail(diagnostic, TURBO_AGENT_COMPILE_SOURCE_INVALID,
                          bind_error.message[0] ? bind_error.message
                                                : "AgentPlan fields are invalid");
    goto cleanup;
  }

  if (!databind_view_equals(&template_id, "inspect")) {
    status = compile_fail(diagnostic, TURBO_AGENT_COMPILE_UNSUPPORTED_TEMPLATE,
                          "Phase 1 source supports only template_id=inspect");
    goto cleanup;
  }

  step_copy = databind_view_copy(&step_id);
  tool_copy = databind_view_copy(&tool);
  if (!step_copy || !tool_copy) {
    status = compile_fail(diagnostic, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
                          "could not copy DataBind plan fields");
    goto cleanup;
  }

  arguments = json_parse(arguments_json.data, arguments_json.length);
  if (!arguments || json_type(arguments) != JSON_OBJECT) {
    status = compile_fail(diagnostic, TURBO_AGENT_COMPILE_SOURCE_INVALID,
                          "arguments_json must contain one JSON object");
    goto cleanup;
  }

  turbo_agent_typed_plan_init(&source);
  source.template_kind = TURBO_AGENT_TEMPLATE_INSPECT;
  source.step_id = step_copy;
  source.tool_name = tool_copy;
  source.arguments = arguments;
  status = turbo_agent_compile_plan(config, source_registry, &source, out_plan,
                                    diagnostic);

cleanup:
  turbo_runtime_json_destroy(arguments);
  free(tool_copy);
  free(step_copy);
  data_bind_record_free(record);
  data_bind_free(codec);
  return status;
}

void turbo_agent_executable_plan_destroy(turbo_agent_executable_plan_t *plan) {
  if (!plan) return;
  executable_plan_clear(plan);
  free(plan);
}

turbo_tool_status_t turbo_agent_execute_compiled_plan(
    const turbo_agent_executable_plan_t *plan,
    const turbo_tool_execution_context_t *context,
    json_value_t **out_result) {
  if (!plan || !plan->projection || !plan->tool_name || !plan->arguments ||
      !out_result) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  *out_result = NULL;
  return turbo_tool_registry_execute_json_value_with_context(
      plan->projection, plan->tool_name, plan->arguments, context, out_result);
}

uint64_t turbo_agent_executable_plan_hash(
    const turbo_agent_executable_plan_t *plan) {
  return plan ? plan->plan_hash : 0;
}

const char *turbo_agent_executable_plan_tool_name(
    const turbo_agent_executable_plan_t *plan) {
  return plan ? plan->tool_name : NULL;
}

const char *turbo_agent_executable_plan_step_id(
    const turbo_agent_executable_plan_t *plan) {
  return plan ? plan->step_id : NULL;
}

turbo_agent_template_kind_t turbo_agent_executable_plan_template_kind(
    const turbo_agent_executable_plan_t *plan) {
  return plan ? plan->template_kind : TURBO_AGENT_TEMPLATE_INVALID;
}

turbo_tool_execution_policy_t turbo_agent_executable_plan_execution_policy(
    const turbo_agent_executable_plan_t *plan) {
  turbo_tool_execution_policy_t empty = {0};
  return plan ? plan->execution_policy : empty;
}

turbo_agent_compile_status_t
turbo_agent_executable_plan_required_capabilities(
    const turbo_agent_executable_plan_t *plan,
    const char *const **out_capabilities,
    size_t *out_count) {
  if (!plan || !out_capabilities || !out_count) {
    return TURBO_AGENT_COMPILE_INVALID_ARGUMENT;
  }
  *out_capabilities = (const char *const *)plan->capabilities;
  *out_count = plan->capability_count;
  return TURBO_AGENT_COMPILE_OK;
}

json_value_t *turbo_agent_executable_plan_certificate_json_value(
    const turbo_agent_executable_plan_t *plan) {
  json_value_t *root = NULL;
  json_value_t *caps = NULL;
  json_value_t *field = NULL;
  char hash_text[17];
  size_t i;

  if (!plan) return NULL;
  root = json_create_object();
  caps = json_create_array();
  if (!root || !caps) goto fail;

  field = json_create_int64(TURBO_AGENT_PLAN_CERTIFICATE_VERSION);
  if (!field ||
      turbo_runtime_json_object_set(root, "version", field) !=
          TURBO_RUNTIME_JSON_OK)
    goto fail;
  field = NULL;

  field = json_create_string(plan->template_descriptor->name);
  if (!field ||
      turbo_runtime_json_object_set(root, "template", field) !=
          TURBO_RUNTIME_JSON_OK)
    goto fail;
  field = NULL;

  field = json_create_int64((int64_t)plan->template_descriptor->version);
  if (!field ||
      turbo_runtime_json_object_set(root, "template_version", field) !=
          TURBO_RUNTIME_JSON_OK)
    goto fail;
  field = NULL;

  field = json_create_string(plan->template_descriptor->input_contract);
  if (!field ||
      turbo_runtime_json_object_set(root, "input_contract", field) !=
          TURBO_RUNTIME_JSON_OK)
    goto fail;
  field = NULL;

  field = json_create_string(plan->step_id);
  if (!field ||
      turbo_runtime_json_object_set(root, "step_id", field) !=
          TURBO_RUNTIME_JSON_OK)
    goto fail;
  field = NULL;

  field = json_create_string(plan->tool_name);
  if (!field ||
      turbo_runtime_json_object_set(root, "tool", field) !=
          TURBO_RUNTIME_JSON_OK)
    goto fail;
  field = NULL;

  field = json_create_string("runtime_tools");
  if (!field ||
      turbo_runtime_json_object_set(root, "backend", field) !=
          TURBO_RUNTIME_JSON_OK)
    goto fail;
  field = NULL;

  snprintf(hash_text, sizeof(hash_text), "%016llx",
           (unsigned long long)plan->plan_hash);
  field = json_create_string(hash_text);
  if (!field ||
      turbo_runtime_json_object_set(root, "plan_hash", field) !=
          TURBO_RUNTIME_JSON_OK)
    goto fail;
  field = NULL;

  field = json_create_int64((int64_t)plan->execution_policy.mode);
  if (!field ||
      turbo_runtime_json_object_set(root, "execution_mode", field) !=
          TURBO_RUNTIME_JSON_OK)
    goto fail;
  field = NULL;

  field = json_create_int64((int64_t)plan->execution_policy.idempotency);
  if (!field ||
      turbo_runtime_json_object_set(root, "idempotency", field) !=
          TURBO_RUNTIME_JSON_OK)
    goto fail;
  field = NULL;

  for (i = 0; i < plan->capability_count; ++i) {
    field = json_create_string(plan->capabilities[i]);
    if (!field ||
        turbo_runtime_json_array_append(caps, field) != TURBO_RUNTIME_JSON_OK)
      goto fail;
    field = NULL;
  }
  if (turbo_runtime_json_object_set(root, "capabilities", caps) !=
      TURBO_RUNTIME_JSON_OK)
    goto fail;
  caps = NULL;

  if (plan->execution_metadata) {
    field = json_clone(plan->execution_metadata);
    if (!field ||
        turbo_runtime_json_object_set(root, "execution_metadata", field) !=
            TURBO_RUNTIME_JSON_OK)
      goto fail;
    field = NULL;
  }

  return root;

fail:
  turbo_runtime_json_destroy(field);
  turbo_runtime_json_destroy(caps);
  turbo_runtime_json_destroy(root);
  return NULL;
}
