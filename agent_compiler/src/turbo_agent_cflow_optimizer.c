#include "turbo_agent_cflow_optimizer.h"

#include <stdlib.h>
#include <string.h>

typedef struct turbo_agent_cflow_optimizer_region_s {
  turbo_agent_cflow_region_plan_t *plan;
  size_t *dag_step_indices;
  size_t step_count;
} turbo_agent_cflow_optimizer_region_t;

struct turbo_agent_cflow_optimizer_result_s {
  const turbo_agent_executable_dag_t *dag;
  int enabled;
  turbo_agent_cflow_region_status_t *edge_statuses;
  size_t edge_count;
  turbo_agent_cflow_optimizer_region_t *regions;
  size_t region_count;
  size_t region_capacity;
};

static int optimizer_options_valid(
    const turbo_agent_cflow_optimizer_options_t *options) {
  return options &&
         options->struct_size >= sizeof(*options) &&
         options->abi_version == TURBO_AGENT_CFLOW_OPTIMIZER_OPTIONS_ABI_VERSION &&
         (options->enabled == 0 || options->enabled == 1);
}

static int optimizer_source_valid(
    const turbo_agent_executable_dag_t *dag,
    const turbo_agent_cflow_composition_source_t *source) {
  size_t i;
  size_t dag_steps;

  if (!dag || !source ||
      source->struct_size < sizeof(*source) ||
      source->abi_version != TURBO_AGENT_CFLOW_COMPOSITION_ABI_VERSION ||
      (source->edge_count > 0u && !source->edges)) {
    return 0;
  }

  dag_steps = turbo_agent_executable_dag_step_count(dag);
  if (source->edge_count > (dag_steps ? dag_steps - 1u : 0u)) return 0;
  for (i = 0; i < source->edge_count; ++i) {
    const turbo_agent_cflow_value_edge_t *edge = &source->edges[i];
    size_t prior;

    if (edge->struct_size < sizeof(*edge) ||
        edge->abi_version != TURBO_AGENT_CFLOW_VALUE_EDGE_ABI_VERSION ||
        edge->operator_kind != TURBO_AGENT_CFLOW_REGION_OPERATOR_MAP ||
        !edge->consumer_property || !edge->consumer_property[0] ||
        edge->producer_step_index >= dag_steps ||
        edge->consumer_step_index >= dag_steps ||
        edge->producer_step_index == edge->consumer_step_index) {
      return 0;
    }

    if (i > 0u &&
        source->edges[i - 1u].consumer_step_index !=
            edge->producer_step_index) {
      return 0;
    }

    /*
     * The initial composition source is one simple linear chain. Reject a
     * repeated DAG step rather than silently accepting a cycle/re-entry.
     */
    for (prior = 0; prior < i; ++prior) {
      if (source->edges[prior].producer_step_index ==
              edge->consumer_step_index ||
          source->edges[prior].consumer_step_index ==
              edge->consumer_step_index) {
        return 0;
      }
    }
  }
  return 1;
}

static void optimizer_region_clear(
    turbo_agent_cflow_optimizer_region_t *region) {
  if (!region) return;
  turbo_agent_cflow_region_plan_destroy(region->plan);
  free(region->dag_step_indices);
  memset(region, 0, sizeof(*region));
}

static void optimizer_result_clear(
    turbo_agent_cflow_optimizer_result_t *result) {
  size_t i;
  if (!result) return;
  for (i = 0; i < result->region_count; ++i) {
    optimizer_region_clear(&result->regions[i]);
  }
  free(result->regions);
  free(result->edge_statuses);
  memset(result, 0, sizeof(*result));
}

