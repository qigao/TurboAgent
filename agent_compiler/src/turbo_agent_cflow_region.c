#include "turbo_agent_cflow_region.h"

#include <cflow/direct.h>
#include <cflow/function_projection.h>
#include <cflow/plan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  TURBO_AGENT_CFLOW_REGION_MIN_STEPS = 2,
  TURBO_AGENT_CFLOW_REGION_MAX_STEPS = CFLOW_AOT_STAGE_LIMIT
};

#define REGION_FNV_OFFSET UINT64_C(1469598103934665603)
#define REGION_FNV_PRIME UINT64_C(1099511628211)

struct turbo_agent_cflow_region_s {
  cflow_plan plan;
  cflow_plan_compile_stats stats;
  size_t step_count;
  char **step_ids;
  char **tool_names;
  char **input_properties;
  char **function_names;
  uint64_t source_dag_hash;
  uint64_t hash;
};

static char *region_strdup(const char *value) {
  size_t size;
  char *copy;
  if (!value) return NULL;
  size = strlen(value) + 1u;
  copy = (char *)malloc(size);
  if (!copy) return NULL;
  memcpy(copy, value, size);
  return copy;
}

static turbo_agent_compile_status_t region_fail(
    turbo_agent_compile_diagnostic_t *diagnostic,
    turbo_agent_compile_status_t status,
    const char *message) {
  if (diagnostic) {
    diagnostic->status = status;
    snprintf(diagnostic->message, sizeof(diagnostic->message), "%s",
             message ? message : "CFlow region compilation failed");
  }
  return status;
}

static uint64_t region_hash_bytes(uint64_t hash, const void *data, size_t size) {
  const unsigned char *bytes = (const unsigned char *)data;
  size_t i;
  for (i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= REGION_FNV_PRIME;
  }
  return hash;
}

static uint64_t region_hash_u64(uint64_t hash, uint64_t value) {
  return region_hash_bytes(hash, &value, sizeof(value));
}

static uint64_t region_hash_cstring(uint64_t hash, const char *value) {
  if (!value) return 0u;
  return region_hash_bytes(hash, value, strlen(value) + 1u);
}

static int region_step_has_dependency(
    const turbo_agent_executable_dag_t *dag,
    size_t step_index,
    const char *required_step_id) {
  turbo_agent_dag_step_view_t view = {0};
  size_t dependency;
  if (!dag || !required_step_id ||
      turbo_agent_executable_dag_step_view(dag, step_index, &view) !=
          TURBO_AGENT_COMPILE_OK) {
    return 0;
  }
  for (dependency = 0; dependency < view.dependency_count; ++dependency) {
    const char *dependency_step_id = NULL;
    if (turbo_agent_executable_dag_dependency_step_id(
            dag, step_index, dependency, &dependency_step_id) !=
            TURBO_AGENT_COMPILE_OK) {
      return 0;
    }
    if (dependency_step_id &&
        strcmp(dependency_step_id, required_step_id) == 0) {
      return 1;
    }
  }
  return 0;
}

static int region_step_index_seen(
    const size_t *indices, size_t count, size_t candidate) {
  size_t i;
  for (i = 0; i < count; ++i) {
    if (indices[i] == candidate) return 1;
  }
  return 0;
}

static int region_native_map_contract_valid(
    const turbo_tool_native_projection_t *native,
    cflow_function_projection *out_projection) {
  const cmeta_param_desc *param;
  const cmeta_type_desc *direct_flow_type;
  cflow_function_projection_status projection_status;

  if (!native || !out_projection || !native->function ||
      native->function->result_flags != CMETA_RESULT_VALUE ||
      native->function->param_count != 1u) {
    return 0;
  }

  param = cmeta_function_param(native->function, 0u);
  if (!param ||
      (param->flags & CMETA_PARAM_DIRECTION_MASK) != CMETA_PARAM_IN ||
      (param->flags & (CMETA_PARAM_NULLABLE | CMETA_PARAM_OWNERSHIP_MASK)) !=
          0u ||
      !cflow_direct_type_eligible(param->type) ||
      !cflow_direct_type_eligible(native->function->return_type)) {
    return 0;
  }

  projection_status = cflow_function_projection_admit(
      native->function, native->abi, native->callable, CFLOW_OP_MAP,
      out_projection);
  if (projection_status != CFLOW_FUNCTION_PROJECTION_OK) {
    return 0;
  }

  direct_flow_type = out_projection->input_type;
  if (!cflow_direct_stage_eligible(
          out_projection->callable, CFLOW_DIRECT_STAGE_MAP,
          &direct_flow_type) ||
      !cmeta_type_equal(direct_flow_type, out_projection->output_type)) {
    return 0;
  }
  return 1;
}

