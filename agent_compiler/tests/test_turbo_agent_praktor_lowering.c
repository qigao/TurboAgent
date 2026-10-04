#include "tinytest.h"

#include "turbo_agent_praktor_lowering.h"
#include "turbo_runtime_json.h"

#include <string.h>

typedef struct lowering_probe_s {
  int calls;
} lowering_probe_t;

static turbo_tool_status_t lowering_handler(
    const json_value_t *arguments,
    const turbo_tool_execution_context_t *context,
    json_value_t **out_result,
    void *user_data) {
  lowering_probe_t *probe = (lowering_probe_t *)user_data;
  (void)context;
  if (!probe || !arguments || !out_result) return TURBO_TOOL_INVALID_ARGUMENT;
  ++probe->calls;
  *out_result = json_clone(arguments);
  return *out_result ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static int lowering_add_tool(
    turbo_tool_registry_t *registry,
    const char *name,
    turbo_tool_idempotency_t idempotency,
    lowering_probe_t *probe) {
  static const char *const capabilities[] = {"runtime_tools"};
  turbo_tool_definition_v4_t definition = {0};
  definition.struct_size = sizeof(definition);
  definition.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
  definition.definition.name = name;
  definition.definition.description = "Lowering test tool.";
  definition.definition.parameters_json =
      "{\"type\":\"object\",\"additionalProperties\":true}";
  definition.definition.strict = 1;
  definition.definition.user_data = probe;
  definition.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
  definition.execution_policy.idempotency = idempotency;
  definition.required_capabilities = capabilities;
  definition.required_capability_count = 1u;
  definition.json_value_context_handler = lowering_handler;
  return turbo_tool_registry_add_v4(registry, &definition) == TURBO_TOOL_OK
             ? 0
             : -1;
}

static turbo_tool_registry_t *lowering_registry(
    lowering_probe_t *read_probe,
    lowering_probe_t *write_probe) {
  turbo_tool_registry_t *registry = turbo_tool_registry_create();
  if (!registry) return NULL;
  if (lowering_add_tool(
          registry, "repo.inspect", TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
          read_probe) != 0 ||
      lowering_add_tool(
          registry, "repo.verify", TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
          read_probe) != 0 ||
      lowering_add_tool(
          registry, "repo.patch", TURBO_TOOL_IDEMPOTENCY_NONE,
          write_probe) != 0) {
    turbo_tool_registry_destroy(registry);
    return NULL;
  }
  return registry;
}

static void lowering_config(turbo_agent_compiler_config_t *config) {
  static const char *const allowed[] = {"runtime_tools"};
  turbo_agent_compiler_config_init(config);
  config->allowed_capabilities = allowed;
  config->allowed_capability_count = 1u;
}

static void lowering_slot(
    turbo_agent_template_slot_t *slot,
    const char *tool,
    const json_value_t *arguments,
    uint32_t retry_limit) {
  turbo_agent_template_slot_init(slot);
  slot->tool_name = tool;
  slot->arguments = arguments;
  slot->retry_limit = retry_limit;
}

static json_value_t *lowering_args_order_ab(void) {
  json_value_t *root = json_create_object();
  json_value_t *nested = json_create_object();
  if (!root || !nested ||
      turbo_runtime_json_object_set(
          nested, "y", json_create_int64(2)) != TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set(
          nested, "z", json_create_int64(3)) != TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set(
          root, "a", json_create_int64(1)) != TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set(root, "b", nested) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(nested);
    turbo_runtime_json_destroy(root);
    return NULL;
  }
  return root;
}

static json_value_t *lowering_args_order_ba(void) {
  json_value_t *root = json_create_object();
  json_value_t *nested = json_create_object();
  if (!root || !nested ||
      turbo_runtime_json_object_set(
          nested, "z", json_create_int64(3)) != TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set(
          nested, "y", json_create_int64(2)) != TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set(root, "b", nested) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set(
          root, "a", json_create_int64(1)) != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(nested);
    turbo_runtime_json_destroy(root);
    return NULL;
  }
  return root;
}

static json_value_t *lowering_empty_args(void) {
  return json_create_object();
}

spec("AgentCompiler deterministic Praktor lowering") {
  it("produces byte-identical YAML for semantically identical frozen plans") {
    lowering_probe_t read_probe = {0};
    lowering_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        lowering_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_change_source_t first_source;
    turbo_agent_change_source_t second_source;
    turbo_agent_template_plan_t *first_plan = NULL;
    turbo_agent_template_plan_t *second_plan = NULL;
    turbo_agent_praktor_inline_source_t *first = NULL;
    turbo_agent_praktor_inline_source_t *second = NULL;
    json_value_t *first_inspect = lowering_args_order_ab();
    json_value_t *second_inspect = lowering_args_order_ba();
    json_value_t *change_args = lowering_empty_args();
    json_value_t *verify_args = lowering_empty_args();
    const char *yaml;

    check_not_null(registry);
    check_not_null(first_inspect);
    check_not_null(second_inspect);
    check_not_null(change_args);
    check_not_null(verify_args);
    lowering_config(&config);

    turbo_agent_change_source_init(&first_source);
    first_source.plan_version = 1u;
    lowering_slot(&first_source.inspect, "repo.inspect", first_inspect, 0u);
    lowering_slot(&first_source.change, "repo.patch", change_args, 0u);
    lowering_slot(&first_source.verify, "repo.verify", verify_args, 0u);

    turbo_agent_change_source_init(&second_source);
    second_source.plan_version = 1u;
    lowering_slot(&second_source.inspect, "repo.inspect", second_inspect, 0u);
    lowering_slot(&second_source.change, "repo.patch", change_args, 0u);
    lowering_slot(&second_source.verify, "repo.verify", verify_args, 0u);

    check_equal(
        turbo_agent_compile_change_template(
            &config, registry, &first_source, &first_plan, NULL),
        TURBO_AGENT_COMPILE_OK);
    check_equal(
        turbo_agent_compile_change_template(
            &config, registry, &second_source, &second_plan, NULL),
        TURBO_AGENT_COMPILE_OK);
    check_true(
        turbo_agent_template_plan_hash(first_plan) ==
        turbo_agent_template_plan_hash(second_plan));

    check_equal(
        turbo_agent_template_lower_praktor_inline(
            first_plan, &first, NULL),
        TURBO_AGENT_PRAKTOR_LOWERING_OK);
    check_equal(
        turbo_agent_template_lower_praktor_inline(
            second_plan, &second, NULL),
        TURBO_AGENT_PRAKTOR_LOWERING_OK);
    check_not_null(first);
    check_not_null(second);
    check_equal(
        turbo_agent_praktor_inline_source_id(first),
        turbo_agent_praktor_inline_source_id(second));
    check_equal(
        turbo_agent_praktor_inline_source_yaml_size(first),
        turbo_agent_praktor_inline_source_yaml_size(second));
    check_true(memcmp(
                   turbo_agent_praktor_inline_source_yaml(first),
                   turbo_agent_praktor_inline_source_yaml(second),
                   turbo_agent_praktor_inline_source_yaml_size(first)) == 0);

    yaml = turbo_agent_praktor_inline_source_yaml(first);
    check_not_null(yaml);
    check_not_null(strstr(yaml, "input_policy: strict\n"));
    check_not_null(strstr(
        yaml,
        "value: \"{{ tasks.verify.outputs.result }}\"\n"));
    check_not_null(strstr(yaml, "with: {\"a\":1,\"b\":{\"y\":2,\"z\":3}}\n"));
    check_not_null(strstr(yaml, "depends_on: [\"inspect\"]\n"));
    check_not_null(strstr(yaml, "depends_on: [\"change\"]\n"));
    check_equal(read_probe.calls, 0);
    check_equal(write_probe.calls, 0);

    turbo_agent_praktor_inline_source_destroy(second);
    turbo_agent_praktor_inline_source_destroy(first);
    turbo_agent_template_plan_destroy(second_plan);
    turbo_agent_template_plan_destroy(first_plan);
    turbo_runtime_json_destroy(verify_args);
    turbo_runtime_json_destroy(change_args);
    turbo_runtime_json_destroy(second_inspect);
    turbo_runtime_json_destroy(first_inspect);
    turbo_tool_registry_destroy(registry);
  }

  it("changes logical source identity when Repair plan generation changes") {
    lowering_probe_t read_probe = {0};
    lowering_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        lowering_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_repair_source_t source;
    turbo_agent_template_plan_t *v1_plan = NULL;
    turbo_agent_template_plan_t *v2_plan = NULL;
    turbo_agent_praktor_inline_source_t *v1 = NULL;
    turbo_agent_praktor_inline_source_t *v2 = NULL;
    json_value_t *args = lowering_empty_args();

    lowering_config(&config);
    turbo_agent_repair_source_init(&source);
    source.max_replans = 2u;
    lowering_slot(&source.diagnose, "repo.inspect", args, 0u);
    lowering_slot(&source.change, "repo.patch", args, 0u);
    lowering_slot(&source.verify, "repo.verify", args, 0u);

    source.plan_version = 1u;
    check_equal(
        turbo_agent_compile_repair_template(
            &config, registry, &source, &v1_plan, NULL),
        TURBO_AGENT_COMPILE_OK);
    source.plan_version = 2u;
    check_equal(
        turbo_agent_compile_repair_template(
            &config, registry, &source, &v2_plan, NULL),
        TURBO_AGENT_COMPILE_OK);

    check_equal(
        turbo_agent_template_lower_praktor_inline(v1_plan, &v1, NULL),
        TURBO_AGENT_PRAKTOR_LOWERING_OK);
    check_equal(
        turbo_agent_template_lower_praktor_inline(v2_plan, &v2, NULL),
        TURBO_AGENT_PRAKTOR_LOWERING_OK);
    check_true(
        turbo_agent_praktor_inline_source_plan_hash(v1) !=
        turbo_agent_praktor_inline_source_plan_hash(v2));
    check_true(strcmp(
                   turbo_agent_praktor_inline_source_id(v1),
                   turbo_agent_praktor_inline_source_id(v2)) != 0);
    check_equal(
        turbo_agent_praktor_inline_source_yaml_size(v1),
        turbo_agent_praktor_inline_source_yaml_size(v2));
    check_true(memcmp(
                   turbo_agent_praktor_inline_source_yaml(v1),
                   turbo_agent_praktor_inline_source_yaml(v2),
                   turbo_agent_praktor_inline_source_yaml_size(v1)) == 0);

    turbo_agent_praktor_inline_source_destroy(v2);
    turbo_agent_praktor_inline_source_destroy(v1);
    turbo_agent_template_plan_destroy(v2_plan);
    turbo_agent_template_plan_destroy(v1_plan);
    turbo_runtime_json_destroy(args);
    turbo_tool_registry_destroy(registry);
  }

  it("encodes compiler-admitted finite retries as Praktor HostTool retries.count") {
    lowering_probe_t read_probe = {0};
    lowering_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        lowering_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_change_source_t source;
    turbo_agent_template_plan_t *plan = NULL;
    turbo_agent_praktor_inline_source_t *lowered = NULL;
    json_value_t *args = lowering_empty_args();
    const char *yaml;

    lowering_config(&config);
    turbo_agent_change_source_init(&source);
    lowering_slot(&source.inspect, "repo.inspect", args, 1u);
    lowering_slot(&source.change, "repo.patch", args, 0u);
    lowering_slot(&source.verify, "repo.verify", args, 2u);

    check_equal(
        turbo_agent_compile_change_template(
            &config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_OK);
    check_equal(
        turbo_agent_template_lower_praktor_inline(
            plan, &lowered, NULL),
        TURBO_AGENT_PRAKTOR_LOWERING_OK);
    check_not_null(lowered);
    yaml = turbo_agent_praktor_inline_source_yaml(lowered);
    check_not_null(yaml);
    check_not_null(strstr(
        yaml,
        "name: \"inspect\"\n    tool: \"repo.inspect\"\n"
        "    retries:\n      count: 1\n"));
    check_not_null(strstr(
        yaml,
        "name: \"verify\"\n    tool: \"repo.verify\"\n"
        "    depends_on: [\"change\"]\n"
        "    retries:\n      count: 2\n"));
    check_equal(read_probe.calls, 0);
    check_equal(write_probe.calls, 0);

    turbo_agent_praktor_inline_source_destroy(lowered);
    turbo_agent_template_plan_destroy(plan);
    turbo_runtime_json_destroy(args);
    turbo_tool_registry_destroy(registry);
  }

  it("rejects frozen arguments that would be reinterpreted as Praktor templates") {
    lowering_probe_t read_probe = {0};
    lowering_probe_t write_probe = {0};
    turbo_tool_registry_t *registry =
        lowering_registry(&read_probe, &write_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_change_source_t source;
    turbo_agent_template_plan_t *plan = NULL;
    turbo_agent_praktor_inline_source_t *lowered = NULL;
    turbo_agent_praktor_lowering_diagnostic_t diagnostic;
    json_value_t *inspect_args = json_create_object();
    json_value_t *empty = lowering_empty_args();

    check_not_null(inspect_args);
    check_equal(
        turbo_runtime_json_object_set(
            inspect_args, "path",
            json_create_string("{{ variables.path }}")),
        TURBO_RUNTIME_JSON_OK);

    lowering_config(&config);
    turbo_agent_change_source_init(&source);
    lowering_slot(&source.inspect, "repo.inspect", inspect_args, 0u);
    lowering_slot(&source.change, "repo.patch", empty, 0u);
    lowering_slot(&source.verify, "repo.verify", empty, 0u);

    check_equal(
        turbo_agent_compile_change_template(
            &config, registry, &source, &plan, NULL),
        TURBO_AGENT_COMPILE_OK);
    check_equal(
        turbo_agent_template_lower_praktor_inline(
            plan, &lowered, &diagnostic),
        TURBO_AGENT_PRAKTOR_LOWERING_INVALID_PLAN);
    check_null(lowered);
    check_not_null(strstr(diagnostic.message, "interpolation"));

    turbo_agent_template_plan_destroy(plan);
    turbo_runtime_json_destroy(empty);
    turbo_runtime_json_destroy(inspect_args);
    turbo_tool_registry_destroy(registry);
  }
}
