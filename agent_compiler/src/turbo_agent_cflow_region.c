#include "turbo_agent_cflow_region.h"

#include "turbo_agent_contracts.h"
#include "turbo_tool_registry.h"

#include <cflow/function_projection.h>
#include <cflow/plan.h>

#include <stdlib.h>
#include <string.h>

struct turbo_agent_cflow_region_plan_s {
  cflow_plan plan;
  const cmeta_type_desc *input_type;
  const cmeta_type_desc *output_type;
  size_t step_count;
};

typedef struct turbo_agent_cflow_region_prepared_s {
  const turbo_tool_registry_t *registry;
  cflow_function_projection *projections;
  const char **tool_names;
  size_t step_count;
} turbo_agent_cflow_region_prepared_t;

static int region_source_valid(const turbo_agent_cflow_region_source_t *source) {
  size_t i;
  if (!source ||
      source->struct_size < sizeof(*source) ||
      source->abi_version != TURBO_AGENT_CFLOW_REGION_SOURCE_ABI_VERSION ||
      !source->steps || source->step_count < 2u) {
    return 0;
  }
  for (i = 0; i < source->step_count; ++i) {
    const turbo_agent_cflow_region_step_t *step = &source->steps[i];
    if (step->struct_size < sizeof(*step) ||
        step->abi_version != TURBO_AGENT_CFLOW_REGION_STEP_ABI_VERSION ||
        step->operator_kind != TURBO_AGENT_CFLOW_REGION_OPERATOR_MAP) {
      return 0;
    }
    if (i == 0u) {
      if (step->consumer_property && step->consumer_property[0]) return 0;
    } else if (!step->consumer_property || !step->consumer_property[0]) {
      return 0;
    }
  }
  return 1;
}

static int region_scalar_type_supported(const cmeta_type_desc *type) {
  if (!type || !cmeta_type_desc_valid(type) || type->size == 0u) return 0;
  return type->kind == CMETA_T_BOOL ||
         type->kind == CMETA_T_INTEGER ||
         type->kind == CMETA_T_FLOAT;
}

static turbo_agent_cflow_region_status_t region_admit_native_map(
    const turbo_tool_registry_t *registry,
    const char *tool_name,
    cflow_function_projection *out_projection) {
  turbo_tool_execution_policy_t policy = {0};
  turbo_tool_effect_flags_t effects = TURBO_TOOL_EFFECT_UNKNOWN;
  turbo_tool_native_projection_t native = {0};
  const cmeta_param_desc *param;
  cflow_function_projection_status projection_status;

  if (turbo_tool_registry_get_execution_policy(
          registry, tool_name, &policy) != TURBO_TOOL_OK ||
      turbo_tool_registry_get_effects(
          registry, tool_name, &effects) != TURBO_TOOL_OK) {
    return TURBO_AGENT_CFLOW_REGION_NOT_APPROVED;
  }

  if (effects != TURBO_TOOL_EFFECT_PURE ||
      policy.idempotency != TURBO_TOOL_IDEMPOTENCY_READ_ONLY ||
      policy.mode != TURBO_TOOL_EXECUTION_PARALLEL_SAFE) {
    return TURBO_AGENT_CFLOW_REGION_POLICY_BARRIER;
  }

  native.struct_size = sizeof(native);
  native.abi_version = TURBO_TOOL_NATIVE_PROJECTION_ABI_VERSION;
  if (turbo_tool_registry_get_native_projection(
          registry, tool_name, &native) != TURBO_TOOL_OK) {
    return TURBO_AGENT_CFLOW_REGION_NATIVE_PROJECTION_BARRIER;
  }

  if (!native.function || !native.abi ||
      native.function->param_count != 1u ||
      native.function->result_flags != CMETA_RESULT_VALUE ||
      !cmeta_effects_are_pure(native.function->effects)) {
    return TURBO_AGENT_CFLOW_REGION_OWNERSHIP_BARRIER;
  }

  param = cmeta_function_param(native.function, 0u);
  if (!param || param->flags != CMETA_PARAM_IN ||
      !region_scalar_type_supported(param->type) ||
      !region_scalar_type_supported(native.function->return_type)) {
    return TURBO_AGENT_CFLOW_REGION_TYPE_BARRIER;
  }

  projection_status = cflow_function_projection_admit(
      native.function, native.abi, native.callable, CFLOW_OP_MAP,
      out_projection);
  if (projection_status != CFLOW_FUNCTION_PROJECTION_OK) {
    return TURBO_AGENT_CFLOW_REGION_CFLOW_REJECTED;
  }
  return TURBO_AGENT_CFLOW_REGION_OK;
}

