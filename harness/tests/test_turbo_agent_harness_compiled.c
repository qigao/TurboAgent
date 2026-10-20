#include "tinytest.h"

#include "turbo_agent_harness.h"
#include "turbo_agent_policy.h"
#include "turbo_agent_session.h"
#include "turbo_agent_inbox.h"
#include "turbo_agent_session_internal.h"
#include "turbo_tool_registry.h"
#include "turbo_runtime_json.h"
#include "turbo_runtime_store.h"
#include "turbo_runtime_store_sqlite.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <salts/thread_pool.h>

static const char compiled_inspect_source[] = "{\"template_id\":\"inspect\",\"step_id\":\"read-1\",\"tool\":\"repo_inspect\",\"arguments_json\":\"{\\\"value\\\":\\\"hello\\\"}\"}";
static const char compiled_inspect_other_valid[] = "{\"template_id\":\"inspect\",\"step_id\":\"read-1\",\"tool\":\"repo_inspect\",\"arguments_json\":\"{\\\"value\\\":\\\"different\\\"}\"}";
static const char compiled_inspect_bad_args[] = "{\"template_id\":\"inspect\",\"step_id\":\"read-1\",\"tool\":\"repo_inspect\",\"arguments_json\":\"{\\\"extra\\\":1}\"}";
static const char compiled_inspect_unknown_tool[] = "{\"template_id\":\"inspect\",\"step_id\":\"read-1\",\"tool\":\"nonexistent_tool\",\"arguments_json\":\"{}\"}";
#define COMPILED_INSPECT_JOURNAL_COLLECTION "agent_compiled_execution_journal"
#define COMPILED_INSPECT_ATTEMPT_COLLECTION "compiled_inspect_attempts"
#define COMPILED_INSPECT_TEST_THREAD_ID "compiled-inspect-test-thread"

typedef struct compiled_inspect_probe_s {
  int calls;
  int produce_invalid_result;
  int saw_started_journal;
  turbo_agent_runtime_t *runtime;
  const char *thread_id;
  turbo_agent_t *agent;
  int revoke_authority_during_callback;
} compiled_inspect_probe_t;

static int compiled_harness_transport(const char *request, char **response, void *user) {
  (void)request;
  (void)response;
  (void)user;
  return -1;
}

static int compiled_journal_has_phase(turbo_agent_runtime_t *runtime,
                                      const char *thread_id, const char *phase) {
  json_value_t *records = NULL;
  size_t i;
  int found = 0;
  if (!runtime || !thread_id || !phase ||
      turbo_agent_runtime_store_list_json(runtime, COMPILED_INSPECT_JOURNAL_COLLECTION,
                                          "thread_id", thread_id, &records) != SALTS_OK ||
      !records || json_type(records) != JSON_ARRAY) goto done;
  for (i = 0; i < json_array_size(records); ++i) {
    const json_value_t *record = json_array_get(records, i);
    const char *record_phase;
    const char *tool_name;
    if (!record || json_type(record) != JSON_OBJECT) continue;
    record_phase = json_get_string(record, "phase");
    tool_name = json_get_string(record, "tool_name");
    if (record_phase && strcmp(record_phase, phase) == 0 && tool_name &&
        strcmp(tool_name, "repo.inspect") == 0) {
      found = 1;
      break;
    }
  }
done:
  turbo_runtime_json_destroy(records);
  return found;
}

static void attach_compiled_probe_runtime(turbo_agent_harness_t *harness,
                                          compiled_inspect_probe_t *probe) {
  turbo_agent_app_t *app = turbo_agent_harness_app(harness);
  probe->runtime = turbo_agent_session_runtime(turbo_agent_app_session(app));
  /* Session summary capture replaces its owned thread_id string after each
   * Run. The probe must not retain that borrowed pointer across executions. */
  probe->thread_id = COMPILED_INSPECT_TEST_THREAD_ID;
}