static void region_free_strings(turbo_agent_cflow_region_t *region) {
  size_t i;
  if (!region) return;
  for (i = 0; i < region->step_count; ++i) {
    free(region->step_ids ? region->step_ids[i] : NULL);
    free(region->tool_names ? region->tool_names[i] : NULL);
    free(region->input_properties ? region->input_properties[i] : NULL);
    free(region->function_names ? region->function_names[i] : NULL);
  }
  free(region->step_ids);
  free(region->tool_names);
  free(region->input_properties);
  free(region->function_names);
  region->step_ids = NULL;
  region->tool_names = NULL;
  region->input_properties = NULL;
  region->function_names = NULL;
}

void turbo_agent_cflow_region_step_source_init(
    turbo_agent_cflow_region_step_source_t *step) {
  if (!step) return;
  memset(step, 0, sizeof(*step));
  step->struct_size = sizeof(*step);
  step->abi_version = TURBO_AGENT_CFLOW_REGION_STEP_ABI_VERSION;
  step->op = TURBO_AGENT_CFLOW_REGION_OP_MAP;
}

void turbo_agent_cflow_region_source_init(
    turbo_agent_cflow_region_source_t *source) {
  if (!source) return;
  memset(source, 0, sizeof(*source));
  source->struct_size = sizeof(*source);
  source->abi_version = TURBO_AGENT_CFLOW_REGION_SOURCE_ABI_VERSION;
}

