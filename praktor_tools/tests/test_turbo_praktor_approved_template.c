#include "tinytest.h"
#include "turbo_praktor_approved_template.h"
#include "turbo_runtime_json.h"
#include "turbo_runtime_store_sqlite.h"
#include <salts/thread.h>
#include <salts/clock.h>
#include <stdatomic.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct approved_probe_s {
  int inspect;
  int change;
  int verify;
  int block_change;
  atomic_int change_entered;
  atomic_int release_change;
} approved_probe_t;

static turbo_tool_status_t approved_inspect(
    const json_value_t *args, const turbo_tool_execution_context_t *context,
    json_value_t **out, void *user) {
  approved_probe_t *p = (approved_probe_t *)user;
  (void)args; (void)context;
  ++p->inspect;
  *out = json_create_object();
  return *out ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static turbo_tool_status_t approved_change(
    const json_value_t *args, const turbo_tool_execution_context_t *context,
    json_value_t **out, void *user) {
  approved_probe_t *p = (approved_probe_t *)user;
  (void)args; (void)context;
  ++p->change;
  if (p->block_change) {
    atomic_store_explicit(&p->change_entered, 1, memory_order_release);
    while (!atomic_load_explicit(&p->release_change, memory_order_acquire))
      cmeta_thread_yield();
  }
  *out = json_create_object();
  return *out ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static turbo_tool_status_t approved_verify(
    const json_value_t *args, const turbo_tool_execution_context_t *context,
    json_value_t **out, void *user) {
  approved_probe_t *p = (approved_probe_t *)user;
  (void)args; (void)context;
  ++p->verify;
  *out = json_create_object();
  if (!*out) return TURBO_TOOL_OUT_OF_MEMORY;
  if (turbo_runtime_json_object_set(*out, "verified", json_create_bool(1)) !=
      TURBO_RUNTIME_JSON_OK) return TURBO_TOOL_OUT_OF_MEMORY;
  return TURBO_TOOL_OK;
}

static int approved_add_tool(
    turbo_tool_registry_t *registry, const char *name,
    turbo_tool_idempotency_t idempotency, turbo_tool_effect_flags_t effects,
    turbo_tool_json_value_context_handler_fn callback, approved_probe_t *probe) {
  static const char *const caps[] = {"runtime_tools"};
  turbo_tool_definition_v6_t definition = {0};
  definition.struct_size = sizeof(definition);
  definition.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
  definition.definition.name = name;
  definition.definition.description = name;
  definition.definition.parameters_json =
      "{\"type\":\"object\",\"additionalProperties\":true}";
  definition.definition.strict = 1;
  definition.definition.user_data = probe;
  definition.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
  definition.execution_policy.idempotency = idempotency;
  definition.required_capabilities = caps;
  definition.required_capability_count = 1;
  definition.json_value_context_handler = callback;
  definition.effect_flags = effects;
  return turbo_tool_registry_add_v6(registry, &definition) == TURBO_TOOL_OK;
}

static turbo_tool_registry_t *approved_registry(
    approved_probe_t *probe, turbo_tool_effect_flags_t change_effect) {
  turbo_tool_registry_t *r = turbo_tool_registry_create();
  if (!r) return NULL;
  if (!approved_add_tool(r, "repo.inspect", TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
                         TURBO_TOOL_EFFECT_READ, approved_inspect, probe) ||
      !approved_add_tool(r, "repo.change", TURBO_TOOL_IDEMPOTENCY_NONE,
                         change_effect, approved_change, probe) ||
      !approved_add_tool(r, "repo.verify", TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
                         TURBO_TOOL_EFFECT_READ, approved_verify, probe)) {
    turbo_tool_registry_destroy(r);
    return NULL;
  }
  return r;
}

static turbo_agent_template_plan_t *approved_template(
    turbo_tool_registry_t *registry, json_value_t *arguments, int retry) {
  static const char *const caps[] = {"runtime_tools"};
  turbo_agent_compiler_config_t compiler;
  turbo_agent_change_source_t src;
  turbo_agent_template_plan_t *plan = NULL;
  turbo_agent_compiler_config_init(&compiler);
  compiler.allowed_capabilities = caps;
  compiler.allowed_capability_count = 1;
  turbo_agent_change_source_init(&src);
  src.inspect.tool_name = "repo.inspect";
  src.inspect.arguments = arguments;
  src.change.tool_name = "repo.change";
  src.change.arguments = arguments;
  src.verify.tool_name = "repo.verify";
  src.verify.arguments = arguments;
  src.verify.retry_limit = (uint32_t)retry;
  if (turbo_agent_compile_change_template(
          &compiler, registry, &src, &plan, NULL) != TURBO_AGENT_COMPILE_OK)
    return NULL;
  return plan;
}

typedef struct approved_fault_store_s {
  turbo_agent_runtime_store_t inner;
  int fail_committed;
  int failures;
} approved_fault_store_t;

static int approved_fault_put(void *user, const char *collection,
                              const char *id, const char *record_json) {
  approved_fault_store_t *fault = (approved_fault_store_t *)user;
  if (fault->fail_committed && strcmp(collection, "praktor_template_attempts") == 0) {
    json_value_t *parsed = json_parse(record_json, strlen(record_json));
    const char *phase = parsed ? json_get_string(parsed, "phase") : NULL;
    int reject = phase && strcmp(phase, "committed") == 0;
    json_free(parsed);
    if (reject) {
      ++fault->failures;
      return SALTS_EIO;
    }
  }
  return fault->inner.put(fault->inner.user_data, collection, id, record_json);
}

static int approved_fault_get(void *user, const char *collection,
                              const char *id, char **output) {
  approved_fault_store_t *fault = (approved_fault_store_t *)user;
  return fault->inner.get(fault->inner.user_data, collection, id, output);
}

static int approved_fault_list(void *user, const char *collection,
                               const char *filter_key, const char *filter_value,
                               char **output) {
  approved_fault_store_t *fault = (approved_fault_store_t *)user;
  return fault->inner.list(fault->inner.user_data, collection,
                           filter_key, filter_value, output);
}

static void approved_fault_destroy(void *user) {
  approved_fault_store_t *fault = (approved_fault_store_t *)user;
  if (fault->inner.user_data_free)
    fault->inner.user_data_free(fault->inner.user_data);
  fault->inner = (turbo_agent_runtime_store_t){0};
}

typedef struct approved_concurrent_worker_s {
  turbo_agent_runtime_t *runtime;
  const turbo_agent_template_plan_t *plan;
  turbo_tool_status_t status;
  int replayed;
  turbo_agent_template_outcome_t outcome;
  json_value_t *result;
} approved_concurrent_worker_t;

static void approved_concurrent_execute(void *arg) {
  approved_concurrent_worker_t *worker = (approved_concurrent_worker_t *)arg;
  worker->status = turbo_praktor_template_execute_approved(
      worker->runtime, worker->plan, "thread_A", "concurrent",
      "host_approval", NULL, &worker->replayed, &worker->outcome,
      &worker->result);
}

spec("approved Praktor ABI 2.6 execution / durable attempt") {
  it("rejects absent host approval and replays a committed three-step workflow") {
    approved_probe_t probe = {0};
    turbo_tool_registry_t *registry = approved_registry(&probe, TURBO_TOOL_EFFECT_WRITE);
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    json_value_t *args = json_create_object();
    turbo_agent_template_plan_t *plan = approved_template(registry, args, 0);
    turbo_agent_template_outcome_t outcome;
    json_value_t *result = NULL;
    int replayed = -1;

    check_not_null(registry); check_not_null(runtime); check_not_null(plan);
    check_true(turbo_agent_template_praktor_execution_preflight(plan));
    check_equal(turbo_praktor_template_execute_approved(
        runtime, plan, "thread_A", "attempt_A", "host_approval_A",
        NULL, &replayed, &outcome, &result), TURBO_TOOL_INVALID_ARGUMENT);
    check_equal(probe.inspect + probe.change + probe.verify, 0);
    check_equal(turbo_praktor_template_approval_commit(
        runtime, plan, "thread_A", "attempt_A", "host_approval_A"), TURBO_TOOL_OK);

    check_equal(turbo_praktor_template_execute_approved(
        runtime, plan, "thread_A", "attempt_A", "host_approval_A",
        NULL, &replayed, &outcome, &result), TURBO_TOOL_OK);
    check_equal(outcome, TURBO_AGENT_TEMPLATE_OUTCOME_COMPLETED);
    check_equal(replayed, 0);
    check_not_null(result);
    check_equal(probe.inspect, 1);
    check_equal(probe.change, 1);
    check_equal(probe.verify, 1);
    json_free(result); result = NULL;
    check_equal(turbo_praktor_template_execute_approved(
        runtime, plan, "thread_A", "attempt_A", "host_approval_A",
        NULL, &replayed, &outcome, &result), TURBO_TOOL_OK);
    check_equal(replayed, 1);
    check_equal(probe.inspect + probe.change + probe.verify, 3);
    json_free(result);
    turbo_agent_template_plan_destroy(plan);
    json_free(args);
    turbo_agent_runtime_destroy(runtime);
    turbo_tool_registry_destroy(registry);
  }

  it("prevents execution after STARTED recovery or mismatched approved identity") {
    approved_probe_t probe = {0};
    turbo_tool_registry_t *registry = approved_registry(&probe, TURBO_TOOL_EFFECT_WRITE);
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    json_value_t *args = json_create_object(), *record = NULL;
    turbo_agent_template_plan_t *plan = approved_template(registry, args, 0);
    turbo_agent_template_outcome_t outcome;
    json_value_t *result = NULL;
    int replayed = -1;

    check_not_null(plan);
    check_equal(turbo_praktor_template_approval_commit(
        runtime, plan, "thread_A", "attempt_B", "approved_B"), TURBO_TOOL_OK);
    check_equal(turbo_praktor_template_execute_approved(
        runtime, plan, "thread_A", "attempt_B", "WRONG_APPROVAL",
        NULL, &replayed, &outcome, &result), TURBO_TOOL_INVALID_ARGUMENT);
    check_equal(probe.inspect + probe.change + probe.verify, 0);

    check_equal(turbo_praktor_template_execute_approved(
        runtime, plan, "thread_A", "attempt_B", "approved_B",
        NULL, &replayed, &outcome, &result), TURBO_TOOL_OK);
    json_free(result); result = NULL;
    check_equal(turbo_agent_runtime_store_get_json_status(
        runtime, "praktor_template_attempts", "008_thread_A_attempt_B", &record),
        SALTS_OK);
    check_equal(turbo_runtime_json_object_set_string(
        record, "phase", "started"), TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_agent_runtime_store_put_json(
        runtime, "praktor_template_attempts", "008_thread_A_attempt_B", record),
        SALTS_OK);
    json_free(record);
    check_equal(turbo_praktor_template_execute_approved(
        runtime, plan, "thread_A", "attempt_B", "approved_B",
        NULL, &replayed, &outcome, &result), TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
    check_equal(replayed, 0);
    check_equal(probe.inspect + probe.change + probe.verify, 3);
    turbo_agent_template_plan_destroy(plan);
    json_free(args);
    turbo_agent_runtime_destroy(runtime);
    turbo_tool_registry_destroy(registry);
  }

  it("rejects unknown effects and backend retry before approval is recorded") {
    approved_probe_t probe = {0};
    turbo_tool_registry_t *registry = approved_registry(
        &probe, TURBO_TOOL_EFFECT_UNKNOWN);
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    json_value_t *args = json_create_object();
    turbo_agent_template_plan_t *plan = approved_template(registry, args, 0);
    json_value_t *record = NULL;
    check_not_null(plan);
    check_false(turbo_agent_template_praktor_execution_preflight(plan));
    check_equal(turbo_praktor_template_approval_commit(
        runtime, plan, "thread_A", "reject", "approved"), TURBO_TOOL_INVALID_ARGUMENT);
    check_equal(turbo_agent_runtime_store_get_json_status(
        runtime, "praktor_template_approvals", "008_thread_A_reject", &record), SALTS_ENOENT);
    turbo_agent_template_plan_destroy(plan);
    turbo_agent_runtime_destroy(runtime);
    turbo_tool_registry_destroy(registry);

    registry = approved_registry(&probe, TURBO_TOOL_EFFECT_WRITE);
    store = turbo_agent_runtime_store_memory_create();
    runtime = turbo_agent_runtime_create(&store);
    plan = approved_template(registry, args, 1);
    check_not_null(plan);
    check_false(turbo_agent_template_praktor_execution_preflight(plan));
    check_equal(turbo_praktor_template_approval_commit(
        runtime, plan, "thread_A", "retry", "approved"), TURBO_TOOL_INVALID_ARGUMENT);
    check_equal(probe.inspect + probe.change + probe.verify, 0);
    turbo_agent_template_plan_destroy(plan);
    json_free(args);
    turbo_agent_runtime_destroy(runtime);
    turbo_tool_registry_destroy(registry);
  }

  it("replays a committed outcome across an exclusive SQLite store reopen") {
    approved_probe_t probe = {0};
    turbo_tool_registry_t *registry = approved_registry(&probe, TURBO_TOOL_EFFECT_WRITE);
    json_value_t *args = json_create_object();
    turbo_agent_template_plan_t *plan = approved_template(registry, args, 0);
    turbo_runtime_sqlite_config_t cfg;
    turbo_agent_runtime_store_t store = {0};
    turbo_agent_runtime_t *runtime;
    turbo_agent_template_outcome_t outcome;
    char *directory = tt_make_temp_dir("praktor-approved");
    char db_path[1024];
    json_value_t *result = NULL;
    int replayed = -1;

    check_not_null(registry); check_not_null(plan); check_not_null(directory);
    check_true(snprintf(db_path, sizeof(db_path), "%s/approval.db", directory) <
               (int)sizeof(db_path));
    turbo_runtime_sqlite_config_init(&cfg);
    cfg.path = db_path;
    check_equal(turbo_runtime_store_sqlite_open(&cfg, &store), SALTS_OK);
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    check_equal(turbo_praktor_template_approval_commit(
        runtime, plan, "thread_A", "sqlite", "host_reviewed"), TURBO_TOOL_OK);
    check_equal(turbo_praktor_template_execute_approved(
        runtime, plan, "thread_A", "sqlite", "host_reviewed",
        NULL, &replayed, &outcome, &result), TURBO_TOOL_OK);
    json_free(result); result = NULL;
    check_equal(probe.change, 1);
    turbo_agent_runtime_destroy(runtime);

    cfg.create_if_missing = 0;
    check_equal(turbo_runtime_store_sqlite_open(&cfg, &store), SALTS_OK);
    runtime = turbo_agent_runtime_create(&store);
    check_equal(turbo_praktor_template_execute_approved(
        runtime, plan, "thread_A", "sqlite", "host_reviewed",
        NULL, &replayed, &outcome, &result), TURBO_TOOL_OK);
    check_equal(replayed, 1);
    check_equal(probe.change, 1);
    json_free(result);
    turbo_agent_runtime_destroy(runtime);
    turbo_agent_template_plan_destroy(plan);
    json_free(args);
    turbo_tool_registry_destroy(registry);
    tt_remove_tree(directory);
    free(directory);
  }
  it("never retries a Praktor HostTool after a failed durable COMMITTED write") {
    approved_probe_t probe = {0};
    approved_fault_store_t fault = {0};
    turbo_agent_runtime_store_t callbacks;
    turbo_tool_registry_t *registry = approved_registry(&probe, TURBO_TOOL_EFFECT_WRITE);
    json_value_t *args = json_create_object(), *record = NULL, *result = NULL;
    turbo_agent_template_plan_t *plan = approved_template(registry, args, 0);
    turbo_agent_runtime_t *runtime;
    turbo_agent_template_outcome_t outcome;
    int replayed = -1;

    check_not_null(registry);
    check_not_null(plan);
    fault.inner = turbo_agent_runtime_store_memory_create();
    check_not_null(fault.inner.put);
    callbacks = (turbo_agent_runtime_store_t){
        approved_fault_put, approved_fault_get, approved_fault_list,
        &fault, approved_fault_destroy};
    runtime = turbo_agent_runtime_create(&callbacks);
    check_not_null(runtime);
    check_equal(turbo_praktor_template_approval_commit(
        runtime, plan, "thread_A", "commit_fault", "host_reviewed"), TURBO_TOOL_OK);
    fault.fail_committed = 1;
    check_equal(turbo_praktor_template_execute_approved(
        runtime, plan, "thread_A", "commit_fault", "host_reviewed",
        NULL, &replayed, &outcome, &result), TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
    check_equal(probe.inspect + probe.change + probe.verify, 3);
    check_equal(replayed, 0);
    check_null(result);
    check_equal(fault.failures, 1);
    check_equal(turbo_agent_runtime_store_get_json_status(
        runtime, "praktor_template_attempts", "008_thread_A_commit_fault", &record),
        SALTS_OK);
    check_equal(json_get_string(record, "phase"), "started");
    json_free(record); record = NULL;
    fault.fail_committed = 0;
    check_equal(turbo_praktor_template_execute_approved(
        runtime, plan, "thread_A", "commit_fault", "host_reviewed",
        NULL, &replayed, &outcome, &result), TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
    check_equal(probe.inspect + probe.change + probe.verify, 3);

    turbo_agent_runtime_destroy(runtime);
    turbo_agent_template_plan_destroy(plan);
    json_free(args);
    turbo_tool_registry_destroy(registry);
  }

  it("rejects same-attempt execution while a released Praktor HostTool is active") {
    approved_probe_t probe = {0};
    turbo_tool_registry_t *registry = approved_registry(&probe, TURBO_TOOL_EFFECT_WRITE);
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    json_value_t *args = json_create_object(), *result = NULL;
    turbo_agent_template_plan_t *plan = approved_template(registry, args, 0);
    approved_concurrent_worker_t worker = {0};
    cmeta_thread_t thread = {0};
    turbo_agent_template_outcome_t outcome;
    int replayed = -1;
    int entered;
    check_not_null(registry);
    check_not_null(runtime);
    check_not_null(plan);
    probe.block_change = 1;
    atomic_init(&probe.change_entered, 0);
    atomic_init(&probe.release_change, 0);

    check_equal(turbo_praktor_template_approval_commit(
        runtime, plan, "thread_A", "concurrent", "host_approval"), TURBO_TOOL_OK);
    worker.runtime = runtime;
    worker.plan = plan;
    check_equal(cmeta_thread_create(&thread, approved_concurrent_execute, &worker),
                SALTS_OK);
    {
      const uint64_t deadline = cmeta_monotonic_ms() + UINT64_C(10000);
      while (!atomic_load_explicit(&probe.change_entered, memory_order_acquire) &&
             cmeta_monotonic_ms() < deadline)
        cmeta_thread_yield();
    }
    entered = atomic_load_explicit(&probe.change_entered, memory_order_acquire);
    if (entered) {
      check_equal(turbo_praktor_template_execute_approved(
          runtime, plan, "thread_A", "concurrent", "host_approval",
          NULL, &replayed, &outcome, &result), TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
      check_equal(replayed, 0);
      check_null(result);
    }
    atomic_store_explicit(&probe.release_change, 1, memory_order_release);
    check_equal(cmeta_thread_join(&thread), SALTS_OK);
    check_true(entered);
    check_equal(worker.status, TURBO_TOOL_OK);
    check_equal(worker.outcome, TURBO_AGENT_TEMPLATE_OUTCOME_COMPLETED);
    check_equal(probe.inspect, 1);
    check_equal(probe.change, 1);
    check_equal(probe.verify, 1);
    json_free(worker.result);
    worker.result = NULL;
    check_equal(turbo_praktor_template_execute_approved(
        runtime, plan, "thread_A", "concurrent", "host_approval",
        NULL, &replayed, &outcome, &result), TURBO_TOOL_OK);
    check_equal(replayed, 1);
    check_equal(probe.inspect + probe.change + probe.verify, 3);
    json_free(result);
    turbo_agent_template_plan_destroy(plan);
    turbo_agent_runtime_destroy(runtime);
    json_free(args);
    turbo_tool_registry_destroy(registry);
  }

}