static void region_prepared_clear(
    turbo_agent_cflow_region_prepared_t *prepared) {
  if (!prepared) return;
  free(prepared->tool_names);
  free(prepared->projections);
  memset(prepared, 0, sizeof(*prepared));
}

static turbo_agent_cflow_region_status_t region_prepare(
    const turbo_agent_executable_dag_t *dag,
    const turbo_agent_cflow_region_source_t *source,
    turbo_agent_cflow_region_prepared_t *prepared) {
  turbo_agent_cflow_region_status_t status =
      TURBO_AGENT_CFLOW_REGION_INVALID_ARGUMENT;
  size_t i;

  if (!prepared) return TURBO_AGENT_CFLOW_REGION_INVALID_ARGUMENT;
  memset(prepared, 0, sizeof(*prepared));

  if (!dag || !region_source_valid(source)) {
    return TURBO_AGENT_CFLOW_REGION_INVALID_ARGUMENT;
  }

  prepared->registry = turbo_agent_executable_dag_approved_tools(dag);
  if (!prepared->registry) return TURBO_AGENT_CFLOW_REGION_NOT_APPROVED;

  prepared->projections = (cflow_function_projection *)calloc(
      source->step_count, sizeof(*prepared->projections));
  prepared->tool_names = (const char **)calloc(
      source->step_count, sizeof(*prepared->tool_names));
  if (!prepared->projections || !prepared->tool_names) {
    status = TURBO_AGENT_CFLOW_REGION_OUT_OF_MEMORY;
    goto fail;
  }
  prepared->step_count = source->step_count;

  for (i = 0; i < source->step_count; ++i) {
    size_t prior;
    const turbo_agent_cflow_region_step_t *step = &source->steps[i];
    const char *tool_name =
        turbo_agent_executable_dag_step_tool_name(dag, step->dag_step_index);

    if (!tool_name || !tool_name[0]) {
      status = TURBO_AGENT_CFLOW_REGION_NOT_APPROVED;
      goto fail;
    }
    for (prior = 0; prior < i; ++prior) {
      if (source->steps[prior].dag_step_index == step->dag_step_index) {
        status = TURBO_AGENT_CFLOW_REGION_INVALID_ARGUMENT;
        goto fail;
      }
    }

    prepared->tool_names[i] = tool_name;
    status = region_admit_native_map(
        prepared->registry, tool_name, &prepared->projections[i]);
    if (status != TURBO_AGENT_CFLOW_REGION_OK) goto fail;

    if (i > 0u) {
      if (turbo_agent_tool_result_slot_compatibility(
              prepared->registry, prepared->tool_names[i - 1u], tool_name,
              step->consumer_property) != TURBO_AGENT_CONTRACT_COMPATIBLE) {
        status = TURBO_AGENT_CFLOW_REGION_LOGICAL_CONTRACT_BARRIER;
        goto fail;
      }
      if (!cmeta_type_equal(
              prepared->projections[i - 1u].output_type,
              prepared->projections[i].input_type)) {
        status = TURBO_AGENT_CFLOW_REGION_TYPE_BARRIER;
        goto fail;
      }
    }
  }

  return TURBO_AGENT_CFLOW_REGION_OK;

fail:
  region_prepared_clear(prepared);
  return status;
}

void turbo_agent_cflow_region_step_init(
    turbo_agent_cflow_region_step_t *step) {
  if (!step) return;
  memset(step, 0, sizeof(*step));
  step->struct_size = sizeof(*step);
  step->abi_version = TURBO_AGENT_CFLOW_REGION_STEP_ABI_VERSION;
  step->operator_kind = TURBO_AGENT_CFLOW_REGION_OPERATOR_MAP;
}

void turbo_agent_cflow_region_source_init(
    turbo_agent_cflow_region_source_t *source) {
  if (!source) return;
  memset(source, 0, sizeof(*source));
  source->struct_size = sizeof(*source);
  source->abi_version = TURBO_AGENT_CFLOW_REGION_SOURCE_ABI_VERSION;
}

turbo_agent_cflow_region_status_t turbo_agent_cflow_region_admit(
    const turbo_agent_executable_dag_t *dag,
    const turbo_agent_cflow_region_source_t *source) {
  turbo_agent_cflow_region_prepared_t prepared;
  turbo_agent_cflow_region_status_t status =
      region_prepare(dag, source, &prepared);
  region_prepared_clear(&prepared);
  return status;
}

