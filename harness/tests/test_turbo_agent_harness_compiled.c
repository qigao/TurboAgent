#include "tinytest.h"

#include "turbo_agent_harness.h"
#include "turbo_agent_policy.h"
#include "turbo_agent_session.h"
#include "turbo_tool_registry.h"
#include "turbo_runtime_json.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <salts/thread_pool.h>

static const char compiled_inspect_source[] = "{\"template_id\":\"inspect\",\"step_id\":\"read-1\",\"tool\":\"repo_inspect\",\"arguments_json\":\"{\\\"value\\\":\\\"hello\\\"}\"}";
static const char compiled_inspect_bad_args[] = "{\"template_id\":\"inspect\",\"step_id\":\"read-1\",\"tool\":\"repo_inspect\",\"arguments_json\":\"{\\\"extra\\\":1}\"}";
static const char compiled_inspect_unknown_tool[] = "{\"template_id\":\"inspect\",\"step_id\":\"read-1\",\"tool\":\"nonexistent_tool\",\"arguments_json\":\"{}\"}";

typedef struct compiled_inspect_probe_s {
  int calls;
  int produce_invalid_result;
} compiled_inspect_probe_t;

static int compiled_harness_transport(const char *request, char **response, void *user) {
  (void)request;
  (void)response;
  (void)user;
  return -1;
}

static int compiled_inspect_handler(const json_value_t *arguments,
                                    json_value_t **out_result, void *user) {
  compiled_inspect_probe_t *probe = (compiled_inspect_probe_t *)user;
  json_value_t *result;
  if (!probe || !out_result || !arguments ||
      !json_get_string(arguments, "value")) return -1;
  ++probe->calls;
  if (probe->produce_invalid_result) {
    *out_result = json_create_string("unexpected type");
    return *out_result ? 0 : -1;
  }
  result = json_create_object();
  if (!result || turbo_runtime_json_object_set(result, "ok", json_create_bool(1)) !=
                     TURBO_RUNTIME_JSON_OK) {
    json_free(result);
    return -1;
  }
  *out_result = result;
  return 0;
}

static turbo_tool_status_t register_compiled_inspect(
    turbo_tool_registry_t *registry, compiled_inspect_probe_t *probe,
    turbo_tool_effect_flags_t effects, int strict_result) {
  const char *required[] = {"runtime_tools"};
  turbo_tool_definition_v6_t definition = {0};
  definition.struct_size = sizeof(definition);
  definition.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
  definition.definition.name = "repo.inspect";
  definition.definition.description = "Read-only Harness tool";
  definition.definition.parameters_json =
      "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"string\"}},"
      "\"required\":[\"value\"],\"additionalProperties\":false}";
  definition.definition.json_value_handler = compiled_inspect_handler;
  definition.definition.user_data = probe;
  definition.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
  definition.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
  definition.required_capabilities = required;
  definition.required_capability_count = 1;
  definition.result_schema_json =
      "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"}},"
      "\"required\":[\"ok\"],\"additionalProperties\":false}";
  definition.strict_result = strict_result;
  definition.effect_flags = effects;
  return turbo_tool_registry_add_v6(registry, &definition);
}

static turbo_agent_harness_t *make_compiled_harness(
    cmeta_threadpool_t *pool, turbo_tool_registry_t *registry) {
  turbo_agent_session_config_t session_config = {0};
  turbo_agent_app_config_t app_config = {0};
  turbo_agent_harness_config_t config;
  session_config.runtime_store = turbo_agent_runtime_store_memory_create();
  session_config.agent_config.model = "gpt-5.4";
  session_config.agent_config.transport_fn = compiled_harness_transport;
  session_config.agent_config.tool_registry = registry;
  app_config.session_config = &session_config;
  turbo_agent_harness_config_init(&config);
  config.app_config = &app_config;
  config.executor = pool;
  return turbo_agent_harness_create(&config);
}

static int set_compiled_host_policy(turbo_agent_harness_t *harness, int allow_runtime_tools) {
  turbo_agent_policy_t policy = turbo_agent_policy_default();
  turbo_agent_t *agent = turbo_agent_session_agent(
      turbo_agent_app_session(turbo_agent_harness_app(harness)));
  policy.allow_custom_tools = 0;
  policy.allow_runtime_tools = allow_runtime_tools;
  policy.allow_delegate = 0;
  policy.allow_network = 0;
  policy.allow_shell = 0;
  policy.allow_patch = 0;
  policy.allow_outside_workspace = 0;
  return agent ? turbo_agent_set_tool_policy(agent, &policy) : -1;
}

