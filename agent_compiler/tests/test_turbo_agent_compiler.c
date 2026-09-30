#include "tinytest.h"

#include "turbo_agent_compiler.h"
#include "turbo_runtime_json.h"

#include <string.h>

typedef struct compiler_probe_s {
  int calls;
  const char *last_value;
} compiler_probe_t;

static turbo_tool_status_t probe_json_handler(
    const json_value_t *arguments,
    const turbo_tool_execution_context_t *context,
    json_value_t **out_result,
    void *user_data) {
  compiler_probe_t *probe = (compiler_probe_t *)user_data;
  const char *value;
  (void)context;

  if (!probe || !arguments || !out_result) return TURBO_TOOL_INVALID_ARGUMENT;
  value = json_get_string(arguments, "value");
  ++probe->calls;
  probe->last_value = value;
  *out_result = json_clone(arguments);
  return *out_result ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static turbo_tool_status_t mutating_json_handler(
    const json_value_t *arguments,
    const turbo_tool_execution_context_t *context,
    json_value_t **out_result,
    void *user_data) {
  compiler_probe_t *probe = (compiler_probe_t *)user_data;
  (void)arguments;
  (void)context;
  if (probe) ++probe->calls;
  if (out_result) *out_result = json_create_object();
  return out_result && *out_result ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static turbo_tool_registry_t *make_registry(compiler_probe_t *read_probe,
                                            compiler_probe_t *mutation_probe) {
  turbo_tool_registry_t *registry = turbo_tool_registry_create();
  const char *read_caps[] = {"runtime_tools"};
  const char *network_caps[] = {"runtime_tools", "network"};
  const char *unknown_caps[] = {"mystery_capability"};
  turbo_tool_definition_v4_t read = {0};
  turbo_tool_definition_v4_t mutate = {0};
  turbo_tool_definition_v4_t network = {0};
  turbo_tool_definition_v4_t unknown = {0};
  turbo_tool_definition_v4_t legacy = {0};

  if (!registry) return NULL;

  read.struct_size = sizeof(read);
  read.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
  read.definition.name = "repo.inspect";
  read.definition.description = "Read-only repository inspection.";
  read.definition.parameters_json =
      "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"string\"}}}";
  read.definition.strict = 1;
  read.definition.user_data = read_probe;
  read.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
  read.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
  read.required_capabilities = read_caps;
  read.required_capability_count = 1;
  read.json_value_context_handler = probe_json_handler;

  mutate = read;
  mutate.definition.name = "repo.patch";
  mutate.definition.description = "Mutation probe.";
  mutate.definition.user_data = mutation_probe;
  mutate.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_NONE;
  mutate.json_value_context_handler = mutating_json_handler;

  network = read;
  network.definition.name = "repo.inspect_network";
  network.required_capabilities = network_caps;
  network.required_capability_count = 2;

  unknown = read;
  unknown.definition.name = "repo.inspect_unknown_cap";
  unknown.required_capabilities = unknown_caps;
  unknown.required_capability_count = 1;

  legacy = read;
  legacy.definition.name = "repo.inspect_legacy";
  legacy.required_capabilities = NULL;
  legacy.required_capability_count = 0;

  if (turbo_tool_registry_add_v4(registry, &read) != TURBO_TOOL_OK ||
      turbo_tool_registry_add_v4(registry, &mutate) != TURBO_TOOL_OK ||
      turbo_tool_registry_add_v4(registry, &network) != TURBO_TOOL_OK ||
      turbo_tool_registry_add_v4(registry, &unknown) != TURBO_TOOL_OK ||
      turbo_tool_registry_add_v4(registry, &legacy) != TURBO_TOOL_OK) {
    turbo_tool_registry_destroy(registry);
    return NULL;
  }
  return registry;
}

static void init_source(turbo_agent_typed_plan_t *source,
                        const char *step,
                        const char *tool,
                        const json_value_t *arguments) {
  turbo_agent_typed_plan_init(source);
  source->template_kind = TURBO_AGENT_TEMPLATE_INSPECT;
  source->step_id = step;
  source->tool_name = tool;
  source->arguments = arguments;
}

spec("agent compiler Phase 1 boundary") {
  it("publishes one immutable Inspect template descriptor") {
    const turbo_agent_template_descriptor_t *descriptor =
        turbo_agent_template_descriptor(TURBO_AGENT_TEMPLATE_INSPECT);
    check_not_null(descriptor);
    check_equal(descriptor->abi_version,
                TURBO_AGENT_TEMPLATE_DESCRIPTOR_ABI_VERSION);
    check_equal(descriptor->version, 1);
    check_equal(descriptor->name, "inspect");
    check_equal(descriptor->input_contract, "TurboAgent.Inspect.v1");
    check_true((descriptor->properties &
                TURBO_AGENT_TEMPLATE_PROPERTY_READ_ONLY) != 0u);
    check_null(turbo_agent_template_descriptor(TURBO_AGENT_TEMPLATE_INVALID));
  }

  it("binds model JSON through DataBind before compiling") {
    compiler_probe_t read_probe = {0};
    compiler_probe_t mutation_probe = {0};
    turbo_tool_registry_t *registry = make_registry(&read_probe, &mutation_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_executable_plan_t *plan = NULL;
    json_value_t *result = NULL;
    const char *allowed[] = {"runtime_tools"};
    static const char source_json[] =
        "{"
        "\"template_id\":\"inspect\","
        "\"step_id\":\"inspect-json\","
        "\"tool\":\"repo.inspect\","
        "\"arguments_json\":\"{\\\"value\\\":\\\"bound\\\"}\""
        "}";

    turbo_agent_compiler_config_init(&config);
    config.allowed_capabilities = allowed;
    config.allowed_capability_count = 1;

    check_equal(turbo_agent_compile_plan_json(
                    &config, registry, source_json, sizeof(source_json) - 1u,
                    &plan, NULL),
                TURBO_AGENT_COMPILE_OK);
    check_not_null(plan);
    check_equal(turbo_agent_execute_compiled_plan(plan, NULL, &result),
                TURBO_TOOL_OK);
    check_not_null(result);
    check_equal(json_get_string(result, "value"), "bound");
    check_equal(read_probe.calls, 1);

    turbo_runtime_json_destroy(result);
    turbo_agent_executable_plan_destroy(plan);
    turbo_tool_registry_destroy(registry);
  }

  it("rejects malformed model JSON through DataBind with zero calls") {
    compiler_probe_t read_probe = {0};
    compiler_probe_t mutation_probe = {0};
    turbo_tool_registry_t *registry = make_registry(&read_probe, &mutation_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_executable_plan_t *plan = NULL;
    const char *allowed[] = {"runtime_tools"};
    static const char source_json[] = "{";

    turbo_agent_compiler_config_init(&config);
    config.allowed_capabilities = allowed;
    config.allowed_capability_count = 1;

    check_equal(turbo_agent_compile_plan_json(
                    &config, registry, source_json, sizeof(source_json) - 1u,
                    &plan, NULL),
                TURBO_AGENT_COMPILE_SOURCE_INVALID);
    check_null(plan);
    check_equal(read_probe.calls, 0);
    check_equal(mutation_probe.calls, 0);

    turbo_tool_registry_destroy(registry);
  }

  it("rejects structurally invalid model JSON through DataBind with zero calls") {
    compiler_probe_t read_probe = {0};
    compiler_probe_t mutation_probe = {0};
    turbo_tool_registry_t *registry = make_registry(&read_probe, &mutation_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_executable_plan_t *plan = NULL;
    const char *allowed[] = {"runtime_tools"};
    static const char source_json[] =
        "{"
        "\"template_id\":\"inspect\","
        "\"step_id\":\"inspect-json\","
        "\"tool\":7,"
        "\"arguments_json\":\"{}\""
        "}";

    turbo_agent_compiler_config_init(&config);
    config.allowed_capabilities = allowed;
    config.allowed_capability_count = 1;

    check_equal(turbo_agent_compile_plan_json(
                    &config, registry, source_json, sizeof(source_json) - 1u,
                    &plan, NULL),
                TURBO_AGENT_COMPILE_SOURCE_INVALID);
    check_null(plan);
    check_equal(read_probe.calls, 0);
    check_equal(mutation_probe.calls, 0);

    turbo_tool_registry_destroy(registry);
  }

  it("rejects unknown tools before any callback") {
    compiler_probe_t read_probe = {0};
    compiler_probe_t mutation_probe = {0};
    turbo_tool_registry_t *registry = make_registry(&read_probe, &mutation_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_typed_plan_t source;
    turbo_agent_executable_plan_t *plan = NULL;
    turbo_agent_compile_diagnostic_t diagnostic = {0};
    json_value_t *arguments = json_create_object();
    const char *allowed[] = {"runtime_tools"};

    check_not_null(registry);
    check_not_null(arguments);
    turbo_agent_compiler_config_init(&config);
    config.allowed_capabilities = allowed;
    config.allowed_capability_count = 1;
    init_source(&source, "inspect", "repo.missing", arguments);

    check_equal(turbo_agent_compile_plan(&config, registry, &source, &plan, &diagnostic),
                TURBO_AGENT_COMPILE_UNRESOLVED_TOOL);
    check_null(plan);
    check_equal(read_probe.calls, 0);
    check_equal(mutation_probe.calls, 0);

    turbo_runtime_json_destroy(arguments);
    turbo_tool_registry_destroy(registry);
  }

  it("rejects unknown capability before any callback") {
    compiler_probe_t read_probe = {0};
    compiler_probe_t mutation_probe = {0};
    turbo_tool_registry_t *registry = make_registry(&read_probe, &mutation_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_typed_plan_t source;
    turbo_agent_executable_plan_t *plan = NULL;
    json_value_t *arguments = json_create_object();
    const char *allowed[] = {"runtime_tools"};

    turbo_agent_compiler_config_init(&config);
    config.allowed_capabilities = allowed;
    config.allowed_capability_count = 1;
    init_source(&source, "inspect", "repo.inspect_unknown_cap", arguments);

    check_equal(turbo_agent_compile_plan(&config, registry, &source, &plan, NULL),
                TURBO_AGENT_COMPILE_CAPABILITY_DENIED);
    check_null(plan);
    check_equal(read_probe.calls, 0);

    turbo_runtime_json_destroy(arguments);
    turbo_tool_registry_destroy(registry);
  }

  it("rejects a known but host-denied capability before any callback") {
    compiler_probe_t read_probe = {0};
    compiler_probe_t mutation_probe = {0};
    turbo_tool_registry_t *registry = make_registry(&read_probe, &mutation_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_typed_plan_t source;
    turbo_agent_executable_plan_t *plan = NULL;
    json_value_t *arguments = json_create_object();
    const char *allowed[] = {"runtime_tools"};

    turbo_agent_compiler_config_init(&config);
    config.allowed_capabilities = allowed;
    config.allowed_capability_count = 1;
    init_source(&source, "inspect", "repo.inspect_network", arguments);

    check_equal(turbo_agent_compile_plan(&config, registry, &source, &plan, NULL),
                TURBO_AGENT_COMPILE_CAPABILITY_DENIED);
    check_null(plan);
    check_equal(read_probe.calls, 0);

    turbo_runtime_json_destroy(arguments);
    turbo_tool_registry_destroy(registry);
  }

  it("normalizes legacy zero-capability tools to custom_tools and fails closed") {
    compiler_probe_t read_probe = {0};
    compiler_probe_t mutation_probe = {0};
    turbo_tool_registry_t *registry = make_registry(&read_probe, &mutation_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_typed_plan_t source;
    turbo_agent_executable_plan_t *plan = NULL;
    json_value_t *arguments = json_create_object();
    const char *allowed[] = {"runtime_tools"};

    turbo_agent_compiler_config_init(&config);
    config.allowed_capabilities = allowed;
    config.allowed_capability_count = 1;
    init_source(&source, "inspect", "repo.inspect_legacy", arguments);

    check_equal(turbo_agent_compile_plan(&config, registry, &source, &plan, NULL),
                TURBO_AGENT_COMPILE_CAPABILITY_DENIED);
    check_null(plan);
    check_equal(read_probe.calls, 0);

    turbo_runtime_json_destroy(arguments);
    turbo_tool_registry_destroy(registry);
  }

  it("rejects mutation-capable tools from Inspect before any callback") {
    compiler_probe_t read_probe = {0};
    compiler_probe_t mutation_probe = {0};
    turbo_tool_registry_t *registry = make_registry(&read_probe, &mutation_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_typed_plan_t source;
    turbo_agent_executable_plan_t *plan = NULL;
    json_value_t *arguments = json_create_object();
    const char *allowed[] = {"runtime_tools"};

    turbo_agent_compiler_config_init(&config);
    config.allowed_capabilities = allowed;
    config.allowed_capability_count = 1;
    init_source(&source, "inspect", "repo.patch", arguments);

    check_equal(turbo_agent_compile_plan(&config, registry, &source, &plan, NULL),
                TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION);
    check_null(plan);
    check_equal(mutation_probe.calls, 0);

    turbo_runtime_json_destroy(arguments);
    turbo_tool_registry_destroy(registry);
  }

  it("freezes source strings and arguments before execution") {
    compiler_probe_t read_probe = {0};
    compiler_probe_t mutation_probe = {0};
    turbo_tool_registry_t *registry = make_registry(&read_probe, &mutation_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_typed_plan_t source;
    turbo_agent_executable_plan_t *plan = NULL;
    turbo_tool_execution_context_t context = {0};
    json_value_t *arguments = json_create_object();
    json_value_t *result = NULL;
    char tool_name[32] = "repo.inspect";
    const char *allowed[] = {"runtime_tools"};
    uint64_t hash_before;

    check_not_null(registry);
    check_not_null(arguments);
    check_true(json_object_add_checked(arguments, "value", json_create_string("before")));

    turbo_agent_compiler_config_init(&config);
    config.allowed_capabilities = allowed;
    config.allowed_capability_count = 1;
    init_source(&source, "inspect-1", tool_name, arguments);

    check_equal(turbo_agent_compile_plan(&config, registry, &source, &plan, NULL),
                TURBO_AGENT_COMPILE_OK);
    check_not_null(plan);
    hash_before = turbo_agent_executable_plan_hash(plan);
    check_true(hash_before != 0);

    strcpy(tool_name, "repo.patch");
    check_equal(turbo_runtime_json_object_set(
                    arguments, "value", json_create_string("after")),
                TURBO_RUNTIME_JSON_OK);

    context.struct_size = sizeof(context);
    context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
    context.turn_id = "turn-phase1";

    check_equal(turbo_agent_execute_compiled_plan(plan, &context, &result), TURBO_TOOL_OK);
    check_not_null(result);
    check_equal(read_probe.calls, 1);
    check_equal(mutation_probe.calls, 0);
    check_equal(json_get_string(result, "value"), "before");
    check_equal(turbo_agent_executable_plan_tool_name(plan), "repo.inspect");
    check_true(turbo_agent_executable_plan_hash(plan) == hash_before);

    turbo_runtime_json_destroy(result);
    turbo_agent_executable_plan_destroy(plan);
    turbo_runtime_json_destroy(arguments);
    turbo_tool_registry_destroy(registry);
  }

  it("emits a deterministic certificate from frozen semantics") {
    compiler_probe_t read_probe = {0};
    compiler_probe_t mutation_probe = {0};
    turbo_tool_registry_t *registry = make_registry(&read_probe, &mutation_probe);
    turbo_agent_compiler_config_t config;
    turbo_agent_typed_plan_t first_source;
    turbo_agent_typed_plan_t second_source;
    turbo_agent_executable_plan_t *first = NULL;
    turbo_agent_executable_plan_t *second = NULL;
    json_value_t *args1 = json_create_object();
    json_value_t *args2 = json_create_object();
    json_value_t *certificate = NULL;
    const char *allowed[] = {"runtime_tools"};

    check_true(json_object_add_checked(args1, "b", json_create_int64(2)));
    check_true(json_object_add_checked(args1, "a", json_create_int64(1)));
    check_true(json_object_add_checked(args2, "a", json_create_int64(1)));
    check_true(json_object_add_checked(args2, "b", json_create_int64(2)));

    turbo_agent_compiler_config_init(&config);
    config.allowed_capabilities = allowed;
    config.allowed_capability_count = 1;
    init_source(&first_source, "inspect-1", "repo.inspect", args1);
    init_source(&second_source, "inspect-1", "repo.inspect", args2);

    check_equal(turbo_agent_compile_plan(&config, registry, &first_source, &first, NULL),
                TURBO_AGENT_COMPILE_OK);
    check_equal(turbo_agent_compile_plan(&config, registry, &second_source, &second, NULL),
                TURBO_AGENT_COMPILE_OK);
    check_true(turbo_agent_executable_plan_hash(first) ==
               turbo_agent_executable_plan_hash(second));

    certificate = turbo_agent_executable_plan_certificate_json_value(first);
    check_not_null(certificate);
    check_equal(json_get_string(certificate, "template"), "inspect");
    check_equal(json_get_int(certificate, "template_version", 0), 1);
    check_equal(json_get_string(certificate, "input_contract"),
                "TurboAgent.Inspect.v1");
    check_equal(json_get_string(certificate, "tool"), "repo.inspect");
    check_equal(json_get_string(certificate, "backend"), "runtime_tools");

    turbo_runtime_json_destroy(certificate);
    turbo_agent_executable_plan_destroy(second);
    turbo_agent_executable_plan_destroy(first);
    turbo_runtime_json_destroy(args2);
    turbo_runtime_json_destroy(args1);
    turbo_tool_registry_destroy(registry);
  }
}
