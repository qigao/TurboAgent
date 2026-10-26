#include "tinytest.h"

#include "turbo_agent_harness.h"
#include "turbo_agent_job.h"
#include "turbo_agent_job_acceptance.h"
#include "turbo_agent_job_delivery.h"
#include "turbo_agent_job_verifier.h"
#include "turbo_agent_policy.h"
#include "turbo_agent_session.h"
#include "turbo_agent_inbox.h"
#include "turbo_tool_registry.h"
#include "turbo_runtime_json.h"
#include "turbo_runtime_store.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <salts/thread_pool.h>

static const char compiled_inspect_source[] = "{\"template_id\":\"inspect\",\"step_id\":\"read-1\",\"tool\":\"repo_inspect\",\"arguments_json\":\"{\\\"value\\\":\\\"hello\\\"}\"}";
static const char compiled_inspect_source_alternate[] = "{\"template_id\":\"inspect\",\"step_id\":\"read-1\",\"tool\":\"repo_inspect\",\"arguments_json\":\"{\\\"value\\\":\\\"different\\\"}\"}";
static const char compiled_inspect_bad_args[] = "{\"template_id\":\"inspect\",\"step_id\":\"read-1\",\"tool\":\"repo_inspect\",\"arguments_json\":\"{\\\"extra\\\":1}\"}";
static const char compiled_inspect_unknown_tool[] = "{\"template_id\":\"inspect\",\"step_id\":\"read-1\",\"tool\":\"nonexistent_tool\",\"arguments_json\":\"{}\"}";
#define COMPILED_INSPECT_JOURNAL_COLLECTION "compiled_inspect_attempts"
#define COMPILED_INSPECT_RUN_JOURNAL_COLLECTION "agent_compiled_execution_journal"
#define COMPILED_INSPECT_TEST_THREAD_ID "compiled-inspect-test-thread"

