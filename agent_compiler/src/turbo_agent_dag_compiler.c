#include "turbo_agent_dag_compiler.h"

#include "turbo_runtime_json.h"
#include "turbo_tool_schema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DAG_FNV_OFFSET UINT64_C(14695981039346656037)
#define DAG_FNV_PRIME UINT64_C(1099511628211)

enum {
  TURBO_AGENT_DAG_MAX_STEPS = 1024,
  TURBO_AGENT_DAG_MAX_EDGES = 16384
};

typedef struct turbo_agent_executable_dag_step_s {
  char *step_id;
  char *tool_name;
  json_value_t *arguments;
  size_t *dependency_indices;
  size_t dependency_count;
  uint32_t retry_limit;
  uint32_t flags;
  turbo_tool_execution_policy_t execution_policy;
  turbo_tool_effect_flags_t effect_flags;
  char **capabilities;
  size_t capability_count;
  json_value_t *execution_metadata;
} turbo_agent_executable_dag_step_t;

struct turbo_agent_executable_dag_s {
  turbo_agent_executable_dag_step_t *steps;
  size_t step_count;
  uint32_t replan_budget;
  turbo_agent_dag_template_kind_t template_kind;
  uint32_t plan_generation;
  turbo_tool_registry_t *approved_tools;
  char **capabilities;
  size_t capability_count;
  turbo_tool_effect_flags_t effect_flags;
  uint64_t plan_hash;
};

static void dag_diagnostic_set(turbo_agent_compile_diagnostic_t *diagnostic,
                               turbo_agent_compile_status_t status,
                               const char *message) {
  if (!diagnostic) return;
  memset(diagnostic, 0, sizeof(*diagnostic));
  diagnostic->status = status;
  if (message) {
    snprintf(diagnostic->message, sizeof(diagnostic->message), "%s", message);
  }
}

static turbo_agent_compile_status_t dag_fail(
    turbo_agent_compile_diagnostic_t *diagnostic,
    turbo_agent_compile_status_t status,
    const char *message) {
  dag_diagnostic_set(diagnostic, status, message);
  return status;
}

static char *dag_strdup(const char *value) {
  size_t size;
  char *copy;
  if (!value) return NULL;
  size = strlen(value) + 1u;
  copy = (char *)malloc(size);
  if (!copy) return NULL;
  memcpy(copy, value, size);
  return copy;
}

static int dag_string_compare(const void *lhs, const void *rhs) {
  const char *const *a = (const char *const *)lhs;
  const char *const *b = (const char *const *)rhs;
  return strcmp(*a, *b);
}

static int dag_config_valid(const turbo_agent_compiler_config_t *config) {
  return config &&
         config->struct_size == sizeof(*config) &&
         config->abi_version == TURBO_AGENT_COMPILER_CONFIG_ABI_VERSION &&
         (config->allowed_capability_count == 0u ||
          config->allowed_capabilities != NULL) &&
         config->max_argument_bytes != 0u &&
         config->max_argument_nodes != 0u &&
         config->max_argument_depth != 0u;
}

static int dag_source_valid(const turbo_agent_dag_source_t *source) {
  if (!source) return 0;
  if (source->abi_version == TURBO_AGENT_DAG_SOURCE_ABI_VERSION_V1) {
    return source->struct_size >= TURBO_AGENT_DAG_SOURCE_V1_SIZE;
  }
  return source->abi_version == TURBO_AGENT_DAG_SOURCE_ABI_VERSION &&
         source->struct_size >= sizeof(*source);
}

static turbo_agent_dag_template_kind_t dag_source_template_kind(
    const turbo_agent_dag_source_t *source) {
  if (!source ||
      source->abi_version < TURBO_AGENT_DAG_SOURCE_ABI_VERSION ||
      source->struct_size < sizeof(*source)) {
    return TURBO_AGENT_DAG_TEMPLATE_GENERIC;
  }
  return source->template_kind;
}

static uint32_t dag_source_plan_generation(
    const turbo_agent_dag_source_t *source) {
  if (!source ||
      source->abi_version < TURBO_AGENT_DAG_SOURCE_ABI_VERSION ||
      source->struct_size < sizeof(*source)) {
    return 0u;
  }
  return source->plan_generation;
}

static int dag_template_kind_valid(turbo_agent_dag_template_kind_t kind) {
  return kind >= TURBO_AGENT_DAG_TEMPLATE_GENERIC &&
         kind <= TURBO_AGENT_DAG_TEMPLATE_REPAIR;
}

static const char *dag_template_kind_name(turbo_agent_dag_template_kind_t kind) {
  switch (kind) {
    case TURBO_AGENT_DAG_TEMPLATE_CHANGE:
      return "change";
    case TURBO_AGENT_DAG_TEMPLATE_REPAIR:
      return "repair";
    case TURBO_AGENT_DAG_TEMPLATE_GENERIC:
    default:
      return "generic";
  }
}

static int dag_capability_allowed(
    const turbo_agent_compiler_config_t *config,
    const char *capability) {
  size_t i;
  if (!config || !capability || !capability[0]) return 0;
  if (!config->deny_unlisted_capabilities) return 1;
  for (i = 0; i < config->allowed_capability_count; ++i) {
    const char *allowed = config->allowed_capabilities[i];
    if (allowed && strcmp(allowed, capability) == 0) return 1;
  }
  return 0;
}