turbo_agent_cflow_region_status_t turbo_agent_compile_cflow_region(
    const turbo_agent_executable_dag_t *dag,
    const turbo_agent_cflow_region_source_t *source,
    turbo_agent_cflow_region_plan_t **out_plan) {
  turbo_agent_cflow_region_prepared_t prepared;
  cflow_graph graph = {0};
  turbo_agent_cflow_region_plan_t *region = NULL;
  turbo_agent_cflow_region_status_t status;
  size_t i;
  int graph_initialized = 0;

  if (out_plan) *out_plan = NULL;
  if (!out_plan) return TURBO_AGENT_CFLOW_REGION_INVALID_ARGUMENT;

  status = region_prepare(dag, source, &prepared);
  if (status != TURBO_AGENT_CFLOW_REGION_OK) return status;

  cflow_graph_init(&graph, prepared.projections[0].input_type);
  graph_initialized = 1;
  if (graph.error) {
    status = TURBO_AGENT_CFLOW_REGION_CFLOW_REJECTED;
    goto done;
  }

  for (i = 0; i < prepared.step_count; ++i) {
    if (!cflow_graph_add_function_projection(
            &graph, &prepared.projections[i])) {
      status = TURBO_AGENT_CFLOW_REGION_CFLOW_REJECTED;
      goto done;
    }
  }

  region = (turbo_agent_cflow_region_plan_t *)calloc(1, sizeof(*region));
  if (!region) {
    status = TURBO_AGENT_CFLOW_REGION_OUT_OF_MEMORY;
    goto done;
  }
  if (!cflow_plan_compile_surface(&region->plan, &graph, NULL)) {
    status = TURBO_AGENT_CFLOW_REGION_CFLOW_REJECTED;
    goto done;
  }
  region->input_type = prepared.projections[0].input_type;
  region->output_type =
      prepared.projections[prepared.step_count - 1u].output_type;
  region->step_count = prepared.step_count;

  *out_plan = region;
  region = NULL;
  status = TURBO_AGENT_CFLOW_REGION_OK;

done:
  if (region) {
    cflow_plan_destroy(&region->plan);
    free(region);
  }
  if (graph_initialized) cflow_graph_destroy(&graph);
  region_prepared_clear(&prepared);
  return status;
}

void turbo_agent_cflow_region_plan_destroy(
    turbo_agent_cflow_region_plan_t *plan) {
  if (!plan) return;
  cflow_plan_destroy(&plan->plan);
  free(plan);
}

const cmeta_type_desc *turbo_agent_cflow_region_input_type(
    const turbo_agent_cflow_region_plan_t *plan) {
  return plan ? plan->input_type : NULL;
}

const cmeta_type_desc *turbo_agent_cflow_region_output_type(
    const turbo_agent_cflow_region_plan_t *plan) {
  return plan ? plan->output_type : NULL;
}

size_t turbo_agent_cflow_region_step_count(
    const turbo_agent_cflow_region_plan_t *plan) {
  return plan ? plan->step_count : 0u;
}

turbo_agent_cflow_region_status_t turbo_agent_cflow_region_eval_array(
    const turbo_agent_cflow_region_plan_t *plan,
    const void *inputs,
    size_t input_count,
    cflow_result *out_result) {
  if (!plan || !out_result || (input_count > 0u && !inputs)) {
    return TURBO_AGENT_CFLOW_REGION_INVALID_ARGUMENT;
  }
  memset(out_result, 0, sizeof(*out_result));
  if (!cflow_plan_eval_array(&plan->plan, inputs, input_count, out_result)) {
    return TURBO_AGENT_CFLOW_REGION_EXECUTION_FAILED;
  }
  return TURBO_AGENT_CFLOW_REGION_OK;
}

const char *turbo_agent_cflow_region_status_string(
    turbo_agent_cflow_region_status_t status) {
  switch (status) {
    case TURBO_AGENT_CFLOW_REGION_OK: return "ok";
    case TURBO_AGENT_CFLOW_REGION_INVALID_ARGUMENT: return "invalid_argument";
    case TURBO_AGENT_CFLOW_REGION_NOT_APPROVED: return "not_approved";
    case TURBO_AGENT_CFLOW_REGION_POLICY_BARRIER: return "policy_barrier";
    case TURBO_AGENT_CFLOW_REGION_LOGICAL_CONTRACT_BARRIER:
      return "logical_contract_barrier";
    case TURBO_AGENT_CFLOW_REGION_NATIVE_PROJECTION_BARRIER:
      return "native_projection_barrier";
    case TURBO_AGENT_CFLOW_REGION_OWNERSHIP_BARRIER:
      return "ownership_barrier";
    case TURBO_AGENT_CFLOW_REGION_TYPE_BARRIER: return "type_barrier";
    case TURBO_AGENT_CFLOW_REGION_CFLOW_REJECTED: return "cflow_rejected";
    case TURBO_AGENT_CFLOW_REGION_OUT_OF_MEMORY: return "out_of_memory";
    case TURBO_AGENT_CFLOW_REGION_EXECUTION_FAILED: return "execution_failed";
  }
  return "unknown";
}
