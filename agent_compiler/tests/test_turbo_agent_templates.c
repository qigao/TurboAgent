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

static turbo_tool_registry_t *template_registry(
    template_probe_t *read_probe,
    template_probe_t *write_probe) {
  turbo_tool_registry_t *registry = turbo_tool_registry_create();
  const char *caps[] = {"runtime_tools"};
  turbo_tool_definition_v4_t inspect = {0};
  turbo_tool_definition_v4_t patch = {0};
  turbo_tool_definition_v4_t keyed_patch = {0};

  if (!registry) return NULL;
  inspect.struct_size = sizeof(inspect);
  inspect.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
  inspect.definition.name = "repo.inspect";
  inspect.definition.description = "Read repository state.";
  inspect.definition.parameters_json =
      "{\"type\":\"object\",\"additionalProperties\":true}";
  inspect.definition.strict = 1;
  inspect.definition.user_data = read_probe;
  inspect.execution_policy.mode = TURBO_TOOL_EXECUTION_PARALLEL_SAFE;
  inspect.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
  inspect.required_capabilities = caps;
  inspect.required_capability_count = 1u;
  inspect.json_value_context_handler = template_probe_handler;

  patch = inspect;
  patch.definition.name = "repo.patch";
  patch.definition.description = "Apply bounded mutation.";
  patch.definition.user_data = write_probe;
  patch.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
  patch.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_NONE;

  keyed_patch = patch;
  keyed_patch.definition.name = "repo.patch_keyed";
  keyed_patch.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_KEYED;

  if (turbo_tool_registry_add_v4(registry, &inspect) != TURBO_TOOL_OK ||
      turbo_tool_registry_add_v4(registry, &patch) != TURBO_TOOL_OK ||
      turbo_tool_registry_add_v4(registry, &keyed_patch) != TURBO_TOOL_OK) {
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

static void template_config(
    turbo_agent_compiler_config_t *config,
    const char *const *allowed,
    size_t count) {
  turbo_agent_compiler_config_init(config);
  config->allowed_capabilities = allowed;
  config->allowed_capability_count = count;
}

static void bind_slot(
    turbo_agent_template_slot_t *slot,
    const char *tool,
    const json_value_t *args,
    uint32_t retry_limit) {
  turbo_agent_template_slot_init(slot);
  slot->tool_name = tool;
  slot->arguments = args;
  slot->retry_limit = retry_limit;
}

spec("AgentCompiler Change/Repair template semantics") {
  it("compiles canonical Change topology with mutation boundaries") {
    template_probe_t read_probe = {0};
    template_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        template_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_change_source_t source;
    turbo_agent_template_plan_t *plan = NULL;
    json_value_t *args[3] = {
        template_args("inspect"), template_args("change"),
        template_args("verify")};
    json_value_t *certificate = NULL;
    const char *allowed[] = {"runtime_tools"};
    const json_value_t *dag;
    const json_value_t *steps;
    const json_value_t *change;

    check_not_null(registry);
    check_not_null(args[0]);
    check_not_null(args[1]);
    check_not_null(args[2]);

    template_config(&config, allowed, 1u);
    turbo_agent_change_source_init(&source);
    source.plan_version = 1u;
    bind_slot(&source.inspect, "repo.inspect", args[0], 1u);
    bind_slot(&source.change, "repo.patch", args[1], 0u);
    bind_slot(&source.verify, "repo.inspect", args[2], 1u);

    check_equal(
        turbo_agent_compile_change_template(
            &config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_OK);
    check_not_null(plan);
    check_equal(turbo_agent_template_plan_kind(plan),
                TURBO_AGENT_TEMPLATE_CHANGE);
    check_equal(turbo_agent_template_plan_version(plan), 1);
    check_equal(
        turbo_agent_executable_dag_step_count(
            turbo_agent_template_plan_dag(plan)),
        3);
    check_equal(read_probe.calls, 0);
    check_equal(write_probe.calls, 0);

    certificate = turbo_agent_template_plan_certificate_json_value(plan);
    check_not_null(certificate);
    check_equal(json_get_string(certificate, "template"), "change");
    check_true(turbo_agent_template_plan_hash(plan) ==
               turbo_agent_executable_dag_hash(
                   turbo_agent_template_plan_dag(plan)));
    dag = json_object_get(certificate, "dag");
    check_not_null(dag);
    check_equal(json_get_string(dag, "template"), "change");
    check_equal(json_get_int(dag, "plan_generation", -1), 1);
    steps = json_object_get(dag, "steps");
    check_not_null(steps);
    check_equal(json_array_size(steps), 3);
    change = json_array_get(steps, 1);
    check_equal(json_get_string(change, "step_id"), "change");
    check_equal(
        json_get_int(change, "flags", 0),
        TURBO_AGENT_DAG_STEP_APPROVAL_BEFORE |
            TURBO_AGENT_DAG_STEP_CHECKPOINT_AFTER);

    turbo_runtime_json_destroy(certificate);
    turbo_agent_template_plan_destroy(plan);
    turbo_runtime_json_destroy(args[2]);
    turbo_runtime_json_destroy(args[1]);
    turbo_runtime_json_destroy(args[0]);
    turbo_tool_registry_destroy(registry);
  }

  it("returns REPLAN_REQUIRED without executing model or tools") {
    template_probe_t read_probe = {0};
    template_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        template_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_repair_source_t source;
    turbo_agent_template_plan_t *plan = NULL;
    json_value_t *args[3] = {
        template_args("diagnose"), template_args("change"),
        template_args("verify")};
    const char *allowed[] = {"runtime_tools"};

    template_config(&config, allowed, 1u);
    turbo_agent_repair_source_init(&source);
    source.plan_version = 1u;
    source.max_replans = 2u;
    bind_slot(&source.diagnose, "repo.inspect", args[0], 1u);
    bind_slot(&source.change, "repo.patch", args[1], 0u);
    bind_slot(&source.verify, "repo.inspect", args[2], 1u);

    check_equal(
        turbo_agent_compile_repair_template(
            &config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_OK);
    check_not_null(plan);
    check_equal(
        turbo_agent_template_finish_verify(
            plan, TURBO_AGENT_VERIFY_PASSED),
        TURBO_AGENT_TEMPLATE_OUTCOME_COMPLETED);
    check_equal(
        turbo_agent_template_finish_verify(
            plan, TURBO_AGENT_VERIFY_SEMANTIC_FAILURE),
        TURBO_AGENT_TEMPLATE_OUTCOME_REPLAN_REQUIRED);
    check_equal(
        turbo_agent_template_finish_verify(
            plan, TURBO_AGENT_VERIFY_EXECUTION_FAILURE),
        TURBO_AGENT_TEMPLATE_OUTCOME_EXECUTION_FAILED);
    check_equal(read_probe.calls, 0);
    check_equal(write_probe.calls, 0);

    turbo_agent_template_plan_destroy(plan);
    turbo_runtime_json_destroy(args[2]);
    turbo_runtime_json_destroy(args[1]);
    turbo_runtime_json_destroy(args[0]);
    turbo_tool_registry_destroy(registry);
  }

  it("changes plan identity for each Repair generation and bounds replans") {
    template_probe_t read_probe = {0};
    template_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        template_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_repair_source_t source;
    turbo_agent_template_plan_t *v1 = NULL;
    turbo_agent_template_plan_t *v2 = NULL;
    turbo_agent_template_plan_t *v3 = NULL;
    json_value_t *args[3] = {
        template_args("diagnose"), template_args("change"),
        template_args("verify")};
    const char *allowed[] = {"runtime_tools"};

    template_config(&config, allowed, 1u);
    turbo_agent_repair_source_init(&source);
    source.max_replans = 2u;
    bind_slot(&source.diagnose, "repo.inspect", args[0], 0u);
    bind_slot(&source.change, "repo.patch", args[1], 0u);
    bind_slot(&source.verify, "repo.inspect", args[2], 0u);

    source.plan_version = 1u;
    check_equal(
        turbo_agent_compile_repair_template(
            &config, registry, &source, &v1, NULL),
        TURBO_AGENT_COMPILE_OK);
    source.plan_version = 2u;
    check_equal(
        turbo_agent_compile_repair_template(
            &config, registry, &source, &v2, NULL),
        TURBO_AGENT_COMPILE_OK);
    source.plan_version = 3u;
    check_equal(
        turbo_agent_compile_repair_template(
            &config, registry, &source, &v3, NULL),
        TURBO_AGENT_COMPILE_OK);

    check_true(turbo_agent_template_plan_hash(v1) !=
               turbo_agent_template_plan_hash(v2));
    check_true(turbo_agent_template_plan_hash(v2) !=
               turbo_agent_template_plan_hash(v3));
    check_equal(
        turbo_agent_template_finish_verify(
            v2, TURBO_AGENT_VERIFY_SEMANTIC_FAILURE),
        TURBO_AGENT_TEMPLATE_OUTCOME_REPLAN_REQUIRED);
    check_equal(
        turbo_agent_template_finish_verify(
            v3, TURBO_AGENT_VERIFY_SEMANTIC_FAILURE),
        TURBO_AGENT_TEMPLATE_OUTCOME_REPLAN_LIMIT_REACHED);

    turbo_agent_template_plan_destroy(v3);
    turbo_agent_template_plan_destroy(v2);
    turbo_agent_template_plan_destroy(v1);
    turbo_runtime_json_destroy(args[2]);
    turbo_runtime_json_destroy(args[1]);
    turbo_runtime_json_destroy(args[0]);
    turbo_tool_registry_destroy(registry);
  }

  it("rejects mutating read slots and read-only change slots") {
    template_probe_t read_probe = {0};
    template_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        template_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_change_source_t source;
    turbo_agent_template_plan_t *plan = NULL;
    json_value_t *args[3] = {
        template_args(NULL), template_args(NULL), template_args(NULL)};
    const char *allowed[] = {"runtime_tools"};

    template_config(&config, allowed, 1u);
    turbo_agent_change_source_init(&source);
    bind_slot(&source.inspect, "repo.patch", args[0], 0u);
    bind_slot(&source.change, "repo.patch", args[1], 0u);
    bind_slot(&source.verify, "repo.inspect", args[2], 0u);
    check_equal(
        turbo_agent_compile_change_template(
            &config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION);
    check_null(plan);

    bind_slot(&source.inspect, "repo.inspect", args[0], 0u);
    bind_slot(&source.change, "repo.inspect", args[1], 0u);
    check_equal(
        turbo_agent_compile_change_template(
            &config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION);
    check_null(plan);
    check_equal(read_probe.calls, 0);
    check_equal(write_probe.calls, 0);

    turbo_runtime_json_destroy(args[2]);
    turbo_runtime_json_destroy(args[1]);
    turbo_runtime_json_destroy(args[0]);
    turbo_tool_registry_destroy(registry);
  }

  it("allows retry only when tool idempotency permits it") {
    template_probe_t read_probe = {0};
    template_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        template_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_change_source_t source;
    turbo_agent_template_plan_t *plan = NULL;
    json_value_t *args[3] = {
        template_args(NULL), template_args(NULL), template_args(NULL)};
    const char *allowed[] = {"runtime_tools"};

    template_config(&config, allowed, 1u);
    turbo_agent_change_source_init(&source);
    bind_slot(&source.inspect, "repo.inspect", args[0], 0u);
    bind_slot(&source.change, "repo.patch", args[1], 1u);
    bind_slot(&source.verify, "repo.inspect", args[2], 0u);
    check_equal(
        turbo_agent_compile_change_template(
            &config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_RETRY_UNSAFE);
    check_null(plan);

    bind_slot(&source.change, "repo.patch_keyed", args[1], 1u);
    check_equal(
        turbo_agent_compile_change_template(
            &config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_OK);
    check_not_null(plan);

    turbo_agent_template_plan_destroy(plan);
    turbo_runtime_json_destroy(args[2]);
    turbo_runtime_json_destroy(args[1]);
    turbo_runtime_json_destroy(args[0]);
    turbo_tool_registry_destroy(registry);
  }
}