static int dag_budget_add(size_t amount, size_t limit, size_t *total) {
  if (!total || *total > limit || amount > limit - *total) return 0;
  *total += amount;
  return 1;
}

static int dag_arguments_within_limits(
    const json_value_t *value,
    size_t depth,
    const turbo_agent_compiler_config_t *config,
    size_t *nodes,
    size_t *bytes) {
  size_t i;
  if (!value || !config || !nodes || !bytes ||
      depth > config->max_argument_depth ||
      *nodes >= config->max_argument_nodes) {
    return 0;
  }
  ++*nodes;

  switch (json_type(value)) {
    case JSON_NULL:
    case JSON_BOOL:
      return 1;
    case JSON_NUMBER: {
      size_t len = 0u;
      const char *text = json_number_text(value, &len);
      return text && dag_budget_add(len, config->max_argument_bytes, bytes);
    }
    case JSON_STRING:
      return dag_budget_add(strlen(json_string(value)),
                            config->max_argument_bytes, bytes);
    case JSON_ARRAY:
      for (i = 0; i < json_array_size(value); ++i) {
        if (!dag_arguments_within_limits(
                json_array_get(value, i), depth + 1u, config, nodes, bytes)) {
          return 0;
        }
      }
      return 1;
    case JSON_OBJECT:
      for (i = 0; i < json_object_size(value); ++i) {
        const char *key = json_object_key(value, i);
        const json_value_t *field = json_object_value(value, i);
        if (!key || !field ||
            !dag_budget_add(strlen(key), config->max_argument_bytes, bytes) ||
            !dag_arguments_within_limits(
                field, depth + 1u, config, nodes, bytes)) {
          return 0;
        }
      }
      return 1;
    default:
      return 0;
  }
}

static uint64_t dag_hash_bytes(uint64_t hash, const void *data, size_t size) {
  const unsigned char *bytes = (const unsigned char *)data;
  size_t i;
  for (i = 0; i < size; ++i) {
    hash ^= (uint64_t)bytes[i];
    hash *= DAG_FNV_PRIME;
  }
  return hash;
}

static uint64_t dag_hash_u32(uint64_t hash, uint32_t value) {
  const unsigned char bytes[4] = {
      (unsigned char)(value & UINT32_C(0xff)),
      (unsigned char)((value >> 8) & UINT32_C(0xff)),
      (unsigned char)((value >> 16) & UINT32_C(0xff)),
      (unsigned char)((value >> 24) & UINT32_C(0xff))};
  return dag_hash_bytes(hash, bytes, sizeof(bytes));
}

static uint64_t dag_hash_u64(uint64_t hash, uint64_t value) {
  unsigned char bytes[8];
  size_t i;
  for (i = 0; i < 8u; ++i) {
    bytes[i] = (unsigned char)((value >> (i * 8u)) & UINT64_C(0xff));
  }
  return dag_hash_bytes(hash, bytes, sizeof(bytes));
}

static uint64_t dag_hash_cstring(uint64_t hash, const char *value) {
  static const unsigned char separator = 0xffu;
  if (value) hash = dag_hash_bytes(hash, value, strlen(value));
  return dag_hash_bytes(hash, &separator, 1u);
}

static uint64_t dag_hash_json(uint64_t hash, const json_value_t *value);

static uint64_t dag_hash_json_object(uint64_t hash, const json_value_t *value) {
  size_t count = json_object_size(value);
  const char **keys = NULL;
  size_t i;

  hash = dag_hash_cstring(hash, "object");
  if (count == 0u) return hash;
  keys = (const char **)calloc(count, sizeof(*keys));
  if (!keys) return 0u;
  for (i = 0; i < count; ++i) {
    keys[i] = json_object_key(value, i);
    if (!keys[i]) {
      free(keys);
      return 0u;
    }
  }
  qsort(keys, count, sizeof(*keys), dag_string_compare);
  for (i = 0; i < count; ++i) {
    hash = dag_hash_cstring(hash, keys[i]);
    hash = dag_hash_json(hash, json_object_get(value, keys[i]));
    if (!hash) break;
  }
  free(keys);
  return hash;
}

static uint64_t dag_hash_json(uint64_t hash, const json_value_t *value) {
  size_t i;
  if (!value) return dag_hash_cstring(hash, "null-pointer");
  switch (json_type(value)) {
    case JSON_NULL:
      return dag_hash_cstring(hash, "null");
    case JSON_BOOL:
      hash = dag_hash_cstring(hash, "bool");
      return dag_hash_cstring(hash, json_bool(value) ? "true" : "false");
    case JSON_NUMBER: {
      size_t len = 0u;
      const char *text = json_number_text(value, &len);
      hash = dag_hash_cstring(hash, "number");
      return text ? dag_hash_bytes(hash, text, len) : 0u;
    }
    case JSON_STRING:
      hash = dag_hash_cstring(hash, "string");
      return dag_hash_cstring(hash, json_string(value));
    case JSON_ARRAY:
      hash = dag_hash_cstring(hash, "array");
      for (i = 0; i < json_array_size(value); ++i) {
        hash = dag_hash_json(hash, json_array_get(value, i));
        if (!hash) return 0u;
      }
      return hash;
    case JSON_OBJECT:
      return dag_hash_json_object(hash, value);
    default:
      return 0u;
  }
}

