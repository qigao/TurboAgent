#include "tinytest.h"
#include "turbo_agent_compiler.h"
#include "turbo_agent_policy.h"
#include "../src/turbo_agent_tool_executor_internal.h"

#include <stdlib.h>
#include <string.h>

/* The Agent ToolExecutor and AgentCompiler intentionally have distinct
 * execution modes, but must consume the same canonical V6 registry facts.
 * This fixture uses a NULL Runtime only for trusted Host policy admission;
 * it asserts no Journal, approval or durable-effect guarantee. */
typedef struct authority_probe_s {
  int text_calls;
  int json_calls;
} authority_probe_t;

static turbo_tool_status_t authority_text(const char *arguments,
    const turbo_tool_execution_context_t *ctx, char **out, void *user) {
  authority_probe_t *probe = user;
  (void)arguments;
  if (!out || !ctx) return TURBO_TOOL_INVALID_ARGUMENT;
  *out = malloc(3);
  if (!*out) return TURBO_TOOL_OUT_OF_MEMORY;
  memcpy(*out, "ok", 3);
  ++probe->text_calls;
  return TURBO_TOOL_OK;
}

static turbo_tool_status_t authority_json(const json_value_t *arguments,
    const turbo_tool_execution_context_t *ctx, json_value_t **out, void *user) {
  authority_probe_t *probe = user;
  (void)ctx;
  if (!arguments || !out) return TURBO_TOOL_INVALID_ARGUMENT;
  *out = json_clone(arguments);
  if (!*out) return TURBO_TOOL_OUT_OF_MEMORY;
  ++probe->json_calls;
  return TURBO_TOOL_OK;
}

static int register_tool(turbo_tool_registry_t *registry, authority_probe_t *probe,
    const char *name, const char *const *caps, size_t cap_count,
    turbo_tool_idempotency_t idempotency, turbo_tool_effect_flags_t effects) {
  turbo_tool_definition_v6_t def = {0};
  def.struct_size = sizeof(def);
  def.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
  def.definition.name = name;
  def.definition.description = "Selected Host authorization conformance";
  def.definition.parameters_json = "{\"type\":\"object\"}";
  def.definition.strict = 1;
  def.definition.user_data = probe;
  def.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
  def.execution_policy.idempotency = idempotency;
  def.required_capabilities = caps;
  def.required_capability_count = cap_count;
  def.context_handler = authority_text;
  def.json_value_context_handler = authority_json;
  def.result_schema_json = "{\"type\":\"object\"}";
  def.effect_flags = effects;
  return turbo_tool_registry_add_v6(registry, &def);
}

static turbo_agent_tool_execution_t make_call(const char *tool) {
  turbo_agent_tool_execution_t call = {0};
  call.call_id = "call";
  call.turn_key = "turn";
  call.tool_name = tool;
  call.arguments_json = "{}";
  return call;
}

static int execute_host_tool(turbo_agent_tool_executor_t *executor,
    const turbo_tool_registry_t *registry, const turbo_agent_policy_t *policy,
    turbo_agent_tool_execution_t *call) {
  return turbo_agent_tool_executor_execute(executor, NULL, NULL, NULL, NULL,
      NULL, NULL, registry, policy, call, 1);
}

static turbo_agent_compile_status_t compile_inspect(const turbo_tool_registry_t *registry,
    const char *tool, const char *const *caps, size_t cap_count,
    const json_value_t *arguments, turbo_agent_executable_plan_t **out) {
  turbo_agent_compiler_config_t config;
  turbo_agent_typed_plan_t source;
  turbo_agent_compiler_config_init(&config);
  config.allowed_capabilities = caps;
  config.allowed_capability_count = cap_count;
  turbo_agent_typed_plan_init(&source);
  source.template_kind = TURBO_AGENT_TEMPLATE_INSPECT;
  source.step_id = "inspect-authority";
  source.tool_name = tool;
  source.arguments = arguments;
  return turbo_agent_compile_plan(&config, registry, &source, out, NULL);
}