static int compiled_inspect_handler(const json_value_t *arguments,
                                    json_value_t **out_result, void *user) {
  compiled_inspect_probe_t *probe = (compiled_inspect_probe_t *)user;
  json_value_t *result;
  if (!probe || !out_result || !arguments ||
      !json_get_string(arguments, "value")) return -1;
  ++probe->calls;
  if (probe->runtime && probe->thread_id) {
    if (!compiled_journal_has_phase(probe->runtime, probe->thread_id, "started")) return -1;
    ++probe->saw_started_journal;
  }
  if (probe->revoke_authority_during_callback) {
    turbo_agent_policy_t denied = turbo_agent_policy_default();
    denied.allow_runtime_tools = 0;
    if (!probe->agent || turbo_agent_set_tool_policy(probe->agent, &denied) != 0) return -1;
  }
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

typedef struct compiled_inspect_store_fault_s {
  turbo_agent_runtime_store_t inner;
  const char *failed_phase;
  int failed_once;
} compiled_inspect_store_fault_t;

static int compiled_inspect_fault_store_put(void *user_data, const char *collection,
                                            const char *id, const char *record_json) {
  compiled_inspect_store_fault_t *fault = (compiled_inspect_store_fault_t *)user_data;
  char phase_token[64];
  int length;
  if (!fault || !record_json) return -1;
  length = snprintf(phase_token, sizeof(phase_token), "\"phase\":\"%s\"",
                    fault->failed_phase ? fault->failed_phase : "");
  if (!fault->failed_once && fault->failed_phase &&
      strcmp(fault->failed_phase, "inbox_applied") == 0 &&
      strcmp(collection, "agent_inbox_transitions") == 0 &&
      strstr(record_json, "\"status\":\"applied\"")) {
    fault->failed_once = 1;
    return -1;
  }
  if (!fault->failed_once && length > 0 && (size_t)length < sizeof(phase_token) &&
      strcmp(collection, COMPILED_INSPECT_JOURNAL_COLLECTION) == 0 &&
      strstr(record_json, phase_token)) {
    fault->failed_once = 1;
    return -1;
  }
  return fault->inner.put(fault->inner.user_data, collection, id, record_json);
}

static int compiled_inspect_fault_store_get(void *user_data, const char *collection,
                                            const char *id, char **out_record_json) {
  compiled_inspect_store_fault_t *fault = (compiled_inspect_store_fault_t *)user_data;
  return fault ? fault->inner.get(fault->inner.user_data, collection, id, out_record_json) : -1;
}

static int compiled_inspect_fault_store_list(void *user_data, const char *collection,
                                             const char *filter_key, const char *filter_value,
                                             char **out_records_json) {
  compiled_inspect_store_fault_t *fault = (compiled_inspect_store_fault_t *)user_data;
  return fault ? fault->inner.list(fault->inner.user_data, collection, filter_key,
                                   filter_value, out_records_json) : -1;
}

static void compiled_inspect_fault_store_free(void *user_data) {
  compiled_inspect_store_fault_t *fault = (compiled_inspect_store_fault_t *)user_data;
  if (!fault) return;
  if (fault->inner.user_data_free)
    fault->inner.user_data_free(fault->inner.user_data);
  free(fault);
}

static turbo_agent_runtime_store_t compiled_inspect_store_fail_phase(const char *phase) {
  turbo_agent_runtime_store_t store = {0};
  turbo_agent_runtime_store_t inner = turbo_agent_runtime_store_memory_create();
  compiled_inspect_store_fault_t *fault;
  if (!inner.put || !inner.get || !inner.list || !inner.user_data) return store;
  fault = (compiled_inspect_store_fault_t *)calloc(1, sizeof(*fault));
  if (!fault) {
    if (inner.user_data_free) inner.user_data_free(inner.user_data);
    return store;
  }
  fault->inner = inner;
  fault->failed_phase = phase;
  store.put = compiled_inspect_fault_store_put;
  store.get = compiled_inspect_fault_store_get;
  store.list = compiled_inspect_fault_store_list;
  store.user_data = fault;
  store.user_data_free = compiled_inspect_fault_store_free;
  return store;
}

static turbo_agent_harness_t *make_compiled_harness_with_store(
    cmeta_threadpool_t *pool, turbo_tool_registry_t *registry,
    turbo_agent_runtime_store_t runtime_store) {
  turbo_agent_session_config_t session_config = {0};
  turbo_agent_app_config_t app_config = {0};
  turbo_agent_harness_config_t config;
  session_config.thread_id = COMPILED_INSPECT_TEST_THREAD_ID;
  session_config.runtime_store = runtime_store;
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
      pool, registry, turbo_agent_runtime_store_memory_create());
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

static int run_compiled_inspect_attempt(turbo_agent_harness_t *harness,
                                        const char *source, const char *const *allowed,
                                        size_t allowed_count, const char *attempt_id,
                                        int *replayed,
                                        turbo_agent_compile_diagnostic_t *diagnostic,
                                        turbo_tool_status_t *tool_status,
                                        json_value_t **result,
                                        json_value_t **certificate) {
  turbo_agent_compiler_config_t config;
  turbo_agent_compiler_config_init(&config);
  config.allowed_capabilities = allowed;
  config.allowed_capability_count = allowed_count;
  return turbo_agent_harness_execute_compiled_inspect_durable_json(
      harness, &config, source, strlen(source), NULL, attempt_id, replayed,
      diagnostic, tool_status, result, certificate);
}

spec("Harness Phase-1 compiled Inspect boundary") {
  it("compiles a DataBind source, freezes authority and verifies the result") {
    const char *allowed[] = {"runtime_tools"};
    const char *alias_allowed[] = {"wasm"};
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
    attach_compiled_probe_runtime(harness, &probe);
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

    {
      int run_rc = run_compiled_inspect(harness, compiled_inspect_source,
          allowed, 1, &diagnostic, &tool_status, &result, &certificate);
      /* Check callback reachability before the aggregate Runtime return code.
       * This distinguishes graph-node failure from post-graph finalization. */
      check_equal(probe.calls, 1);
      check_equal(run_rc, SALTS_OK);
    }
    check_equal(tool_status, TURBO_TOOL_OK);
    check_equal(diagnostic.status, TURBO_AGENT_COMPILE_OK);
    check_true(json_get_bool(result, "ok", false));
    check_not_null(certificate);
    check_equal(probe.calls, 1);

    json_free(result);
    json_free(certificate);
    result = NULL;
    certificate = NULL;
    check_equal(run_compiled_inspect(harness, compiled_inspect_source,
        alias_allowed, 1, &diagnostic, &tool_status, &result, &certificate), SALTS_OK);
    check_equal(diagnostic.status, TURBO_AGENT_COMPILE_OK);
    check_equal(tool_status, TURBO_TOOL_OK);
    check_true(json_get_bool(result, "ok", false));
    check_equal(probe.calls, 2);
    json_free(result);
    json_free(certificate);
    result = NULL;
    certificate = NULL;
    check_equal(probe.saw_started_journal, 2);
    {
      turbo_agent_app_t *app = turbo_agent_harness_app(harness);
      const char *run_id = turbo_agent_app_last_run_id(app);
      json_value_t *run_record = NULL;
      json_value_t *journal_records = NULL;
      const json_value_t *run_state;
      const json_value_t *run_execution;
      turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(
          turbo_agent_app_session(app));
      size_t i;
      check_not_null(run_id);
      check_equal(turbo_agent_app_get_run(app, run_id, &run_record), SALTS_OK);
      check_not_null(run_record);
      run_state = json_object_get(run_record, "state_snapshot");
      run_execution = json_object_get(run_state, "compiled_execution");
      check_not_null(run_execution);
      check_equal(json_get_int(run_execution, "schema_version", 0), 2);
      check_equal(json_get_string(run_execution, "phase"), "committed");
      check_equal(json_get_string(run_execution, "backend"), "runtime_tools");
      check_equal(json_get_string(run_execution, "tool_name"), "repo.inspect");
      check_not_null(json_get_string(run_execution, "execution_id"));
      check_not_null(json_get_string(run_execution, "plan_hash"));
      check_not_null(json_object_get(run_execution, "authority_generation"));
      check_not_null(json_object_get(run_execution, "certificate"));
      /* Durable typed results live only in compiled_inspect_attempts. */
      check_null(json_object_get(run_execution, "result"));

      check_equal(turbo_agent_runtime_store_list_json(
          runtime, COMPILED_INSPECT_JOURNAL_COLLECTION, "thread_id",
          probe.thread_id, &journal_records), SALTS_OK);
      check_not_null(journal_records);
      check_equal(json_array_size(journal_records), 2u);
      for (i = 0; i < json_array_size(journal_records); ++i) {
        const json_value_t *journal = json_array_get(journal_records, i);
        check_not_null(journal);
        check_equal(json_get_int(journal, "schema_version", 0), 2);
        check_equal(json_get_string(journal, "phase"), "committed");
        check_equal(json_get_string(journal, "thread_id"), probe.thread_id);
        check_equal(json_get_string(journal, "tool_name"), "repo.inspect");
        check_equal(json_get_string(journal, "backend"), "runtime_tools");
        check_equal(json_get_int(journal, "tool_status", -1), TURBO_TOOL_OK);
        check_not_null(json_get_string(journal, "execution_id"));
        check_not_null(json_get_string(journal, "plan_hash"));
        check_not_null(json_object_get(journal, "authority_generation"));
        check_not_null(json_object_get(journal, "certificate"));
        check_null(json_object_get(journal, "result"));
        check_not_null(json_get_string(journal, "attempt_id"));
        check_not_null(json_get_string(journal, "attempt_key"));
        {
          json_value_t *attempt_records = NULL;
          const json_value_t *attempt_record;
          check_equal(turbo_agent_runtime_store_list_json(
              runtime, COMPILED_INSPECT_ATTEMPT_COLLECTION,
              "thread_id", probe.thread_id, &attempt_records), SALTS_OK);
          check_not_null(attempt_records);
          size_t attempt_index;
          int found_matching_attempt = 0;
          check_equal(json_array_size(attempt_records), 2u);
          for (attempt_index = 0; attempt_index < json_array_size(attempt_records);
               ++attempt_index) {
            attempt_record = json_array_get(attempt_records, attempt_index);
            if (attempt_record &&
                json_get_string(attempt_record, "attempt_key") &&
                strcmp(json_get_string(attempt_record, "attempt_key"),
                       json_get_string(journal, "attempt_key")) == 0) {
              check_equal(json_get_string(attempt_record, "phase"), "committed");
              check_true(json_get_bool(json_object_get(attempt_record, "result"), "ok", false));
              found_matching_attempt = 1;
            }
          }
          check_equal(found_matching_attempt, 1);
          turbo_runtime_json_destroy(attempt_records);
        }
      }
      turbo_runtime_json_destroy(journal_records);
      turbo_runtime_json_destroy(run_record);
    }
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

  it("does not invoke the callback when the started journal write fails") {
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
    turbo_agent_runtime_store_t store = compiled_inspect_store_fail_phase("started");

    check_not_null(pool);
    check_not_null(registry);
    check_not_null(store.user_data);
    check_equal(register_compiled_inspect(registry, &probe, TURBO_TOOL_EFFECT_READ, 1),
                TURBO_TOOL_OK);
    harness = make_compiled_harness_with_store(pool, registry, store);
    check_not_null(harness);
    check_equal(set_compiled_host_policy(harness, 1), 0);

    check_not_equal(run_compiled_inspect(harness, compiled_inspect_source,
        allowed, 1, &diagnostic, &tool_status, &result, &certificate), SALTS_OK);
    check_equal(probe.calls, 0);
    check_null(result);
    /* Compilation succeeded, so the API intentionally still returns its certificate
     * on execution failure; only the tool result must remain withheld. */
    check_not_null(certificate);

    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }

  it("withholds the result when the committed journal write fails") {
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
    turbo_agent_runtime_store_t store = compiled_inspect_store_fail_phase("committed");

    check_not_null(pool);
    check_not_null(registry);
    check_not_null(store.user_data);
    check_equal(register_compiled_inspect(registry, &probe, TURBO_TOOL_EFFECT_READ, 1),
                TURBO_TOOL_OK);
    harness = make_compiled_harness_with_store(pool, registry, store);
    check_not_null(harness);
    check_equal(set_compiled_host_policy(harness, 1), 0);
    attach_compiled_probe_runtime(harness, &probe);

    check_not_equal(run_compiled_inspect(harness, compiled_inspect_source,
        allowed, 1, &diagnostic, &tool_status, &result, &certificate), SALTS_OK);
    check_equal(probe.calls, 1);
    check_null(result);
    /* Admission/compilation succeeded; execution failure withholds only result. */
    check_not_null(certificate);
    check_equal(probe.saw_started_journal, 1);
    {
      json_value_t *journal_records = NULL;
      const json_value_t *journal;
      check_equal(turbo_agent_runtime_store_list_json(
          probe.runtime, COMPILED_INSPECT_JOURNAL_COLLECTION, "thread_id",
          probe.thread_id, &journal_records), SALTS_OK);
      check_not_null(journal_records);
      check_equal(json_array_size(journal_records), 1u);
      journal = json_array_get(journal_records, 0);
      check_not_null(journal);
      /* The commit failed; the best-effort conservative terminal write must win. */
      check_equal(json_get_string(journal, "phase"), "outcome_unknown");
      check_equal(json_get_string(journal, "tool_name"), "repo.inspect");
      check_equal(json_get_int(journal, "tool_status", -1), TURBO_TOOL_ERROR);
      check_not_null(json_get_string(journal, "execution_id"));
      check_not_null(json_get_string(journal, "plan_hash"));
      check_null(json_object_get(journal, "result"));
      turbo_runtime_json_destroy(journal_records);
    }

    json_free(certificate);
    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }

  it("keeps the committed Inspect journal when Session inbox finalization fails") {
    const char *allowed[] = {"runtime_tools"};
    compiled_inspect_probe_t probe = {0};
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_runtime_store_t store = compiled_inspect_store_fail_phase("inbox_applied");
    turbo_agent_harness_t *harness;
    turbo_agent_session_t *session;
    turbo_agent_inbox_config_t inbox_config = {
        sizeof(inbox_config), TURBO_AGENT_INBOX_ABI_VERSION, 4, 4096, 1024, 1};
    turbo_agent_compile_diagnostic_t diagnostic = {0};
    turbo_tool_status_t tool_status = TURBO_TOOL_OK;
    json_value_t *message = json_create_object();
    json_value_t *claim = NULL;
    json_value_t *result = NULL;
    json_value_t *certificate = NULL;
    json_value_t *journal_records = NULL;
    const json_value_t *journal;
    char *inbox_id = NULL;
    int replayed = -1;
    int run_rc;

    check_not_null(pool);
    check_not_null(registry);
    check_not_null(store.user_data);
    check_not_null(message);
    check_equal(register_compiled_inspect(registry, &probe, TURBO_TOOL_EFFECT_READ, 1),
                TURBO_TOOL_OK);
    harness = make_compiled_harness_with_store(pool, registry, store);
    check_not_null(harness);
    check_equal(set_compiled_host_policy(harness, 1), 0);
    attach_compiled_probe_runtime(harness, &probe);
    session = turbo_agent_app_session(turbo_agent_harness_app(harness));
    check_not_null(session);
    check_equal(turbo_agent_session_inbox_configure(session, &inbox_config), SALTS_OK);
    json_object_set_string(message, "role", "user");
    json_object_set_string(message, "content", "compiled Inspect finalization failure");
    check_equal(turbo_agent_session_enqueue(
        session, TURBO_AGENT_INBOX_STEER, message, 0, &inbox_id), SALTS_OK);
    check_not_null(inbox_id);
    check_equal(turbo_agent_session_inbox_claim(
        session, TURBO_AGENT_INBOX_STEER, "compiled-inspect-finalization-test", &claim), SALTS_OK);
    check_not_null(claim);
    check_not_null(json_get_string(claim, "inbox_id"));
    check_equal(turbo_agent_session_bind_claim_event_internal(
        session, json_get_string(claim, "inbox_id"),
        "compiled-inspect-finalization-event"), SALTS_OK);

    run_rc = run_compiled_inspect_attempt(harness, compiled_inspect_source,
        allowed, 1, "session_finalize_retry", &replayed,
        &diagnostic, &tool_status, &result, &certificate);
    check_equal(run_rc, SALTS_EIO);
    check_equal(((compiled_inspect_store_fault_t *)store.user_data)->failed_once, 1);
    check_equal(replayed, 0);
    check_equal(probe.calls, 1);
    check_null(result);
    check_not_null(certificate);
    check_equal(probe.saw_started_journal, 1);
    check_equal(turbo_agent_runtime_store_list_json(
        probe.runtime, COMPILED_INSPECT_JOURNAL_COLLECTION, "thread_id",
        probe.thread_id, &journal_records), SALTS_OK);
    check_not_null(journal_records);
    check_equal(json_array_size(journal_records), 1u);
    journal = json_array_get(journal_records, 0);
    check_not_null(journal);
    /* The Run journal references a committed attempt but never owns its result. */
    check_equal(json_get_string(journal, "phase"), "committed");
    check_null(json_object_get(journal, "result"));
    check_equal(json_get_string(journal, "attempt_id"), "session_finalize_retry");
    turbo_runtime_json_destroy(journal_records);
    journal_records = NULL;

    check_equal(turbo_agent_runtime_store_list_json(
        probe.runtime, COMPILED_INSPECT_ATTEMPT_COLLECTION,
        "thread_id", probe.thread_id, &journal_records), SALTS_OK);
    check_not_null(journal_records);
    check_equal(json_array_size(journal_records), 1u);
    journal = json_array_get(journal_records, 0);
    check_not_null(journal);
    check_equal(json_get_string(journal, "phase"), "committed");
    check_true(json_get_bool(json_object_get(journal, "result"), "ok", false));
    turbo_runtime_json_destroy(journal_records);
    journal_records = NULL;
    json_free(certificate); certificate = NULL;

    /* Retry the same durable identity. The committed result is replayed,
     * finalization gets another chance, and the tool callback is not repeated. */
    run_rc = run_compiled_inspect_attempt(harness, compiled_inspect_source,
        allowed, 1, "session_finalize_retry", &replayed,
        &diagnostic, &tool_status, &result, &certificate);
    check_equal(run_rc, SALTS_OK);
    check_equal(replayed, 1);
    check_equal(probe.calls, 1);
    check_not_null(result);
    check_true(json_get_bool(result, "ok", false));
    json_free(result); result = NULL;
    json_free(certificate); certificate = NULL;

    /* An attempt ID is not permission to replay a result for a different plan.
     * The conflicting plan must fail closed without changing the canonical row
     * or entering the tool callback again. */
    run_rc = run_compiled_inspect_attempt(harness, compiled_inspect_other_valid,
        allowed, 1, "session_finalize_retry", &replayed,
        &diagnostic, &tool_status, &result, &certificate);
    check_not_equal(run_rc, SALTS_OK);
    check_equal(replayed, 0);
    check_equal(probe.calls, 1);
    check_null(result);
    check_not_null(certificate);
    json_free(certificate); certificate = NULL;

    check_equal(turbo_agent_runtime_store_list_json(
        probe.runtime, COMPILED_INSPECT_JOURNAL_COLLECTION, "thread_id",
        probe.thread_id, &journal_records), SALTS_OK);
    check_not_null(journal_records);
    check_equal(json_array_size(journal_records), 3u);
    turbo_runtime_json_destroy(journal_records);
    journal_records = NULL;
    check_equal(turbo_agent_runtime_store_list_json(
        probe.runtime, COMPILED_INSPECT_ATTEMPT_COLLECTION,
        "thread_id", probe.thread_id, &journal_records), SALTS_OK);
    check_not_null(journal_records);
    check_equal(json_array_size(journal_records), 1u);
    journal = json_array_get(journal_records, 0);
    check_not_null(journal);
    check_equal(json_get_string(journal, "phase"), "committed");
    check_true(json_get_bool(json_object_get(journal, "result"), "ok", false));
    turbo_runtime_json_destroy(journal_records);
    journal_records = NULL;

    turbo_runtime_json_destroy(claim);
    json_free(message);
    free(inbox_id);
    json_free(certificate);
    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }

  it("replays a committed Inspect attempt after SQLite and Harness reopen") {
    const char *allowed[] = {"runtime_tools"};
    compiled_inspect_probe_t probe = {0};
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_harness_t *harness = NULL;
    turbo_agent_compile_diagnostic_t diagnostic = {0};
    turbo_tool_status_t tool_status = TURBO_TOOL_ERROR;
    turbo_runtime_sqlite_config_t sqlite_config;
    turbo_agent_runtime_store_t store = {0};
    json_value_t *result = NULL;
    json_value_t *certificate = NULL;
    json_value_t *records = NULL;
    const json_value_t *attempt;
    char *directory = tt_make_temp_dir("compiled-inspect-replay");
    char path[1024];
    int replayed = -1;
    int run_rc;
    int path_len;

    check_not_null(pool);
    check_not_null(registry);
    check_not_null(directory);
    check_equal(register_compiled_inspect(
        registry, &probe, TURBO_TOOL_EFFECT_READ, 1), TURBO_TOOL_OK);
    path_len = snprintf(path, sizeof(path), "%s/compiled-inspect.db", directory);
    check_true(path_len > 0 && (size_t)path_len < sizeof(path));
    turbo_runtime_sqlite_config_init(&sqlite_config);
    sqlite_config.path = path;
    check_equal(turbo_runtime_store_sqlite_open(&sqlite_config, &store), SALTS_OK);
    harness = make_compiled_harness_with_store(pool, registry, store);
    store = (turbo_agent_runtime_store_t){0}; /* Runtime owns SQLite after creation. */
    check_not_null(harness);
    check_equal(set_compiled_host_policy(harness, 1), 0);
    attach_compiled_probe_runtime(harness, &probe);

    run_rc = run_compiled_inspect_attempt(harness, compiled_inspect_source,
        allowed, 1, "sqlite_restart_replay", &replayed,
        &diagnostic, &tool_status, &result, &certificate);
    check_equal(run_rc, SALTS_OK);
    check_equal(replayed, 0);
    check_equal(probe.calls, 1);
    check_true(json_get_bool(result, "ok", false));
    json_free(result); result = NULL;
    json_free(certificate); certificate = NULL;

    turbo_agent_harness_release(harness);
    harness = NULL;

    /* Reopen the same committed store under a new Runtime/Session owner. */
    sqlite_config.create_if_missing = 0;
    check_equal(turbo_runtime_store_sqlite_open(&sqlite_config, &store), SALTS_OK);
    harness = make_compiled_harness_with_store(pool, registry, store);
    store = (turbo_agent_runtime_store_t){0};
    check_not_null(harness);
    check_equal(set_compiled_host_policy(harness, 1), 0);
    attach_compiled_probe_runtime(harness, &probe);

    run_rc = run_compiled_inspect_attempt(harness, compiled_inspect_source,
        allowed, 1, "sqlite_restart_replay", &replayed,
        &diagnostic, &tool_status, &result, &certificate);
    check_equal(run_rc, SALTS_OK);
    check_equal(replayed, 1);
    check_equal(probe.calls, 1);
    check_true(json_get_bool(result, "ok", false));
    check_equal(turbo_agent_runtime_store_list_json(
        probe.runtime, COMPILED_INSPECT_ATTEMPT_COLLECTION,
        "thread_id", probe.thread_id, &records), SALTS_OK);
    check_not_null(records);
    check_equal(json_array_size(records), 1u);
    attempt = json_array_get(records, 0);
    check_not_null(attempt);
    check_equal(json_get_string(attempt, "phase"), "committed");
    check_true(json_get_bool(json_object_get(attempt, "result"), "ok", false));
    turbo_runtime_json_destroy(records);
    records = NULL;

    json_free(result);
    json_free(certificate);
    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
    tt_remove_tree(directory);
    free(directory);
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
    int replayed = -1;

    check_not_null(pool);
    check_not_null(registry);
    check_equal(register_compiled_inspect(registry, &probe, TURBO_TOOL_EFFECT_READ, 1), TURBO_TOOL_OK);
    harness = make_compiled_harness(pool, registry);
    check_not_null(harness);
    check_equal(set_compiled_host_policy(harness, 1), 0);
    attach_compiled_probe_runtime(harness, &probe);
    {
      int run_rc = run_compiled_inspect_attempt(harness, compiled_inspect_source,
          allowed, 1, "invalid_result_no_retry", &replayed,
          &diagnostic, &tool_status, &result, &certificate);
      /* This assertion isolates schema-mismatch fail-closed behavior;
       * Session finalization failure is covered by the dedicated test above. */
      check_equal(probe.calls, 1);
      check_equal(replayed, 0);
      check_equal(run_rc, SALTS_EPROTO);
    }
    check_equal(tool_status, TURBO_TOOL_ERROR);
    check_null(result);
    check_not_null(certificate);
    check_equal(probe.calls, 1);
    check_equal(probe.saw_started_journal, 1);
    {
      json_value_t *journal_records = NULL;
      const json_value_t *journal;
      check_equal(turbo_agent_runtime_store_list_json(
          probe.runtime, COMPILED_INSPECT_JOURNAL_COLLECTION, "thread_id",
          probe.thread_id, &journal_records), SALTS_OK);
      check_not_null(journal_records);
      check_equal(json_array_size(journal_records), 1u);
      journal = json_array_get(journal_records, 0);
      check_not_null(journal);
      check_equal(json_get_string(journal, "phase"), "outcome_unknown");
      check_equal(json_get_int(journal, "tool_status", -1), TURBO_TOOL_ERROR);
      check_equal(json_get_string(journal, "tool_name"), "repo.inspect");
      check_not_null(json_get_string(journal, "execution_id"));
      check_not_null(json_get_string(journal, "plan_hash"));
      check_not_null(json_object_get(journal, "certificate"));
      check_true(strstr(json_get_string(journal, "diagnostic") ?
                        json_get_string(journal, "diagnostic") : "",
                        "violating its admitted schema") != NULL);
      check_null(json_object_get(journal, "result"));
      turbo_runtime_json_destroy(journal_records);
    }

    /* The same durable identity is fail-closed after an invalid callback result.
     * Retrying must not call the ostensibly read-only tool. */
    json_free(certificate); certificate = NULL;
    {
      int retry_rc = run_compiled_inspect_attempt(harness, compiled_inspect_source,
          allowed, 1, "invalid_result_no_retry", &replayed,
          &diagnostic, &tool_status, &result, &certificate);
      check_not_equal(retry_rc, SALTS_OK);
      check_equal(replayed, 0);
      check_equal(probe.calls, 1);
      check_null(result);
      check_not_null(certificate);
      check_equal(tool_status, TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
      check_true(strstr(diagnostic.message, "automatic callback replay is forbidden") != NULL);
    }
    {
      json_value_t *attempt_records = NULL;
      const json_value_t *attempt;
      check_equal(turbo_agent_runtime_store_list_json(
          probe.runtime, COMPILED_INSPECT_ATTEMPT_COLLECTION,
          "thread_id", probe.thread_id, &attempt_records), SALTS_OK);
      check_not_null(attempt_records);
      check_equal(json_array_size(attempt_records), 1u);
      attempt = json_array_get(attempt_records, 0);
      check_not_null(attempt);
      check_equal(json_get_string(attempt, "phase"), "outcome_unknown");
      check_null(json_object_get(attempt, "result"));
      turbo_runtime_json_destroy(attempt_records);
    }

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

  it("withholds the result when Host Tool Authority is revoked during the callback") {
    const char *allowed[] = {"runtime_tools"};
    compiled_inspect_probe_t probe = {.revoke_authority_during_callback = 1};
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_harness_t *harness;
    turbo_agent_compile_diagnostic_t diagnostic = {0};
    turbo_tool_status_t tool_status = TURBO_TOOL_OK;
    json_value_t *result = NULL;
    json_value_t *certificate = NULL;
    json_value_t *journal_records = NULL;
    const json_value_t *journal;

    check_not_null(pool);
    check_not_null(registry);
    check_equal(register_compiled_inspect(registry, &probe, TURBO_TOOL_EFFECT_READ, 1),
                TURBO_TOOL_OK);
    harness = make_compiled_harness(pool, registry);
    check_not_null(harness);
    check_equal(set_compiled_host_policy(harness, 1), 0);
    attach_compiled_probe_runtime(harness, &probe);
    probe.agent = turbo_agent_session_agent(
        turbo_agent_app_session(turbo_agent_harness_app(harness)));
    check_not_null(probe.agent);

    check_not_equal(run_compiled_inspect(harness, compiled_inspect_source,
        allowed, 1, &diagnostic, &tool_status, &result, &certificate), SALTS_OK);
    check_equal(probe.calls, 1);
    check_equal(probe.saw_started_journal, 1);
    check_null(result);
    check_not_null(certificate);
    check_equal(tool_status, TURBO_TOOL_ERROR);
    check_true(strstr(diagnostic.message, "Host Tool Authority changed during callback") != NULL);

    check_equal(turbo_agent_runtime_store_list_json(
        probe.runtime, COMPILED_INSPECT_JOURNAL_COLLECTION, "thread_id",
        probe.thread_id, &journal_records), SALTS_OK);
    check_not_null(journal_records);
    check_equal(json_array_size(journal_records), 1u);
    journal = json_array_get(journal_records, 0);
    check_not_null(journal);
    check_equal(json_get_string(journal, "phase"), "outcome_unknown");
    check_equal(json_get_string(journal, "tool_name"), "repo.inspect");
    check_equal(json_get_int(journal, "tool_status", -1), TURBO_TOOL_ERROR);
    check_null(json_object_get(journal, "result"));

    turbo_runtime_json_destroy(journal_records);
    json_free(certificate);
    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }

}