static int dag_find_source_step(
    const turbo_agent_dag_source_t *source,
    const char *step_id,
    size_t *out_index) {
  size_t i;
  if (!source || !step_id) return 0;
  for (i = 0; i < source->step_count; ++i) {
    if (source->steps[i].step_id &&
        strcmp(source->steps[i].step_id, step_id) == 0) {
      if (out_index) *out_index = i;
      return 1;
    }
  }
  return 0;
}

static int dag_find_definition(
    const turbo_tool_registry_t *registry,
    const char *canonical_name,
    turbo_tool_definition_t *out_definition) {
  size_t i;
  if (!registry || !canonical_name || !out_definition) return 0;
  for (i = 0; i < turbo_tool_registry_count(registry); ++i) {
    turbo_tool_definition_t definition = {0};
    if (turbo_tool_registry_get_definition(registry, i, &definition) !=
        TURBO_TOOL_OK) {
      return 0;
    }
    if (definition.name &&
        strcmp(definition.name, canonical_name) == 0) {
      *out_definition = definition;
      return 1;
    }
  }
  return 0;
}

static void dag_step_clear(turbo_agent_executable_dag_step_t *step) {
  size_t i;
  if (!step) return;
  free(step->step_id);
  free(step->tool_name);
  turbo_runtime_json_destroy(step->arguments);
  free(step->dependency_indices);
  for (i = 0; i < step->capability_count; ++i) {
    free(step->capabilities[i]);
  }
  free(step->capabilities);
  turbo_runtime_json_destroy(step->execution_metadata);
  memset(step, 0, sizeof(*step));
}

static void dag_clear(turbo_agent_executable_dag_t *plan) {
  size_t i;
  if (!plan) return;
  for (i = 0; i < plan->step_count; ++i) {
    dag_step_clear(&plan->steps[i]);
  }
  free(plan->steps);
  turbo_tool_registry_destroy(plan->approved_tools);
  for (i = 0; i < plan->capability_count; ++i) {
    free(plan->capabilities[i]);
  }
  free(plan->capabilities);
  memset(plan, 0, sizeof(*plan));
}

static turbo_agent_compile_status_t dag_copy_capabilities(
    const turbo_agent_compiler_config_t *config,
    const char *const *required,
    size_t required_count,
    turbo_agent_executable_dag_step_t *step,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  static const char *const implicit_custom_tools[] = {"custom_tools"};
  const char *const *effective =
      required_count ? required : implicit_custom_tools;
  size_t count = required_count ? required_count : 1u;
  size_t i;

  for (i = 0; i < count; ++i) {
    if (!effective[i] || !effective[i][0] ||
        !dag_capability_allowed(config, effective[i])) {
      return dag_fail(
          diagnostic, TURBO_AGENT_COMPILE_CAPABILITY_DENIED,
          "DAG step requires a capability not admitted by the compiler");
    }
  }

  step->capabilities = (char **)calloc(count, sizeof(*step->capabilities));
  if (!step->capabilities) {
    return dag_fail(diagnostic, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
                    "could not allocate DAG capability set");
  }
  step->capability_count = count;
  for (i = 0; i < count; ++i) {
    step->capabilities[i] = dag_strdup(effective[i]);
    if (!step->capabilities[i]) {
      return dag_fail(diagnostic, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
                      "could not copy DAG capability");
    }
  }
  qsort(step->capabilities, count, sizeof(*step->capabilities),
        dag_string_compare);
  return TURBO_AGENT_COMPILE_OK;
}