static turbo_agent_cflow_region_status_t optimizer_admit_edge(
    const turbo_agent_executable_dag_t *dag,
    const turbo_agent_cflow_value_edge_t *edge) {
  turbo_agent_cflow_region_step_t steps[2];
  turbo_agent_cflow_region_source_t region_source;

  turbo_agent_cflow_region_step_init(&steps[0]);
  steps[0].dag_step_index = edge->producer_step_index;
  steps[0].operator_kind = edge->operator_kind;

  turbo_agent_cflow_region_step_init(&steps[1]);
  steps[1].dag_step_index = edge->consumer_step_index;
  steps[1].operator_kind = edge->operator_kind;
  steps[1].consumer_property = edge->consumer_property;

  turbo_agent_cflow_region_source_init(&region_source);
  region_source.steps = steps;
  region_source.step_count = 2u;

  return turbo_agent_cflow_region_admit(dag, &region_source);
}

static turbo_agent_cflow_optimizer_status_t optimizer_reserve_region(
    turbo_agent_cflow_optimizer_result_t *result) {
  turbo_agent_cflow_optimizer_region_t *grown;
  size_t next_capacity;

  if (result->region_count < result->region_capacity) {
    return TURBO_AGENT_CFLOW_OPTIMIZER_OK;
  }

  next_capacity = result->region_capacity ? result->region_capacity * 2u : 2u;
  if (next_capacity < result->region_capacity ||
      next_capacity > SIZE_MAX / sizeof(*result->regions)) {
    return TURBO_AGENT_CFLOW_OPTIMIZER_OUT_OF_MEMORY;
  }

  grown = (turbo_agent_cflow_optimizer_region_t *)realloc(
      result->regions, next_capacity * sizeof(*result->regions));
  if (!grown) return TURBO_AGENT_CFLOW_OPTIMIZER_OUT_OF_MEMORY;

  memset(
      grown + result->region_capacity, 0,
      (next_capacity - result->region_capacity) * sizeof(*grown));
  result->regions = grown;
  result->region_capacity = next_capacity;
  return TURBO_AGENT_CFLOW_OPTIMIZER_OK;
}

static turbo_agent_cflow_optimizer_status_t optimizer_compile_region(
    turbo_agent_cflow_optimizer_result_t *result,
    const turbo_agent_cflow_composition_source_t *source,
    size_t first_edge,
    size_t last_edge) {
  turbo_agent_cflow_optimizer_region_t *region;
  turbo_agent_cflow_region_step_t *steps = NULL;
  turbo_agent_cflow_region_source_t region_source;
  turbo_agent_cflow_region_status_t region_status;
  turbo_agent_cflow_optimizer_status_t status;
  size_t edge_count;
  size_t step_count;
  size_t i;

  if (last_edge < first_edge) {
    return TURBO_AGENT_CFLOW_OPTIMIZER_INVALID_ARGUMENT;
  }

  edge_count = last_edge - first_edge + 1u;
  step_count = edge_count + 1u;

  status = optimizer_reserve_region(result);
  if (status != TURBO_AGENT_CFLOW_OPTIMIZER_OK) return status;

  region = &result->regions[result->region_count];
  region->dag_step_indices =
      (size_t *)calloc(step_count, sizeof(*region->dag_step_indices));
  steps = (turbo_agent_cflow_region_step_t *)calloc(
      step_count, sizeof(*steps));
  if (!region->dag_step_indices || !steps) {
    free(steps);
    optimizer_region_clear(region);
    return TURBO_AGENT_CFLOW_OPTIMIZER_OUT_OF_MEMORY;
  }

  turbo_agent_cflow_region_step_init(&steps[0]);
  steps[0].dag_step_index = source->edges[first_edge].producer_step_index;
  steps[0].operator_kind = source->edges[first_edge].operator_kind;
  region->dag_step_indices[0] = steps[0].dag_step_index;

  for (i = 0; i < edge_count; ++i) {
    const turbo_agent_cflow_value_edge_t *edge =
        &source->edges[first_edge + i];
    turbo_agent_cflow_region_step_init(&steps[i + 1u]);
    steps[i + 1u].dag_step_index = edge->consumer_step_index;
    steps[i + 1u].operator_kind = edge->operator_kind;
    steps[i + 1u].consumer_property = edge->consumer_property;
    region->dag_step_indices[i + 1u] = edge->consumer_step_index;
  }

  turbo_agent_cflow_region_source_init(&region_source);
  region_source.steps = steps;
  region_source.step_count = step_count;

  region_status = turbo_agent_compile_cflow_region(
      result->dag, &region_source, &region->plan);
  free(steps);

  if (region_status != TURBO_AGENT_CFLOW_REGION_OK || !region->plan) {
    optimizer_region_clear(region);
    return TURBO_AGENT_CFLOW_OPTIMIZER_REGION_COMPILE_FAILED;
  }

  region->step_count = step_count;
  ++result->region_count;
  return TURBO_AGENT_CFLOW_OPTIMIZER_OK;
}