turbo_agent_compile_status_t turbo_agent_compile_cflow_region(
    const turbo_agent_executable_dag_t *dag,
    const turbo_agent_cflow_region_source_t *source,
    turbo_agent_cflow_region_t **out_region,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  const turbo_tool_registry_t *approved;
  turbo_agent_cflow_region_t *region = NULL;
  cflow_graph graph = {0};
  const cmeta_type_desc *previous_output_type = NULL;
  const char *previous_tool = NULL;
  const char *previous_step_id = NULL;
  size_t step_indices[TURBO_AGENT_CFLOW_REGION_MAX_STEPS] = {0};
  uint64_t hash = REGION_FNV_OFFSET;
  size_t i;
  int graph_initialized = 0;

  if (out_region) *out_region = NULL;
  if (diagnostic) {
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->status = TURBO_AGENT_COMPILE_OK;
  }

  if (!dag || !source || !out_region ||
      source->struct_size < sizeof(*source) ||
      source->abi_version != TURBO_AGENT_CFLOW_REGION_SOURCE_ABI_VERSION ||
      !source->steps ||
      source->step_count < TURBO_AGENT_CFLOW_REGION_MIN_STEPS ||
      source->step_count > TURBO_AGENT_CFLOW_REGION_MAX_STEPS) {
    return region_fail(diagnostic, TURBO_AGENT_COMPILE_INVALID_ARGUMENT,
                       "invalid explicit CFlow region source");
  }

  approved = turbo_agent_executable_dag_approved_tools(dag);
  if (!approved) {
    return region_fail(diagnostic, TURBO_AGENT_COMPILE_SOURCE_INVALID,
                       "compiled DAG has no approved RuntimeTools projection");
  }

  region = (turbo_agent_cflow_region_t *)calloc(1, sizeof(*region));
  if (!region) {
    return region_fail(diagnostic, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
                       "failed to allocate CFlow region");
  }
  region->step_count = source->step_count;
  region->source_dag_hash = turbo_agent_executable_dag_hash(dag);
  region->step_ids = (char **)calloc(source->step_count, sizeof(char *));
  region->tool_names = (char **)calloc(source->step_count, sizeof(char *));
  region->input_properties =
      (char **)calloc(source->step_count, sizeof(char *));
  region->function_names =
      (char **)calloc(source->step_count, sizeof(char *));
  if (!region->step_ids || !region->tool_names ||
      !region->input_properties || !region->function_names) {
    turbo_agent_cflow_region_destroy(region);
    return region_fail(diagnostic, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
                       "failed to allocate CFlow region identity");
  }

  hash = region_hash_cstring(hash, "TurboAgent.CFlowRegion.v1");
  hash = region_hash_u64(hash, region->source_dag_hash);
  hash = region_hash_u64(hash, (uint64_t)source->step_count);

  for (i = 0; i < source->step_count; ++i) {
    const turbo_agent_cflow_region_step_source_t *source_step =
        &source->steps[i];
    turbo_agent_dag_step_view_t dag_step = {0};
    turbo_tool_native_projection_t native = {0};
    cflow_function_projection projection = {0};
    size_t dag_step_index = 0u;
    turbo_agent_contract_compatibility_t logical_native_input;
    turbo_agent_contract_compatibility_t logical_native_result;

    if (source_step->struct_size < sizeof(*source_step) ||
        source_step->abi_version !=
            TURBO_AGENT_CFLOW_REGION_STEP_ABI_VERSION ||
        source_step->op != TURBO_AGENT_CFLOW_REGION_OP_MAP ||
        !source_step->step_id || !source_step->step_id[0] ||
        !source_step->input_property || !source_step->input_property[0]) {
      cflow_graph_destroy(&graph);
      turbo_agent_cflow_region_destroy(region);
      return region_fail(diagnostic, TURBO_AGENT_COMPILE_SOURCE_INVALID,
                         "CFlow region step is invalid or not explicit MAP");
    }

    if (turbo_agent_executable_dag_find_step(
            dag, source_step->step_id, &dag_step_index) !=
            TURBO_AGENT_COMPILE_OK ||
        turbo_agent_executable_dag_step_view(
            dag, dag_step_index, &dag_step) != TURBO_AGENT_COMPILE_OK) {
      cflow_graph_destroy(&graph);
      turbo_agent_cflow_region_destroy(region);
      return region_fail(diagnostic, TURBO_AGENT_COMPILE_MISSING_DEPENDENCY,
                         "CFlow region references a step outside the compiled DAG");
    }
    if (region_step_index_seen(step_indices, i, dag_step_index)) {
      cflow_graph_destroy(&graph);
      turbo_agent_cflow_region_destroy(region);
      return region_fail(diagnostic, TURBO_AGENT_COMPILE_DUPLICATE_STEP,
                         "CFlow region repeats one compiled DAG step");
    }
    step_indices[i] = dag_step_index;

    if (dag_step.retry_limit != 0u || dag_step.flags != 0u) {
      cflow_graph_destroy(&graph);
      turbo_agent_cflow_region_destroy(region);
      return region_fail(
          diagnostic, TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION,
          "CFlow region cannot cross retry, approval, or checkpoint boundaries");
    }
    if (dag_step.effect_flags != TURBO_TOOL_EFFECT_PURE ||
        dag_step.execution_policy.idempotency !=
            TURBO_TOOL_IDEMPOTENCY_READ_ONLY ||
        dag_step.execution_policy.mode !=
            TURBO_TOOL_EXECUTION_PARALLEL_SAFE) {
      cflow_graph_destroy(&graph);
      turbo_agent_cflow_region_destroy(region);
      return region_fail(
          diagnostic, TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION,
          "CFlow region step is not PURE, READ_ONLY, and PARALLEL_SAFE");
    }

    if (i > 0u &&
        !region_step_has_dependency(dag, dag_step_index, previous_step_id)) {
      cflow_graph_destroy(&graph);
      turbo_agent_cflow_region_destroy(region);
      return region_fail(
          diagnostic, TURBO_AGENT_COMPILE_MISSING_DEPENDENCY,
          "explicit CFlow value edge lacks the required DAG ordering dependency");
    }

    if (turbo_tool_registry_get_native_projection(
            approved, dag_step.tool_name, &native) != TURBO_TOOL_OK ||
        !region_native_map_contract_valid(&native, &projection)) {
      cflow_graph_destroy(&graph);
      turbo_agent_cflow_region_destroy(region);
      return region_fail(
          diagnostic, TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION,
          "tool lacks an eligible static-native CMeta MAP projection");
    }

    logical_native_input =
        turbo_agent_tool_input_slot_native_compatibility(
            approved, dag_step.tool_name, source_step->input_property,
            projection.input_type);
    logical_native_result =
        turbo_agent_tool_result_native_compatibility(
            approved, dag_step.tool_name, projection.output_type);
    if (logical_native_input != TURBO_AGENT_CONTRACT_COMPATIBLE ||
        logical_native_result != TURBO_AGENT_CONTRACT_COMPATIBLE) {
      cflow_graph_destroy(&graph);
      turbo_agent_cflow_region_destroy(region);
      return region_fail(
          diagnostic, TURBO_AGENT_COMPILE_TOOL_SCHEMA_INVALID,
          "logical RuntimeTools schema and native CMeta scalar contract disagree");
    }

    if (i > 0u) {
      if (!cmeta_type_equal(previous_output_type, projection.input_type)) {
        cflow_graph_destroy(&graph);
        turbo_agent_cflow_region_destroy(region);
        return region_fail(
            diagnostic, TURBO_AGENT_COMPILE_TOOL_SCHEMA_INVALID,
            "adjacent native CMeta MAP types do not form one value chain");
      }
      if (turbo_agent_tool_result_slot_compatibility(
              approved, previous_tool, dag_step.tool_name,
              source_step->input_property) !=
          TURBO_AGENT_CONTRACT_COMPATIBLE) {
        cflow_graph_destroy(&graph);
        turbo_agent_cflow_region_destroy(region);
        return region_fail(
            diagnostic, TURBO_AGENT_COMPILE_TOOL_SCHEMA_INVALID,
            "logical producer result is not provably compatible with consumer slot");
      }
    }

    if (!graph_initialized) {
      cflow_graph_init(&graph, projection.input_type);
      graph_initialized = 1;
    }
    if (!cflow_graph_add_function_projection(&graph, &projection)) {
      cflow_graph_destroy(&graph);
      turbo_agent_cflow_region_destroy(region);
      return region_fail(diagnostic, TURBO_AGENT_COMPILE_SOURCE_INVALID,
                         "CFlow rejected the admitted MAP projection");
    }

    region->step_ids[i] = region_strdup(dag_step.step_id);
    region->tool_names[i] = region_strdup(dag_step.tool_name);
    region->input_properties[i] =
        region_strdup(source_step->input_property);
    region->function_names[i] =
        region_strdup(native.function->name ? native.function->name : "");
    if (!region->step_ids[i] || !region->tool_names[i] ||
        !region->input_properties[i] || !region->function_names[i]) {
      cflow_graph_destroy(&graph);
      turbo_agent_cflow_region_destroy(region);
      return region_fail(diagnostic, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
                         "failed to freeze CFlow region identity");
    }

    hash = region_hash_cstring(hash, region->step_ids[i]);
    hash = region_hash_cstring(hash, region->tool_names[i]);
    hash = region_hash_cstring(hash, region->input_properties[i]);
    hash = region_hash_cstring(hash, region->function_names[i]);
    hash = region_hash_u64(hash, (uint64_t)source_step->op);
    hash = region_hash_cstring(
        hash, projection.input_type->name ? projection.input_type->name : "");
    hash = region_hash_cstring(
        hash, projection.output_type->name ? projection.output_type->name : "");
    hash = region_hash_u64(hash, (uint64_t)native.function->effects);
    hash = region_hash_u64(hash, (uint64_t)native.function->properties);
    hash = region_hash_u64(hash, (uint64_t)native.function->result_flags);
    if (!hash) {
      cflow_graph_destroy(&graph);
      turbo_agent_cflow_region_destroy(region);
      return region_fail(diagnostic, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
                         "failed to compute CFlow region identity");
    }

    previous_output_type = projection.output_type;
    previous_tool = region->tool_names[i];
    previous_step_id = region->step_ids[i];
  }

  {
    const char *graph_error = NULL;
    if (!cflow_graph_validate(&graph, &graph_error)) {
      cflow_graph_destroy(&graph);
      turbo_agent_cflow_region_destroy(region);
      return region_fail(
          diagnostic, TURBO_AGENT_COMPILE_SOURCE_INVALID,
          graph_error ? graph_error : "CFlow region graph validation failed");
    }
  }

  if (!cflow_plan_compile_surface(&region->plan, &graph, &region->stats)) {
    const char *plan_error = region->plan.error;
    char message[256];
    snprintf(message, sizeof(message), "CFlow plan compile failed: %s",
             plan_error ? plan_error : "unsupported region");
    cflow_graph_destroy(&graph);
    turbo_agent_cflow_region_destroy(region);
    return region_fail(diagnostic, TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION,
                       message);
  }
  cflow_graph_destroy(&graph);

  if (!region->plan.input_type || !region->plan.output_type ||
      !cflow_direct_type_eligible(region->plan.input_type) ||
      !cflow_direct_type_eligible(region->plan.output_type)) {
    turbo_agent_cflow_region_destroy(region);
    return region_fail(diagnostic, TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION,
                       "compiled CFlow plan escaped the finite scalar slice");
  }

  hash = region_hash_cstring(hash, "backend:cflow");
  hash = region_hash_cstring(hash, "operator:map");
  region->hash = hash;
  *out_region = region;
  return TURBO_AGENT_COMPILE_OK;
}