static turbo_agent_compile_status_t dag_admit_step(
    const turbo_agent_compiler_config_t *config,
    const turbo_tool_registry_t *source_registry,
    const turbo_agent_dag_step_source_t *source,
    turbo_agent_executable_dag_step_t *step,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  const char *canonical_name = NULL;
  turbo_tool_definition_t definition = {0};
  const char *const *required = NULL;
  size_t required_count = 0u;
  const json_value_t *execution_metadata = NULL;
  turbo_tool_effect_flags_t effect_flags = TURBO_TOOL_EFFECT_UNKNOWN;
  char schema_diagnostic[256] = {0};
  size_t nodes = 0u;
  size_t bytes = 0u;
  turbo_tool_schema_validation_status_t schema_status;
  turbo_agent_compile_status_t status;

  if (!source || !step || !source->step_id || !source->step_id[0] ||
      !source->tool_name || !source->tool_name[0] ||
      !source->arguments || json_type(source->arguments) != JSON_OBJECT ||
      source->struct_size != sizeof(*source) ||
      source->abi_version != TURBO_AGENT_DAG_STEP_ABI_VERSION ||
      (source->dependency_count != 0u && !source->depends_on) ||
      (source->flags &
       ~(TURBO_AGENT_DAG_STEP_APPROVAL_BEFORE |
         TURBO_AGENT_DAG_STEP_CHECKPOINT_AFTER)) != 0u) {
    return dag_fail(diagnostic, TURBO_AGENT_COMPILE_INVALID_ARGUMENT,
                    "invalid DAG step source");
  }

  if (!dag_arguments_within_limits(
          source->arguments, 0u, config, &nodes, &bytes)) {
    return dag_fail(diagnostic, TURBO_AGENT_COMPILE_PLAN_LIMIT,
                    "DAG step arguments exceed compiler limits");
  }

  if (turbo_tool_registry_resolve_name(
          source_registry, source->tool_name, &canonical_name) !=
          TURBO_TOOL_OK ||
      !canonical_name ||
      !dag_find_definition(source_registry, canonical_name, &definition)) {
    return dag_fail(diagnostic, TURBO_AGENT_COMPILE_UNRESOLVED_TOOL,
                    "DAG tool cannot be resolved during compilation");
  }

  schema_status = turbo_tool_schema_validate_arguments_json_value(
      &definition, source->arguments,
      schema_diagnostic, sizeof(schema_diagnostic));
  if (schema_status == TURBO_TOOL_SCHEMA_VALUE_INVALID) {
    return dag_fail(
        diagnostic, TURBO_AGENT_COMPILE_TOOL_ARGUMENTS_INVALID,
        schema_diagnostic[0] ? schema_diagnostic
                             : "DAG tool arguments violate parameter schema");
  }
  if (schema_status != TURBO_TOOL_SCHEMA_VALID) {
    return dag_fail(
        diagnostic, TURBO_AGENT_COMPILE_TOOL_SCHEMA_INVALID,
        schema_diagnostic[0] ? schema_diagnostic
                             : "DAG tool parameter schema is unsupported");
  }

  if (turbo_tool_registry_get_execution_policy(
          source_registry, canonical_name, &step->execution_policy) !=
      TURBO_TOOL_OK ||
      turbo_tool_registry_get_required_capabilities(
          source_registry, canonical_name,
          &required, &required_count) != TURBO_TOOL_OK ||
      turbo_tool_registry_get_execution_metadata(
          source_registry, canonical_name, &execution_metadata) !=
      TURBO_TOOL_OK) {
    return dag_fail(diagnostic, TURBO_AGENT_COMPILE_UNRESOLVED_TOOL,
                    "DAG tool metadata is unavailable");
  }

  if (source->retry_limit != 0u &&
      step->execution_policy.idempotency == TURBO_TOOL_IDEMPOTENCY_NONE) {
    return dag_fail(
        diagnostic, TURBO_AGENT_COMPILE_RETRY_UNSAFE,
        "DAG retry requires READ_ONLY or KEYED tool idempotency");
  }

  status = dag_copy_capabilities(
      config, required, required_count, step, diagnostic);
  if (status != TURBO_AGENT_COMPILE_OK) return status;

  step->step_id = dag_strdup(source->step_id);
  step->tool_name = dag_strdup(canonical_name);
  step->arguments = json_clone(source->arguments);
  step->retry_limit = source->retry_limit;
  step->flags = source->flags;
  step->effect_flags = effect_flags;
  if (execution_metadata) {
    step->execution_metadata = json_clone(execution_metadata);
  }
  if (!step->step_id || !step->tool_name || !step->arguments ||
      (execution_metadata && !step->execution_metadata)) {
    return dag_fail(diagnostic, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
                    "could not freeze DAG step semantics");
  }
  return TURBO_AGENT_COMPILE_OK;
}

static turbo_agent_compile_status_t dag_resolve_dependencies(
    const turbo_agent_dag_source_t *source,
    turbo_agent_executable_dag_t *plan,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  size_t i;
  size_t total_edges = 0u;

  for (i = 0; i < source->step_count; ++i) {
    const turbo_agent_dag_step_source_t *src = &source->steps[i];
    turbo_agent_executable_dag_step_t *dst = &plan->steps[i];
    size_t d;

    if (src->dependency_count > TURBO_AGENT_DAG_MAX_EDGES - total_edges) {
      return dag_fail(diagnostic, TURBO_AGENT_COMPILE_PLAN_LIMIT,
                      "DAG edge count exceeds compiler limit");
    }
    total_edges += src->dependency_count;
    if (!src->dependency_count) continue;

    dst->dependency_indices =
        (size_t *)calloc(src->dependency_count, sizeof(size_t));
    if (!dst->dependency_indices) {
      return dag_fail(diagnostic, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
                      "could not allocate DAG dependencies");
    }
    dst->dependency_count = src->dependency_count;

    for (d = 0; d < src->dependency_count; ++d) {
      size_t dep_index;
      size_t prior;
      const char *dep = src->depends_on[d];
      if (!dep || !dep[0] ||
          !dag_find_source_step(source, dep, &dep_index)) {
        return dag_fail(diagnostic, TURBO_AGENT_COMPILE_MISSING_DEPENDENCY,
                        "DAG dependency does not reference a known step");
      }
      for (prior = 0; prior < d; ++prior) {
        if (dst->dependency_indices[prior] == dep_index) {
          return dag_fail(diagnostic, TURBO_AGENT_COMPILE_INVALID_ARGUMENT,
                          "DAG step contains a duplicate dependency edge");
        }
      }
      dst->dependency_indices[d] = dep_index;
    }
  }
  return TURBO_AGENT_COMPILE_OK;
}

