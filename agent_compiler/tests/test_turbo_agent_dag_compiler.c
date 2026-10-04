#include "tinytest.h"

#include "turbo_agent_dag_compiler.h"
#include "turbo_runtime_json.h"

#include <string.h>

typedef struct dag_probe_s {
  int calls;
} dag_probe_t;

static turbo_tool_status_t dag_probe_handler(
    const json_value_t *arguments,
    const turbo_tool_execution_context_t *context,
    json_value_t **out_result,
    void *user_data) {
  dag_probe_t *probe = (dag_probe_t *)user_data;
  (void)context;
  if (!probe || !arguments || !out_result) return TURBO_TOOL_INVALID_ARGUMENT;
  ++probe->calls;
  *out_result = json_clone(arguments);
  return *out_result ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static turbo_tool_registry_t *make_dag_registry(
    dag_probe_t *read_probe,
    dag_probe_t *write_probe) {
  turbo_tool_registry_t *registry = turbo_tool_registry_create();
  const char *runtime_caps[] = {"runtime_tools"};
  const char *network_caps[] = {"runtime_tools", "network"};
  turbo_tool_definition_v4_t read = {0};
  turbo_tool_definition_v4_t write = {0};
  turbo_tool_definition_v4_t network = {0};
  json_value_t *metadata = NULL;

  if (!registry) return NULL;

  read.struct_size = sizeof(read);
  read.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
  read.definition.name = "repo.inspect";
  read.definition.description = "Read repository state.";
  read.definition.parameters_json =
      "{\"type\":\"object\",\"additionalProperties\":true}";
  read.definition.strict = 1;
  read.definition.user_data = read_probe;
  read.execution_policy.mode = TURBO_TOOL_EXECUTION_PARALLEL_SAFE;
  read.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
  read.required_capabilities = runtime_caps;
  read.required_capability_count = 1;
  read.json_value_context_handler = dag_probe_handler;

  write = read;
  write.definition.name = "repo.patch";
  write.definition.description = "Apply one bounded patch.";
  write.definition.user_data = write_probe;
  write.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
  write.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_NONE;

  network = read;
  network.definition.name = "repo.remote_inspect";
  network.required_capabilities = network_caps;
  network.required_capability_count = 2;

  if (turbo_tool_registry_add_v4(registry, &read) != TURBO_TOOL_OK ||
      turbo_tool_registry_add_v4(registry, &write) != TURBO_TOOL_OK ||
      turbo_tool_registry_add_v4(registry, &network) != TURBO_TOOL_OK) {
    turbo_tool_registry_destroy(registry);
    return NULL;
  }

  metadata = json_create_object();
  if (!metadata ||
      turbo_runtime_json_object_set(
          metadata, "backend", json_create_string("native")) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_tool_registry_set_execution_metadata(
          registry, "repo.patch", metadata) != TURBO_TOOL_OK) {
    turbo_runtime_json_destroy(metadata);
    turbo_tool_registry_destroy(registry);
    return NULL;
  }
  turbo_runtime_json_destroy(metadata);
  return registry;
}

static json_value_t *dag_args(const char *value) {
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

static void dag_step(
    turbo_agent_dag_step_source_t *step,
    const char *step_id,
    const char *tool_name,
    const json_value_t *arguments,
    const char *const *depends_on,
    size_t dependency_count) {
  turbo_agent_dag_step_source_init(step);
  step->step_id = step_id;
  step->tool_name = tool_name;
  step->arguments = arguments;
  step->depends_on = depends_on;
  step->dependency_count = dependency_count;
}

static void dag_source(
    turbo_agent_dag_source_t *source,
    const turbo_agent_dag_step_source_t *steps,
    size_t step_count) {
  turbo_agent_dag_source_init(source);
  source->steps = steps;
  source->step_count = step_count;
  source->replan_budget = 2u;
}

static void dag_config(
    turbo_agent_compiler_config_t *config,
    const char *const *allowed,
    size_t allowed_count) {
  turbo_agent_compiler_config_init(config);
  config->allowed_capabilities = allowed;
  config->allowed_capability_count = allowed_count;
}

spec("AgentCompiler Phase 3 DAG admission") {
  it("freezes a finite multi-step DAG and approved tool projection") {
    dag_probe_t read_probe = {0};
    dag_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        make_dag_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_dag_source_t source;
    turbo_agent_dag_step_source_t steps[3];
    turbo_agent_executable_dag_t *plan = NULL;
    json_value_t *args[3] = {dag_args("inspect"), dag_args("patch"),
                             dag_args("verify")};
    json_value_t *certificate = NULL;
    const char *allowed[] = {"runtime_tools"};
    const char *patch_dep[] = {"inspect"};
    const char *verify_dep[] = {"patch"};

    check_not_null(registry);
    check_not_null(args[0]);
    check_not_null(args[1]);
    check_not_null(args[2]);

    dag_config(&config, allowed, 1u);
    dag_step(&steps[0], "inspect", "repo.inspect", args[0], NULL, 0u);
    dag_step(&steps[1], "patch", "repo.patch", args[1], patch_dep, 1u);
    steps[1].flags =
        TURBO_AGENT_DAG_STEP_APPROVAL_BEFORE |
        TURBO_AGENT_DAG_STEP_CHECKPOINT_AFTER;
    dag_step(&steps[2], "verify", "repo.inspect", args[2], verify_dep, 1u);
    dag_source(&source, steps, 3u);

    check_equal(
        turbo_agent_compile_dag(&config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_OK);
    check_not_null(plan);
    check_equal(turbo_agent_executable_dag_step_count(plan), 3);
    check_true(turbo_agent_executable_dag_hash(plan) != 0u);
    check_not_null(turbo_agent_executable_dag_approved_tools(plan));
    check_equal(
        turbo_tool_registry_count(
            turbo_agent_executable_dag_approved_tools(plan)),
        2);
    check_equal(read_probe.calls, 0);
    check_equal(write_probe.calls, 0);

    certificate = turbo_agent_executable_dag_certificate_json_value(plan);
    check_not_null(certificate);
    check_equal(json_get_string(certificate, "backend"), "praktor_host_tool");
    check_equal(json_get_int(certificate, "version", -1),
                TURBO_AGENT_DAG_CERTIFICATE_VERSION);
    check_equal(json_get_string(certificate, "template"), "generic");
    check_equal(json_get_int(certificate, "plan_generation", -1), 0);
    check_equal(json_get_int(certificate, "replan_budget", -1), 2);
    check_equal(json_array_size(json_object_get(certificate, "steps")), 3);

    turbo_runtime_json_destroy(certificate);
    turbo_agent_executable_dag_destroy(plan);
    turbo_runtime_json_destroy(args[2]);
    turbo_runtime_json_destroy(args[1]);
    turbo_runtime_json_destroy(args[0]);
    turbo_tool_registry_destroy(registry);
  }

  it("rejects duplicate step IDs before any tool callback") {
    dag_probe_t read_probe = {0};
    dag_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        make_dag_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_dag_source_t source;
    turbo_agent_dag_step_source_t steps[2];
    turbo_agent_executable_dag_t *plan = NULL;
    json_value_t *args[2] = {dag_args(NULL), dag_args(NULL)};
    const char *allowed[] = {"runtime_tools"};

    dag_config(&config, allowed, 1u);
    dag_step(&steps[0], "same", "repo.inspect", args[0], NULL, 0u);
    dag_step(&steps[1], "same", "repo.patch", args[1], NULL, 0u);
    dag_source(&source, steps, 2u);

    check_equal(
        turbo_agent_compile_dag(&config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_DUPLICATE_STEP);
    check_null(plan);
    check_equal(read_probe.calls, 0);
    check_equal(write_probe.calls, 0);

    turbo_runtime_json_destroy(args[1]);
    turbo_runtime_json_destroy(args[0]);
    turbo_tool_registry_destroy(registry);
  }

  it("rejects missing dependency before any tool callback") {
    dag_probe_t read_probe = {0};
    dag_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        make_dag_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_dag_source_t source;
    turbo_agent_dag_step_source_t steps[2];
    turbo_agent_executable_dag_t *plan = NULL;
    json_value_t *args[2] = {dag_args(NULL), dag_args(NULL)};
    const char *allowed[] = {"runtime_tools"};
    const char *missing[] = {"does-not-exist"};

    dag_config(&config, allowed, 1u);
    dag_step(&steps[0], "inspect", "repo.inspect", args[0], NULL, 0u);
    dag_step(&steps[1], "patch", "repo.patch", args[1], missing, 1u);
    dag_source(&source, steps, 2u);

    check_equal(
        turbo_agent_compile_dag(&config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_MISSING_DEPENDENCY);
    check_null(plan);
    check_equal(read_probe.calls, 0);
    check_equal(write_probe.calls, 0);

    turbo_runtime_json_destroy(args[1]);
    turbo_runtime_json_destroy(args[0]);
    turbo_tool_registry_destroy(registry);
  }

  it("rejects dependency cycles before any tool callback") {
    dag_probe_t read_probe = {0};
    dag_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        make_dag_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_dag_source_t source;
    turbo_agent_dag_step_source_t steps[2];
    turbo_agent_executable_dag_t *plan = NULL;
    json_value_t *args[2] = {dag_args(NULL), dag_args(NULL)};
    const char *allowed[] = {"runtime_tools"};
    const char *a_dep[] = {"b"};
    const char *b_dep[] = {"a"};

    dag_config(&config, allowed, 1u);
    dag_step(&steps[0], "a", "repo.inspect", args[0], a_dep, 1u);
    dag_step(&steps[1], "b", "repo.patch", args[1], b_dep, 1u);
    dag_source(&source, steps, 2u);

    check_equal(
        turbo_agent_compile_dag(&config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_CYCLE);
    check_null(plan);
    check_equal(read_probe.calls, 0);
    check_equal(write_probe.calls, 0);

    turbo_runtime_json_destroy(args[1]);
    turbo_runtime_json_destroy(args[0]);
    turbo_tool_registry_destroy(registry);
  }

  it("rejects unknown and denied tool authority before callbacks") {
    dag_probe_t read_probe = {0};
    dag_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        make_dag_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_dag_source_t source;
    turbo_agent_dag_step_source_t step;
    turbo_agent_executable_dag_t *plan = NULL;
    json_value_t *args = dag_args(NULL);
    const char *allowed[] = {"runtime_tools"};

    dag_config(&config, allowed, 1u);
    dag_step(&step, "x", "repo.missing", args, NULL, 0u);
    dag_source(&source, &step, 1u);
    check_equal(
        turbo_agent_compile_dag(&config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_UNRESOLVED_TOOL);
    check_null(plan);

    dag_step(&step, "x", "repo.remote_inspect", args, NULL, 0u);
    check_equal(
        turbo_agent_compile_dag(&config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_CAPABILITY_DENIED);
    check_null(plan);
    check_equal(read_probe.calls, 0);
    check_equal(write_probe.calls, 0);

    turbo_runtime_json_destroy(args);
    turbo_tool_registry_destroy(registry);
  }

  it("changes plan identity when dependency topology changes") {
    dag_probe_t read_probe = {0};
    dag_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        make_dag_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_dag_source_t first_source;
    turbo_agent_dag_source_t second_source;
    turbo_agent_dag_step_source_t first[3];
    turbo_agent_dag_step_source_t second[3];
    turbo_agent_executable_dag_t *first_plan = NULL;
    turbo_agent_executable_dag_t *second_plan = NULL;
    json_value_t *args[3] = {dag_args("a"), dag_args("b"), dag_args("c")};
    const char *allowed[] = {"runtime_tools"};
    const char *b_dep[] = {"a"};
    const char *c_dep_b[] = {"b"};
    const char *c_dep_a[] = {"a"};

    dag_config(&config, allowed, 1u);

    dag_step(&first[0], "a", "repo.inspect", args[0], NULL, 0u);
    dag_step(&first[1], "b", "repo.patch", args[1], b_dep, 1u);
    dag_step(&first[2], "c", "repo.inspect", args[2], c_dep_b, 1u);
    dag_source(&first_source, first, 3u);

    dag_step(&second[0], "a", "repo.inspect", args[0], NULL, 0u);
    dag_step(&second[1], "b", "repo.patch", args[1], b_dep, 1u);
    dag_step(&second[2], "c", "repo.inspect", args[2], c_dep_a, 1u);
    dag_source(&second_source, second, 3u);

    check_equal(
        turbo_agent_compile_dag(
            &config, registry, &first_source, &first_plan, NULL),
        TURBO_AGENT_COMPILE_OK);
    check_equal(
        turbo_agent_compile_dag(
            &config, registry, &second_source, &second_plan, NULL),
        TURBO_AGENT_COMPILE_OK);
    check_not_null(first_plan);
    check_not_null(second_plan);
    check_true(
        turbo_agent_executable_dag_hash(first_plan) !=
        turbo_agent_executable_dag_hash(second_plan));
    check_equal(read_probe.calls, 0);
    check_equal(write_probe.calls, 0);

    turbo_agent_executable_dag_destroy(second_plan);
    turbo_agent_executable_dag_destroy(first_plan);
    turbo_runtime_json_destroy(args[2]);
    turbo_runtime_json_destroy(args[1]);
    turbo_runtime_json_destroy(args[0]);
    turbo_tool_registry_destroy(registry);
  }

  it("freezes source arguments and canonical tool identities") {
    dag_probe_t read_probe = {0};
    dag_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        make_dag_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_dag_source_t source;
    turbo_agent_dag_step_source_t step;
    turbo_agent_executable_dag_t *plan = NULL;
    json_value_t *args = dag_args("before");
    json_value_t *certificate = NULL;
    json_value_t *steps = NULL;
    json_value_t *first = NULL;
    const char *allowed[] = {"runtime_tools"};
    uint64_t hash_before;

    dag_config(&config, allowed, 1u);
    dag_step(&step, "inspect", "repo_inspect", args, NULL, 0u);
    dag_source(&source, &step, 1u);

    check_equal(
        turbo_agent_compile_dag(&config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_OK);
    check_not_null(plan);
    hash_before = turbo_agent_executable_dag_hash(plan);

    check_equal(
        turbo_runtime_json_object_set(
            args, "value", json_create_string("after")),
        TURBO_RUNTIME_JSON_OK);

    certificate = turbo_agent_executable_dag_certificate_json_value(plan);
    check_not_null(certificate);
    steps = json_object_get(certificate, "steps");
    first = json_array_get(steps, 0u);
    check_equal(json_get_string(first, "tool"), "repo.inspect");
    check_equal(
        json_get_string(json_object_get(first, "arguments"), "value"),
        "before");
    check_true(turbo_agent_executable_dag_hash(plan) == hash_before);

    turbo_runtime_json_destroy(certificate);
    turbo_agent_executable_dag_destroy(plan);
    turbo_runtime_json_destroy(args);
    turbo_tool_registry_destroy(registry);
  }
  it("changes plan identity across template and replan generation") {
    dag_probe_t read_probe = {0};
    dag_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        make_dag_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_dag_source_t change_source;
    turbo_agent_dag_source_t repair_source;
    turbo_agent_dag_source_t replan_source;
    turbo_agent_dag_step_source_t step;
    turbo_agent_executable_dag_t *change = NULL;
    turbo_agent_executable_dag_t *repair = NULL;
    turbo_agent_executable_dag_t *replan = NULL;
    json_value_t *args = dag_args("same");
    json_value_t *certificate = NULL;
    const char *allowed[] = {"runtime_tools"};

    check_not_null(registry);
    check_not_null(args);
    dag_config(&config, allowed, 1u);
    dag_step(&step, "verify", "repo.inspect", args, NULL, 0u);

    dag_source(&change_source, &step, 1u);
    change_source.template_kind = TURBO_AGENT_DAG_TEMPLATE_CHANGE;
    change_source.plan_generation = 1u;

    dag_source(&repair_source, &step, 1u);
    repair_source.template_kind = TURBO_AGENT_DAG_TEMPLATE_REPAIR;
    repair_source.plan_generation = 1u;

    dag_source(&replan_source, &step, 1u);
    replan_source.template_kind = TURBO_AGENT_DAG_TEMPLATE_REPAIR;
    replan_source.plan_generation = 2u;

    check_equal(turbo_agent_compile_dag(
                    &config, registry, &change_source, &change, NULL),
                TURBO_AGENT_COMPILE_OK);
    check_equal(turbo_agent_compile_dag(
                    &config, registry, &repair_source, &repair, NULL),
                TURBO_AGENT_COMPILE_OK);
    check_equal(turbo_agent_compile_dag(
                    &config, registry, &replan_source, &replan, NULL),
                TURBO_AGENT_COMPILE_OK);

    check_true(turbo_agent_executable_dag_hash(change) !=
               turbo_agent_executable_dag_hash(repair));
    check_true(turbo_agent_executable_dag_hash(repair) !=
               turbo_agent_executable_dag_hash(replan));
    check_equal(turbo_agent_executable_dag_template_kind(replan),
                TURBO_AGENT_DAG_TEMPLATE_REPAIR);
    check_equal(turbo_agent_executable_dag_plan_generation(replan), 2u);

    certificate = turbo_agent_executable_dag_certificate_json_value(replan);
    check_not_null(certificate);
    check_equal(json_get_string(certificate, "template"), "repair");
    check_equal(json_get_int(certificate, "plan_generation", -1), 2);

    turbo_runtime_json_destroy(certificate);
    turbo_agent_executable_dag_destroy(replan);
    turbo_agent_executable_dag_destroy(repair);
    turbo_agent_executable_dag_destroy(change);
    turbo_runtime_json_destroy(args);
    turbo_tool_registry_destroy(registry);
  }

  it("rejects retries for non-idempotent tools before callbacks") {
    dag_probe_t read_probe = {0};
    dag_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        make_dag_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_dag_source_t source;
    turbo_agent_dag_step_source_t step;
    turbo_agent_executable_dag_t *plan = NULL;
    json_value_t *args = dag_args("patch");
    const char *allowed[] = {"runtime_tools"};

    dag_config(&config, allowed, 1u);
    dag_step(&step, "patch", "repo.patch", args, NULL, 0u);
    step.retry_limit = 1u;
    dag_source(&source, &step, 1u);
    source.template_kind = TURBO_AGENT_DAG_TEMPLATE_CHANGE;

    check_equal(
        turbo_agent_compile_dag(&config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_RETRY_UNSAFE);
    check_null(plan);
    check_equal(read_probe.calls, 0);
    check_equal(write_probe.calls, 0);

    turbo_runtime_json_destroy(args);
    turbo_tool_registry_destroy(registry);
  }

  it("admits bounded retries for read-only tools") {
    dag_probe_t read_probe = {0};
    dag_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        make_dag_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_dag_source_t source;
    turbo_agent_dag_step_source_t step;
    turbo_agent_executable_dag_t *plan = NULL;
    json_value_t *args = dag_args("verify");
    json_value_t *certificate = NULL;
    const json_value_t *steps = NULL;
    const json_value_t *first = NULL;
    const char *allowed[] = {"runtime_tools"};

    dag_config(&config, allowed, 1u);
    dag_step(&step, "verify", "repo.inspect", args, NULL, 0u);
    step.retry_limit = 2u;
    dag_source(&source, &step, 1u);
    source.template_kind = TURBO_AGENT_DAG_TEMPLATE_REPAIR;

    check_equal(
        turbo_agent_compile_dag(&config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_OK);
    check_not_null(plan);
    certificate = turbo_agent_executable_dag_certificate_json_value(plan);
    check_not_null(certificate);
    steps = json_object_get(certificate, "steps");
    first = json_array_get(steps, 0u);
    check_equal(json_get_int(first, "retry_limit", -1), 2);

    turbo_runtime_json_destroy(certificate);
    turbo_agent_executable_dag_destroy(plan);
    turbo_runtime_json_destroy(args);
    turbo_tool_registry_destroy(registry);
  }

  it("accepts DAG source v1 prefix without reading v2 identity fields") {
    dag_probe_t read_probe = {0};
    dag_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        make_dag_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_dag_source_t source;
    turbo_agent_dag_step_source_t step;
    turbo_agent_executable_dag_t *plan = NULL;
    json_value_t *args = dag_args("legacy");
    json_value_t *certificate = NULL;
    const char *allowed[] = {"runtime_tools"};

    dag_config(&config, allowed, 1u);
    dag_step(&step, "inspect", "repo.inspect", args, NULL, 0u);
    dag_source(&source, &step, 1u);
    source.struct_size = TURBO_AGENT_DAG_SOURCE_V1_SIZE;
    source.abi_version = TURBO_AGENT_DAG_SOURCE_ABI_VERSION_V1;
    source.template_kind = (turbo_agent_dag_template_kind_t)999;
    source.plan_generation = 999u;

    check_equal(
        turbo_agent_compile_dag(&config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_OK);
    check_not_null(plan);
    check_equal(turbo_agent_executable_dag_template_kind(plan),
                TURBO_AGENT_DAG_TEMPLATE_GENERIC);
    check_equal(turbo_agent_executable_dag_plan_generation(plan), 0u);

    certificate = turbo_agent_executable_dag_certificate_json_value(plan);
    check_not_null(certificate);
    check_equal(json_get_string(certificate, "template"), "generic");
    check_equal(json_get_int(certificate, "plan_generation", -1), 0);

    turbo_runtime_json_destroy(certificate);
    turbo_agent_executable_dag_destroy(plan);
    turbo_runtime_json_destroy(args);
    turbo_tool_registry_destroy(registry);
  }

}
