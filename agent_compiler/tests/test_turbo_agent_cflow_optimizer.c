#include "tinytest.h"

#include "turbo_agent_cflow_optimizer.h"
#include "turbo_runtime_json.h"

#include <cflow/function_projection.h>
#include <string.h>

FunctionDeclResult(
    value, int, CMETA_RESULT_VALUE, optimizer_add_one,
    (int, value, CMETA_PARAM_IN));

int optimizer_add_one(int value) {
  return value + 1;
}

CFLOW_REFLECTED_ADAPTER(optimizer_add_one);

FunctionDeclResult(
    value, int, CMETA_RESULT_VALUE, optimizer_times_two,
    (int, value, CMETA_PARAM_IN));

int optimizer_times_two(int value) {
  return value * 2;
}

CFLOW_REFLECTED_ADAPTER(optimizer_times_two);

static turbo_tool_status_t optimizer_add_handler(
    const json_value_t *arguments,
    const turbo_tool_execution_context_t *context,
    json_value_t **out_result,
    void *user_data) {
  int value;
  (void)context;
  (void)user_data;
  if (!arguments || !out_result) return TURBO_TOOL_INVALID_ARGUMENT;
  value = json_get_int(arguments, "value", 0);
  *out_result = json_create_int64(optimizer_add_one(value));
  return *out_result ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static turbo_tool_status_t optimizer_times_handler(
    const json_value_t *arguments,
    const turbo_tool_execution_context_t *context,
    json_value_t **out_result,
    void *user_data) {
  int value;
  (void)context;
  (void)user_data;
  if (!arguments || !out_result) return TURBO_TOOL_INVALID_ARGUMENT;
  value = json_get_int(arguments, "value", 0);
  *out_result = json_create_int64(optimizer_times_two(value));
  return *out_result ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static turbo_tool_status_t optimizer_barrier_handler(
    const json_value_t *arguments,
    const turbo_tool_execution_context_t *context,
    json_value_t **out_result,
    void *user_data) {
  int value;
  (void)context;
  (void)user_data;
  if (!arguments || !out_result) return TURBO_TOOL_INVALID_ARGUMENT;
  value = json_get_int(arguments, "value", 0);
  *out_result = json_create_int64(value);
  return *out_result ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static int optimizer_add_definition(
    turbo_tool_registry_t *registry,
    const char *name,
    turbo_tool_json_value_context_handler_fn handler,
    const cmeta_function_desc *function,
    const cmeta_function_abi_desc *abi,
    cmeta_callable callable,
    int publish_native) {
  static const char *caps[] = {"runtime_tools"};
  turbo_tool_definition_v6_t definition = {0};
  turbo_tool_native_projection_t native = {0};

  definition.struct_size = sizeof(definition);
  definition.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
  definition.definition.name = name;
  definition.definition.description = "Phase-4E optimizer fixture.";
  definition.definition.parameters_json =
      "{\"type\":\"object\",\"properties\":{"
      "\"value\":{\"type\":\"integer\"}},"
      "\"additionalProperties\":false}";
  definition.definition.strict = 1;
  definition.execution_policy.mode = TURBO_TOOL_EXECUTION_PARALLEL_SAFE;
  definition.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
  definition.required_capabilities = caps;
  definition.required_capability_count = 1u;
  definition.json_value_context_handler = handler;
  definition.result_schema_json = "{\"type\":\"integer\"}";
  definition.strict_result = 1;
  definition.effect_flags = TURBO_TOOL_EFFECT_PURE;

  if (turbo_tool_registry_add_v6(registry, &definition) != TURBO_TOOL_OK) {
    return 0;
  }
  if (!publish_native) return 1;

  native.struct_size = sizeof(native);
  native.abi_version = TURBO_TOOL_NATIVE_PROJECTION_ABI_VERSION;
  native.function = function;
  native.abi = abi;
  native.callable = callable;
  return turbo_tool_registry_publish_native_projection(
             registry, name, &native) == TURBO_TOOL_OK;
}

static turbo_tool_registry_t *optimizer_registry(void) {
  turbo_tool_registry_t *registry = turbo_tool_registry_create();
  if (!registry) return NULL;

  if (!optimizer_add_definition(
          registry, "chain.a", optimizer_add_handler,
          FunctionMeta(optimizer_add_one), FunctionAbi(optimizer_add_one),
          CFLOW_REFLECTED_CALLABLE(optimizer_add_one), 1) ||
      !optimizer_add_definition(
          registry, "chain.b", optimizer_times_handler,
          FunctionMeta(optimizer_times_two), FunctionAbi(optimizer_times_two),
          CFLOW_REFLECTED_CALLABLE(optimizer_times_two), 1) ||
      !optimizer_add_definition(
          registry, "chain.barrier", optimizer_barrier_handler,
          NULL, NULL, (cmeta_callable){0}, 0) ||
      !optimizer_add_definition(
          registry, "chain.d", optimizer_add_handler,
          FunctionMeta(optimizer_add_one), FunctionAbi(optimizer_add_one),
          CFLOW_REFLECTED_CALLABLE(optimizer_add_one), 1) ||
      !optimizer_add_definition(
          registry, "chain.e", optimizer_times_handler,
          FunctionMeta(optimizer_times_two), FunctionAbi(optimizer_times_two),
          CFLOW_REFLECTED_CALLABLE(optimizer_times_two), 1)) {
    turbo_tool_registry_destroy(registry);
    return NULL;
  }
  return registry;
}

static turbo_agent_executable_dag_t *optimizer_dag(
    const turbo_tool_registry_t *registry) {
  static const char *allowed[] = {"runtime_tools"};
  static const char *names[] = {
      "chain.a", "chain.b", "chain.barrier", "chain.d", "chain.e"};
  turbo_agent_compiler_config_t config;
  turbo_agent_dag_step_source_t steps[5];
  turbo_agent_dag_source_t source;
  turbo_agent_executable_dag_t *dag = NULL;
  turbo_agent_compile_diagnostic_t diagnostic;
  json_value_t *args[5] = {0};
  size_t i;

  turbo_agent_compiler_config_init(&config);
  config.allowed_capabilities = allowed;
  config.allowed_capability_count = 1u;

  for (i = 0; i < 5u; ++i) {
    args[i] = json_parse("{}", 2u);
    if (!args[i]) goto done;
    turbo_agent_dag_step_source_init(&steps[i]);
    steps[i].step_id = names[i];
    steps[i].tool_name = names[i];
    steps[i].arguments = args[i];
  }

  turbo_agent_dag_source_init(&source);
  source.steps = steps;
  source.step_count = 5u;

  if (turbo_agent_compile_dag(
          &config, registry, &source, &dag, &diagnostic) !=
      TURBO_AGENT_COMPILE_OK) {
    dag = NULL;
  }

done:
  for (i = 0; i < 5u; ++i) turbo_runtime_json_destroy(args[i]);
  return dag;
}

static void optimizer_chain_source(
    turbo_agent_cflow_value_edge_t edges[4],
    turbo_agent_cflow_composition_source_t *source) {
  size_t i;
  for (i = 0; i < 4u; ++i) {
    turbo_agent_cflow_value_edge_init(&edges[i]);
    edges[i].producer_step_index = i;
    edges[i].consumer_step_index = i + 1u;
    edges[i].consumer_property = "value";
  }
  turbo_agent_cflow_composition_source_init(source);
  source->edges = edges;
  source->edge_count = 4u;
}

static int optimizer_baseline_pair(
    const turbo_tool_registry_t *registry,
    const char *first_tool,
    const char *second_tool,
    int input,
    int *out) {
  json_value_t *args = NULL;
  json_value_t *first = NULL;
  json_value_t *second = NULL;
  int intermediate;

  args = json_create_object();
  if (!args ||
      turbo_runtime_json_object_set(
          args, "value", json_create_int64(input)) != TURBO_RUNTIME_JSON_OK ||
      turbo_tool_registry_execute_json_value(
          registry, first_tool, args, &first) != TURBO_TOOL_OK ||
      !first) {
    goto fail;
  }
  intermediate =
      (int)turbo_runtime_json_value_as_int64(first, 0);

  turbo_runtime_json_destroy(args);
  args = json_create_object();
  if (!args ||
      turbo_runtime_json_object_set(
          args, "value", json_create_int64(intermediate)) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_tool_registry_execute_json_value(
          registry, second_tool, args, &second) != TURBO_TOOL_OK ||
      !second) {
    goto fail;
  }

  *out = (int)turbo_runtime_json_value_as_int64(second, 0);
  turbo_runtime_json_destroy(second);
  turbo_runtime_json_destroy(first);
  turbo_runtime_json_destroy(args);
  return 1;

fail:
  turbo_runtime_json_destroy(second);
  turbo_runtime_json_destroy(first);
  turbo_runtime_json_destroy(args);
  return 0;
}

suite("AgentCompiler CFlow optimizer region discovery") {
  it("partitions one explicit chain into maximal eligible regions") {
    turbo_tool_registry_t *registry = optimizer_registry();
    turbo_agent_executable_dag_t *dag = optimizer_dag(registry);
    turbo_agent_cflow_value_edge_t edges[4];
    turbo_agent_cflow_composition_source_t source;
    turbo_agent_cflow_optimizer_options_t options;
    turbo_agent_cflow_optimizer_result_t *result = NULL;
    const turbo_agent_cflow_region_plan_t *plan;
    cflow_result optimized = {0};
    int input = 3;
    int baseline = 0;

    check_not_null(registry);
    check_not_null(dag);
    optimizer_chain_source(edges, &source);
    turbo_agent_cflow_optimizer_options_init(&options);

    check_equal(
        turbo_agent_cflow_optimizer_discover(
            dag, &source, &options, &result),
        TURBO_AGENT_CFLOW_OPTIMIZER_OK);
    check_not_null(result);
    check_true(turbo_agent_cflow_optimizer_enabled(result));
    check_equal(turbo_agent_cflow_optimizer_region_count(result), (size_t)2u);

    check_equal(
        turbo_agent_cflow_optimizer_edge_status(result, 0u),
        TURBO_AGENT_CFLOW_REGION_OK);
    check_equal(
        turbo_agent_cflow_optimizer_edge_status(result, 1u),
        TURBO_AGENT_CFLOW_REGION_NATIVE_PROJECTION_BARRIER);
    check_equal(
        turbo_agent_cflow_optimizer_edge_status(result, 2u),
        TURBO_AGENT_CFLOW_REGION_NATIVE_PROJECTION_BARRIER);
    check_equal(
        turbo_agent_cflow_optimizer_edge_status(result, 3u),
        TURBO_AGENT_CFLOW_REGION_OK);

    check_equal(
        turbo_agent_cflow_optimizer_region_step_count(result, 0u),
        (size_t)2u);
    check_equal(
        turbo_agent_cflow_optimizer_region_dag_step_index(result, 0u, 0u),
        (size_t)0u);
    check_equal(
        turbo_agent_cflow_optimizer_region_dag_step_index(result, 0u, 1u),
        (size_t)1u);
    check_equal(
        turbo_agent_cflow_optimizer_region_tool_name(result, 0u, 0u),
        "chain.a");
    check_equal(
        turbo_agent_cflow_optimizer_region_tool_name(result, 0u, 1u),
        "chain.b");

    check_equal(
        turbo_agent_cflow_optimizer_region_step_count(result, 1u),
        (size_t)2u);
    check_equal(
        turbo_agent_cflow_optimizer_region_dag_step_index(result, 1u, 0u),
        (size_t)3u);
    check_equal(
        turbo_agent_cflow_optimizer_region_dag_step_index(result, 1u, 1u),
        (size_t)4u);

    plan = turbo_agent_cflow_optimizer_region_plan(result, 0u);
    check_not_null(plan);
    check_equal(
        turbo_agent_cflow_region_eval_array(
            plan, &input, 1u, &optimized),
        TURBO_AGENT_CFLOW_REGION_OK);
    check_equal(optimized.count, (size_t)1u);
    check_true(optimizer_baseline_pair(
        registry, "chain.a", "chain.b", input, &baseline));
    check_equal(((const int *)optimized.data)[0], baseline);
    check_equal(baseline, 8);

    cflow_result_destroy(&optimized);
    turbo_agent_cflow_optimizer_result_destroy(result);
    turbo_agent_executable_dag_destroy(dag);
    turbo_tool_registry_destroy(registry);
  }

  it("returns zero selected regions when optimization is disabled") {
    turbo_tool_registry_t *registry = optimizer_registry();
    turbo_agent_executable_dag_t *dag = optimizer_dag(registry);
    turbo_agent_cflow_value_edge_t edges[4];
    turbo_agent_cflow_composition_source_t source;
    turbo_agent_cflow_optimizer_options_t options;
    turbo_agent_cflow_optimizer_result_t *result = NULL;

    check_not_null(registry);
    check_not_null(dag);
    optimizer_chain_source(edges, &source);
    turbo_agent_cflow_optimizer_options_init(&options);
    options.enabled = 0;

    check_equal(
        turbo_agent_cflow_optimizer_discover(
            dag, &source, &options, &result),
        TURBO_AGENT_CFLOW_OPTIMIZER_OK);
    check_not_null(result);
    check_false(turbo_agent_cflow_optimizer_enabled(result));
    check_equal(turbo_agent_cflow_optimizer_region_count(result), (size_t)0u);
    check_equal(
        turbo_agent_cflow_optimizer_edge_status(result, 0u),
        TURBO_AGENT_CFLOW_REGION_INVALID_ARGUMENT);

    turbo_agent_cflow_optimizer_result_destroy(result);
    turbo_agent_executable_dag_destroy(dag);
    turbo_tool_registry_destroy(registry);
  }

  it("rejects a non-linear composition source instead of inferring topology") {
    turbo_tool_registry_t *registry = optimizer_registry();
    turbo_agent_executable_dag_t *dag = optimizer_dag(registry);
    turbo_agent_cflow_value_edge_t edges[2];
    turbo_agent_cflow_composition_source_t source;
    turbo_agent_cflow_optimizer_options_t options;
    turbo_agent_cflow_optimizer_result_t *result = NULL;

    check_not_null(registry);
    check_not_null(dag);

    turbo_agent_cflow_value_edge_init(&edges[0]);
    edges[0].producer_step_index = 0u;
    edges[0].consumer_step_index = 1u;
    edges[0].consumer_property = "value";

    turbo_agent_cflow_value_edge_init(&edges[1]);
    edges[1].producer_step_index = 3u;
    edges[1].consumer_step_index = 4u;
    edges[1].consumer_property = "value";

    turbo_agent_cflow_composition_source_init(&source);
    source.edges = edges;
    source.edge_count = 2u;
    turbo_agent_cflow_optimizer_options_init(&options);

    check_equal(
        turbo_agent_cflow_optimizer_discover(
            dag, &source, &options, &result),
        TURBO_AGENT_CFLOW_OPTIMIZER_INVALID_ARGUMENT);
    check_null(result);

    turbo_agent_executable_dag_destroy(dag);
    turbo_tool_registry_destroy(registry);
  }
}