static turbo_agent_compile_status_t dag_validate_acyclic(
    const turbo_agent_executable_dag_t *plan,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  size_t *indegree = NULL;
  size_t *queue = NULL;
  size_t head = 0u;
  size_t tail = 0u;
  size_t visited = 0u;
  size_t i;

  indegree = (size_t *)calloc(plan->step_count, sizeof(*indegree));
  queue = (size_t *)calloc(plan->step_count, sizeof(*queue));
  if (!indegree || !queue) {
    free(queue);
    free(indegree);
    return dag_fail(diagnostic, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
                    "could not allocate DAG cycle detector");
  }

  for (i = 0; i < plan->step_count; ++i) {
    indegree[i] = plan->steps[i].dependency_count;
    if (indegree[i] == 0u) queue[tail++] = i;
  }

  while (head < tail) {
    size_t current = queue[head++];
    ++visited;
    for (i = 0; i < plan->step_count; ++i) {
      size_t d;
      for (d = 0; d < plan->steps[i].dependency_count; ++d) {
        if (plan->steps[i].dependency_indices[d] == current) {
          if (indegree[i] == 0u) {
            free(queue);
            free(indegree);
            return dag_fail(diagnostic, TURBO_AGENT_COMPILE_CYCLE,
                            "DAG topology is inconsistent");
          }
          --indegree[i];
          if (indegree[i] == 0u) queue[tail++] = i;
        }
      }
    }
  }

  free(queue);
  free(indegree);
  if (visited != plan->step_count) {
    return dag_fail(diagnostic, TURBO_AGENT_COMPILE_CYCLE,
                    "DAG contains a dependency cycle");
  }
  return TURBO_AGENT_COMPILE_OK;
}

static int dag_capability_exists(
    const turbo_agent_executable_dag_t *plan,
    const char *capability) {
  size_t i;
  for (i = 0; i < plan->capability_count; ++i) {
    if (strcmp(plan->capabilities[i], capability) == 0) return 1;
  }
  return 0;
}

static turbo_agent_compile_status_t dag_build_capability_union(
    turbo_agent_executable_dag_t *plan,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  size_t capacity = 0u;
  size_t i;
  for (i = 0; i < plan->step_count; ++i) {
    capacity += plan->steps[i].capability_count;
  }
  if (!capacity) return TURBO_AGENT_COMPILE_OK;

  plan->capabilities = (char **)calloc(capacity, sizeof(*plan->capabilities));
  if (!plan->capabilities) {
    return dag_fail(diagnostic, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
                    "could not allocate DAG capability union");
  }

  for (i = 0; i < plan->step_count; ++i) {
    size_t c;
    for (c = 0; c < plan->steps[i].capability_count; ++c) {
      const char *cap = plan->steps[i].capabilities[c];
      if (!dag_capability_exists(plan, cap)) {
        plan->capabilities[plan->capability_count] = dag_strdup(cap);
        if (!plan->capabilities[plan->capability_count]) {
          return dag_fail(diagnostic, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
                          "could not copy DAG capability union");
        }
        ++plan->capability_count;
      }
    }
  }
  qsort(plan->capabilities, plan->capability_count,
        sizeof(*plan->capabilities), dag_string_compare);
  return TURBO_AGENT_COMPILE_OK;
}

static turbo_tool_effect_flags_t dag_effect_summary(
    const turbo_agent_executable_dag_t *plan) {
  turbo_tool_effect_flags_t known = 0;
  int all_pure = 1;
  size_t i;
  if (!plan || !plan->step_count) return TURBO_TOOL_EFFECT_UNKNOWN;
  for (i = 0; i < plan->step_count; ++i) {
    turbo_tool_effect_flags_t flags = plan->steps[i].effect_flags;
    if (flags == TURBO_TOOL_EFFECT_UNKNOWN ||
        (flags & TURBO_TOOL_EFFECT_UNKNOWN) != 0) {
      return TURBO_TOOL_EFFECT_UNKNOWN;
    }
    if (flags == TURBO_TOOL_EFFECT_PURE) continue;
    all_pure = 0;
    known |= flags & ~TURBO_TOOL_EFFECT_PURE;
  }
  if (all_pure) return TURBO_TOOL_EFFECT_PURE;
  return known ? known : TURBO_TOOL_EFFECT_UNKNOWN;
}

static int dag_pure_region_eligible_internal(
    const turbo_agent_executable_dag_t *plan) {
  size_t i;
  if (!plan || !plan->step_count) return 0;
  for (i = 0; i < plan->step_count; ++i) {
    const turbo_agent_executable_dag_step_t *step = &plan->steps[i];
    if (step->effect_flags != TURBO_TOOL_EFFECT_PURE ||
        step->execution_policy.idempotency != TURBO_TOOL_IDEMPOTENCY_READ_ONLY ||
        step->execution_policy.mode != TURBO_TOOL_EXECUTION_PARALLEL_SAFE) {
      return 0;
    }
  }
  return 1;
}

static turbo_agent_compile_status_t dag_build_projection(
    const turbo_tool_registry_t *source_registry,
    turbo_agent_executable_dag_t *plan,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  const char **names = NULL;
  size_t name_count = 0u;
  size_t i;

  names = (const char **)calloc(plan->step_count, sizeof(*names));
  if (!names) {
    return dag_fail(diagnostic, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
                    "could not allocate DAG tool projection");
  }
  for (i = 0; i < plan->step_count; ++i) {
    size_t n;
    int duplicate = 0;
    for (n = 0; n < name_count; ++n) {
      if (strcmp(names[n], plan->steps[i].tool_name) == 0) {
        duplicate = 1;
        break;
      }
    }
    if (!duplicate) names[name_count++] = plan->steps[i].tool_name;
  }

  if (turbo_tool_registry_project(
          source_registry, names, name_count, &plan->approved_tools) !=
      TURBO_TOOL_OK ||
      !plan->approved_tools) {
    free(names);
    return dag_fail(diagnostic, TURBO_AGENT_COMPILE_UNRESOLVED_TOOL,
                    "could not freeze DAG RuntimeTools projection");
  }
  free(names);
  return TURBO_AGENT_COMPILE_OK;
}

