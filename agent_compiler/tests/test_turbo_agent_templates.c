#include "tinytest.h"

#include "turbo_agent_templates.h"
#include "turbo_runtime_json.h"

#include <string.h>

typedef struct template_probe_s {
  int calls;
} template_probe_t;

static turbo_tool_status_t template_probe_handler(
    const json_value_t *arguments,
    const turbo_tool_execution_context_t *context,
    json_value_t **out_result,
    void *user_data) {
  template_probe_t *probe = (template_probe_t *)user_data;
  (void)context;
  if (!probe || !arguments || !out_result) return TURBO_TOOL_INVALID_ARGUMENT;
  ++probe->calls;
  *out_result = json_clone(arguments);
  return *out_result ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static int add_template_tool(
    turbo_tool_registry_t *registry,
    const char *name,
    turbo_tool_idempotency_t idempotency,
    template_probe_t *probe) {
  static const char *const caps[] = {"runtime_tools"};
  turbo_tool_definition_v4_t definition = {0};
  definition.struct_size = sizeof(definition);
  definition.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
  definition.definition.name = name;
  definition.definition.description = "Template test tool.";
  definition.definition.parameters_json =
      "{\"type\":\"object\",\"additionalProperties\":true}";
  definition.definition.strict = 1;
  definition.definition.user_data = probe;
  definition.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
  definition.execution_policy.idempotency = idempotency;
  definition.required_capabilities = caps;
  definition.required_capability_count = 1u;
  definition.json_value_context_handler = template_probe_handler;
  return turbo_tool_registry_add_v4(registry, &definition) == TURBO_TOOL_OK
             ? 0
             : -1;
}

static turbo_tool_registry_t *make_template_registry(
    template_probe_t *read_probe,
    template_probe_t *write_probe) {
  turbo_tool_registry_t *registry = turbo_tool_registry_create();
  if (!registry) return NULL;
  if (add_template_tool(
          registry, "repo.inspect", TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
          read_probe) != 0 ||
      add_template_tool(
          registry, "repo.diagnose", TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
          read_probe) != 0 ||
      add_template_tool(
          registry, "repo.verify", TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
          read_probe) != 0 ||
      add_template_tool(
          registry, "repo.patch", TURBO_TOOL_IDEMPOTENCY_NONE,
          write_probe) != 0) {
    turbo_tool_registry_destroy(registry);
    return NULL;
  }
  return registry;
}

static json_value_t *template_args(const char *value) {
  json_value_t *args = json_create_object();
  if (!args) return NULL;
  if (value &&
      turbo_runtime_json_object_set(
          args, "value", json_create_string(value)) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(args);
    return NULL;
  }
  return args;
}

static void template_tool(
    turbo_agent_template_tool_source_t *source,
    const char *tool_name,
    const json_value_t *arguments) {
  turbo_agent_template_tool_source_init(source);
  source->tool_name = tool_name;
  source->arguments = arguments;
}

static void template_config(
    turbo_agent_compiler_config_t *config) {
  static const char *const allowed[] = {"runtime_tools"};
  turbo_agent_compiler_config_init(config);
  config->allowed_capabilities = allowed;
  config->allowed_capability_count = 1u;
}

spec("AgentCompiler Change and Repair templates") {
  it("uses one canonical template identity registry") {
    const turbo_agent_template_descriptor_t *inspect =
        turbo_agent_template_descriptor(TURBO_AGENT_TEMPLATE_INSPECT);
    const turbo_agent_template_descriptor_t *change =
        turbo_agent_template_descriptor(TURBO_AGENT_TEMPLATE_CHANGE);
    const turbo_agent_template_descriptor_t *repair =
        turbo_agent_template_descriptor(TURBO_AGENT_TEMPLATE_REPAIR);

    check_not_null(inspect);
    check_not_null(change);
    check_not_null(repair);
    check_equal(inspect->name, "inspect");
    check_equal(change->name, "change");
    check_equal(change->input_contract, "TurboAgent.Change.v1");
    check_equal(repair->name, "repair");
    check_equal(repair->input_contract, "TurboAgent.Repair.v1");
    check_true((int)TURBO_AGENT_DAG_TEMPLATE_CHANGE ==
               (int)TURBO_AGENT_TEMPLATE_CHANGE);
    check_true((int)TURBO_AGENT_DAG_TEMPLATE_REPAIR ==
               (int)TURBO_AGENT_TEMPLATE_REPAIR);
  }

  it("keeps the legacy single-step compiler Inspect-only") {
    template_probe_t read_probe = {0};
    template_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        make_template_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_typed_plan_t source;
    turbo_agent_executable_plan_t *plan = NULL;
    json_value_t *args = template_args("single");

    check_not_null(registry);
    check_not_null(args);
    template_config(&config);
    turbo_agent_typed_plan_init(&source);
    source.template_kind = TURBO_AGENT_TEMPLATE_CHANGE;
    source.step_id = "change";
    source.tool_name = "repo.patch";
    source.arguments = args;

    check_equal(
        turbo_agent_compile_plan(&config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_UNSUPPORTED_TEMPLATE);
    check_null(plan);
    check_equal(read_probe.calls, 0);
    check_equal(write_probe.calls, 0);

    turbo_runtime_json_destroy(args);
    turbo_tool_registry_destroy(registry);
  }

  it("rejects Inspect identity on the multi-step DAG surface") {
    template_probe_t read_probe = {0};
    template_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        make_template_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_dag_source_t dag;
    turbo_agent_dag_step_source_t step;
    turbo_agent_executable_dag_t *plan = NULL;
    json_value_t *args = template_args("inspect");

    check_not_null(registry);
    check_not_null(args);
    template_config(&config);
    turbo_agent_dag_step_source_init(&step);
    step.step_id = "inspect";
    step.tool_name = "repo.inspect";
    step.arguments = args;
    turbo_agent_dag_source_init(&dag);
    dag.steps = &step;
    dag.step_count = 1u;
    dag.template_kind = TURBO_AGENT_TEMPLATE_INSPECT;

    check_equal(
        turbo_agent_compile_dag(&config, registry, &dag, &plan, NULL),
        TURBO_AGENT_COMPILE_INVALID_ARGUMENT);
    check_null(plan);

    turbo_runtime_json_destroy(args);
    turbo_tool_registry_destroy(registry);
  }

  it("compiles Change into the fixed inspect-change-verify topology") {
    template_probe_t read_probe = {0};
    template_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        make_template_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_change_source_t source;
    turbo_agent_executable_dag_t *plan = NULL;
    json_value_t *inspect_args = template_args("inspect");
    json_value_t *change_args = template_args("change");
    json_value_t *verify_args = template_args("verify");
    json_value_t *certificate = NULL;
    const json_value_t *steps;
    const json_value_t *inspect;
    const json_value_t *change;
    const json_value_t *verify;

    check_not_null(registry);
    check_not_null(inspect_args);
    check_not_null(change_args);
    check_not_null(verify_args);

    template_config(&config);
    turbo_agent_change_source_init(&source);
    template_tool(&source.inspect, "repo.inspect", inspect_args);
    template_tool(&source.change, "repo.patch", change_args);
    template_tool(&source.verify, "repo.verify", verify_args);
    source.plan_generation = 4u;

    check_equal(
        turbo_agent_compile_change_template(
            &config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_OK);
    check_not_null(plan);
    check_equal(turbo_agent_executable_dag_template_kind(plan),
                TURBO_AGENT_TEMPLATE_CHANGE);
    check_equal(turbo_agent_executable_dag_plan_generation(plan), 4u);
    check_equal(read_probe.calls, 0);
    check_equal(write_probe.calls, 0);

    certificate = turbo_agent_executable_dag_certificate_json_value(plan);
    check_not_null(certificate);
    check_equal(json_get_string(certificate, "template"), "change");
    check_equal(json_get_int(certificate, "plan_generation", -1), 4);
    check_equal(json_get_int(certificate, "replan_budget", -1), 0);

    steps = json_object_get(certificate, "steps");
    check_equal(json_array_size(steps), 3u);
    inspect = json_array_get(steps, 0u);
    change = json_array_get(steps, 1u);
    verify = json_array_get(steps, 2u);
    check_equal(json_get_string(inspect, "step_id"), "inspect");
    check_equal(json_get_string(change, "step_id"), "change");
    check_equal(json_get_string(verify, "step_id"), "verify");
    check_equal(json_array_size(json_object_get(inspect, "depends_on")), 0u);
    check_equal(
        json_get_string(
            json_array_get(json_object_get(change, "depends_on"), 0u)),
        "inspect");
    check_equal(
        json_get_string(
            json_array_get(json_object_get(verify, "depends_on"), 0u)),
        "change");
    check_equal(
        json_get_int(change, "flags", -1),
        TURBO_AGENT_DAG_STEP_APPROVAL_BEFORE |
            TURBO_AGENT_DAG_STEP_CHECKPOINT_AFTER);

    turbo_runtime_json_destroy(certificate);
    turbo_agent_executable_dag_destroy(plan);
    turbo_runtime_json_destroy(verify_args);
    turbo_runtime_json_destroy(change_args);
    turbo_runtime_json_destroy(inspect_args);
    turbo_tool_registry_destroy(registry);
  }

  it("compiles Repair with bounded replan identity and fixed mutation boundary") {
    template_probe_t read_probe = {0};
    template_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        make_template_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_repair_source_t source;
    turbo_agent_executable_dag_t *plan = NULL;
    json_value_t *diagnose_args = template_args("diagnose");
    json_value_t *change_args = template_args("repair");
    json_value_t *verify_args = template_args("verify");
    json_value_t *certificate = NULL;
    const json_value_t *steps;
    const json_value_t *change;

    template_config(&config);
    turbo_agent_repair_source_init(&source);
    template_tool(&source.diagnose, "repo.diagnose", diagnose_args);
    template_tool(&source.change, "repo.patch", change_args);
    template_tool(&source.verify, "repo.verify", verify_args);
    source.plan_generation = 7u;
    source.replan_budget = 3u;

    check_equal(
        turbo_agent_compile_repair_template(
            &config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_OK);
    check_not_null(plan);
    check_equal(turbo_agent_executable_dag_template_kind(plan),
                TURBO_AGENT_TEMPLATE_REPAIR);
    check_equal(turbo_agent_executable_dag_plan_generation(plan), 7u);

    certificate = turbo_agent_executable_dag_certificate_json_value(plan);
    check_not_null(certificate);
    check_equal(json_get_string(certificate, "template"), "repair");
    check_equal(json_get_int(certificate, "replan_budget", -1), 3);
    steps = json_object_get(certificate, "steps");
    check_equal(json_get_string(json_array_get(steps, 0u), "step_id"),
                "diagnose");
    change = json_array_get(steps, 1u);
    check_equal(
        json_get_int(change, "flags", -1),
        TURBO_AGENT_DAG_STEP_APPROVAL_BEFORE |
            TURBO_AGENT_DAG_STEP_CHECKPOINT_AFTER);

    turbo_runtime_json_destroy(certificate);
    turbo_agent_executable_dag_destroy(plan);
    turbo_runtime_json_destroy(verify_args);
    turbo_runtime_json_destroy(change_args);
    turbo_runtime_json_destroy(diagnose_args);
    turbo_tool_registry_destroy(registry);
  }

  it("rejects read roles backed by mutating tools and change roles backed by read-only tools") {
    template_probe_t read_probe = {0};
    template_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        make_template_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_change_source_t source;
    turbo_agent_executable_dag_t *plan = NULL;
    json_value_t *args = template_args(NULL);

    template_config(&config);
    turbo_agent_change_source_init(&source);
    template_tool(&source.inspect, "repo.patch", args);
    template_tool(&source.change, "repo.patch", args);
    template_tool(&source.verify, "repo.verify", args);
    check_equal(
        turbo_agent_compile_change_template(
            &config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION);
    check_null(plan);

    template_tool(&source.inspect, "repo.inspect", args);
    template_tool(&source.change, "repo.inspect", args);
    check_equal(
        turbo_agent_compile_change_template(
            &config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION);
    check_null(plan);
    check_equal(read_probe.calls, 0);
    check_equal(write_probe.calls, 0);

    turbo_runtime_json_destroy(args);
    turbo_tool_registry_destroy(registry);
  }

  it("classifies the Repair verify postcondition without hidden replanning") {
    json_value_t *passed = json_create_object();
    json_value_t *failed = json_create_object();
    json_value_t *missing = json_create_object();
    json_value_t *wrong = json_create_object();

    check_not_null(passed);
    check_not_null(failed);
    check_not_null(missing);
    check_not_null(wrong);
    json_object_set_bool(passed, "verified", true);
    json_object_set_bool(failed, "verified", false);
    json_object_set_string(wrong, "verified", "false");

    check_equal(
        turbo_agent_repair_classify_verify_result(passed),
        TURBO_AGENT_REPAIR_OUTCOME_COMPLETED);
    check_equal(
        turbo_agent_repair_classify_verify_result(failed),
        TURBO_AGENT_REPAIR_OUTCOME_REPLAN_REQUIRED);
    check_equal(
        turbo_agent_repair_classify_verify_result(missing),
        TURBO_AGENT_REPAIR_OUTCOME_INVALID);
    check_equal(
        turbo_agent_repair_classify_verify_result(wrong),
        TURBO_AGENT_REPAIR_OUTCOME_INVALID);

    turbo_runtime_json_destroy(wrong);
    turbo_runtime_json_destroy(missing);
    turbo_runtime_json_destroy(failed);
    turbo_runtime_json_destroy(passed);
  }
}