void turbo_agent_cflow_region_destroy(turbo_agent_cflow_region_t *region) {
  if (!region) return;
  cflow_plan_destroy(&region->plan);
  region_free_strings(region);
  free(region);
}

size_t turbo_agent_cflow_region_step_count(
    const turbo_agent_cflow_region_t *region) {
  return region ? region->step_count : 0u;
}

uint64_t turbo_agent_cflow_region_hash(
    const turbo_agent_cflow_region_t *region) {
  return region ? region->hash : 0u;
}

uint64_t turbo_agent_cflow_region_source_dag_hash(
    const turbo_agent_cflow_region_t *region) {
  return region ? region->source_dag_hash : 0u;
}

const cmeta_type_desc *turbo_agent_cflow_region_input_type(
    const turbo_agent_cflow_region_t *region) {
  return region ? region->plan.input_type : NULL;
}

const cmeta_type_desc *turbo_agent_cflow_region_output_type(
    const turbo_agent_cflow_region_t *region) {
  return region ? region->plan.output_type : NULL;
}

bool turbo_agent_cflow_region_eval_array(
    const turbo_agent_cflow_region_t *region,
    const void *inputs,
    size_t input_count,
    cflow_result *out_result) {
  if (out_result) memset(out_result, 0, sizeof(*out_result));
  if (!region || !out_result || (input_count > 0u && !inputs)) {
    return false;
  }
  return cflow_plan_eval_array(
      &region->plan, inputs, input_count, out_result);
}