static void free_call(turbo_agent_tool_execution_t *call) {
  free(call->output);
  json_free(call->detail);
  *call = (turbo_agent_tool_execution_t){0};
}

spec("shared RuntimeTools authority admission between Agent and Compiler") {
  static authority_probe_t read_probe, network_probe, write_probe, unknown_probe;
  static turbo_tool_registry_t *registry;
  static turbo_agent_tool_executor_t *executor;
  static json_value_t *args;
  static const char *const runtime_caps[] = {"runtime_tools"};
  static const char *const network_caps[] = {"runtime_tools", "network"};
  static const char *const patch_caps[] = {"patch"};
  static const char *const mystery_caps[] = {"mystery_capability"};

  before_each() {
    read_probe = network_probe = write_probe = unknown_probe = (authority_probe_t){0};
    registry = turbo_tool_registry_create();
    check_not_null(registry);
    check_equal(register_tool(registry, &read_probe, "authority.read", runtime_caps, 1,
        TURBO_TOOL_IDEMPOTENCY_READ_ONLY, TURBO_TOOL_EFFECT_READ), TURBO_TOOL_OK);
    check_equal(register_tool(registry, &network_probe, "authority.network", network_caps, 2,
        TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
        TURBO_TOOL_EFFECT_READ | TURBO_TOOL_EFFECT_NETWORK), TURBO_TOOL_OK);
    check_equal(register_tool(registry, &write_probe, "authority.write", patch_caps, 1,
        TURBO_TOOL_IDEMPOTENCY_NONE, TURBO_TOOL_EFFECT_WRITE), TURBO_TOOL_OK);
    check_equal(register_tool(registry, &unknown_probe, "authority.unknown", mystery_caps, 1,
        TURBO_TOOL_IDEMPOTENCY_READ_ONLY, TURBO_TOOL_EFFECT_PURE), TURBO_TOOL_OK);
    args = json_create_object(); check_not_null(args);
    check_equal(turbo_agent_tool_executor_create(NULL, &executor), SALTS_OK);
  }

  after_each() {
    turbo_agent_tool_executor_destroy(executor); executor = NULL;
    turbo_tool_registry_destroy(registry); registry = NULL;
    json_free(args); args = NULL;
  }

  it("denies the same read tool under both default Host capability gates") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_agent_tool_execution_t call = make_call("authority.read");
    turbo_agent_executable_plan_t *plan = NULL;
    const char *reason = NULL;
    check_equal(turbo_agent_policy_check_tool(&policy, registry,
        "authority.read", &reason), TURBO_AGENT_POLICY_DENY);
    check_equal(reason, "runtime_tools_disabled");
    check_equal(execute_host_tool(executor, registry, &policy, &call), SALTS_OK);
    check_equal(call.status, TURBO_TOOL_ERROR);
    check_equal(call.policy_reason, "runtime_tools_disabled");
    check_null(call.output);
    check_equal(compile_inspect(registry, "authority.read", NULL, 0, args, &plan),
        TURBO_AGENT_COMPILE_CAPABILITY_DENIED);
    check_null(plan);
    check_equal(read_probe.text_calls + read_probe.json_calls, 0);
    free_call(&call);
  }

  it("admits the same read metadata under explicit Host grants, but keeps distinct result owners") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_agent_tool_execution_t call = make_call("authority.read");
    turbo_agent_executable_plan_t *plan = NULL;
    json_value_t *result = NULL;
    policy.allow_runtime_tools = 1;
    check_equal(turbo_agent_policy_check_tool(&policy, registry,
        "authority.read", NULL), TURBO_AGENT_POLICY_ALLOW);
    check_equal(compile_inspect(registry, "authority.read", runtime_caps, 1, args, &plan),
        TURBO_AGENT_COMPILE_OK);
    check_not_null(plan);
    check_equal(turbo_agent_executable_plan_tool_name(plan), "authority.read");
    check_equal(read_probe.text_calls + read_probe.json_calls, 0);
    check_equal(execute_host_tool(executor, registry, &policy, &call), SALTS_OK);
    check_equal(call.status, TURBO_TOOL_OK);
    check_false(call.replayed);
    check_equal(call.output, "ok");
    check_equal(turbo_agent_execute_compiled_plan(plan, NULL, &result), TURBO_TOOL_OK);
    check_not_null(result);
    check_equal(json_type(result), JSON_OBJECT);
    check_equal(read_probe.text_calls, 1);
    check_equal(read_probe.json_calls, 1);
    json_free(result);
    free_call(&call);
    turbo_agent_executable_plan_destroy(plan);
  }

  it("requires network Host grants in both paths for READ|NETWORK metadata") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_agent_tool_execution_t denied = make_call("authority.network");
    turbo_agent_executable_plan_t *plan = NULL;
    json_value_t *result = NULL;
    policy.allow_runtime_tools = 1;
    check_equal(turbo_agent_policy_check_tool(&policy, registry,
        "authority.network", NULL), TURBO_AGENT_POLICY_DENY);
    check_equal(execute_host_tool(executor, registry, &policy, &denied), SALTS_OK);
    check_equal(denied.status, TURBO_TOOL_ERROR);
    check_equal(denied.policy_reason, "network_disabled");
    check_equal(compile_inspect(registry, "authority.network", runtime_caps, 1, args, &plan),
        TURBO_AGENT_COMPILE_CAPABILITY_DENIED);
    check_null(plan);
    check_equal(network_probe.text_calls + network_probe.json_calls, 0);
    free_call(&denied);

    policy.allow_network = 1;
    check_equal(turbo_agent_policy_check_tool(&policy, registry,
        "authority.network", NULL), TURBO_AGENT_POLICY_ALLOW);
    check_equal(compile_inspect(registry, "authority.network", network_caps, 2, args, &plan),
        TURBO_AGENT_COMPILE_OK);
    check_not_null(plan);
    check_equal(turbo_agent_execute_compiled_plan(plan, NULL, &result), TURBO_TOOL_OK);
    check_not_null(result);
    check_equal(network_probe.json_calls, 1);
    check_equal(network_probe.text_calls, 0);
    json_free(result);
    turbo_agent_executable_plan_destroy(plan);
  }

  it("keeps the stricter Inspect effect rule separate from authorized Host mutation") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_agent_tool_execution_t call = make_call("authority.write");
    turbo_agent_executable_plan_t *plan = NULL;
    policy.allow_patch = 1;
    check_equal(turbo_agent_policy_check_tool(&policy, registry,
        "authority.write", NULL), TURBO_AGENT_POLICY_ALLOW);
    check_equal(compile_inspect(registry, "authority.write", patch_caps, 1, args, &plan),
        TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION);
    check_null(plan);
    check_equal(write_probe.text_calls + write_probe.json_calls, 0);
    check_equal(execute_host_tool(executor, registry, &policy, &call), SALTS_OK);
    check_equal(call.status, TURBO_TOOL_OK);
    check_equal(write_probe.text_calls, 1);
    check_equal(write_probe.json_calls, 0);
    free_call(&call);
  }

  it("rejects unrecognized capability metadata before either callback") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_agent_tool_execution_t call = make_call("authority.unknown");
    turbo_agent_executable_plan_t *plan = NULL;
    const char *reason = NULL;
    policy.allow_runtime_tools = 1;
    check_equal(turbo_agent_policy_check_tool(&policy, registry,
        "authority.unknown", &reason), TURBO_AGENT_POLICY_DENY);
    check_equal(reason, "unknown_tool_capability");
    check_equal(execute_host_tool(executor, registry, &policy, &call), SALTS_OK);
    check_equal(call.status, TURBO_TOOL_ERROR);
    check_equal(call.policy_reason, "unknown_tool_capability");
    check_equal(compile_inspect(registry, "authority.unknown", runtime_caps, 1, args, &plan),
        TURBO_AGENT_COMPILE_CAPABILITY_DENIED);
    check_null(plan);
    check_equal(unknown_probe.text_calls + unknown_probe.json_calls, 0);
    free_call(&call);
  }
}
