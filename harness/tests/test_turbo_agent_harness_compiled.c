#include "tinytest.h"

#include "turbo_agent_harness.h"
#include "turbo_tool_registry.h"
#include "turbo_runtime_json.h"
#include "turbo_runtime_store_sqlite.h"
#include <stdio.h>

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

static turbo_agent_harness_t *make_compiled_harness_with_store(
    cmeta_threadpool_t *pool, turbo_tool_registry_t *registry,
    turbo_agent_runtime_store_t store, const char *thread_id) {
  turbo_agent_session_config_t session_config = {0};
  turbo_agent_app_config_t app_config = {0};
  turbo_agent_harness_config_t config;
  session_config.runtime_store = store;
  session_config.thread_id = thread_id;
  session_config.agent_config.model = "gpt-5.4";
  session_config.agent_config.transport_fn = compiled_harness_transport;
  session_config.agent_config.tool_registry = registry;
  app_config.session_config = &session_config;
  turbo_agent_harness_config_init(&config);
  config.app_config = &app_config;
  config.executor = pool;
  return turbo_agent_harness_create(&config);
}

static turbo_agent_harness_t *make_compiled_harness(
    cmeta_threadpool_t *pool, turbo_tool_registry_t *registry) {
  return make_compiled_harness_with_store(
      pool, registry, turbo_agent_runtime_store_memory_create(), NULL);
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

static int run_durable_inspect(turbo_agent_harness_t *harness,
                               const char *source, const char *attempt_id,
                               int *replayed, turbo_tool_status_t *status,
                               json_value_t **result, json_value_t **certificate) {
  const char *allowed[] = {"runtime_tools"};
  turbo_agent_compiler_config_t config;
  turbo_agent_compiler_config_init(&config);
  config.allowed_capabilities = allowed;
  config.allowed_capability_count = 1;
  return turbo_agent_harness_execute_compiled_inspect_durable_json(
      harness, &config, source, strlen(source), NULL, attempt_id, replayed,
      NULL, status, result, certificate);
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
}


typedef struct compiled_fault_store_s {
  turbo_agent_runtime_store_t inner;
  int fail_committed;
  int failed_writes;
} compiled_fault_store_t;

static int compiled_fault_put(void *user, const char *collection, const char *id,
                              const char *record_json) {
  compiled_fault_store_t *fault = (compiled_fault_store_t *)user;
  if (fault->fail_committed &&
      strcmp(collection, "compiled_inspect_attempts") == 0) {
    json_value_t *record = json_parse(record_json, strlen(record_json));
    const char *phase = record ? json_get_string(record, "phase") : NULL;
    int reject = phase && strcmp(phase, "committed") == 0;
    json_free(record);
    if (reject) {
      ++fault->failed_writes;
      return SALTS_EIO;
    }
  }
  return fault->inner.put(fault->inner.user_data, collection, id, record_json);
}

static int compiled_fault_get(void *user, const char *collection, const char *id,
                              char **out_json) {
  compiled_fault_store_t *fault = (compiled_fault_store_t *)user;
  return fault->inner.get(fault->inner.user_data, collection, id, out_json);
}

static int compiled_fault_list(void *user, const char *collection, const char *filter_key,
                               const char *filter_value, char **out_json) {
  compiled_fault_store_t *fault = (compiled_fault_store_t *)user;
  return fault->inner.list(fault->inner.user_data, collection, filter_key, filter_value,
                           out_json);
}

static void compiled_fault_destroy(void *user) {
  compiled_fault_store_t *fault = (compiled_fault_store_t *)user;
  if (fault->inner.user_data_free)
    fault->inner.user_data_free(fault->inner.user_data);
  fault->inner = (turbo_agent_runtime_store_t){0};
}

spec("Harness durable compiled Inspect attempt") {
  it("replays committed results across bounded SQLite reopen without another callback") {
    static const char changed_source[] =
        "{\"template_id\":\"inspect\",\"step_id\":\"read-1\","
        "\"tool\":\"repo_inspect\",\"arguments_json\":\"{\\\"value\\\":\\\"changed\\\"}\"}";
    compiled_inspect_probe_t probe = {0};
    turbo_runtime_sqlite_config_t sqlite_config;
    turbo_agent_runtime_store_t store = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_agent_harness_t *harness;
    turbo_agent_runtime_t *runtime;
    json_value_t *result = NULL, *certificate = NULL, *record = NULL;
    char *directory = tt_make_temp_dir("compiled-inspect-attempt");
    char path[1024];
    int replayed = -1;
    turbo_tool_status_t tool_status = TURBO_TOOL_ERROR;

    check_not_null(directory);
    check_true(snprintf(path, sizeof(path), "%s/attempts.db", directory) <
               (int)sizeof(path));
    turbo_runtime_sqlite_config_init(&sqlite_config);
    sqlite_config.path = path;
    check_equal(turbo_runtime_store_sqlite_open(&sqlite_config, &store), SALTS_OK);
    check_not_null(pool);
    check_not_null(registry);
    check_equal(register_compiled_inspect(
        registry, &probe, TURBO_TOOL_EFFECT_READ, 1), TURBO_TOOL_OK);
    harness = make_compiled_harness_with_store(
        pool, registry, store, "compiled-test-thread");
    check_not_null(harness);
    runtime = turbo_agent_session_runtime(
        turbo_agent_app_session(turbo_agent_harness_app(harness)));
    check_not_null(runtime);

    check_equal(run_durable_inspect(harness, compiled_inspect_source, "run_one",
        &replayed, &tool_status, &result, &certificate), SALTS_OK);
    check_equal(replayed, 0);
    check_equal(tool_status, TURBO_TOOL_OK);
    check_true(json_get_bool(result, "ok", false));
    check_equal(probe.calls, 1);
    json_free(result); result = NULL;
    json_free(certificate); certificate = NULL;

    check_equal(turbo_agent_runtime_store_get_json_status(
        runtime, "compiled_inspect_attempts", "020_compiled-test-thread_run_one",
        &record), SALTS_OK);
    check_equal(json_get_string(record, "phase"), "committed");
    json_free(record); record = NULL;

    check_equal(run_durable_inspect(harness, compiled_inspect_source, "run_one",
        &replayed, &tool_status, &result, &certificate), SALTS_OK);
    check_equal(replayed, 1);
    check_equal(probe.calls, 1);
    check_true(json_get_bool(result, "ok", false));
    json_free(result); result = NULL;
    json_free(certificate); certificate = NULL;

    /* Same attempt key with a different compiled source is identity collision,
     * not an invitation to execute a new tool call. */
    check_equal(run_durable_inspect(harness, changed_source, "run_one",
        &replayed, &tool_status, &result, &certificate), SALTS_EPROTO);
    check_equal(replayed, 0);
    check_equal(probe.calls, 1);
    json_free(certificate); certificate = NULL;

    /* A rejected plan does not create a durable STARTED record. */
    check_equal(run_durable_inspect(harness, compiled_inspect_bad_args, "bad_plan",
        &replayed, &tool_status, &result, &certificate), SALTS_EPROTO);
    check_equal(turbo_agent_runtime_store_get_json_status(
        runtime, "compiled_inspect_attempts", "020_compiled-test-thread_bad_plan",
        &record), SALTS_ENOENT);
    check_equal(probe.calls, 1);

    turbo_agent_harness_release(harness);
    cmeta_threadpool_destroy(pool);
    pool = cmeta_threadpool_create_with_config(&pool_config);
    sqlite_config.create_if_missing = 0;
    check_equal(turbo_runtime_store_sqlite_open(&sqlite_config, &store), SALTS_OK);
    harness = make_compiled_harness_with_store(
        pool, registry, store, "compiled-test-thread");
    check_not_null(harness);
    check_equal(run_durable_inspect(harness, compiled_inspect_source, "run_one",
        &replayed, &tool_status, &result, &certificate), SALTS_OK);
    check_equal(replayed, 1);
    check_equal(probe.calls, 1);
    json_free(result); result = NULL;
    json_free(certificate); certificate = NULL;

    runtime = turbo_agent_session_runtime(
        turbo_agent_app_session(turbo_agent_harness_app(harness)));
    check_equal(turbo_agent_runtime_store_get_json_status(
        runtime, "compiled_inspect_attempts", "020_compiled-test-thread_run_one",
        &record), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_string(
        record, "phase", "started"), TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_agent_runtime_store_put_json(
        runtime, "compiled_inspect_attempts", "020_compiled-test-thread_run_one",
        record), SALTS_OK);
    json_free(record); record = NULL;

    check_equal(run_durable_inspect(harness, compiled_inspect_source, "run_one",
        &replayed, &tool_status, &result, &certificate), SALTS_EBUSY);
    check_equal(replayed, 0);
    check_equal(tool_status, TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
    check_equal(probe.calls, 1);
    json_free(certificate);

    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
    tt_remove_tree(directory);
    free(directory);
  }

  it("persists invalid results as terminal errors and never retries them") {
    compiled_inspect_probe_t probe = {.produce_invalid_result = 1};
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_agent_harness_t *harness;
    json_value_t *result = NULL, *certificate = NULL;
    int replayed = -1;
    turbo_tool_status_t tool_status = TURBO_TOOL_OK;

    check_not_null(pool);
    check_not_null(registry);
    check_equal(register_compiled_inspect(
        registry, &probe, TURBO_TOOL_EFFECT_READ, 1), TURBO_TOOL_OK);
    harness = make_compiled_harness_with_store(
        pool, registry, store, "compiled-test-thread");
    check_not_null(harness);
    check_equal(run_durable_inspect(harness, compiled_inspect_source, "failed_result",
        &replayed, &tool_status, &result, &certificate), SALTS_EPROTO);
    check_equal(probe.calls, 1);
    check_equal(tool_status, TURBO_TOOL_ERROR);
    check_equal(replayed, 0);
    check_null(result);
    check_not_null(certificate);
    json_free(certificate); certificate = NULL;

    check_equal(run_durable_inspect(harness, compiled_inspect_source, "failed_result",
        &replayed, &tool_status, &result, &certificate), SALTS_EPROTO);
    check_equal(replayed, 1);
    check_equal(tool_status, TURBO_TOOL_ERROR);
    check_equal(probe.calls, 1);
    check_null(result);
    json_free(certificate);

    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }
  it("keeps STARTED after a failed commit and never re-invokes the backend") {
    compiled_inspect_probe_t probe = {0};
    compiled_fault_store_t fault = {0};
    turbo_agent_runtime_store_t callbacks = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_agent_harness_t *harness;
    turbo_agent_runtime_t *runtime;
    json_value_t *result = NULL, *certificate = NULL, *record = NULL;
    int replayed = -1;
    turbo_tool_status_t tool_status = TURBO_TOOL_OK;

    check_not_null(pool);
    check_not_null(registry);
    check_equal(register_compiled_inspect(
        registry, &probe, TURBO_TOOL_EFFECT_READ, 1), TURBO_TOOL_OK);
    fault.inner = turbo_agent_runtime_store_memory_create();
    check_not_null(fault.inner.put);
    fault.fail_committed = 1;
    callbacks = (turbo_agent_runtime_store_t){
        compiled_fault_put, compiled_fault_get, compiled_fault_list,
        &fault, compiled_fault_destroy};
    harness = make_compiled_harness_with_store(
        pool, registry, callbacks, "compiled-test-thread");
    check_not_null(harness);

    check_equal(run_durable_inspect(harness, compiled_inspect_source, "fault_run",
        &replayed, &tool_status, &result, &certificate), SALTS_EIO);
    check_equal(probe.calls, 1);
    check_equal(replayed, 0);
    check_equal(tool_status, TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
    check_null(result);
    check_not_null(certificate);
    check_equal(fault.failed_writes, 1);
    json_free(certificate); certificate = NULL;

    runtime = turbo_agent_session_runtime(
        turbo_agent_app_session(turbo_agent_harness_app(harness)));
    check_equal(turbo_agent_runtime_store_get_json_status(
        runtime, "compiled_inspect_attempts",
        "020_compiled-test-thread_fault_run", &record), SALTS_OK);
    check_equal(json_get_string(record, "phase"), "started");
    json_free(record); record = NULL;

    fault.fail_committed = 0;
    check_equal(run_durable_inspect(harness, compiled_inspect_source, "fault_run",
        &replayed, &tool_status, &result, &certificate), SALTS_EBUSY);
    check_equal(tool_status, TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
    check_equal(replayed, 0);
    check_equal(probe.calls, 1);
    json_free(certificate);

    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }

}

spec("Harness compiled Inspect canonical Session Run/Checkpoint") {
  it("interrupts before the tool and safely resumes the exact graph checkpoint across SQLite reopen") {
    const char *allowed[] = {"runtime_tools"};
    compiled_inspect_probe_t probe = {0};
    turbo_runtime_sqlite_config_t sqlite_config;
    turbo_agent_runtime_store_t store = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_agent_harness_t *harness;
    turbo_agent_runtime_t *runtime;
    turbo_agent_compiler_config_t config;
    json_value_t *summary = NULL, *state = NULL, *record = NULL;
    char *directory = tt_make_temp_dir("compiled-graph-checkpoint");
    char *checkpoint_id = NULL;
    char db_path[1024];
    const char *checkpoint_view;

    check_not_null(directory);
    check_true(snprintf(db_path, sizeof(db_path), "%s/session.db", directory) <
               (int)sizeof(db_path));
    turbo_runtime_sqlite_config_init(&sqlite_config);
    sqlite_config.path = db_path;
    check_equal(turbo_runtime_store_sqlite_open(&sqlite_config, &store), SALTS_OK);
    check_not_null(registry);
    check_not_null(pool);
    check_equal(register_compiled_inspect(
        registry, &probe, TURBO_TOOL_EFFECT_READ, 1), TURBO_TOOL_OK);
    harness = make_compiled_harness_with_store(
        pool, registry, store, "compiled-graph-thread");
    check_not_null(harness);
    turbo_agent_compiler_config_init(&config);
    config.allowed_capabilities = allowed;
    config.allowed_capability_count = 1;

    check_equal(turbo_agent_harness_compiled_inspect_graph_run_json(
        harness, &config, compiled_inspect_source, strlen(compiled_inspect_source),
        "graph_attempt", NULL, 1, &summary, &state), SALTS_OK);
    check_equal(json_get_string(summary, "status"), "interrupted");
    checkpoint_view = json_get_string(summary, "checkpoint_id");
    check_not_null(checkpoint_view);
    checkpoint_id = malloc(strlen(checkpoint_view) + 1);
    check_not_null(checkpoint_id);
    strcpy(checkpoint_id, checkpoint_view);
    check_equal(probe.calls, 0);
    runtime = turbo_agent_session_runtime(
        turbo_agent_app_session(turbo_agent_harness_app(harness)));
    check_equal(turbo_agent_runtime_store_get_json_status(
        runtime, "compiled_inspect_attempts",
        "021_compiled-graph-thread_graph_attempt", &record), SALTS_ENOENT);
    json_free(summary); summary = NULL;
    json_free(state); state = NULL;

    /* A distinct but valid source changes the compiled plan/topology.
     * Resuming its checkpoint must not invoke even one callback. */
    {
      static const char changed_source[] = "{\"template_id\":\"inspect\",\"step_id\":\"read-1\",\"tool\":\"repo_inspect\",\"arguments_json\":\"{\\\"value\\\":\\\"another\\\"}\"}";
      check_not_equal(turbo_agent_harness_compiled_inspect_graph_run_json(
          harness, &config, changed_source, strlen(changed_source),
          "graph_attempt", checkpoint_id, 0, &summary, &state), SALTS_OK);
      json_free(summary); summary = NULL;
      json_free(state); state = NULL;
    }
    check_equal(probe.calls, 0);

    /* Invalid compilation also never reaches graph execution or tool code. */
    check_equal(turbo_agent_harness_compiled_inspect_graph_run_json(
        harness, &config, compiled_inspect_bad_args, strlen(compiled_inspect_bad_args),
        "graph_attempt", checkpoint_id, 0, &summary, &state), SALTS_EPROTO);
    check_equal(probe.calls, 0);

    turbo_agent_harness_release(harness);
    cmeta_threadpool_destroy(pool);
    pool = cmeta_threadpool_create_with_config(&pool_config);
    check_not_null(pool);
    sqlite_config.create_if_missing = 0;
    check_equal(turbo_runtime_store_sqlite_open(&sqlite_config, &store), SALTS_OK);
    harness = make_compiled_harness_with_store(
        pool, registry, store, "compiled-graph-thread");
    check_not_null(harness);

    check_equal(turbo_agent_harness_compiled_inspect_graph_run_json(
        harness, &config, compiled_inspect_source, strlen(compiled_inspect_source),
        "graph_attempt", checkpoint_id, 0, &summary, &state), SALTS_OK);
    check_equal(json_get_string(summary, "status"), "completed");
    check_true(json_get_bool(
        json_object_get(state, "compiled_result"), "ok", false));
    check_equal(probe.calls, 1);
    runtime = turbo_agent_session_runtime(
        turbo_agent_app_session(turbo_agent_harness_app(harness)));
    check_equal(turbo_agent_runtime_store_get_json_status(
        runtime, "compiled_inspect_attempts",
        "021_compiled-graph-thread_graph_attempt", &record), SALTS_OK);
    check_equal(json_get_string(record, "phase"), "committed");
    json_free(record); record = NULL;
    json_free(summary); summary = NULL;
    json_free(state); state = NULL;

    /* Replaying the old checkpoint must consume the committed attempt record
     * rather than invoking the backend again. */
    check_equal(turbo_agent_harness_compiled_inspect_graph_run_json(
        harness, &config, compiled_inspect_source, strlen(compiled_inspect_source),
        "graph_attempt", checkpoint_id, 0, &summary, &state), SALTS_OK);
    check_true(json_get_bool(state, "compiled_replayed", false));
    check_equal(probe.calls, 1);
    json_free(summary);
    json_free(state);
    free(checkpoint_id);
    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
    tt_remove_tree(directory);
    free(directory);
  }
}