json_value_t *turbo_agent_cflow_region_certificate_json_value(
    const turbo_agent_cflow_region_t *region) {
  json_value_t *root = NULL;
  json_value_t *steps = NULL;
  json_value_t *field = NULL;
  char hash_text[17];
  char dag_hash_text[17];
  size_t i;

  if (!region) return NULL;
  root = json_create_object();
  steps = json_create_array();
  if (!root || !steps) goto fail;

#define REGION_SET_FIELD(key, value_expr)                                      \
  do {                                                                          \
    field = (value_expr);                                                        \
    if (!field ||                                                               \
        turbo_runtime_json_object_set(root, (key), field) !=                    \
            TURBO_RUNTIME_JSON_OK) {                                             \
      turbo_runtime_json_destroy(field);                                         \
      field = NULL;                                                              \
      goto fail;                                                                 \
    }                                                                            \
    field = NULL;                                                                \
  } while (0)

  REGION_SET_FIELD("version",
                   json_create_int64(TURBO_AGENT_CFLOW_REGION_CERTIFICATE_VERSION));
  REGION_SET_FIELD("backend", json_create_string("cflow"));
  REGION_SET_FIELD("operator", json_create_string("map"));
  REGION_SET_FIELD("step_count", json_create_int64((int64_t)region->step_count));

  snprintf(hash_text, sizeof(hash_text), "%016llx",
           (unsigned long long)region->hash);
  snprintf(dag_hash_text, sizeof(dag_hash_text), "%016llx",
           (unsigned long long)region->source_dag_hash);
  REGION_SET_FIELD("region_hash", json_create_string(hash_text));
  REGION_SET_FIELD("source_dag_hash", json_create_string(dag_hash_text));
  REGION_SET_FIELD(
      "input_type",
      json_create_string(region->plan.input_type &&
                                 region->plan.input_type->name
                             ? region->plan.input_type->name
                             : ""));
  REGION_SET_FIELD(
      "output_type",
      json_create_string(region->plan.output_type &&
                                 region->plan.output_type->name
                             ? region->plan.output_type->name
                             : ""));

  for (i = 0; i < region->step_count; ++i) {
    json_value_t *item = json_create_object();
    if (!item) goto fail;
#define REGION_ITEM_STRING(key, value)                                         \
    do {                                                                         \
      json_value_t *v_ = json_create_string((value));                            \
      if (!v_ ||                                                                 \
          turbo_runtime_json_object_set(item, (key), v_) !=                     \
              TURBO_RUNTIME_JSON_OK) {                                           \
        turbo_runtime_json_destroy(v_);                                          \
        turbo_runtime_json_destroy(item);                                        \
        goto fail;                                                               \
      }                                                                          \
    } while (0)
    REGION_ITEM_STRING("step_id", region->step_ids[i]);
    REGION_ITEM_STRING("tool", region->tool_names[i]);
    REGION_ITEM_STRING("input_property", region->input_properties[i]);
    REGION_ITEM_STRING("function", region->function_names[i]);
#undef REGION_ITEM_STRING
    if (turbo_runtime_json_array_append(steps, item) !=
        TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_json_destroy(item);
      goto fail;
    }
  }

  if (turbo_runtime_json_object_set(root, "steps", steps) !=
      TURBO_RUNTIME_JSON_OK) {
    goto fail;
  }
  steps = NULL;
#undef REGION_SET_FIELD
  return root;

fail:
  turbo_runtime_json_destroy(field);
  turbo_runtime_json_destroy(steps);
  turbo_runtime_json_destroy(root);
  return NULL;
}
