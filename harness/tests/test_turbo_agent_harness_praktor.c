#include "tinytest.h"

#include "turbo_agent_harness.h"
#include "turbo_runtime_json.h"
#include "turbo_tool_registry.h"

#include <string.h>
#include <salts/thread_pool.h>

typedef struct praktor_probe_s {
  int tool_calls;
} praktor_probe_t;

static turbo_tool_status_t praktor_probe_tool(
    const json_value_t *arguments, const turbo_tool_execution_context_t *context,
    json_value_t **out_result, void *user_data) {
  praktor_probe_t *probe = (praktor_probe_t *)user_data;
  (void)arguments;
  (void)context;
  if (probe) ++probe->tool_calls;
  if (out_result) *out_result = json_create_object();
  return out_result && *out_result ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static int praktor_no_model(const char *request, char **response, void *user) {
  (void)request;
  (void)response;
  (void)user;
  return -1;
}

static turbo_tool_status_t praktor_add_tool(
    turbo_tool_registry_t *registry, const char *name,
    turbo_tool_idempotency_t idempotency, turbo_tool_effect_flags_t effects,
    praktor_probe_t *probe) {
  static const char *const capabilities[] = {"runtime_tools"};
  turbo_tool_definition_v6_t definition = {0};
  definition.struct_size = sizeof(definition);
  definition.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
  definition.definition.name = name;
  definition.definition.description = "Host-approved Praktor preview fixture";
  definition.definition.parameters_json =
      "{\"type\":\"object\",\"additionalProperties\":true}";
  definition.definition.strict = 1;
  definition.definition.user_data = probe;
  definition.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
  definition.execution_policy.idempotency = idempotency;
  definition.required_capabilities = capabilities;
  definition.required_capability_count = 1;
  definition.json_value_context_handler = praktor_probe_tool;
  definition.effect_flags = effects;
  return turbo_tool_registry_add_v6(registry, &definition);
}

static turbo_tool_registry_t *praktor_registry(
    praktor_probe_t *probe, turbo_tool_effect_flags_t change_effect,
    turbo_tool_effect_flags_t inspect_effect) {
  turbo_tool_registry_t *registry = turbo_tool_registry_create();
  if (!registry) return NULL;
  if (praktor_add_tool(registry, "repo.inspect", TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
                       inspect_effect, probe) != TURBO_TOOL_OK ||
      praktor_add_tool(registry, "repo.change", TURBO_TOOL_IDEMPOTENCY_NONE,
                       change_effect, probe) != TURBO_TOOL_OK ||
      praktor_add_tool(registry, "repo.verify", TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
                       TURBO_TOOL_EFFECT_READ, probe) != TURBO_TOOL_OK) {
    turbo_tool_registry_destroy(registry);
    return NULL;
  }
  return registry;
}

static turbo_agent_harness_t *praktor_harness(
    cmeta_threadpool_t *pool, turbo_tool_registry_t *registry) {
  turbo_agent_session_config_t session = {0};
  turbo_agent_app_config_t app = {0};
  turbo_agent_harness_config_t config;
  session.runtime_store = turbo_agent_runtime_store_memory_create();
  session.agent_config.model = "test";
  session.agent_config.transport_fn = praktor_no_model;
  session.agent_config.tool_registry = registry;
  app.session_config = &session;
  turbo_agent_harness_config_init(&config);
  config.app_config = &app;
  config.executor = pool;
  return turbo_agent_harness_create(&config);
}

static void praktor_slot(
    turbo_agent_template_slot_t *slot, const char *tool, const json_value_t *args) {
  turbo_agent_template_slot_init(slot);
  slot->tool_name = tool;
  slot->arguments = args;
}

static void praktor_change_source(
    turbo_agent_change_source_t *source, const json_value_t *args) {
  turbo_agent_change_source_init(source);
  praktor_slot(&source->inspect, "repo_inspect", args);
  praktor_slot(&source->change, "repo_change", args);
  praktor_slot(&source->verify, "repo_verify", args);
}

static void praktor_repair_source(
    turbo_agent_repair_source_t *source, const json_value_t *args) {
  turbo_agent_repair_source_init(source);
  praktor_slot(&source->diagnose, "repo.inspect", args);
  praktor_slot(&source->change, "repo.change", args);
  praktor_slot(&source->verify, "repo.verify", args);
}

spec("Harness explicit multi-step Praktor admission") {
  it("returns a deterministic Change source with frozen approval/checkpoint gates and no callbacks") {
    praktor_probe_t probe = {0};
    const char *allowed[] = {"runtime_tools"};
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_tool_registry_t *registry = praktor_registry(
        &probe, TURBO_TOOL_EFFECT_WRITE, TURBO_TOOL_EFFECT_READ);
    turbo_agent_harness_t *harness;
    turbo_agent_compiler_config_t config;
    turbo_agent_change_source_t source;
    turbo_agent_compile_diagnostic_t diagnostic = {0};
    json_value_t *args = json_create_object();
    json_value_t *first = NULL, *second = NULL;
    const json_value_t *cert, *dag, *steps, *change;

    check_not_null(pool);
    check_not_null(registry);
    check_not_null(args);
    harness = praktor_harness(pool, registry);
    check_not_null(harness);
    praktor_change_source(&source, args);
    turbo_agent_compiler_config_init(&config);
    config.allowed_capabilities = allowed;
    config.allowed_capability_count = 1;

    check_equal(turbo_agent_harness_prepare_praktor_template(
        harness, &config, &source, NULL, &diagnostic, &first), SALTS_OK);
    check_not_null(first);
    check_equal(diagnostic.status, TURBO_AGENT_COMPILE_OK);
    check_equal(json_get_string(first, "backend"), "praktor_inline");
    check_false(json_get_bool(first, "executable", true));
    check_contains(json_get_string(first, "source_id"), "turboagent:plan:");
    check_contains(json_get_string(first, "yaml"), "repo.change");
    check_equal(json_get_int(first, "yaml_bytes", -1),
                (int)strlen(json_get_string(first, "yaml")));
    cert = json_object_get(first, "certificate");
    dag = json_object_get(cert, "dag");
    steps = json_object_get(dag, "steps");
    check_equal(json_array_size(steps), 3);
    change = json_array_get(steps, 1);
    check_equal(json_get_string(change, "tool"), "repo.change");
    check_equal(json_get_int(change, "flags", -1),
                TURBO_AGENT_DAG_STEP_APPROVAL_BEFORE |
                TURBO_AGENT_DAG_STEP_CHECKPOINT_AFTER);
    check_equal(json_get_int(change, "retry_limit", -1), 0);
    check_equal(probe.tool_calls, 0);

    check_equal(turbo_agent_harness_prepare_praktor_template(
        harness, &config, &source, NULL, &diagnostic, &second), SALTS_OK);
    check_equal(json_get_string(second, "yaml"), json_get_string(first, "yaml"));
    check_equal(json_get_string(second, "source_id"),
                json_get_string(first, "source_id"));
    check_equal(probe.tool_calls, 0);
    json_free(second);
    json_free(first);
    json_free(args);
    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }

  it("freezes Repair plan generation and replan budget without invoking any tool") {
    praktor_probe_t probe = {0};
    const char *allowed[] = {"runtime_tools"};
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_tool_registry_t *registry = praktor_registry(
        &probe, TURBO_TOOL_EFFECT_EXTERNAL_MUTATION, TURBO_TOOL_EFFECT_PURE);
    turbo_agent_harness_t *harness;
    turbo_agent_repair_source_t source;
    turbo_agent_compiler_config_t config;
    json_value_t *args = json_create_object();
    json_value_t *bundle = NULL;
    const json_value_t *cert, *dag;

    check_not_null(pool);
    check_not_null(registry);
    harness = praktor_harness(pool, registry);
    check_not_null(harness);
    praktor_repair_source(&source, args);
    source.plan_version = 2;
    source.max_replans = 2;
    turbo_agent_compiler_config_init(&config);
    config.allowed_capabilities = allowed;
    config.allowed_capability_count = 1;
    check_equal(turbo_agent_harness_prepare_praktor_template(
        harness, &config, NULL, &source, NULL, &bundle), SALTS_OK);
    cert = json_object_get(bundle, "certificate");
    dag = json_object_get(cert, "dag");
    check_equal(json_get_int(cert, "plan_version", -1), 2);
    check_equal(json_get_int(cert, "max_replans", -1), 2);
    check_equal(json_get_int(dag, "plan_generation", -1), 2);
    check_false(json_get_bool(bundle, "executable", true));
    check_equal(probe.tool_calls, 0);
    json_free(bundle);
    json_free(args);
    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }

  it("rejects unknown effects, hidden retries and denied capabilities before lowering") {
    const char *allowed[] = {"runtime_tools"};
    for (int scenario = 0; scenario < 4; ++scenario) {
      praktor_probe_t probe = {0};
      turbo_tool_registry_t *registry = praktor_registry(
          &probe, scenario == 0 ? TURBO_TOOL_EFFECT_UNKNOWN : TURBO_TOOL_EFFECT_WRITE,
          scenario == 1 ? TURBO_TOOL_EFFECT_UNKNOWN : TURBO_TOOL_EFFECT_READ);
      cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
      cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
      turbo_agent_harness_t *harness;
      turbo_agent_compiler_config_t config;
      turbo_agent_change_source_t source;
      json_value_t *args = json_create_object();
      json_value_t *bundle = NULL;
      turbo_agent_compile_diagnostic_t diagnostic = {0};

      check_not_null(pool);
      check_not_null(registry);
      harness = praktor_harness(pool, registry);
      check_not_null(harness);
      praktor_change_source(&source, args);
      turbo_agent_compiler_config_init(&config);
      config.allowed_capabilities = scenario == 3 ? NULL : allowed;
      config.allowed_capability_count = scenario == 3 ? 0 : 1;
      if (scenario == 2) source.verify.retry_limit = 1;
      check_equal(turbo_agent_harness_prepare_praktor_template(
          harness, &config, &source, NULL, &diagnostic, &bundle), SALTS_EPROTO);
      check_null(bundle);
      check_equal(probe.tool_calls, 0);
      json_free(args);
      turbo_agent_harness_release(harness);
      turbo_tool_registry_destroy(registry);
      cmeta_threadpool_destroy(pool);
    }
  }

  it("rejects ambiguous source selection with no callback or executable bundle") {
    praktor_probe_t probe = {0};
    const char *allowed[] = {"runtime_tools"};
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_tool_registry_t *registry = praktor_registry(
        &probe, TURBO_TOOL_EFFECT_WRITE, TURBO_TOOL_EFFECT_READ);
    turbo_agent_harness_t *harness;
    turbo_agent_compiler_config_t config;
    turbo_agent_change_source_t change;
    turbo_agent_repair_source_t repair;
    json_value_t *args = json_create_object(), *bundle = NULL;
    turbo_agent_compiler_config_init(&config);
    config.allowed_capabilities = allowed;
    config.allowed_capability_count = 1;
    praktor_change_source(&change, args);
    praktor_repair_source(&repair, args);
    check_not_null(registry);
    check_not_null(pool);
    harness = praktor_harness(pool, registry);
    check_not_null(harness);
    check_equal(turbo_agent_harness_prepare_praktor_template(
        harness, &config, NULL, NULL, NULL, &bundle), SALTS_EINVAL);
    check_null(bundle);
    check_equal(turbo_agent_harness_prepare_praktor_template(
        harness, &config, &change, &repair, NULL, &bundle), SALTS_EINVAL);
    check_null(bundle);
    check_equal(probe.tool_calls, 0);
    json_free(args);
    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }
}