typedef struct compiled_inspect_probe_s {
  int calls;
  int produce_invalid_result;
  int saw_started_journal;
  turbo_agent_runtime_t *runtime;
  char last_value[32];
  const char *thread_id;
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
  snprintf(probe->last_value, sizeof(probe->last_value), "%s", json_get_string(arguments, "value"));
  if (probe->runtime && probe->thread_id) {
    if (!compiled_journal_has_phase(probe->runtime, probe->thread_id, "started")) return -1;
    ++probe->saw_started_journal;
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
  int claim_failure_status;
  int job_read_status;
  turbo_agent_harness_t *nested_harness;
  int nested_entered, nested_status;
  int acceptance_status, acceptance_after_commit, acceptance_claims, acceptance_puts;
  int acceptance_unacknowledged, reads_after_acceptance_error;
  const char *proof_read_failure;
  const char *delivery_collection;
  int delivery_status, delivery_after_commit, delivery_claims, delivery_puts;
  int delivery_unacknowledged, reads_after_delivery_error;
  int delivery_compete;
} compiled_inspect_store_fault_t;

static int compiled_inspect_fault_store_put(void *user_data, const char *collection,
                                            const char *id, const char *record_json) {
  compiled_inspect_store_fault_t *fault = (compiled_inspect_store_fault_t *)user_data;
  char phase_token[64];
  int length;
  if (!fault || !record_json) return -1;
  if (!strcmp(collection, "agent_job_acceptance")) ++fault->acceptance_puts;
  if (!strcmp(collection, "agent_job_delivery_intents") || !strcmp(collection, "agent_job_delivery_receipts"))
    ++fault->delivery_puts;
  length = snprintf(phase_token, sizeof(phase_token), "\"phase\":\"%s\"",
                    fault->failed_phase ? fault->failed_phase : "");
  if (fault->failed_phase && strcmp(fault->failed_phase, "inbox_applied") == 0 &&
      strcmp(collection, "agent_inbox_transitions") == 0 &&
      strstr(record_json, "\"status\":\"applied\"")) return -1;
  if (length > 0 && (size_t)length < sizeof(phase_token) &&
      strstr(record_json, phase_token)) return -1;
  return fault->inner.put(fault->inner.user_data, collection, id, record_json);
}

static int compiled_inspect_fault_store_create_if_absent(
    void *user_data, const char *collection, const char *id,
    const char *record_json) {
  compiled_inspect_store_fault_t *fault = (compiled_inspect_store_fault_t *)user_data;
  if (!fault || !fault->inner.create_if_absent) return SALTS_ENOTSUP;
  if (!strcmp(collection, "agent_job_delivery_intents") || !strcmp(collection, "agent_job_delivery_receipts")) {
    int rc, inject = fault->delivery_collection && !strcmp(collection, fault->delivery_collection);
    ++fault->delivery_claims;
    if (fault->delivery_compete) {
      fault->delivery_compete = 0;
      rc = fault->inner.create_if_absent(fault->inner.user_data, collection, id, record_json);
      if (rc != SALTS_OK) return rc;
    }
    if (inject && fault->delivery_status && !fault->delivery_after_commit) {
      fault->delivery_unacknowledged = 1;
      return fault->delivery_status;
    }
    rc = fault->inner.create_if_absent(fault->inner.user_data, collection, id, record_json);
    if (!rc && inject && fault->delivery_status) {
      fault->delivery_unacknowledged = 1;
      return fault->delivery_status;
    }
    return rc;
  }
  if (!strcmp(collection, "agent_job_acceptance")) {
    int rc;
    ++fault->acceptance_claims;
    if (fault->acceptance_status && !fault->acceptance_after_commit) {
      fault->acceptance_unacknowledged = 1;
      return fault->acceptance_status;
    }
    rc = fault->inner.create_if_absent(fault->inner.user_data, collection, id, record_json);
    if (rc == SALTS_OK && fault->acceptance_status) {
      fault->acceptance_unacknowledged = 1;
      return fault->acceptance_status;
    }
    return rc;
  }
  if (fault->claim_failure_status != SALTS_OK) return fault->claim_failure_status;
  return fault->inner.create_if_absent(fault->inner.user_data, collection, id, record_json);
}

static int compiled_inspect_fault_store_get(void *user_data, const char *collection,
                                            const char *id, char **out_record_json) {
  compiled_inspect_store_fault_t *fault = (compiled_inspect_store_fault_t *)user_data;
  if (fault && fault->acceptance_unacknowledged) ++fault->reads_after_acceptance_error;
  if (fault && fault->delivery_unacknowledged) ++fault->reads_after_delivery_error;
  if (fault && fault->proof_read_failure && !strcmp(collection, fault->proof_read_failure)) {
    *out_record_json = NULL;
    return SALTS_EIO;
  }
  if (fault && fault->job_read_status &&
      (!strcmp(collection, "agent_jobs") || !strcmp(collection, "agent_job_revisions"))) {
    *out_record_json = NULL;
    return fault->job_read_status;
  }
  if (fault && fault->nested_harness && !fault->nested_entered && !strcmp(collection, "agent_jobs")) {
    const char *allowed[] = {"runtime_tools"};
    turbo_agent_compiler_config_t config;
    turbo_agent_compile_diagnostic_t diagnostic;
    turbo_tool_status_t status;
    json_value_t *result = NULL, *certificate = NULL, *reference = NULL;
    fault->nested_entered = 1;
    turbo_agent_compiler_config_init(&config);
    config.allowed_capabilities = allowed; config.allowed_capability_count = 1;
    fault->nested_status = turbo_agent_harness_execute_job_inspect_json(
        fault->nested_harness, "job-bound", 2, &config, NULL, &diagnostic,
        &status, &result, &certificate, &reference);
    json_free(result); json_free(certificate); json_free(reference);
  }
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
  store.create_if_absent = compiled_inspect_fault_store_create_if_absent;
  store.get = compiled_inspect_fault_store_get;
  store.list = compiled_inspect_fault_store_list;
  store.user_data = fault;
  store.user_data_free = compiled_inspect_fault_store_free;
  return store;
}

static turbo_agent_harness_t *make_compiled_harness_configured(
    cmeta_threadpool_t *pool, turbo_tool_registry_t *registry,
    turbo_agent_runtime_store_t runtime_store, turbo_agent_transport_fn transport,
    const turbo_agent_job_verifier_t *verifier) {
  turbo_agent_session_config_t session_config = {0};
  turbo_agent_app_config_t app_config = {0};
  turbo_agent_harness_config_t config;
  session_config.thread_id = COMPILED_INSPECT_TEST_THREAD_ID;
  session_config.runtime_store = runtime_store;
  session_config.agent_config.model = "gpt-5.4";
  session_config.agent_config.transport_fn = transport;
  session_config.agent_config.tool_registry = registry;
  app_config.session_config = &session_config;
  turbo_agent_harness_config_init(&config);
  config.app_config = &app_config;
  config.executor = pool;
  turbo_agent_harness_t *harness = NULL;
  if (verifier) (void)turbo_agent_harness_create_verified_with_store(&config, verifier, &harness);
  else (void)turbo_agent_harness_create_with_store(&config, &harness);
  return harness;
}

static turbo_agent_harness_t *make_compiled_harness_with_transport(
    cmeta_threadpool_t *pool, turbo_tool_registry_t *registry,
    turbo_agent_runtime_store_t runtime_store, turbo_agent_transport_fn transport) {
  return make_compiled_harness_configured(pool, registry, runtime_store, transport, NULL);
}

static turbo_agent_harness_t *make_compiled_harness_with_store(
    cmeta_threadpool_t *pool, turbo_tool_registry_t *registry,
    turbo_agent_runtime_store_t runtime_store) {
  return make_compiled_harness_with_transport(pool, registry, runtime_store,
      compiled_harness_transport);
}

static int compiled_inbox_prepare_transport(const char *request, char **response, void *user) {
  static const char result[] = "{\"id\":\"inbox_prepare\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"prepared\"}]}]}";
  (void)request; (void)user;
  *response = malloc(sizeof(result));
  if (!*response) return -1;
  memcpy(*response, result, sizeof(result));
  return 0;
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
                                        const char *attempt_id,
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
  return turbo_agent_harness_execute_compiled_inspect_attempt_json(
      harness, &config, source, strlen(source), NULL, attempt_id, diagnostic,
      tool_status, result, certificate);
}

spec("Harness Phase-1 compiled Inspect boundary") {
  it("keeps Job binding separate from compiled admission and the canonical result") {
    const char *allowed[] = {"runtime_tools"};
    compiled_inspect_probe_t probe = {0};
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_harness_t *harness;
    turbo_agent_job_t *job = NULL;
    turbo_agent_compile_diagnostic_t diagnostic = {0};
    turbo_tool_status_t status = TURBO_TOOL_ERROR;
    json_value_t *snapshot = NULL, *result = NULL, *certificate = NULL, *records = NULL;
    const json_value_t *proposal, *attempt;
    check_not_null(pool);
    check_not_null(registry);
    check_equal(register_compiled_inspect(registry, &probe, TURBO_TOOL_EFFECT_READ, 1), TURBO_TOOL_OK);
    harness = make_compiled_harness(pool, registry);
    check_not_null(harness);
    attach_compiled_probe_runtime(harness, &probe);
    check_equal(set_compiled_host_policy(harness, 0), 0);
    check_equal(turbo_agent_job_create(probe.runtime, "inspect-job", "inspect repository",
                "Host verifies evidence", &job), SALTS_OK);
    check_equal(turbo_agent_job_propose(job, 0, compiled_inspect_source,
                strlen(compiled_inspect_source), "source-v1"), SALTS_OK);
    check_equal(turbo_agent_job_bind_compiled_inspect(job, 1,
                COMPILED_INSPECT_TEST_THREAD_ID, "job-attempt"), SALTS_OK);
    check_equal(turbo_agent_job_get_snapshot(job, &snapshot), SALTS_OK);
    proposal = json_object_get(snapshot, "proposal");
    attempt = json_object_get(snapshot, "attempt");
    check_not_equal(run_compiled_inspect_attempt(harness,
        json_get_string(attempt, "attempt_id"), json_get_string(proposal, "source_json"),
        allowed, 1, &diagnostic, &status, &result, &certificate), SALTS_OK);
    check_equal(probe.calls, 0);
    check_null(result);
    json_free(certificate); certificate = NULL;
    check_equal(set_compiled_host_policy(harness, 1), 0);
    check_equal(run_compiled_inspect_attempt(harness,
        json_get_string(attempt, "attempt_id"), json_get_string(proposal, "source_json"),
        allowed, 1, &diagnostic, &status, &result, &certificate), SALTS_OK);
    check_equal(probe.calls, 1);
    check_true(json_get_bool(result, "ok", false));
    json_free(result); result = NULL;
    json_free(certificate); certificate = NULL;
    json_free(snapshot); snapshot = NULL;
    turbo_agent_job_destroy(job); job = NULL;
    check_equal(turbo_agent_job_open(probe.runtime, "inspect-job", &job), SALTS_OK);
    check_equal(turbo_agent_job_get_snapshot(job, &snapshot), SALTS_OK);
    check_equal(json_get_string(snapshot, "state"), "attempt_bound");
    check_null(json_object_get(snapshot, "result"));
    proposal = json_object_get(snapshot, "proposal");
    attempt = json_object_get(snapshot, "attempt");
    check_equal(run_compiled_inspect_attempt(harness,
        json_get_string(attempt, "attempt_id"), json_get_string(proposal, "source_json"),
        allowed, 1, &diagnostic, &status, &result, &certificate), SALTS_OK);
    check_equal(probe.calls, 1);
    check_equal(turbo_agent_runtime_store_list_json(probe.runtime,
        COMPILED_INSPECT_JOURNAL_COLLECTION, NULL, NULL, &records), SALTS_OK);
    check_equal(json_array_size(records), 1u);
    check_not_null(json_object_get(json_array_get(records, 0), "result"));
    json_free(records); records = NULL;
    json_free(result); result = NULL;
    json_free(certificate); certificate = NULL;
    check_equal(set_compiled_host_policy(harness, 0), 0);
    check_not_equal(run_compiled_inspect_attempt(harness,
        json_get_string(attempt, "attempt_id"), json_get_string(proposal, "source_json"),
        allowed, 1, &diagnostic, &status, &result, &certificate), SALTS_OK);
    check_null(result);
    check_equal(probe.calls, 1);
    turbo_agent_job_destroy(job);
    json_free(snapshot);
    json_free(certificate);
    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }

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
      check_equal(json_get_string(run_execution, "phase"), "committed");
      check_equal(json_get_string(run_execution, "backend"), "runtime_tools");
      check_equal(json_get_string(run_execution, "tool_name"), "repo.inspect");
      check_not_null(json_get_string(run_execution, "execution_id"));
      check_not_null(json_get_string(run_execution, "plan_hash"));
      check_not_null(json_object_get(run_execution, "authority_generation"));
      check_not_null(json_object_get(run_execution, "certificate"));
      check_not_null(json_get_string(run_execution, "attempt_id"));
      check_not_null(json_get_string(run_execution, "attempt_record_id"));
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
        check_true(json_get_bool(json_object_get(journal, "result"), "ok", false));
      }
      turbo_runtime_json_destroy(journal_records);
      journal_records = NULL;
      check_equal(turbo_agent_runtime_store_list_json(
          runtime, COMPILED_INSPECT_RUN_JOURNAL_COLLECTION, "thread_id",
          probe.thread_id, &journal_records), SALTS_OK);
      check_not_null(journal_records);
      check_equal(json_array_size(journal_records), 2u);
      for (i = 0; i < json_array_size(journal_records); ++i) {
        const json_value_t *run_journal = json_array_get(journal_records, i);
        check_not_null(run_journal);
        check_equal(json_get_string(run_journal, "phase"), "committed");
        check_not_null(json_get_string(run_journal, "attempt_id"));
        check_not_null(json_get_string(run_journal, "attempt_record_id"));
        check_null(json_object_get(run_journal, "result"));
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

  it("fails closed and preserves the atomic claim error before callback dispatch") {
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
    turbo_agent_runtime_store_t store = compiled_inspect_store_fail_phase(NULL);

    check_not_null(pool);
    check_not_null(registry);
    check_not_null(store.user_data);
    ((compiled_inspect_store_fault_t *)store.user_data)->claim_failure_status = SALTS_ENOTSUP;
    check_equal(register_compiled_inspect(registry, &probe, TURBO_TOOL_EFFECT_READ, 1),
                TURBO_TOOL_OK);
    harness = make_compiled_harness_with_store(pool, registry, store);
    check_not_null(harness);
    check_equal(set_compiled_host_policy(harness, 1), 0);

    check_equal(run_compiled_inspect(harness, compiled_inspect_source,
        allowed, 1, &diagnostic, &tool_status, &result, &certificate), SALTS_ENOTSUP);
    check_equal(probe.calls, 0);
    check_null(result);
    check_not_null(certificate);
    check_true(strstr(diagnostic.message, "does not support atomic Inspect attempt claims") != NULL);

    json_free(certificate);
    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }


  it("fails closed on an atomic claim backend error before callback dispatch") {
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
    turbo_agent_runtime_store_t store = compiled_inspect_store_fail_phase(NULL);

    check_not_null(pool);
    check_not_null(registry);
    check_not_null(store.user_data);
    ((compiled_inspect_store_fault_t *)store.user_data)->claim_failure_status = SALTS_EIO;
    check_equal(register_compiled_inspect(registry, &probe, TURBO_TOOL_EFFECT_READ, 1),
                TURBO_TOOL_OK);
    harness = make_compiled_harness_with_store(pool, registry, store);
    check_not_null(harness);
    check_equal(set_compiled_host_policy(harness, 1), 0);

    check_equal(run_compiled_inspect(harness, compiled_inspect_source,
        allowed, 1, &diagnostic, &tool_status, &result, &certificate), SALTS_EIO);
    check_equal(probe.calls, 0);
    check_null(result);
    check_not_null(certificate);
    check_true(strstr(diagnostic.message, "could not persist durable Inspect STARTED attempt") != NULL);

    json_free(certificate);
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
    json_value_t *journal_records = NULL;
    const json_value_t *journal;
    turbo_agent_runtime_store_t store = compiled_inspect_store_fail_phase("committed");
    compiled_inspect_store_fault_t *store_fault =
        (compiled_inspect_store_fault_t *)store.user_data;
    int run_rc;

    check_not_null(pool);
    check_not_null(registry);
    check_not_null(store.user_data);
    check_not_null(store_fault);
    check_equal(register_compiled_inspect(registry, &probe, TURBO_TOOL_EFFECT_READ, 1),
                TURBO_TOOL_OK);
    harness = make_compiled_harness_with_store(pool, registry, store);
    check_not_null(harness);
    check_equal(set_compiled_host_policy(harness, 1), 0);
    attach_compiled_probe_runtime(harness, &probe);

    run_rc = run_compiled_inspect_attempt(harness, "commit-failure-replay",
        compiled_inspect_source, allowed, 1, &diagnostic, &tool_status,
        &result, &certificate);
    check_not_equal(run_rc, SALTS_OK);
    check_equal(probe.calls, 1);
    check_null(result);
    /* Admission/compilation succeeded; execution failure withholds only result. */
    check_not_null(certificate);
    check_equal(probe.saw_started_journal, 1);
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
    journal_records = NULL;

    /* The attempt commit failed after one callback, so the durable phase must
     * remain fail-closed. Replaying the same ID must not redispatch it. */
    store_fault->failed_phase = NULL;
    json_free(certificate);
    certificate = NULL;
    run_rc = run_compiled_inspect_attempt(harness, "commit-failure-replay",
        compiled_inspect_source, allowed, 1, &diagnostic, &tool_status,
        &result, &certificate);
    /* The conservative terminal write left an outcome_unknown attempt.
     * This is an uncertain execution, not an atomic-claim collision. */
    check_equal(run_rc, SALTS_EBUSY);
    check_true(strstr(diagnostic.message, "uncertain outcome") != NULL);
    check_null(result);
    check_equal(probe.calls, 1);
    check_not_null(certificate);
    journal_records = NULL;
    check_equal(turbo_agent_runtime_store_list_json(
        probe.runtime, COMPILED_INSPECT_JOURNAL_COLLECTION, "thread_id",
        probe.thread_id, &journal_records), SALTS_OK);
    check_not_null(journal_records);
    check_equal(json_array_size(journal_records), 1u);
    journal = json_array_get(journal_records, 0);
    check_not_null(journal);
    check_equal(json_get_string(journal, "phase"), "outcome_unknown");
    check_null(json_object_get(journal, "result"));
    turbo_runtime_json_destroy(journal_records);
    journal_records = NULL;

    check_equal(turbo_agent_runtime_store_list_json(
        probe.runtime, COMPILED_INSPECT_RUN_JOURNAL_COLLECTION, "thread_id",
        probe.thread_id, &journal_records), SALTS_OK);
    check_not_null(journal_records);
    check_equal(json_array_size(journal_records), 2u);
    for (size_t i = 0; i < json_array_size(journal_records); ++i) {
      const json_value_t *run_journal = json_array_get(journal_records, i);
      check_not_null(run_journal);
      check_equal(json_get_string(run_journal, "attempt_id"), "commit-failure-replay");
      check_null(json_object_get(run_journal, "result"));
    }
    turbo_runtime_json_destroy(journal_records);

    json_free(certificate);
    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }

  it("refuses to redispatch when the durable attempt remains STARTED") {
    const char *allowed[] = {"runtime_tools"};
    compiled_inspect_probe_t probe = {.produce_invalid_result = 1};
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_runtime_store_t store = compiled_inspect_store_fail_phase("outcome_unknown");
    compiled_inspect_store_fault_t *store_fault =
        (compiled_inspect_store_fault_t *)store.user_data;
    turbo_agent_harness_t *harness;
    turbo_agent_compile_diagnostic_t diagnostic = {0};
    turbo_tool_status_t tool_status = TURBO_TOOL_OK;
    json_value_t *result = NULL;
    json_value_t *certificate = NULL;
    json_value_t *attempt_records = NULL;
    json_value_t *run_records = NULL;
    const json_value_t *record;
    int run_rc;

    check_not_null(pool);
    check_not_null(registry);
    check_not_null(store_fault);
    check_equal(register_compiled_inspect(registry, &probe, TURBO_TOOL_EFFECT_READ, 1),
                TURBO_TOOL_OK);
    harness = make_compiled_harness_with_store(pool, registry, store);
    check_not_null(harness);
    check_equal(set_compiled_host_policy(harness, 1), 0);
    attach_compiled_probe_runtime(harness, &probe);

    /* Callback ran and produced an invalid result; injected failure prevents
     * the terminal OUTCOME_UNKNOWN attempt write, preserving STARTED durably. */
    run_rc = run_compiled_inspect_attempt(harness, "started-replay",
        compiled_inspect_source, allowed, 1, &diagnostic, &tool_status,
        &result, &certificate);
    check_not_equal(run_rc, SALTS_OK);
    check_equal(probe.calls, 1);
    check_null(result);
    check_not_null(certificate);
    check_equal(turbo_agent_runtime_store_list_json(
        probe.runtime, COMPILED_INSPECT_JOURNAL_COLLECTION, "thread_id",
        probe.thread_id, &attempt_records), SALTS_OK);
    check_not_null(attempt_records);
    check_equal(json_array_size(attempt_records), 1u);
    record = json_array_get(attempt_records, 0);
    check_not_null(record);
    check_equal(json_get_string(record, "attempt_id"), "started-replay");
    check_equal(json_get_string(record, "phase"), "started");
    check_null(json_object_get(record, "result"));
    turbo_runtime_json_destroy(attempt_records);
    attempt_records = NULL;

    /* Once storage recovers, retry the exact same ID. STARTED is ambiguous:
     * no callback redispatch is allowed, and the canonical row stays STARTED. */
    store_fault->failed_phase = NULL;
    json_free(certificate);
    certificate = NULL;
    run_rc = run_compiled_inspect_attempt(harness, "started-replay",
        compiled_inspect_source, allowed, 1, &diagnostic, &tool_status,
        &result, &certificate);
    check_equal(run_rc, SALTS_EBUSY);
    check_true(strstr(diagnostic.message, "uncertain outcome") != NULL);
    check_equal(probe.calls, 1);
    check_null(result);
    check_not_null(certificate);

    check_equal(turbo_agent_runtime_store_list_json(
        probe.runtime, COMPILED_INSPECT_JOURNAL_COLLECTION, "thread_id",
        probe.thread_id, &attempt_records), SALTS_OK);
    check_not_null(attempt_records);
    check_equal(json_array_size(attempt_records), 1u);
    record = json_array_get(attempt_records, 0);
    check_not_null(record);
    check_equal(json_get_string(record, "phase"), "started");
    check_equal(json_get_string(record, "attempt_id"), "started-replay");
    check_null(json_object_get(record, "result"));
    turbo_runtime_json_destroy(attempt_records);

    check_equal(turbo_agent_runtime_store_list_json(
        probe.runtime, COMPILED_INSPECT_RUN_JOURNAL_COLLECTION, "thread_id",
        probe.thread_id, &run_records), SALTS_OK);
    check_not_null(run_records);
    check_equal(json_array_size(run_records), 2u);
    {
      size_t i;
      for (i = 0; i < json_array_size(run_records); ++i) {
        const json_value_t *run_record = json_array_get(run_records, i);
        check_not_null(run_record);
        check_equal(json_get_string(run_record, "attempt_id"), "started-replay");
        check_null(json_object_get(run_record, "result"));
      }
    }
    turbo_runtime_json_destroy(run_records);
    json_free(result);
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
    compiled_inspect_store_fault_t *store_fault =
        (compiled_inspect_store_fault_t *)store.user_data;
    turbo_agent_harness_t *harness;
    turbo_agent_session_t *session;
    turbo_agent_tool_authority_snapshot_t committed_authority = {0};
    turbo_agent_tool_authority_snapshot_t restored_authority = {0};
    turbo_agent_inbox_config_t inbox_config = {
        sizeof(inbox_config), TURBO_AGENT_INBOX_ABI_VERSION, 4, 4096, 1024, 1};
    turbo_agent_compile_diagnostic_t diagnostic = {0};
    turbo_tool_status_t tool_status = TURBO_TOOL_OK;
    json_value_t *message = json_create_object();
    json_value_t *prepare_summary = NULL;
    json_value_t *prepare_state = NULL;
    json_value_t *result = NULL;
    json_value_t *certificate = NULL;
    json_value_t *journal_records = NULL;
    const json_value_t *journal;
    char *inbox_id = NULL;
    int run_rc;

    check_not_null(pool);
    check_not_null(registry);
    check_not_null(store.user_data);
    check_not_null(store_fault);
    check_not_null(message);
    check_equal(register_compiled_inspect(registry, &probe, TURBO_TOOL_EFFECT_READ, 1),
                TURBO_TOOL_OK);
    harness = make_compiled_harness_with_transport(pool, registry, store,
        compiled_inbox_prepare_transport);
    check_not_null(harness);
    check_equal(set_compiled_host_policy(harness, 1), 0);
    attach_compiled_probe_runtime(harness, &probe);
    session = turbo_agent_app_session(turbo_agent_harness_app(harness));
    check_not_null(session);
    check_equal(turbo_agent_tool_authority_snapshot_get(
        turbo_agent_session_agent(session), &committed_authority), 0);
    check_equal(turbo_agent_session_inbox_configure(session, &inbox_config), SALTS_OK);
    json_object_set_string(message, "role", "user");
    json_object_set_string(message, "content", "compiled Inspect finalization failure");
    check_equal(turbo_agent_session_enqueue(
        session, TURBO_AGENT_INBOX_STEER, message, 0, &inbox_id), SALTS_OK);
    check_not_null(inbox_id);
    /* Let the real before-turn hook add/bind the Inbox event to a Run state.
     * The injected applied-write failure leaves that legitimate bound claim
     * available for the subsequent compiled Run's finalization retry. */
    check_equal(turbo_agent_session_start_text(session, "prepare Inbox event", NULL,
        &prepare_summary, &prepare_state), SALTS_EIO);
    check_not_null(prepare_summary);
    check_not_null(prepare_state);
    {
      const json_value_t *events = json_object_get(prepare_state, "events");
      size_t i;
      int found = 0;
      for (i = 0; i < json_array_size(events); ++i) {
        const json_value_t *event = json_array_get(events, i);
        const char *id = json_get_string(event, "inbox_id");
        if (id && !strcmp(id, inbox_id) && json_get_string(event, "event_id")) found = 1;
      }
      check_true(found);
    }

    run_rc = run_compiled_inspect_attempt(harness, "finalization-replay",
        compiled_inspect_source, allowed, 1, &diagnostic, &tool_status,
        &result, &certificate);
    check_equal(run_rc, SALTS_EIO);
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
    /* Tool result commit succeeded before Session's applied-inbox transition failed. */
    check_equal(json_get_string(journal, "phase"), "committed");
    check_true(json_get_bool(json_object_get(journal, "result"), "ok", false));

    turbo_runtime_json_destroy(journal_records);
    journal_records = NULL;
    json_free(certificate);
    certificate = NULL;

    /* Remove only the injected finalization fault. Retrying this same attempt
     * must settle the Session Run from the committed attempt without a 2nd call. */
    store_fault->failed_phase = NULL;
    run_rc = run_compiled_inspect_attempt(harness, "finalization-replay",
        compiled_inspect_source, allowed, 1, &diagnostic, &tool_status,
        &result, &certificate);
    check_equal(run_rc, SALTS_OK);
    check_equal(tool_status, TURBO_TOOL_OK);
    check_equal(probe.calls, 1);
    check_true(json_get_bool(result, "ok", false));
    check_not_null(certificate);

    /* Same identity with a different frozen plan must not reuse the old result. */
    json_free(result);
    result = NULL;
    json_free(certificate);
    certificate = NULL;
    run_rc = run_compiled_inspect_attempt(harness, "finalization-replay",
        compiled_inspect_source_alternate, allowed, 1, &diagnostic, &tool_status,
        &result, &certificate);
    check_not_equal(run_rc, SALTS_OK);
    check_null(result);
    check_equal(probe.calls, 1);
    json_free(certificate);
    certificate = NULL;

    /* Revoked Host authority blocks delivery during replay without changing
     * the committed attempt or dispatching the callback again. */
    check_equal(set_compiled_host_policy(harness, 0), 0);
    run_rc = run_compiled_inspect_attempt(harness, "finalization-replay",
        compiled_inspect_source, allowed, 1, &diagnostic, &tool_status,
        &result, &certificate);
    check_equal(run_rc, SALTS_EPROTO);
    check_equal(diagnostic.status, TURBO_AGENT_COMPILE_CAPABILITY_DENIED);
    check_null(result);
    check_null(certificate);
    check_equal(probe.calls, 1);

    /* All synchronous calls have returned before the Host restores the grant.
     * Restoring identical permissions must not revive the old committed
     * generation, even when the caller repeats its retry. */
    check_equal(set_compiled_host_policy(harness, 1), 0);
    check_equal(turbo_agent_tool_authority_snapshot_get(
        turbo_agent_session_agent(session), &restored_authority), 0);
    check_equal(committed_authority.capability_mask, restored_authority.capability_mask);
    check_not_equal(committed_authority.generation, restored_authority.generation);
    check_equal(turbo_agent_tool_authority_snapshot_is_current(
        turbo_agent_session_agent(session), &committed_authority), 0);
    {
      size_t retry;
      for (retry = 0; retry < 2; ++retry) {
        run_rc = run_compiled_inspect_attempt(harness, "finalization-replay",
            compiled_inspect_source, allowed, 1, &diagnostic, &tool_status,
            &result, &certificate);
        check_equal(run_rc, SALTS_EPROTO);
        check_equal(diagnostic.status, TURBO_AGENT_COMPILE_SOURCE_INVALID);
        check_equal(tool_status, TURBO_TOOL_ERROR);
        check_null(result);
        check_not_null(certificate);
        check_equal(probe.calls, 1);
        check_equal(probe.saw_started_journal, 1);
        json_free(certificate);
        certificate = NULL;
      }
    }

    check_equal(turbo_agent_runtime_store_list_json(
        probe.runtime, COMPILED_INSPECT_JOURNAL_COLLECTION, "thread_id",
        probe.thread_id, &journal_records), SALTS_OK);
    check_not_null(journal_records);
    check_equal(json_array_size(journal_records), 1u);
    journal = json_array_get(journal_records, 0);
    check_not_null(journal);
    check_equal(json_get_string(journal, "phase"), "committed");
    check_equal(json_get_string(journal, "attempt_id"), "finalization-replay");
    check_equal(json_get_double(journal, "authority_generation", -1.0),
                (double)committed_authority.generation);
    check_true(json_get_bool(json_object_get(journal, "result"), "ok", false));
    turbo_runtime_json_destroy(journal_records);
    journal_records = NULL;

    check_equal(turbo_agent_runtime_store_list_json(
        probe.runtime, COMPILED_INSPECT_RUN_JOURNAL_COLLECTION, "thread_id",
        probe.thread_id, &journal_records), SALTS_OK);
    check_not_null(journal_records);
    check_equal(json_array_size(journal_records), 2u);
    {
      size_t i;
      for (i = 0; i < json_array_size(journal_records); ++i) {
        const json_value_t *run_journal = json_array_get(journal_records, i);
        check_not_null(run_journal);
        check_equal(json_get_string(run_journal, "phase"), "committed");
        check_equal(json_get_string(run_journal, "attempt_id"), "finalization-replay");
        check_equal(json_get_double(run_journal, "authority_generation", -1.0),
                    (double)committed_authority.generation);
        check_not_null(json_get_string(run_journal, "attempt_record_id"));
        check_null(json_object_get(run_journal, "result"));
      }
    }
    turbo_runtime_json_destroy(journal_records);
    journal_records = NULL;

    turbo_runtime_json_destroy(prepare_summary);
    turbo_runtime_json_destroy(prepare_state);
    json_free(message);
    free(inbox_id);
    json_free(result);
    json_free(certificate);
    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
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
    attach_compiled_probe_runtime(harness, &probe);
    {
      int run_rc = run_compiled_inspect(harness, compiled_inspect_source,
          allowed, 1, &diagnostic, &tool_status, &result, &certificate);
      /* This assertion isolates schema-mismatch fail-closed behavior;
       * Session finalization failure is covered by the dedicated test above. */
      check_equal(probe.calls, 1);
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
      check_true(strstr(json_get_string(journal, "error") ?
                        json_get_string(journal, "error") : "",
                        "violating its admitted schema") != NULL);
      check_null(json_object_get(journal, "result"));
      turbo_runtime_json_destroy(journal_records);
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
    /* New Agents grant no tool authority. A compiler allowlist cannot
     * turn registration or model intent into a Host capability grant. */

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

  it("rejects an old committed generation after a quiescent same-grant policy update") {
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
    {
      turbo_agent_t *agent = turbo_agent_session_agent(
          turbo_agent_app_session(turbo_agent_harness_app(harness)));
      turbo_agent_tool_authority_snapshot_t before = {0};
      turbo_agent_tool_authority_snapshot_t after = {0};
      check_not_null(agent);
      check_equal(turbo_agent_tool_authority_snapshot_get(agent, &before), 0);
      check_equal(run_compiled_inspect_attempt(harness, "quiescent-generation",
          compiled_inspect_source, allowed, 1, &diagnostic, &tool_status,
          &result, &certificate), SALTS_OK);
      check_true(json_get_bool(result, "ok", false));
      check_equal(probe.calls, 1);
      json_free(result);
      result = NULL;
      json_free(certificate);
      certificate = NULL;

      /* An unchanged generation can deliver the committed result without
       * dispatching the callback again. */
      check_equal(run_compiled_inspect_attempt(harness, "quiescent-generation",
          compiled_inspect_source, allowed, 1, &diagnostic, &tool_status,
          &result, &certificate), SALTS_OK);
      check_equal(tool_status, TURBO_TOOL_OK);
      check_true(json_get_bool(result, "ok", false));
      check_not_null(certificate);
      check_equal(probe.calls, 1);
      check_equal(probe.saw_started_journal, 1);
      json_free(result);
      result = NULL;
      json_free(certificate);
      certificate = NULL;

      /* The synchronous Run and all its callbacks have returned. The Host
       * serializes policy replacement here, never inside a callback. Keeping
       * identical grants isolates generation identity from capability denial. */
      check_equal(set_compiled_host_policy(harness, 1), 0);
      check_equal(turbo_agent_tool_authority_snapshot_get(agent, &after), 0);
      check_equal(before.capability_mask, after.capability_mask);
      check_not_equal(before.generation, after.generation);
      check_equal(turbo_agent_tool_authority_snapshot_is_current(agent, &before), 0);
      check_equal(turbo_agent_tool_authority_snapshot_is_current(agent, &after), 1);

      check_not_equal(run_compiled_inspect_attempt(harness, "quiescent-generation",
          compiled_inspect_source, allowed, 1, &diagnostic, &tool_status,
          &result, &certificate), SALTS_OK);
      check_null(result);
      check_equal(tool_status, TURBO_TOOL_ERROR);
      check_equal(diagnostic.status, TURBO_AGENT_COMPILE_SOURCE_INVALID);
      check_true(strstr(diagnostic.message, "identity or contract") != NULL);
      check_equal(probe.calls, 1);
      check_equal(probe.saw_started_journal, 1);
    }

    check_equal(turbo_agent_runtime_store_list_json(
        probe.runtime, COMPILED_INSPECT_JOURNAL_COLLECTION, "thread_id",
        probe.thread_id, &journal_records), SALTS_OK);
    check_not_null(journal_records);
    check_equal(json_array_size(journal_records), 1u);
    journal = json_array_get(journal_records, 0);
    check_not_null(journal);
    check_equal(json_get_string(journal, "phase"), "committed");
    check_equal(json_get_string(journal, "tool_name"), "repo.inspect");
    check_equal(json_get_int(journal, "tool_status", -1), TURBO_TOOL_OK);
    check_true(json_get_bool(json_object_get(journal, "result"), "ok", false));

    turbo_runtime_json_destroy(journal_records);
    json_free(certificate);
    certificate = NULL;

    /* A fresh attempt under the replacement policy still executes and delivers
     * a valid result. Stale replay rejection must not poison the Harness. */
    check_equal(run_compiled_inspect_attempt(harness, "replacement-generation",
        compiled_inspect_source, allowed, 1, &diagnostic, &tool_status,
        &result, &certificate), SALTS_OK);
    check_equal(diagnostic.status, TURBO_AGENT_COMPILE_OK);
    check_equal(tool_status, TURBO_TOOL_OK);
    check_true(json_get_bool(result, "ok", false));
    check_not_null(certificate);
    check_equal(probe.calls, 2);
    check_equal(probe.saw_started_journal, 2);

    json_free(result);
    json_free(certificate);
    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }

}

static int run_job_inspect(turbo_agent_harness_t *harness, const char *id, uint32_t revision,
    turbo_agent_compile_diagnostic_t *diagnostic, turbo_tool_status_t *status,
    json_value_t **result, json_value_t **certificate, json_value_t **reference) {
  const char *allowed[] = {"runtime_tools"};
  turbo_agent_compiler_config_t config;
  turbo_agent_compiler_config_init(&config);
  config.allowed_capabilities = allowed;
  config.allowed_capability_count = 1;
  return turbo_agent_harness_execute_job_inspect_json(harness, id, revision,
      &config, NULL, diagnostic, status, result, certificate, reference);
}

static int record_acceptance(turbo_agent_harness_t *harness, const json_value_t *reference,
    turbo_agent_job_acceptance_decision_t decision, turbo_agent_compile_diagnostic_t *diagnostic,
    json_value_t **out) {
  const char *allowed[] = {"runtime_tools"};
  turbo_agent_compiler_config_t config;
  turbo_agent_compiler_config_init(&config);
  config.allowed_capabilities = allowed; config.allowed_capability_count = 1;
  return turbo_agent_harness_record_job_acceptance_json(harness, "job-bound", 2,
      &config, reference, decision, "host-evidence-v1", diagnostic, out);
}
static int read_acceptance(turbo_agent_harness_t *harness,
    turbo_agent_compile_diagnostic_t *diagnostic, json_value_t **out) {
  const char *allowed[] = {"runtime_tools"};
  turbo_agent_compiler_config_t config;
  turbo_agent_compiler_config_init(&config);
  config.allowed_capabilities = allowed; config.allowed_capability_count = 1;
  return turbo_agent_harness_get_job_acceptance_json(harness, "job-bound", 2, &config, diagnostic, out);
}

static int delivery_operation(turbo_agent_harness_t *harness, int op, const char *delivery,
    turbo_agent_compile_diagnostic_t *diagnostic, json_value_t **out) {
  const char *allowed[] = {"runtime_tools"};
  turbo_agent_compiler_config_t config;
  turbo_agent_compiler_config_init(&config);
  config.allowed_capabilities = allowed; config.allowed_capability_count = 1;
  if (op == 0) return turbo_agent_harness_prepare_job_delivery_json(harness, "job-bound", 2,
      &config, delivery, "host-destination-v1", "host-payload-v1", diagnostic, out);
  if (op == 2) return turbo_agent_harness_record_job_delivery_receipt_json(harness, "job-bound", 2,
      &config, delivery, "host-receipt-v1", diagnostic, out);
  return turbo_agent_harness_get_job_delivery_json(harness, "job-bound", 2, &config, diagnostic, out);
}
static int accept_job_for_delivery(turbo_agent_harness_t *harness,
    turbo_agent_job_acceptance_decision_t decision, turbo_agent_compile_diagnostic_t *diagnostic) {
  json_value_t *result = NULL, *certificate = NULL, *reference = NULL, *record = NULL;
  turbo_tool_status_t status;
  int rc = run_job_inspect(harness, "job-bound", 2, diagnostic, &status, &result, &certificate, &reference);
  if (rc == SALTS_OK) rc = record_acceptance(harness, reference, decision, diagnostic, &record);
  json_free(result); json_free(certificate); json_free(reference); json_free(record);
  return rc;
}

spec("Harness persisted Job dispatch") {
  static cmeta_threadpool_t *pool;
  static turbo_tool_registry_t *registry;
  static turbo_agent_harness_t *harness;
  static turbo_agent_job_t *job, *other;
  static compiled_inspect_probe_t probe;
  static compiled_inspect_store_fault_t *fault;
  static turbo_agent_compile_diagnostic_t diagnostic;
  static turbo_tool_status_t status;
  static json_value_t *result, *certificate, *reference, *record, *snapshot;

  before_each() {
    cmeta_threadpool_config_t options = {.num_threads = 1, .queue_capacity = 1};
    turbo_agent_runtime_store_t callbacks = compiled_inspect_store_fail_phase(NULL);
    pool = cmeta_threadpool_create_with_config(&options);
    registry = turbo_tool_registry_create();
    probe = (compiled_inspect_probe_t){0};
    diagnostic = (turbo_agent_compile_diagnostic_t){0};
    status = TURBO_TOOL_ERROR;
    fault = callbacks.user_data;
    check_not_null(pool);
    check_not_null(registry);
    check_not_null(fault);
    check_equal(register_compiled_inspect(registry, &probe, TURBO_TOOL_EFFECT_READ, 1), TURBO_TOOL_OK);
    harness = make_compiled_harness_with_store(pool, registry, callbacks);
    check_not_null(harness);
    attach_compiled_probe_runtime(harness, &probe);
    check_equal(set_compiled_host_policy(harness, 1), 0);
    check_equal(turbo_agent_job_create(probe.runtime, "job-bound", "inspect", "Host verifies", &job), SALTS_OK);
    check_equal(turbo_agent_job_propose(job, 0, compiled_inspect_source,
                strlen(compiled_inspect_source), "source-v1"), SALTS_OK);
    check_equal(turbo_agent_job_bind_compiled_inspect(job, 1,
                COMPILED_INSPECT_TEST_THREAD_ID, "bound-attempt"), SALTS_OK);
  }
  after_each() {
    json_free(result); json_free(certificate); json_free(reference); json_free(record); json_free(snapshot);
    result = certificate = reference = record = snapshot = NULL;
    turbo_agent_job_destroy(job); turbo_agent_job_destroy(other); job = other = NULL;
    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }

  it("executes the frozen source and returns exact attempt, journal and Runtime Run references") {
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_OK);
    check_equal(status, TURBO_TOOL_OK);
    check_equal(probe.calls, 1);
    check_equal(probe.last_value, "hello");
    check_not_null(result); check_not_null(certificate); check_not_null(reference);
    check_equal(json_get_string(reference, "job_id"), "job-bound");
    check_equal(json_get_int(reference, "job_revision", -1), 2);
    check_equal(json_get_int(reference, "proposal_version", -1), 1);
    check_equal(json_get_string(reference, "attempt_id"), "bound-attempt");
    check_equal(turbo_agent_runtime_store_get_json_status(probe.runtime,
        COMPILED_INSPECT_JOURNAL_COLLECTION, json_get_string(reference, "attempt_record_id"), &record), SALTS_OK);
    check_equal(json_get_string(record, "phase"), "committed");
    check_equal(json_get_string(record, "plan_hash"), json_get_string(reference, "plan_hash"));
    check_equal(json_get_string(record, "thread_id"), json_get_string(reference, "thread_id"));
    check_true(json_get_bool(json_object_get(record, "result"), "ok", false));
    json_free(record); record = NULL;
    check_equal(turbo_agent_runtime_store_get_json_status(probe.runtime,
        COMPILED_INSPECT_RUN_JOURNAL_COLLECTION, json_get_string(reference, "execution_id"), &record), SALTS_OK);
    check_equal(json_get_string(record, "phase"), "committed");
    check_equal(json_get_string(record, "attempt_record_id"), json_get_string(reference, "attempt_record_id"));
    check_null(json_object_get(record, "result"));
    json_free(record); record = NULL;
    check_equal(turbo_agent_runtime_store_get_json_status(probe.runtime,
        "runs", json_get_string(reference, "run_id"), &record), SALTS_OK);
    check_equal(json_get_string(record, "status"), "completed");
    check_equal(json_get_string(record, "thread_id"), json_get_string(reference, "run_thread_id"));
    check_not_equal(json_get_string(reference, "run_id"), json_get_string(reference, "execution_id"));
    check_null(json_object_get(reference, "result"));
    check_equal(turbo_agent_job_get_snapshot(job, &snapshot), SALTS_OK);
    check_equal(json_get_string(snapshot, "state"), "attempt_bound");
    check_null(json_object_get(snapshot, "result"));
  }

  it("replays the bound committed attempt once overall and still rejects revoked authority") {
    json_value_t *first;
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_OK);
    first = reference; reference = NULL;
    json_free(result); result = NULL; json_free(certificate); certificate = NULL;
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_OK);
    check_equal(probe.calls, 1);
    check_equal(json_get_string(reference, "attempt_record_id"), json_get_string(first, "attempt_record_id"));
    check_not_equal(json_get_string(reference, "run_id"), json_get_string(first, "run_id"));
    json_free(first);
    json_free(reference); reference = NULL; json_free(result); result = NULL; json_free(certificate); certificate = NULL;
    check_equal(set_compiled_host_policy(harness, 0), 0);
    check_not_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_OK);
    check_equal(probe.calls, 1);
    check_null(result); check_null(certificate); check_null(reference);
  }

  it("rejects stale or unbound Jobs and preserves store read errors before Run creation") {
    check_equal(run_job_inspect(harness, "job-bound", 1, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_EBUSY);
    check_equal(turbo_agent_job_create(probe.runtime, "unbound", "goal", "criteria", &other), SALTS_OK);
    check_equal(run_job_inspect(harness, "unbound", 0, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_EBUSY);
    check_equal(run_job_inspect(harness, "missing", 0, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_ENOENT);
    fault->job_read_status = SALTS_EIO;
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_EIO);
    fault->job_read_status = 0;
    check_equal(probe.calls, 0);
    check_null(result); check_null(certificate); check_null(reference);
    check_null(turbo_agent_session_last_run_id(turbo_agent_app_session(turbo_agent_harness_app(harness))));
    /* Rejections must release the Harness submission exclusion. */
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_OK);
  }

  it("rejects another thread scope and cannot resolve a Job from another Runtime") {
    turbo_agent_runtime_store_t foreign_store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *foreign = turbo_agent_runtime_create(&foreign_store);
    check_not_null(foreign);
    check_equal(turbo_agent_job_create(foreign, "foreign-only", "goal", "criteria", &other), SALTS_OK);
    check_equal(run_job_inspect(harness, "foreign-only", 0, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_ENOENT);
    turbo_agent_job_destroy(other); other = NULL;
    turbo_agent_runtime_destroy(foreign);
    check_equal(turbo_agent_job_create(probe.runtime, "wrong-scope", "goal", "criteria", &other), SALTS_OK);
    check_equal(turbo_agent_job_propose(other, 0, compiled_inspect_source,
                strlen(compiled_inspect_source), "source-v1"), SALTS_OK);
    check_equal(turbo_agent_job_bind_compiled_inspect(other, 1, "another-thread", "another-attempt"), SALTS_OK);
    check_equal(run_job_inspect(harness, "wrong-scope", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_EPROTO);
    check_equal(probe.calls, 0);
    check_null(result); check_null(certificate); check_null(reference);
    check_null(turbo_agent_session_last_run_id(turbo_agent_app_session(turbo_agent_harness_app(harness))));
  }

  it("uses the newest frozen proposal and reports its exact version") {
    check_equal(turbo_agent_job_create(probe.runtime, "edited-job", "goal", "criteria", &other), SALTS_OK);
    check_equal(turbo_agent_job_propose(other, 0, compiled_inspect_source,
                strlen(compiled_inspect_source), "source-v1"), SALTS_OK);
    check_equal(turbo_agent_job_propose(other, 1, compiled_inspect_source_alternate,
                strlen(compiled_inspect_source_alternate), "source-v2"), SALTS_OK);
    check_equal(turbo_agent_job_bind_compiled_inspect(other, 2,
                COMPILED_INSPECT_TEST_THREAD_ID, "edited-attempt"), SALTS_OK);
    check_equal(run_job_inspect(harness, "edited-job", 3, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_OK);
    check_equal(probe.calls, 1);
    check_equal(probe.last_value, "different");
    check_equal(json_get_int(reference, "proposal_version", -1), 2);
    check_equal(json_get_int(reference, "job_revision", -1), 3);
  }

  it("still compiles persisted source and rejects reuse of an attempt under a different plan") {
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_OK);
    json_free(result); result = NULL; json_free(certificate); certificate = NULL;
    json_free(reference); reference = NULL;
    check_equal(turbo_agent_job_create(probe.runtime, "other-plan", "goal", "criteria", &other), SALTS_OK);
    check_equal(turbo_agent_job_propose(other, 0, compiled_inspect_source_alternate,
                strlen(compiled_inspect_source_alternate), "source-v2"), SALTS_OK);
    check_equal(turbo_agent_job_bind_compiled_inspect(other, 1,
                COMPILED_INSPECT_TEST_THREAD_ID, "bound-attempt"), SALTS_OK);
    check_equal(run_job_inspect(harness, "other-plan", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_EPROTO);
    check_equal(probe.calls, 1);
    check_null(result); check_null(reference); check_not_null(certificate);
    json_free(certificate); certificate = NULL;
    turbo_agent_job_destroy(other); other = NULL;
    check_equal(turbo_agent_job_create(probe.runtime, "invalid-source", "goal", "criteria", &other), SALTS_OK);
    check_equal(turbo_agent_job_propose(other, 0, "{broken", 7, "source-v1"), SALTS_OK);
    check_equal(turbo_agent_job_bind_compiled_inspect(other, 1,
                COMPILED_INSPECT_TEST_THREAD_ID, "invalid-attempt"), SALTS_OK);
    check_equal(run_job_inspect(harness, "invalid-source", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_EPROTO);
    check_equal(probe.calls, 1);
    check_null(result); check_null(reference); check_null(certificate);
  }

  it("holds submission exclusion while reading persisted Job metadata") {
    fault->nested_harness = harness;
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_OK);
    check_equal(fault->nested_entered, 1);
    check_equal(fault->nested_status, SALTS_EBUSY);
    check_equal(probe.calls, 1);
  }

  it("persists one explicit Host acceptance with canonical references and no second result") {
    int rc;
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_OK);
    rc = record_acceptance(harness, reference, TURBO_AGENT_JOB_ACCEPTED, &diagnostic, &record);
    if (rc) fprintf(stderr, "Acceptance failed: %d, %s\n", rc, diagnostic.message);
    check_equal(rc, SALTS_OK);
    check_equal(probe.calls, 1);
    check_equal(json_get_string(record, "decision"), "accepted");
    check_equal(json_get_string(record, "evidence_ref"), "host-evidence-v1");
    check_null(json_object_get(record, "result"));
    check_null(json_object_get(json_object_get(record, "execution_reference"), "result"));
    json_free(record); record = NULL;
    check_equal(read_acceptance(harness, &diagnostic, &record), SALTS_OK);
    check_equal(json_get_string(record, "decision"), "accepted");
    json_free(record); record = NULL;
    check_equal(record_acceptance(harness, reference, TURBO_AGENT_JOB_REJECTED,
                &diagnostic, &record), SALTS_EALREADY);
    check_null(record);
    check_equal(read_acceptance(harness, &diagnostic, &record), SALTS_OK);
    check_equal(json_get_string(record, "decision"), "accepted");
    check_equal(turbo_agent_job_get_snapshot(job, &snapshot), SALTS_OK);
    check_equal(json_get_string(snapshot, "state"), "attempt_bound");
    check_equal(probe.calls, 1);
  }

  it("records Host quality rejection independently of successful tool status") {
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_OK);
    check_equal(status, TURBO_TOOL_OK);
    check_equal(record_acceptance(harness, reference, TURBO_AGENT_JOB_REJECTED,
                &diagnostic, &record), SALTS_OK);
    check_equal(json_get_string(record, "decision"), "rejected");
    check_equal(probe.calls, 1);
    check_equal(fault->acceptance_claims, 1);
    check_equal(fault->acceptance_puts, 0);
  }

  it("does not retry or read back a lost acceptance acknowledgement") {
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_OK);
    fault->acceptance_status = SALTS_EIO; fault->acceptance_after_commit = 1;
    check_equal(record_acceptance(harness, reference, TURBO_AGENT_JOB_ACCEPTED,
                &diagnostic, &record), SALTS_EIO);
    check_null(record);
    check_equal(fault->reads_after_acceptance_error, 0);
    check_equal(fault->acceptance_claims, 1);
    fault->acceptance_unacknowledged = 0; fault->acceptance_status = 0;
    check_equal(read_acceptance(harness, &diagnostic, &record), SALTS_OK);
    check_equal(json_get_string(record, "decision"), "accepted");
    check_equal(probe.calls, 1);
    check_equal(turbo_agent_session_last_run_id(turbo_agent_app_session(turbo_agent_harness_app(harness))),
                json_get_string(reference, "run_id"));
  }

  it("fails closed on unsupported or failed acceptance publication without a put fallback") {
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_OK);
    fault->acceptance_status = SALTS_ENOTSUP;
    check_equal(record_acceptance(harness, reference, TURBO_AGENT_JOB_ACCEPTED,
                &diagnostic, &record), SALTS_ENOTSUP);
    check_null(record);
    check_equal(fault->reads_after_acceptance_error, 0);
    fault->acceptance_unacknowledged = 0; fault->acceptance_status = SALTS_ENOSPC;
    check_equal(record_acceptance(harness, reference, TURBO_AGENT_JOB_ACCEPTED,
                &diagnostic, &record), SALTS_ENOSPC);
    check_null(record);
    check_equal(fault->acceptance_claims, 2);
    check_equal(fault->acceptance_puts, 0);
    fault->acceptance_unacknowledged = 0; fault->acceptance_status = 0;
    check_equal(read_acceptance(harness, &diagnostic, &record), SALTS_ENOENT);
    check_null(record);
    check_equal(probe.calls, 1);
  }

  it("rejects every forged reference identity and preserves canonical read errors") {
    const char *string_keys[] = {"job_id", "thread_id", "attempt_id", "attempt_record_id",
        "run_id", "run_thread_id", "execution_id", "plan_hash"};
    const char *number_keys[] = {"job_revision", "proposal_version", "authority_generation"};
    const char *collections[] = {COMPILED_INSPECT_JOURNAL_COLLECTION,
        COMPILED_INSPECT_RUN_JOURNAL_COLLECTION, "runs"};
    size_t i;
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_OK);
    for (i = 0; i < sizeof(string_keys) / sizeof(string_keys[0]); ++i) {
      snapshot = json_clone(reference);
      check_equal(turbo_runtime_json_object_set_string(snapshot, string_keys[i], "forged"), 0);
      check_not_equal(record_acceptance(harness, snapshot, TURBO_AGENT_JOB_ACCEPTED,
                  &diagnostic, &record), SALTS_OK);
      check_null(record);
      json_free(snapshot); snapshot = NULL;
    }
    for (i = 0; i < sizeof(number_keys) / sizeof(number_keys[0]); ++i) {
      snapshot = json_clone(reference);
      check_equal(turbo_runtime_json_object_set_int64(snapshot, number_keys[i], 9), 0);
      check_not_equal(record_acceptance(harness, snapshot, TURBO_AGENT_JOB_ACCEPTED,
                  &diagnostic, &record), SALTS_OK);
      check_null(record);
      json_free(snapshot); snapshot = NULL;
    }
    for (i = 0; i < sizeof(collections) / sizeof(collections[0]); ++i) {
      fault->proof_read_failure = collections[i];
      check_equal(record_acceptance(harness, reference, TURBO_AGENT_JOB_ACCEPTED,
                  &diagnostic, &record), SALTS_EIO);
      check_null(record);
    }
    fault->proof_read_failure = NULL;
    check_equal(fault->acceptance_claims, 0);
    check_equal(probe.calls, 1);
  }

  it("rejects a cross-wired completed Run and its compiled journal identity") {
    json_value_t *first;
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_OK);
    first = reference; reference = NULL;
    json_free(result); result = NULL; json_free(certificate); certificate = NULL;
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_string(first, "run_id", json_get_string(reference, "run_id")), 0);
    check_equal(record_acceptance(harness, first, TURBO_AGENT_JOB_ACCEPTED,
                &diagnostic, &record), SALTS_EPROTO);
    check_null(record);
    check_equal(fault->acceptance_claims, 0);
    check_equal(probe.calls, 1);
    json_free(first);
  }

  it("rejects invalid canonical result, pending outcome and corrupted Run snapshot") {
    json_value_t *original;
    const char *key;
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_OK);
    key = json_get_string(reference, "attempt_record_id");
    check_equal(turbo_agent_runtime_store_get_json_status(probe.runtime,
        COMPILED_INSPECT_JOURNAL_COLLECTION, key, &snapshot), SALTS_OK);
    original = json_clone(snapshot);
    check_equal(turbo_runtime_json_object_set_string(snapshot, "result", "invalid typed result"), 0);
    check_equal(turbo_agent_runtime_store_put_json(probe.runtime, COMPILED_INSPECT_JOURNAL_COLLECTION, key, snapshot), SALTS_OK);
    check_equal(record_acceptance(harness, reference, TURBO_AGENT_JOB_ACCEPTED,
                &diagnostic, &record), SALTS_EPROTO);
    check_equal(turbo_runtime_json_object_set_string(snapshot, "phase", "started"), 0);
    check_equal(turbo_agent_runtime_store_put_json(probe.runtime, COMPILED_INSPECT_JOURNAL_COLLECTION, key, snapshot), SALTS_OK);
    check_equal(record_acceptance(harness, reference, TURBO_AGENT_JOB_ACCEPTED,
                &diagnostic, &record), SALTS_EBUSY);
    check_equal(turbo_agent_runtime_store_put_json(probe.runtime, COMPILED_INSPECT_JOURNAL_COLLECTION, key, original), SALTS_OK);
    json_free(original); json_free(snapshot); snapshot = NULL;
    key = json_get_string(reference, "run_id");
    check_equal(turbo_agent_runtime_store_get_json_status(probe.runtime, "runs", key, &snapshot), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_string(json_object_get(json_object_get(snapshot,
        "state_snapshot"), "compiled_execution"), "execution_id", "forged"), 0);
    check_equal(turbo_agent_runtime_store_put_json(probe.runtime, "runs", key, snapshot), SALTS_OK);
    check_equal(record_acceptance(harness, reference, TURBO_AGENT_JOB_ACCEPTED,
                &diagnostic, &record), SALTS_EPROTO);
    check_equal(fault->acceptance_claims, 0);
    check_equal(probe.calls, 1);
  }

  it("withholds stored decisions after revocation or same-grant generation replacement") {
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_OK);
    check_equal(record_acceptance(harness, reference, TURBO_AGENT_JOB_ACCEPTED,
                &diagnostic, &record), SALTS_OK);
    json_free(record); record = NULL;
    check_equal(set_compiled_host_policy(harness, 1), 0);
    check_equal(read_acceptance(harness, &diagnostic, &record), SALTS_EPROTO);
    check_null(record);
    check_equal(set_compiled_host_policy(harness, 0), 0);
    check_equal(read_acceptance(harness, &diagnostic, &record), SALTS_EPROTO);
    check_null(record);
    check_equal(turbo_agent_runtime_store_get_json_status(probe.runtime,
        "agent_job_acceptance", "job-bound", &record), SALTS_OK);
    check_equal(json_get_string(record, "decision"), "accepted");
    check_equal(probe.calls, 1);
  }

  it("rejects future or malformed acceptance records and cannot infer acceptance from a tool result") {
    check_equal(read_acceptance(harness, &diagnostic, &record), SALTS_ENOENT);
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_OK);
    check_equal(read_acceptance(harness, &diagnostic, &record), SALTS_ENOENT);
    check_equal(record_acceptance(harness, reference, TURBO_AGENT_JOB_ACCEPTED,
                &diagnostic, &record), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_int64(record, "schema_version", 2), 0);
    check_equal(turbo_agent_runtime_store_put_json(probe.runtime, "agent_job_acceptance", "job-bound", record), SALTS_OK);
    json_free(record); record = NULL;
    check_equal(read_acceptance(harness, &diagnostic, &record), SALTS_ENOTSUP);
    check_null(record);
    check_equal(probe.calls, 1);
  }

  it("persists one delivery intent and an explicit matching Host receipt without execution") {
    const json_value_t *intent;
    check_equal(accept_job_for_delivery(harness, TURBO_AGENT_JOB_ACCEPTED, &diagnostic), SALTS_OK);
    check_equal(delivery_operation(harness, 1, NULL, &diagnostic, &record), SALTS_ENOENT);
    check_equal(delivery_operation(harness, 0, "delivery-1", &diagnostic, &record), SALTS_OK);
    check_equal(json_object_size(record), (size_t)9);
    check_equal(json_get_string(record, "idempotency_key"), "job-bound:delivery-1");
    check_null(json_object_get(record, "result"));
    json_free(record); record = NULL;
    check_equal(delivery_operation(harness, 0, "different", &diagnostic, &record), SALTS_EALREADY);
    check_null(record);
    check_equal(delivery_operation(harness, 1, NULL, &diagnostic, &record), SALTS_OK);
    check_equal(json_get_string(record, "status"), "outcome_unknown");
    check_null(json_object_get(record, "receipt"));
    intent = json_object_get(record, "intent");
    check_equal(json_get_string(intent, "destination_ref"), "host-destination-v1");
    check_equal(json_get_string(intent, "payload_ref"), "host-payload-v1");
    json_free(record); record = NULL;
    check_equal(delivery_operation(harness, 2, "different", &diagnostic, &record), SALTS_EPROTO);
    check_equal(delivery_operation(harness, 2, "delivery-1", &diagnostic, &record), SALTS_OK);
    check_equal(json_object_size(record), (size_t)7);
    check_equal(json_get_string(record, "receipt_ref"), "host-receipt-v1");
    json_free(record); record = NULL;
    check_equal(delivery_operation(harness, 2, "delivery-1", &diagnostic, &record), SALTS_EALREADY);
    check_equal(delivery_operation(harness, 1, NULL, &diagnostic, &record), SALTS_OK);
    check_equal(json_get_string(record, "status"), "delivered");
    check_not_null(json_object_get(record, "receipt"));
    check_equal(fault->delivery_claims, 2);
    check_equal(fault->delivery_puts, 0);
    check_equal(probe.calls, 1);
  }

  it("requires explicit acceptance before any delivery publication") {
    check_equal(delivery_operation(harness, 0, "delivery-1", &diagnostic, &record), SALTS_ENOENT);
    check_equal(accept_job_for_delivery(harness, TURBO_AGENT_JOB_REJECTED, &diagnostic), SALTS_OK);
    for (int op = 0; op < 3; ++op) {
      check_equal(delivery_operation(harness, op, "delivery-1", &diagnostic, &record), SALTS_EPERM);
      check_null(record);
    }
    check_equal(fault->delivery_claims, 0);
    check_equal(probe.calls, 1);
  }

  it("preserves lost intent and receipt acknowledgements without retry or readback") {
    const char *collections[] = {"agent_job_delivery_intents", "agent_job_delivery_receipts"};
    check_equal(accept_job_for_delivery(harness, TURBO_AGENT_JOB_ACCEPTED, &diagnostic), SALTS_OK);
    for (int i = 0; i < 2; ++i) {
      fault->delivery_collection = collections[i];
      fault->delivery_status = SALTS_EIO; fault->delivery_after_commit = 1;
      check_equal(delivery_operation(harness, i ? 2 : 0, "delivery-1", &diagnostic, &record), SALTS_EIO);
      check_null(record);
      check_equal(fault->reads_after_delivery_error, 0);
      check_equal(fault->delivery_claims, i + 1);
      fault->delivery_unacknowledged = 0; fault->delivery_status = 0;
      check_equal(delivery_operation(harness, 1, NULL, &diagnostic, &record), SALTS_OK);
      check_equal(json_get_string(record, "status"), i ? "delivered" : "outcome_unknown");
      json_free(record); record = NULL;
      check_equal(delivery_operation(harness, i ? 2 : 0, "delivery-1", &diagnostic, &record), SALTS_EALREADY);
    }
    check_equal(fault->delivery_puts, 0);
    check_equal(probe.calls, 1);
  }

  it("preserves unsupported, storage and atomic-claim errors with no put fallback") {
    const char *collections[] = {"agent_job_delivery_intents", "agent_job_delivery_receipts"};
    const int statuses[] = {SALTS_ENOTSUP, SALTS_ENOSPC, SALTS_EIO};
    check_equal(accept_job_for_delivery(harness, TURBO_AGENT_JOB_ACCEPTED, &diagnostic), SALTS_OK);
    for (int i = 0; i < 2; ++i) {
      fault->delivery_collection = collections[i];
      for (size_t j = 0; j < sizeof(statuses) / sizeof(statuses[0]); ++j) {
        fault->delivery_status = statuses[j];
        check_equal(delivery_operation(harness, i ? 2 : 0, "delivery-1", &diagnostic, &record), statuses[j]);
        check_null(record);
        check_equal(fault->reads_after_delivery_error, 0);
        fault->delivery_unacknowledged = 0;
      }
      fault->delivery_status = 0;
      fault->delivery_compete = 1; /* Another publisher wins after our read. */
      check_equal(delivery_operation(harness, i ? 2 : 0, "delivery-1", &diagnostic, &record), SALTS_EALREADY);
      check_null(record);
    }
    check_equal(fault->delivery_claims, 8);
    check_equal(fault->delivery_puts, 0);
    check_equal(probe.calls, 1);
  }

  it("revalidates canonical acceptance and preserves every delivery read failure") {
    const char *collections[] = {"agent_job_acceptance", "compiled_inspect_attempts",
        "agent_compiled_execution_journal", "runs", "agent_job_delivery_intents", "agent_job_delivery_receipts"};
    check_equal(accept_job_for_delivery(harness, TURBO_AGENT_JOB_ACCEPTED, &diagnostic), SALTS_OK);
    for (size_t i = 0; i < sizeof(collections) / sizeof(collections[0]); ++i) {
      fault->proof_read_failure = collections[i];
      for (int op = 0; op < 3; ++op) {
        check_equal(delivery_operation(harness, op, "delivery-1", &diagnostic, &record), SALTS_EIO);
        check_null(record);
      }
    }
    fault->proof_read_failure = NULL;
    check_equal(fault->delivery_claims, 0);
    check_equal(probe.calls, 1);
  }

  it("does not publish or expose delivery after same-grant policy replacement") {
    check_equal(accept_job_for_delivery(harness, TURBO_AGENT_JOB_ACCEPTED, &diagnostic), SALTS_OK);
    check_equal(delivery_operation(harness, 0, "delivery-1", &diagnostic, &record), SALTS_OK);
    json_free(record); record = NULL;
    check_equal(set_compiled_host_policy(harness, 1), 0);
    for (int op = 0; op < 3; ++op) {
      check_equal(delivery_operation(harness, op, "delivery-1", &diagnostic, &record), SALTS_EPROTO);
      check_null(record);
    }
    check_equal(turbo_agent_runtime_store_get_json_status(probe.runtime,
        "agent_job_delivery_intents", "job-bound", &record), SALTS_OK);
    check_equal(json_get_string(record, "delivery_id"), "delivery-1");
    check_equal(fault->delivery_claims, 1);
    check_equal(probe.calls, 1);
  }

  it("rechecks typed canonical evidence even after acceptance and intent committed") {
    check_equal(accept_job_for_delivery(harness, TURBO_AGENT_JOB_ACCEPTED, &diagnostic), SALTS_OK);
    check_equal(delivery_operation(harness, 0, "delivery-1", &diagnostic, &record), SALTS_OK);
    json_free(record); record = NULL;
    check_equal(read_acceptance(harness, &diagnostic, &record), SALTS_OK);
    check_equal(turbo_agent_runtime_store_get_json_status(probe.runtime, "compiled_inspect_attempts",
        json_get_string(json_object_get(record, "execution_reference"), "attempt_record_id"), &snapshot), SALTS_OK);
    json_free(record); record = NULL;
    check_equal(turbo_runtime_json_object_set_string(snapshot, "result", "invalid typed result"), 0);
    check_equal(turbo_agent_runtime_store_put_json(probe.runtime, "compiled_inspect_attempts",
        json_get_string(snapshot, "id"), snapshot), SALTS_OK);
    check_equal(delivery_operation(harness, 1, NULL, &diagnostic, &record), SALTS_EPROTO);
    check_null(record);
    check_equal(turbo_runtime_json_object_set_string(snapshot, "phase", "started"), 0);
    check_equal(turbo_agent_runtime_store_put_json(probe.runtime, "compiled_inspect_attempts",
        json_get_string(snapshot, "id"), snapshot), SALTS_OK);
    check_equal(delivery_operation(harness, 2, "delivery-1", &diagnostic, &record), SALTS_EBUSY);
    check_null(record);
    check_equal(fault->delivery_claims, 1);
    check_equal(probe.calls, 1);
  }

  it("refuses orphan receipts without repairing them") {
    check_equal(accept_job_for_delivery(harness, TURBO_AGENT_JOB_ACCEPTED, &diagnostic), SALTS_OK);
    record = json_create_object();
    check_equal(turbo_runtime_json_object_set_int64(record, "schema_version", 1), 0);
    check_equal(turbo_agent_runtime_store_put_json(probe.runtime, "agent_job_delivery_receipts", "job-bound", record), SALTS_OK);
    json_free(record); record = NULL;
    for (int op = 0; op < 3; ++op)
      check_equal(delivery_operation(harness, op, "delivery-1", &diagnostic, &record), SALTS_EPROTO);
    check_equal(fault->delivery_claims, 0);
    check_equal(probe.calls, 1);
  }

  it("rejects each intent identity, unknown schema and extra result payload") {
    const char *fields[] = {"kind", "job_id", "job_revision", "proposal_version", "delivery_id",
        "idempotency_key", "destination_ref", "payload_ref", "result"};
    check_equal(accept_job_for_delivery(harness, TURBO_AGENT_JOB_ACCEPTED, &diagnostic), SALTS_OK);
    check_equal(delivery_operation(harness, 0, "delivery-1", &diagnostic, &record), SALTS_OK);
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
      snapshot = json_clone(record);
      check_equal(turbo_runtime_json_object_set_string(snapshot, fields[i], i == 4 ? "../bad" : ""), 0);
      check_equal(turbo_agent_runtime_store_put_json(probe.runtime, "agent_job_delivery_intents", "job-bound", snapshot), SALTS_OK);
      json_free(snapshot); snapshot = NULL;
      check_equal(delivery_operation(harness, 1, NULL, &diagnostic, &snapshot), SALTS_EPROTO);
      check_null(snapshot);
    }
    check_equal(turbo_runtime_json_object_set_int64(record, "schema_version", 2), 0);
    check_equal(turbo_agent_runtime_store_put_json(probe.runtime, "agent_job_delivery_intents", "job-bound", record), SALTS_OK);
    json_free(record); record = NULL;
    check_equal(delivery_operation(harness, 1, NULL, &diagnostic, &record), SALTS_ENOTSUP);
    check_equal(probe.calls, 1);
  }

  it("rejects each receipt identity and unknown receipt schema") {
    const char *fields[] = {"kind", "job_id", "job_revision", "delivery_id", "idempotency_key", "receipt_ref", "result"};
    check_equal(accept_job_for_delivery(harness, TURBO_AGENT_JOB_ACCEPTED, &diagnostic), SALTS_OK);
    check_equal(delivery_operation(harness, 2, "delivery-1", &diagnostic, &record), SALTS_ENOENT);
    check_equal(delivery_operation(harness, 0, "delivery-1", &diagnostic, &record), SALTS_OK);
    json_free(record); record = NULL;
    check_equal(delivery_operation(harness, 2, "delivery-1", &diagnostic, &record), SALTS_OK);
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
      snapshot = json_clone(record);
      check_equal(turbo_runtime_json_object_set_string(snapshot, fields[i], ""), 0);
      check_equal(turbo_agent_runtime_store_put_json(probe.runtime, "agent_job_delivery_receipts", "job-bound", snapshot), SALTS_OK);
      json_free(snapshot); snapshot = NULL;
      check_equal(delivery_operation(harness, 1, NULL, &diagnostic, &snapshot), SALTS_EPROTO);
      check_null(snapshot);
    }
    check_equal(turbo_runtime_json_object_set_int64(record, "schema_version", 2), 0);
    check_equal(turbo_agent_runtime_store_put_json(probe.runtime, "agent_job_delivery_receipts", "job-bound", record), SALTS_OK);
    json_free(record); record = NULL;
    check_equal(delivery_operation(harness, 1, NULL, &diagnostic, &record), SALTS_ENOTSUP);
    check_equal(probe.calls, 1);
  }

  it("clears delivery outputs for invalid arguments and rejects nested admission") {
    char oversized[1026];
    const char *allowed[] = {"runtime_tools"};
    turbo_agent_compiler_config_t config;
    memset(oversized, 'x', sizeof(oversized) - 1); oversized[sizeof(oversized) - 1] = '\0';
    turbo_agent_compiler_config_init(&config);
    config.allowed_capabilities = allowed; config.allowed_capability_count = 1;
    check_equal(delivery_operation(harness, 0, "../bad", &diagnostic, &record), SALTS_EINVAL);
    check_equal(delivery_operation(harness, 2, NULL, &diagnostic, &record), SALTS_EINVAL);
    check_equal(turbo_agent_harness_prepare_job_delivery_json(harness, "job-bound", 2, &config,
        "delivery-1", oversized, "payload", &diagnostic, &record), SALTS_EINVAL);
    check_equal(turbo_agent_harness_prepare_job_delivery_json(harness, "job-bound", 2, &config,
        "delivery-1", "destination", "", &diagnostic, &record), SALTS_EINVAL);
    check_equal(turbo_agent_harness_record_job_delivery_receipt_json(harness, "job-bound", 2, &config,
        "delivery-1", oversized, &diagnostic, &record), SALTS_EINVAL);
    record = (json_value_t *)&status;
    check_equal(turbo_agent_harness_get_job_delivery_json(harness, "../bad", 2, &config,
        &diagnostic, &record), SALTS_EINVAL);
    check_null(record);
    check_equal(accept_job_for_delivery(harness, TURBO_AGENT_JOB_ACCEPTED, &diagnostic), SALTS_OK);
    fault->nested_harness = harness;
    check_equal(delivery_operation(harness, 0, "delivery-1", &diagnostic, &record), SALTS_OK);
    check_equal(fault->nested_status, SALTS_EBUSY);
    check_equal(probe.calls, 1);
  }

  it("clears outputs on bad arguments and rejects aliased output slots") {
    reference = (json_value_t *)&status;
    check_equal(run_job_inspect(harness, "../bad", 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_EINVAL);
    check_null(reference);
    check_equal(run_job_inspect(harness, NULL, 2, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_EINVAL);
    check_equal(run_job_inspect(harness, "job-bound", UINT32_MAX, &diagnostic, &status,
                &result, &certificate, &reference), SALTS_EINVAL);
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
                &result, &certificate, &result), SALTS_EINVAL);
    check_null(result); check_null(certificate); check_null(reference);
    check_equal(probe.calls, 0);
  }
}