static uint64_t dag_compute_hash(const turbo_agent_executable_dag_t *plan) {
  uint64_t hash = DAG_FNV_OFFSET;
  size_t i;

  hash = dag_hash_cstring(hash, "TurboAgent.ExecutableDAG.v3");
  hash = dag_hash_u32(hash, TURBO_AGENT_DAG_CERTIFICATE_VERSION);
  hash = dag_hash_u32(hash, (uint32_t)plan->template_kind);
  hash = dag_hash_u32(hash, plan->plan_generation);
  hash = dag_hash_u32(hash, plan->replan_budget);
  hash = dag_hash_u64(hash, (uint64_t)plan->step_count);

  for (i = 0; i < plan->step_count; ++i) {
    const turbo_agent_executable_dag_step_t *step = &plan->steps[i];
    size_t d;
    size_t c;
    hash = dag_hash_cstring(hash, step->step_id);
    hash = dag_hash_cstring(hash, step->tool_name);
    hash = dag_hash_json(hash, step->arguments);
    if (!hash) return 0u;
    hash = dag_hash_u32(hash, (uint32_t)step->execution_policy.mode);
    hash = dag_hash_u32(hash, (uint32_t)step->execution_policy.idempotency);
    hash = dag_hash_u64(hash, step->effect_flags);
    hash = dag_hash_u32(hash, step->retry_limit);
    hash = dag_hash_u32(hash, step->flags);
    hash = dag_hash_u64(hash, (uint64_t)step->dependency_count);
    for (d = 0; d < step->dependency_count; ++d) {
      size_t dep_index = step->dependency_indices[d];
      hash = dag_hash_cstring(hash, plan->steps[dep_index].step_id);
    }
    hash = dag_hash_u64(hash, (uint64_t)step->capability_count);
    for (c = 0; c < step->capability_count; ++c) {
      hash = dag_hash_cstring(hash, step->capabilities[c]);
    }
    hash = dag_hash_cstring(hash, "execution_metadata");
    if (step->execution_metadata) {
      hash = dag_hash_json(hash, step->execution_metadata);
    } else {
      hash = dag_hash_cstring(hash, "none");
    }
  }
  return hash;
}

void turbo_agent_dag_step_source_init(turbo_agent_dag_step_source_t *step) {
  if (!step) return;
  memset(step, 0, sizeof(*step));
  step->struct_size = sizeof(*step);
  step->abi_version = TURBO_AGENT_DAG_STEP_ABI_VERSION;
}

void turbo_agent_dag_source_init(turbo_agent_dag_source_t *source) {
  if (!source) return;
  memset(source, 0, sizeof(*source));
  source->struct_size = sizeof(*source);
  source->abi_version = TURBO_AGENT_DAG_SOURCE_ABI_VERSION;
  source->template_kind = TURBO_AGENT_DAG_TEMPLATE_GENERIC;
  source->plan_generation = 0u;
}

turbo_agent_compile_status_t turbo_agent_compile_dag(
    const turbo_agent_compiler_config_t *config,
    const turbo_tool_registry_t *source_registry,
    const turbo_agent_dag_source_t *source,
    turbo_agent_executable_dag_t **out_plan,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  turbo_agent_executable_dag_t *plan = NULL;
  turbo_agent_compile_status_t status;
  size_t i;

  if (out_plan) *out_plan = NULL;
  dag_diagnostic_set(diagnostic, TURBO_AGENT_COMPILE_OK, "");

  if (!dag_config_valid(config) || !source_registry || !source || !out_plan ||
      !dag_source_valid(source) ||
      !source->steps || source->step_count == 0u ||
      source->step_count > TURBO_AGENT_DAG_MAX_STEPS ||
      !dag_template_kind_valid(dag_source_template_kind(source))) {
    return dag_fail(diagnostic, TURBO_AGENT_COMPILE_INVALID_ARGUMENT,
                    "invalid DAG compiler arguments");
  }

  for (i = 0; i < source->step_count; ++i) {
    size_t prior;
    const char *id = source->steps[i].step_id;
    if (!id || !id[0]) {
      return dag_fail(diagnostic, TURBO_AGENT_COMPILE_INVALID_ARGUMENT,
                      "DAG step ID is empty");
    }
    for (prior = 0; prior < i; ++prior) {
      if (source->steps[prior].step_id &&
          strcmp(source->steps[prior].step_id, id) == 0) {
        return dag_fail(diagnostic, TURBO_AGENT_COMPILE_DUPLICATE_STEP,
                        "DAG contains duplicate step IDs");
      }
    }
  }

  plan = (turbo_agent_executable_dag_t *)calloc(1, sizeof(*plan));
  if (!plan) {
    return dag_fail(diagnostic, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
                    "could not allocate executable DAG");
  }
  plan->steps = (turbo_agent_executable_dag_step_t *)calloc(
      source->step_count, sizeof(*plan->steps));
  if (!plan->steps) {
    free(plan);
    return dag_fail(diagnostic, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
                    "could not allocate executable DAG steps");
  }
  plan->step_count = source->step_count;
  plan->replan_budget = source->replan_budget;
  plan->template_kind = dag_source_template_kind(source);
  plan->plan_generation = dag_source_plan_generation(source);

  for (i = 0; i < source->step_count; ++i) {
    status = dag_admit_step(
        config, source_registry, &source->steps[i],
        &plan->steps[i], diagnostic);
    if (status != TURBO_AGENT_COMPILE_OK) goto fail;
  }

  status = dag_resolve_dependencies(source, plan, diagnostic);
  if (status != TURBO_AGENT_COMPILE_OK) goto fail;
  status = dag_validate_acyclic(plan, diagnostic);
  if (status != TURBO_AGENT_COMPILE_OK) goto fail;
  status = dag_build_capability_union(plan, diagnostic);
  if (status != TURBO_AGENT_COMPILE_OK) goto fail;
  status = dag_build_projection(source_registry, plan, diagnostic);
  if (status != TURBO_AGENT_COMPILE_OK) goto fail;

  plan->effect_flags = dag_effect_summary(plan);
  plan->plan_hash = dag_compute_hash(plan);
  if (!plan->plan_hash) {
    status = dag_fail(diagnostic, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
                      "could not hash executable DAG");
    goto fail;
  }

  *out_plan = plan;
  dag_diagnostic_set(diagnostic, TURBO_AGENT_COMPILE_OK, "ok");
  return TURBO_AGENT_COMPILE_OK;

fail:
  dag_clear(plan);
  free(plan);
  return status;
}

