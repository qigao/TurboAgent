#include "tinytest.h"

#include "turbo_agent_cflow_region.h"
#include "turbo_runtime_json.h"

#include <cflow/function_projection.h>
#include <string.h>

FunctionDeclResult(
    value, int, CMETA_RESULT_VALUE, region_add_one,
    (int, value, CMETA_PARAM_IN));

int region_add_one(int value) {
  return value + 1;
}

CFLOW_REFLECTED_ADAPTER(region_add_one);

FunctionDeclResult(
    value, int, CMETA_RESULT_VALUE, region_times_two,
    (int, value, CMETA_PARAM_IN));

int region_times_two(int value) {
  return value * 2;
}

CFLOW_REFLECTED_ADAPTER(region_times_two);

static turbo_tool_status_t region_add_handler(
    const json_value_t *arguments,
    const turbo_tool_execution_context_t *context,
    json_value_t **out_result,
    void *user_data) {
  int value;
  (void)context;
  (void)user_data;
  if (!arguments || !out_result) return TURBO_TOOL_INVALID_ARGUMENT;
  value = json_get_int(arguments, "value", 0);
  *out_result = json_create_int64(region_add_one(value));
  return *out_result ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static turbo_tool_status_t region_times_handler(
    const json_value_t *arguments,
    const turbo_tool_execution_context_t *context,
    json_value_t **out_result,
    void *user_data) {
  int value;
  (void)context;
  (void)user_data;
  if (!arguments || !out_result) return TURBO_TOOL_INVALID_ARGUMENT;
  value = json_get_int(arguments, "value", 0);
  *out_result = json_create_int64(region_times_two(value));
  return *out_result ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static int region_add_tool(
    turbo_tool_registry_t *registry,
    const char *name,
    turbo_tool_json_value_context_handler_fn handler,
    const cmeta_function_desc *function,
    const cmeta_function_abi_desc *abi,
    cmeta_callable callable) {
  static const char *caps[] = {"runtime_tools"};
  turbo_tool_definition_v6_t definition = {0};
  turbo_tool_native_projection_t native = {0};

  definition.struct_size = sizeof(definition);
  definition.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
  definition.definition.name = name;
  definition.definition.description = "Static native CFlow test tool.";
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

  native.struct_size = sizeof(native);
  native.abi_version = TURBO_TOOL_NATIVE_PROJECTION_ABI_VERSION;
  native.function = function;
  native.abi = abi;
  native.callable = callable;
  return turbo_tool_registry_publish_native_projection(
             registry, name, &native) == TURBO_TOOL_OK;
}

static turbo_tool_registry_t *region_registry(void) {
  turbo_tool_registry_t *registry = turbo_tool_registry_create();
  if (!registry) return NULL;
  if (!region_add_tool(
          registry, "math.add_one", region_add_handler,
          FunctionMeta(region_add_one), FunctionAbi(region_add_one),
          CFLOW_REFLECTED_CALLABLE(region_add_one)) ||
      !region_add_tool(
          registry, "math.times_two", region_times_handler,
          FunctionMeta(region_times_two), FunctionAbi(region_times_two),
          CFLOW_REFLECTED_CALLABLE(region_times_two))) {
    turbo_tool_registry_destroy(registry);
    return NULL;
  }
  return registry;
}

static turbo_agent_executable_dag_t *region_dag(
    const turbo_tool_registry_t *registry) {
  static const char *allowed[] = {"runtime_tools"};
  turbo_agent_compiler_config_t config;
  turbo_agent_dag_step_source_t steps[2];
  turbo_agent_dag_source_t source;
  turbo_agent_executable_dag_t *dag = NULL;
  turbo_agent_compile_diagnostic_t diagnostic;
  json_value_t *args0 = json_parse("{}", 2u);
  json_value_t *args1 = json_parse("{}", 2u);

  if (!args0 || !args1) {
    turbo_runtime_json_destroy(args0);
    turbo_runtime_json_destroy(args1);
    return NULL;
  }

  turbo_agent_compiler_config_init(&config);
  config.allowed_capabilities = allowed;
  config.allowed_capability_count = 1u;

  turbo_agent_dag_step_source_init(&steps[0]);
  steps[0].step_id = "first";
  steps[0].tool_name = "math.add_one";
  steps[0].arguments = args0;

  turbo_agent_dag_step_source_init(&steps[1]);
  steps[1].step_id = "second";
  steps[1].tool_name = "math.times_two";
  steps[1].arguments = args1;

  /* Intentionally no depends_on edge: Phase-3 ordering is not a value edge. */
  turbo_agent_dag_source_init(&source);
  source.steps = steps;
  source.step_count = 2u;

  if (turbo_agent_compile_dag(
          &config, registry, &source, &dag, &diagnostic) !=
      TURBO_AGENT_COMPILE_OK) {
    dag = NULL;
  }

  turbo_runtime_json_destroy(args0);
  turbo_runtime_json_destroy(args1);
  return dag;
}

static int region_baseline(
    const turbo_tool_registry_t *registry, int input, int *out) {
  json_value_t *args = json_create_object();
  json_value_t *result1 = NULL;
  json_value_t *result2 = NULL;
  int first;
  int second;

  if (!args ||
      turbo_runtime_json_object_set(
          args, "value", json_create_int64(input)) != TURBO_RUNTIME_JSON_OK ||
      turbo_tool_registry_execute_json_value(
          registry, "math.add_one", args, &result1) != TURBO_TOOL_OK ||
      !result1) {
    turbo_runtime_json_destroy(result1);
    turbo_runtime_json_destroy(args);
    return 0;
  }
  first = (int)turbo_runtime_json_value_as_int64(result1, 0);

  turbo_runtime_json_destroy(args);
  args = json_create_object();
  if (!args ||
      turbo_runtime_json_object_set(
          args, "value", json_create_int64(first)) != TURBO_RUNTIME_JSON_OK ||
      turbo_tool_registry_execute_json_value(
          registry, "math.times_two", args, &result2) != TURBO_TOOL_OK ||
      !result2) {
    turbo_runtime_json_destroy(result2);
    turbo_runtime_json_destroy(result1);
    turbo_runtime_json_destroy(args);
    return 0;
  }
  second = (int)turbo_runtime_json_value_as_int64(result2, 0);
  turbo_runtime_json_destroy(result2);
  turbo_runtime_json_destroy(result1);
  turbo_runtime_json_destroy(args);
  *out = second;
  return 1;
}

suite("AgentCompiler static native CFlow region") {
  it("lowers explicit MAP value edges without using depends_on") {
    turbo_tool_registry_t *registry = region_registry();
    turbo_agent_executable_dag_t *dag = region_dag(registry);
    turbo_agent_cflow_region_step_t steps[2];
    turbo_agent_cflow_region_source_t source;
    turbo_agent_cflow_region_plan_t *plan = NULL;
    cflow_result result = {0};
    int inputs[] = {3, 4};
    int baseline0 = 0;
    int baseline1 = 0;
    const int *values;

    check_not_null(registry);
    check_not_null(dag);

    turbo_agent_cflow_region_step_init(&steps[0]);
    steps[0].dag_step_index = 0u;

    turbo_agent_cflow_region_step_init(&steps[1]);
    steps[1].dag_step_index = 1u;
    steps[1].consumer_property = "value";

    turbo_agent_cflow_region_source_init(&source);
    source.steps = steps;
    source.step_count = 2u;

    check_equal(
        turbo_agent_compile_cflow_region(dag, &source, &plan),
        TURBO_AGENT_CFLOW_REGION_OK);
    check_not_null(plan);
    check_true(cmeta_type_equal(
        turbo_agent_cflow_region_input_type(plan), &cmeta_type_int));
    check_true(cmeta_type_equal(
        turbo_agent_cflow_region_output_type(plan), &cmeta_type_int));
    check_equal(turbo_agent_cflow_region_step_count(plan), (size_t)2u);

    check_equal(
        turbo_agent_cflow_region_eval_array(
            plan, inputs, 2u, &result),
        TURBO_AGENT_CFLOW_REGION_OK);
    check_equal(result.count, (size_t)2u);
    check_true(cmeta_type_equal(result.type, &cmeta_type_int));
    values = (const int *)result.data;

    check_true(region_baseline(registry, inputs[0], &baseline0));
    check_true(region_baseline(registry, inputs[1], &baseline1));
    check_equal(values[0], baseline0);
    check_equal(values[1], baseline1);
    check_equal(values[0], 8);
    check_equal(values[1], 10);

    cflow_result_destroy(&result);
    turbo_agent_cflow_region_plan_destroy(plan);
    turbo_agent_executable_dag_destroy(dag);
    turbo_tool_registry_destroy(registry);
  }

  it("fails closed when an explicit logical value edge is missing") {
    turbo_tool_registry_t *registry = region_registry();
    turbo_agent_executable_dag_t *dag = region_dag(registry);
    turbo_agent_cflow_region_step_t steps[2];
    turbo_agent_cflow_region_source_t source;
    turbo_agent_cflow_region_plan_t *plan = NULL;

    check_not_null(registry);
    check_not_null(dag);

    turbo_agent_cflow_region_step_init(&steps[0]);
    steps[0].dag_step_index = 0u;
    turbo_agent_cflow_region_step_init(&steps[1]);
    steps[1].dag_step_index = 1u;

    turbo_agent_cflow_region_source_init(&source);
    source.steps = steps;
    source.step_count = 2u;

    check_equal(
        turbo_agent_compile_cflow_region(dag, &source, &plan),
        TURBO_AGENT_CFLOW_REGION_INVALID_ARGUMENT);
    check_null(plan);

    turbo_agent_executable_dag_destroy(dag);
    turbo_tool_registry_destroy(registry);
  }
}