void turbo_agent_cflow_value_edge_init(
    turbo_agent_cflow_value_edge_t *edge) {
  if (!edge) return;
  memset(edge, 0, sizeof(*edge));
  edge->struct_size = sizeof(*edge);
  edge->abi_version = TURBO_AGENT_CFLOW_VALUE_EDGE_ABI_VERSION;
  edge->operator_kind = TURBO_AGENT_CFLOW_REGION_OPERATOR_MAP;
}

void turbo_agent_cflow_composition_source_init(
    turbo_agent_cflow_composition_source_t *source) {
  if (!source) return;
  memset(source, 0, sizeof(*source));
  source->struct_size = sizeof(*source);
  source->abi_version = TURBO_AGENT_CFLOW_COMPOSITION_ABI_VERSION;
}

void turbo_agent_cflow_optimizer_options_init(
    turbo_agent_cflow_optimizer_options_t *options) {
  if (!options) return;
  memset(options, 0, sizeof(*options));
  options->struct_size = sizeof(*options);
  options->abi_version = TURBO_AGENT_CFLOW_OPTIMIZER_OPTIONS_ABI_VERSION;
  options->enabled = 1;
}

turbo_agent_cflow_optimizer_status_t turbo_agent_cflow_optimizer_discover(
    const turbo_agent_executable_dag_t *dag,
    const turbo_agent_cflow_composition_source_t *source,
    const turbo_agent_cflow_optimizer_options_t *options,
    turbo_agent_cflow_optimizer_result_t **out_result) {
  turbo_agent_cflow_optimizer_result_t *result = NULL;
  turbo_agent_cflow_optimizer_status_t status =
      TURBO_AGENT_CFLOW_OPTIMIZER_INVALID_ARGUMENT;
  size_t i;

  if (out_result) *out_result = NULL;
  if (!out_result || !optimizer_options_valid(options) ||
      !optimizer_source_valid(dag, source)) {
    return TURBO_AGENT_CFLOW_OPTIMIZER_INVALID_ARGUMENT;
  }

  result = (turbo_agent_cflow_optimizer_result_t *)calloc(1, sizeof(*result));
  if (!result) return TURBO_AGENT_CFLOW_OPTIMIZER_OUT_OF_MEMORY;

  result->dag = dag;
  result->enabled = options->enabled;
  result->edge_count = source->edge_count;

  if (source->edge_count > 0u) {
    result->edge_statuses = (turbo_agent_cflow_region_status_t *)malloc(
        source->edge_count * sizeof(*result->edge_statuses));
    if (!result->edge_statuses) {
      status = TURBO_AGENT_CFLOW_OPTIMIZER_OUT_OF_MEMORY;
      goto fail;
    }
    for (i = 0; i < source->edge_count; ++i) {
      result->edge_statuses[i] = TURBO_AGENT_CFLOW_REGION_INVALID_ARGUMENT;
    }
  }

  if (!result->enabled) {
    *out_result = result;
    return TURBO_AGENT_CFLOW_OPTIMIZER_OK;
  }

  for (i = 0; i < source->edge_count; ++i) {
    result->edge_statuses[i] = optimizer_admit_edge(dag, &source->edges[i]);
  }

  i = 0u;
  while (i < source->edge_count) {
    size_t first;
    size_t last;

    while (i < source->edge_count &&
           result->edge_statuses[i] != TURBO_AGENT_CFLOW_REGION_OK) {
      ++i;
    }
    if (i >= source->edge_count) break;

    first = i;
    while (i < source->edge_count &&
           result->edge_statuses[i] == TURBO_AGENT_CFLOW_REGION_OK) {
      ++i;
    }
    last = i - 1u;

    status = optimizer_compile_region(result, source, first, last);
    if (status != TURBO_AGENT_CFLOW_OPTIMIZER_OK) goto fail;
  }

  *out_result = result;
  return TURBO_AGENT_CFLOW_OPTIMIZER_OK;

fail:
  optimizer_result_clear(result);
  free(result);
  return status;
}