void turbo_agent_executable_dag_destroy(turbo_agent_executable_dag_t *plan) {
  if (!plan) return;
  dag_clear(plan);
  free(plan);
}

uint64_t turbo_agent_executable_dag_hash(
    const turbo_agent_executable_dag_t *plan) {
  return plan ? plan->plan_hash : 0u;
}

size_t turbo_agent_executable_dag_step_count(
    const turbo_agent_executable_dag_t *plan) {
  return plan ? plan->step_count : 0u;
}

turbo_agent_dag_template_kind_t turbo_agent_executable_dag_template_kind(
    const turbo_agent_executable_dag_t *plan) {
  return plan ? plan->template_kind : TURBO_AGENT_DAG_TEMPLATE_GENERIC;
}

uint32_t turbo_agent_executable_dag_plan_generation(
    const turbo_agent_executable_dag_t *plan) {
  return plan ? plan->plan_generation : 0u;
}

turbo_tool_effect_flags_t turbo_agent_executable_dag_effects(
    const turbo_agent_executable_dag_t *plan) {
  return plan ? plan->effect_flags : TURBO_TOOL_EFFECT_UNKNOWN;
}

int turbo_agent_executable_dag_pure_region_eligible(
    const turbo_agent_executable_dag_t *plan) {
  return dag_pure_region_eligible_internal(plan);
}

const turbo_tool_registry_t *turbo_agent_executable_dag_approved_tools(
    const turbo_agent_executable_dag_t *plan) {
  return plan ? plan->approved_tools : NULL;
}