static int run_compiled_inspect(turbo_agent_harness_t *harness,
                                const char *source, const char *const *allowed,
                                size_t allowed_count,
                                turbo_agent_compile_diagnostic_t *diagnostic,
                                turbo_tool_status_t *tool_status,
                                json_value_t **result,
                                json_value_t **certificate) {
  turbo_agent_compiler_config_t config;
  turbo_agent_compiler_config_init(&config);
  config.allowed_capabilities = allowed;
  config.allowed_capability_count = allowed_count;
  return turbo_agent_harness_execute_compiled_inspect_json(
      harness, &config, source, strlen(source), NULL, diagnostic,
      tool_status, result, certificate);
}

spec("Harness Phase-1 compiled Inspect boundary") {
  it("compiles a DataBind source, freezes authority and verifies the result") {
    const char *allowed[] = {"runtime_tools"};
    compiled_inspect_probe_t probe = {0};
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_harness_t *harness;
    turbo_agent_compile_diagnostic_t diagnostic = {0};
    turbo_tool_status_t tool_status = TURBO_TOOL_ERROR;
    json_value_t *result = NULL;
    json_value_t *certificate = NULL;

    check_not_null(pool);
    check_not_null(registry);
    check_equal(register_compiled_inspect(registry, &probe, TURBO_TOOL_EFFECT_READ, 1), TURBO_TOOL_OK);
    harness = make_compiled_harness(pool, registry);
    check_not_null(harness);
    check_equal(set_compiled_host_policy(harness, 1), 0);
    {
      turbo_agent_t *agent = turbo_agent_session_agent(
          turbo_agent_app_session(turbo_agent_harness_app(harness)));
      turbo_agent_tool_authority_snapshot_t first = {0};
      turbo_agent_tool_authority_snapshot_t second = {0};
      turbo_agent_policy_t denied = turbo_agent_policy_default();
      denied.allow_runtime_tools = 0;
      check_equal(turbo_agent_tool_authority_snapshot_get(agent, &first), 0);
      check_equal(turbo_agent_tool_authority_snapshot_is_current(agent, &first), 1);
      check_equal(turbo_agent_set_tool_policy(agent, &denied), 0);
      check_equal(turbo_agent_tool_authority_snapshot_is_current(agent, &first), 0);
      check_equal(set_compiled_host_policy(harness, 1), 0);
      check_equal(turbo_agent_tool_authority_snapshot_get(agent, &second), 0);
      check_not_equal(first.generation, second.generation);
      check_equal(turbo_agent_tool_authority_snapshot_is_current(agent, &second), 1);
    }

    check_equal(run_compiled_inspect(harness, compiled_inspect_source,
        allowed, 1, &diagnostic, &tool_status, &result, &certificate), SALTS_OK);
    check_equal(tool_status, TURBO_TOOL_OK);
    check_equal(diagnostic.status, TURBO_AGENT_COMPILE_OK);
    check_true(json_get_bool(result, "ok", false));
    check_not_null(certificate);
    check_equal(probe.calls, 1);

    json_free(result);
    json_free(certificate);
    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }

  it("rejects schema errors, denied capability and unknown tool with no callbacks") {
    const char *allowed[] = {"runtime_tools"};
    compiled_inspect_probe_t probe = {0};
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_harness_t *harness;
    turbo_agent_compile_diagnostic_t diagnostic = {0};
    turbo_tool_status_t tool_status = TURBO_TOOL_ERROR;
    json_value_t *result = NULL;
    json_value_t *certificate = NULL;

    check_not_null(pool);
    check_not_null(registry);
    check_equal(register_compiled_inspect(registry, &probe, TURBO_TOOL_EFFECT_READ, 1), TURBO_TOOL_OK);
    harness = make_compiled_harness(pool, registry);
    check_not_null(harness);
    check_equal(set_compiled_host_policy(harness, 1), 0);

    check_equal(run_compiled_inspect(harness, compiled_inspect_bad_args,
        allowed, 1, &diagnostic, &tool_status, &result, &certificate), SALTS_EPROTO);
    check_null(result);
    check_null(certificate);
    check_equal(diagnostic.status, TURBO_AGENT_COMPILE_TOOL_ARGUMENTS_INVALID);

    check_equal(run_compiled_inspect(harness, compiled_inspect_source,
        NULL, 0, &diagnostic, &tool_status, &result, &certificate), SALTS_EPROTO);
    check_equal(diagnostic.status, TURBO_AGENT_COMPILE_CAPABILITY_DENIED);

    check_equal(run_compiled_inspect(harness, compiled_inspect_unknown_tool,
        allowed, 1, &diagnostic, &tool_status, &result, &certificate), SALTS_EPROTO);
    check_equal(diagnostic.status, TURBO_AGENT_COMPILE_UNRESOLVED_TOOL);
    check_equal(probe.calls, 0);

    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }

  it("rejects UNKNOWN/mutating effects and undeclared strict result contract before invoking") {
    const char *allowed[] = {"runtime_tools"};
    const turbo_tool_effect_flags_t rejected_effects[] = {
        TURBO_TOOL_EFFECT_UNKNOWN, TURBO_TOOL_EFFECT_WRITE,
        TURBO_TOOL_EFFECT_READ | TURBO_TOOL_EFFECT_PROCESS};
    for (size_t i = 0; i < sizeof(rejected_effects) / sizeof(rejected_effects[0]) + 1; ++i) {
      compiled_inspect_probe_t probe = {0};
      cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
      cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
      turbo_tool_registry_t *registry = turbo_tool_registry_create();
      turbo_agent_harness_t *harness;
      turbo_agent_compile_diagnostic_t diagnostic = {0};
      turbo_tool_status_t tool_status = TURBO_TOOL_ERROR;
      json_value_t *result = NULL;
      json_value_t *certificate = NULL;
      const int missing_strict = i == sizeof(rejected_effects) / sizeof(rejected_effects[0]);

      check_not_null(pool);
      check_not_null(registry);
      check_equal(register_compiled_inspect(
          registry, &probe, missing_strict ? TURBO_TOOL_EFFECT_READ : rejected_effects[i],
          !missing_strict), TURBO_TOOL_OK);
      harness = make_compiled_harness(pool, registry);
      check_not_null(harness);
      check_equal(set_compiled_host_policy(harness, 1), 0);

      check_equal(run_compiled_inspect(harness, compiled_inspect_source,
          allowed, 1, &diagnostic, &tool_status, &result, &certificate), SALTS_EPROTO);
      check_null(result);
      check_null(certificate);
      check_equal(probe.calls, 0);
      turbo_agent_harness_release(harness);
      turbo_tool_registry_destroy(registry);
      cmeta_threadpool_destroy(pool);
    }
  }

  it("does not retry a read-only tool after a result schema mismatch") {
    const char *allowed[] = {"runtime_tools"};
    compiled_inspect_probe_t probe = {.produce_invalid_result = 1};
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_harness_t *harness;
    turbo_agent_compile_diagnostic_t diagnostic = {0};
    turbo_tool_status_t tool_status = TURBO_TOOL_OK;
    json_value_t *result = NULL;
    json_value_t *certificate = NULL;

    check_not_null(pool);
    check_not_null(registry);
    check_equal(register_compiled_inspect(registry, &probe, TURBO_TOOL_EFFECT_READ, 1), TURBO_TOOL_OK);
    harness = make_compiled_harness(pool, registry);
    check_not_null(harness);
    check_equal(set_compiled_host_policy(harness, 1), 0);
    check_equal(run_compiled_inspect(harness, compiled_inspect_source,
        allowed, 1, &diagnostic, &tool_status, &result, &certificate), SALTS_EPROTO);
    check_equal(tool_status, TURBO_TOOL_ERROR);
    check_null(result);
    check_not_null(certificate);
    check_equal(probe.calls, 1);

    json_free(certificate);
    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }

  it("does not let compiler allowlists widen Agent Host Tool Authority") {
    const char *allowed[] = {"runtime_tools"};
    compiled_inspect_probe_t probe = {0};
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_harness_t *harness;
    turbo_agent_compile_diagnostic_t diagnostic = {0};
    turbo_tool_status_t tool_status = TURBO_TOOL_OK;
    json_value_t *result = NULL;
    json_value_t *certificate = NULL;

    check_not_null(pool);
    check_not_null(registry);
    check_equal(register_compiled_inspect(registry, &probe, TURBO_TOOL_EFFECT_READ, 1), TURBO_TOOL_OK);
    harness = make_compiled_harness(pool, registry);
    check_not_null(harness);
    check_equal(set_compiled_host_policy(harness, 0), 0);

    check_equal(run_compiled_inspect(harness, compiled_inspect_source,
        allowed, 1, &diagnostic, &tool_status, &result, &certificate), SALTS_EPROTO);
    check_equal(diagnostic.status, TURBO_AGENT_COMPILE_CAPABILITY_DENIED);
    check_null(result);
    check_null(certificate);
    check_equal(probe.calls, 0);

    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }
}