/* These callbacks observe borrowed real Job/canonical data; no synthetic result
 * or verifier-created authorization is passed into the production path. */
typedef struct job_verifier_probe_s {
  int sources, evidences, source_status, evidence_status, source_fail_at;
  int invalidate_during_evidence, last_decision, saw_typed_result;
  int source_nested_status, evidence_nested_status;
  turbo_agent_harness_t *nested;
} job_verifier_probe_t;
static int verifier_nested(turbo_agent_harness_t *harness) {
  turbo_agent_compile_diagnostic_t diagnostic;
  json_value_t *record = NULL;
  int rc = read_acceptance(harness, &diagnostic, &record);
  json_free(record);
  return rc;
}
static int verify_job_source(const json_value_t *snapshot, void *user) {
  job_verifier_probe_t *p = user;
  const json_value_t *proposal = json_object_get(snapshot, "proposal");
  ++p->sources;
  if (p->nested) p->source_nested_status = verifier_nested(p->nested);
  if (p->source_status) return p->source_status;
  if (p->source_fail_at == p->sources) return SALTS_EIO;
  if (!json_get_string(proposal, "source_revision") ||
      strcmp(json_get_string(proposal, "source_revision"), "source-v1") ||
      !json_get_string(proposal, "source_json") ||
      strcmp(json_get_string(proposal, "source_json"), compiled_inspect_source)) return SALTS_EPROTO;
  return SALTS_OK;
}
static int verify_job_evidence(const json_value_t *snapshot, const json_value_t *reference,
    const json_value_t *result, turbo_agent_job_acceptance_decision_t decision,
    const char *evidence_ref, void *user) {
  job_verifier_probe_t *p = user;
  ++p->evidences;
  p->last_decision = (int)decision;
  p->saw_typed_result = json_type(result) == JSON_OBJECT && json_get_bool(result, "ok", false);
  if (p->nested) p->evidence_nested_status = verifier_nested(p->nested);
  if (p->evidence_status) return p->evidence_status;
  if (!p->saw_typed_result || strcmp(evidence_ref, "host-evidence-v1") ||
      strcmp(json_get_string(snapshot, "id"), json_get_string(reference, "job_id")) ||
      !json_get_string(reference, "run_id") || !json_get_string(reference, "execution_id") ||
      json_get_int(snapshot, "revision", -1) != json_get_int(reference, "job_revision", -2)) return SALTS_EPROTO;
  if (p->invalidate_during_evidence) p->source_status = SALTS_EPROTO;
  return SALTS_OK;
}