json_value_t *turbo_agent_executable_dag_certificate_json_value(
    const turbo_agent_executable_dag_t *plan) {
  json_value_t *root = NULL;
  json_value_t *steps = NULL;
  json_value_t *caps = NULL;
  json_value_t *field = NULL;
  char hash_text[17];
  size_t i;

  if (!plan) return NULL;
  root = json_create_object();
  steps = json_create_array();
  caps = json_create_array();
  if (!root || !steps || !caps) goto fail;

  field = json_create_int64(TURBO_AGENT_DAG_CERTIFICATE_VERSION);
  if (!field ||
      turbo_runtime_json_object_set(root, "version", field) !=
          TURBO_RUNTIME_JSON_OK) goto fail;
  field = NULL;

  field = json_create_string("praktor_host_tool");
  if (!field ||
      turbo_runtime_json_object_set(root, "backend", field) !=
          TURBO_RUNTIME_JSON_OK) goto fail;
  field = NULL;

  field = json_create_string(dag_template_kind_name(plan->template_kind));
  if (!field ||
      turbo_runtime_json_object_set(root, "template", field) !=
          TURBO_RUNTIME_JSON_OK) goto fail;
  field = NULL;

  field = json_create_int64((int64_t)plan->plan_generation);
  if (!field ||
      turbo_runtime_json_object_set(root, "plan_generation", field) !=
          TURBO_RUNTIME_JSON_OK) goto fail;
  field = NULL;

  field = json_create_int64((int64_t)plan->replan_budget);
  if (!field ||
      turbo_runtime_json_object_set(root, "replan_budget", field) !=
          TURBO_RUNTIME_JSON_OK) goto fail;
  field = NULL;

  field = json_create_int64((int64_t)plan->effect_flags);
  if (!field ||
      turbo_runtime_json_object_set(root, "effect_flags", field) !=
          TURBO_RUNTIME_JSON_OK) goto fail;
  field = NULL;

  snprintf(hash_text, sizeof(hash_text), "%016llx",
           (unsigned long long)plan->plan_hash);
  field = json_create_string(hash_text);
  if (!field ||
      turbo_runtime_json_object_set(root, "plan_hash", field) !=
          TURBO_RUNTIME_JSON_OK) goto fail;
  field = NULL;

  for (i = 0; i < plan->capability_count; ++i) {
    field = json_create_string(plan->capabilities[i]);
    if (!field ||
        turbo_runtime_json_array_append(caps, field) !=
            TURBO_RUNTIME_JSON_OK) goto fail;
    field = NULL;
  }
  if (turbo_runtime_json_object_set(root, "capabilities", caps) !=
      TURBO_RUNTIME_JSON_OK) goto fail;
  caps = NULL;

  for (i = 0; i < plan->step_count; ++i) {
    const turbo_agent_executable_dag_step_t *step = &plan->steps[i];
    json_value_t *item = json_create_object();
    json_value_t *deps = json_create_array();
    json_value_t *step_caps = json_create_array();
    size_t d;
    size_t c;
    if (!item || !deps || !step_caps) {
      turbo_runtime_json_destroy(step_caps);
      turbo_runtime_json_destroy(deps);
      turbo_runtime_json_destroy(item);
      goto fail;
    }

#define DAG_SET_STRING(key, value) \
    do { \
      json_value_t *v_ = json_create_string((value)); \
      if (!v_ || turbo_runtime_json_object_set(item, (key), v_) != \
                      TURBO_RUNTIME_JSON_OK) { \
        turbo_runtime_json_destroy(v_); \
        turbo_runtime_json_destroy(step_caps); \
        turbo_runtime_json_destroy(deps); \
        turbo_runtime_json_destroy(item); \
        goto fail; \
      } \
    } while (0)

#define DAG_SET_INT(key, value) \
    do { \
      json_value_t *v_ = json_create_int64((int64_t)(value)); \
      if (!v_ || turbo_runtime_json_object_set(item, (key), v_) != \
                      TURBO_RUNTIME_JSON_OK) { \
        turbo_runtime_json_destroy(v_); \
        turbo_runtime_json_destroy(step_caps); \
        turbo_runtime_json_destroy(deps); \
        turbo_runtime_json_destroy(item); \
        goto fail; \
      } \
    } while (0)

    DAG_SET_STRING("step_id", step->step_id);
    DAG_SET_STRING("tool", step->tool_name);
    DAG_SET_INT("retry_limit", step->retry_limit);
    DAG_SET_INT("flags", step->flags);
    DAG_SET_INT("execution_mode", step->execution_policy.mode);
    DAG_SET_INT("idempotency", step->execution_policy.idempotency);
    DAG_SET_INT("effect_flags", step->effect_flags);

    field = json_clone(step->arguments);
    if (!field ||
        turbo_runtime_json_object_set(item, "arguments", field) !=
            TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_json_destroy(field);
      turbo_runtime_json_destroy(step_caps);
      turbo_runtime_json_destroy(deps);
      turbo_runtime_json_destroy(item);
      goto fail;
    }
    field = NULL;

    for (d = 0; d < step->dependency_count; ++d) {
      field = json_create_string(
          plan->steps[step->dependency_indices[d]].step_id);
      if (!field ||
          turbo_runtime_json_array_append(deps, field) !=
              TURBO_RUNTIME_JSON_OK) {
        turbo_runtime_json_destroy(field);
        turbo_runtime_json_destroy(step_caps);
        turbo_runtime_json_destroy(deps);
        turbo_runtime_json_destroy(item);
        goto fail;
      }
      field = NULL;
    }
    if (turbo_runtime_json_object_set(item, "depends_on", deps) !=
        TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_json_destroy(step_caps);
      turbo_runtime_json_destroy(deps);
      turbo_runtime_json_destroy(item);
      goto fail;
    }
    deps = NULL;

    for (c = 0; c < step->capability_count; ++c) {
      field = json_create_string(step->capabilities[c]);
      if (!field ||
          turbo_runtime_json_array_append(step_caps, field) !=
              TURBO_RUNTIME_JSON_OK) {
        turbo_runtime_json_destroy(field);
        turbo_runtime_json_destroy(step_caps);
        turbo_runtime_json_destroy(item);
        goto fail;
      }
      field = NULL;
    }
    if (turbo_runtime_json_object_set(item, "capabilities", step_caps) !=
        TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_json_destroy(step_caps);
      turbo_runtime_json_destroy(item);
      goto fail;
    }
    step_caps = NULL;

    if (step->execution_metadata) {
      field = json_clone(step->execution_metadata);
      if (!field ||
          turbo_runtime_json_object_set(
              item, "execution_metadata", field) !=
              TURBO_RUNTIME_JSON_OK) {
        turbo_runtime_json_destroy(field);
        turbo_runtime_json_destroy(item);
        goto fail;
      }
      field = NULL;
    }

    if (turbo_runtime_json_array_append(steps, item) !=
        TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_json_destroy(item);
      goto fail;
    }
#undef DAG_SET_INT
#undef DAG_SET_STRING
  }

  if (turbo_runtime_json_object_set(root, "steps", steps) !=
      TURBO_RUNTIME_JSON_OK) goto fail;
  steps = NULL;
  return root;

fail:
  turbo_runtime_json_destroy(field);
  turbo_runtime_json_destroy(caps);
  turbo_runtime_json_destroy(steps);
  turbo_runtime_json_destroy(root);
  return NULL;
}