void turbo_agent_cflow_optimizer_result_destroy(
    turbo_agent_cflow_optimizer_result_t *result) {
  if (!result) return;
  optimizer_result_clear(result);
  free(result);
}

int turbo_agent_cflow_optimizer_enabled(
    const turbo_agent_cflow_optimizer_result_t *result) {
  return result ? result->enabled : 0;
}

size_t turbo_agent_cflow_optimizer_region_count(
    const turbo_agent_cflow_optimizer_result_t *result) {
  return result ? result->region_count : 0u;
}

size_t turbo_agent_cflow_optimizer_region_step_count(
    const turbo_agent_cflow_optimizer_result_t *result,
    size_t region_index) {
  if (!result || region_index >= result->region_count) return 0u;
  return result->regions[region_index].step_count;
}

size_t turbo_agent_cflow_optimizer_region_dag_step_index(
    const turbo_agent_cflow_optimizer_result_t *result,
    size_t region_index,
    size_t position) {
  if (!result || region_index >= result->region_count ||
      position >= result->regions[region_index].step_count) {
    return SIZE_MAX;
  }
  return result->regions[region_index].dag_step_indices[position];
}

const char *turbo_agent_cflow_optimizer_region_tool_name(
    const turbo_agent_cflow_optimizer_result_t *result,
    size_t region_index,
    size_t position) {
  size_t step_index =
      turbo_agent_cflow_optimizer_region_dag_step_index(
          result, region_index, position);
  if (!result || step_index == SIZE_MAX) return NULL;
  return turbo_agent_executable_dag_step_tool_name(result->dag, step_index);
}

const turbo_agent_cflow_region_plan_t *
turbo_agent_cflow_optimizer_region_plan(
    const turbo_agent_cflow_optimizer_result_t *result,
    size_t region_index) {
  if (!result || region_index >= result->region_count) return NULL;
  return result->regions[region_index].plan;
}

turbo_agent_cflow_region_status_t
turbo_agent_cflow_optimizer_edge_status(
    const turbo_agent_cflow_optimizer_result_t *result,
    size_t edge_index) {
  if (!result || !result->enabled || edge_index >= result->edge_count ||
      !result->edge_statuses) {
    return TURBO_AGENT_CFLOW_REGION_INVALID_ARGUMENT;
  }
  return result->edge_statuses[edge_index];
}

const char *turbo_agent_cflow_optimizer_status_string(
    turbo_agent_cflow_optimizer_status_t status) {
  switch (status) {
    case TURBO_AGENT_CFLOW_OPTIMIZER_OK: return "ok";
    case TURBO_AGENT_CFLOW_OPTIMIZER_INVALID_ARGUMENT:
      return "invalid_argument";
    case TURBO_AGENT_CFLOW_OPTIMIZER_OUT_OF_MEMORY:
      return "out_of_memory";
    case TURBO_AGENT_CFLOW_OPTIMIZER_REGION_COMPILE_FAILED:
      return "region_compile_failed";
  }
  return "unknown";
}