spec("Harness mandatory Host Job verification") {
  static cmeta_threadpool_t *pool;
  static turbo_tool_registry_t *registry;
  static turbo_agent_harness_t *harness;
  static turbo_agent_job_t *job;
  static compiled_inspect_probe_t probe;
  static compiled_inspect_store_fault_t *fault;
  static job_verifier_probe_t verification;
  static turbo_agent_job_verifier_t verifier;
  static turbo_agent_compile_diagnostic_t diagnostic;
  static turbo_tool_status_t status;
  static json_value_t *result, *certificate, *reference, *record, *snapshot;

  before_each() {
    cmeta_threadpool_config_t options = {.num_threads = 1, .queue_capacity = 1};
    turbo_agent_runtime_store_t callbacks = compiled_inspect_store_fail_phase(NULL);
    pool = cmeta_threadpool_create_with_config(&options);
    registry = turbo_tool_registry_create();
    probe = (compiled_inspect_probe_t){0};
    verification = (job_verifier_probe_t){0};
    fault = callbacks.user_data;
    check_not_null(pool); check_not_null(registry); check_not_null(fault);
    check_equal(register_compiled_inspect(registry, &probe, TURBO_TOOL_EFFECT_READ, 1), TURBO_TOOL_OK);
    turbo_agent_job_verifier_init(&verifier);
    verifier.source = verify_job_source; verifier.evidence = verify_job_evidence; verifier.user_data = &verification;
    harness = make_compiled_harness_configured(pool, registry, callbacks, compiled_harness_transport, &verifier);
    check_not_null(harness);
    attach_compiled_probe_runtime(harness, &probe);
    check_equal(set_compiled_host_policy(harness, 1), 0);
    check_equal(turbo_agent_job_create(probe.runtime, "job-bound", "inspect", "Host verifies", &job), SALTS_OK);
    check_equal(turbo_agent_job_propose(job, 0, compiled_inspect_source,
        strlen(compiled_inspect_source), "source-v1"), SALTS_OK);
    check_equal(turbo_agent_job_bind_compiled_inspect(job, 1, COMPILED_INSPECT_TEST_THREAD_ID,
        "bound-attempt"), SALTS_OK);
  }
  after_each() {
    json_free(result); json_free(certificate); json_free(reference); json_free(record); json_free(snapshot);
    result = certificate = reference = record = snapshot = NULL;
    turbo_agent_job_destroy(job); job = NULL;
    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }

  it("copies mandatory callbacks and rechecks evidence throughout the delivery composition") {
    memset(&verifier, 0, sizeof(verifier)); /* Original config cannot disable the copied verifier. */
    check_equal(accept_job_for_delivery(harness, TURBO_AGENT_JOB_ACCEPTED, &diagnostic), SALTS_OK);
    check_equal(verification.sources, 4);
    check_equal(verification.evidences, 1);
    check_equal(verification.last_decision, (int)TURBO_AGENT_JOB_ACCEPTED);
    check_equal(verification.saw_typed_result, 1);
    check_equal(read_acceptance(harness, &diagnostic, &record), SALTS_OK);
    json_free(record); record = NULL;
    check_equal(delivery_operation(harness, 0, "delivery-1", &diagnostic, &record), SALTS_OK);
    json_free(record); record = NULL;
    check_equal(delivery_operation(harness, 1, NULL, &diagnostic, &record), SALTS_OK);
    check_equal(json_get_string(record, "status"), "outcome_unknown");
    json_free(record); record = NULL;
    check_equal(delivery_operation(harness, 2, "delivery-1", &diagnostic, &record), SALTS_OK);
    json_free(record); record = NULL;
    check_equal(delivery_operation(harness, 1, NULL, &diagnostic, &record), SALTS_OK);
    check_equal(json_get_string(record, "status"), "delivered");
    check_equal(verification.evidences, 6);
    check_equal(verification.sources, 14);
    check_equal(probe.calls, 1);
  }

  it("preserves stale, missing, unavailable and timed-out source errors before Run creation") {
    const int errors[] = {SALTS_EPROTO, SALTS_ENOENT, SALTS_EIO, SALTS_ETIMEDOUT};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
      verification.source_status = errors[i];
      check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
          &result, &certificate, &reference), errors[i]);
      check_null(result); check_null(certificate); check_null(reference);
    }
    check_equal(turbo_agent_runtime_store_list_json(probe.runtime, "runs", NULL, NULL, &record), SALTS_OK);
    check_equal(json_array_size(record), (size_t)0);
    check_equal(probe.calls, 0); check_equal(verification.evidences, 0);
  }

  it("withholds a committed result when the final source check fails and replays only the attempt") {
    verification.source_fail_at = 2;
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
        &result, &certificate, &reference), SALTS_EIO);
    check_null(result); check_null(reference); check_not_null(certificate);
    json_free(certificate); certificate = NULL;
    check_equal(probe.calls, 1);
    verification.source_fail_at = 0;
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
        &result, &certificate, &reference), SALTS_OK);
    check_equal(probe.calls, 1);
    check_equal(turbo_agent_runtime_store_list_json(probe.runtime, "runs", NULL, NULL, &record), SALTS_OK);
    check_equal(json_array_size(record), (size_t)2);
  }

  it("never caches external evidence success or falls back after verification errors") {
    const int errors[] = {SALTS_ENOENT, SALTS_EPROTO, SALTS_EIO, SALTS_ETIMEDOUT};
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
        &result, &certificate, &reference), SALTS_OK);
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
      verification.evidence_status = errors[i];
      check_equal(record_acceptance(harness, reference, TURBO_AGENT_JOB_ACCEPTED,
          &diagnostic, &record), errors[i]);
      check_null(record);
    }
    check_equal(fault->acceptance_claims, 0);
    verification.evidence_status = 0;
    check_equal(record_acceptance(harness, reference, TURBO_AGENT_JOB_ACCEPTED, &diagnostic, &record), SALTS_OK);
    json_free(record); record = NULL;
    verification.evidence_status = SALTS_EIO;
    check_equal(read_acceptance(harness, &diagnostic, &record), SALTS_EIO);
    for (int op = 0; op < 3; ++op) {
      check_equal(delivery_operation(harness, op, "delivery-1", &diagnostic, &record), SALTS_EIO);
      check_null(record);
    }
    check_equal(fault->delivery_claims, 0); check_equal(probe.calls, 1);
  }

  it("does not call external evidence verification before canonical validation") {
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
        &result, &certificate, &reference), SALTS_OK);
    check_equal(turbo_agent_runtime_store_get_json_status(probe.runtime, "compiled_inspect_attempts",
        json_get_string(reference, "attempt_record_id"), &snapshot), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_string(snapshot, "phase", "started"), 0);
    check_equal(turbo_agent_runtime_store_put_json(probe.runtime, "compiled_inspect_attempts",
        json_get_string(reference, "attempt_record_id"), snapshot), SALTS_OK);
    check_equal(record_acceptance(harness, reference, TURBO_AGENT_JOB_ACCEPTED,
        &diagnostic, &record), SALTS_EBUSY);
    check_equal(verification.evidences, 0);
    check_equal(fault->acceptance_claims, 0); check_equal(probe.calls, 1);
  }

  it("verifies explicit quality rejection without promoting it to acceptance") {
    check_equal(accept_job_for_delivery(harness, TURBO_AGENT_JOB_REJECTED, &diagnostic), SALTS_OK);
    check_equal(verification.last_decision, (int)TURBO_AGENT_JOB_REJECTED);
    check_equal(read_acceptance(harness, &diagnostic, &record), SALTS_OK);
    check_equal(json_get_string(record, "decision"), "rejected");
    json_free(record); record = NULL;
    check_equal(delivery_operation(harness, 0, "delivery-1", &diagnostic, &record), SALTS_EPERM);
    check_equal(fault->delivery_claims, 0); check_equal(probe.calls, 1);
  }

  it("holds submission exclusion through source and evidence callbacks") {
    verification.nested = harness;
    check_equal(accept_job_for_delivery(harness, TURBO_AGENT_JOB_ACCEPTED, &diagnostic), SALTS_OK);
    check_equal(verification.source_nested_status, SALTS_EBUSY);
    check_equal(verification.evidence_nested_status, SALTS_EBUSY);
    check_equal(probe.calls, 1);
  }

  it("rechecks source after evidence verification before publishing a decision") {
    check_equal(run_job_inspect(harness, "job-bound", 2, &diagnostic, &status,
        &result, &certificate, &reference), SALTS_OK);
    verification.invalidate_during_evidence = 1;
    check_equal(record_acceptance(harness, reference, TURBO_AGENT_JOB_ACCEPTED,
        &diagnostic, &record), SALTS_EPROTO);
    check_null(record); check_equal(fault->acceptance_claims, 0);
    verification.invalidate_during_evidence = 0; verification.source_status = 0;
    check_equal(record_acceptance(harness, reference, TURBO_AGENT_JOB_ACCEPTED,
        &diagnostic, &record), SALTS_OK);
    check_equal(probe.calls, 1);
  }
}
