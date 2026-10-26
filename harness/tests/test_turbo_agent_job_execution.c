#include "tinytest.h"
#include "turbo_agent_job_execution.h"
#include "turbo_agent_job_cancel.h"
#include "turbo_agent_job_verifier.h"
#include "turbo_agent_policy.h"
#include <salts/clock.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static const char source[] = "{\"template_id\":\"inspect\",\"step_id\":\"read\",\"tool\":\"inspect\",\"arguments_json\":\"{}\"}";
typedef struct gate_s { atomic_int entered, open; } gate_t;
typedef struct probe_s {
  turbo_agent_runtime_store_t inner;
  turbo_agent_runtime_t *runtime;
  turbo_agent_harness_t *harness;
  turbo_agent_job_execution_t *nested;
  gate_t callback, publication;
  atomic_int calls, reads, claims, cancellation_claims, frees, sources, evidences;
  atomic_int reads_after_error, unacknowledged;
  int publication_error, after_commit, source_error, source_fail_at, nested_status;
  const char *read_failure;
} probe_t;
static int await(atomic_int *value) {
  uint64_t limit = cmeta_monotonic_ms() + 5000;
  while (!atomic_load_explicit(value, memory_order_acquire)) {
    if (cmeta_monotonic_ms() >= limit) return 0;
    cmeta_thread_yield();
  }
  return 1;
}
static void block(void *user) {
  gate_t *g = user;
  atomic_store_explicit(&g->entered, 1, memory_order_release);
  (void)await(&g->open);
}
static int get(void *user, const char *collection, const char *id, char **out) {
  probe_t *p = user;
  atomic_fetch_add(&p->reads, 1);
  if (atomic_load(&p->unacknowledged)) atomic_fetch_add(&p->reads_after_error, 1);
  if (p->read_failure && !strcmp(collection, p->read_failure)) { *out = NULL; return SALTS_EIO; }
  return p->inner.get(p->inner.user_data, collection, id, out);
}
static int put(void *user, const char *collection, const char *id, const char *value) {
  probe_t *p = user;
  return p->inner.put(p->inner.user_data, collection, id, value);
}
static int list(void *user, const char *collection, const char *key, const char *value, char **out) {
  probe_t *p = user;
  return p->inner.list(p->inner.user_data, collection, key, value, out);
}
static int claim(void *user, const char *collection, const char *id, const char *value) {
  probe_t *p = user;
  int rc;
  atomic_fetch_add(&p->claims, 1);
  if (!strcmp(collection, "agent_job_cancellations")) {
    atomic_fetch_add(&p->cancellation_claims, 1);
    block(&p->publication);
    if (p->publication_error && !p->after_commit) {
      atomic_store(&p->unacknowledged, 1); return p->publication_error;
    }
    rc = p->inner.create_if_absent(p->inner.user_data, collection, id, value);
    if (!rc && p->publication_error) {
      atomic_store(&p->unacknowledged, 1); return p->publication_error;
    }
    return rc;
  }
  return p->inner.create_if_absent(p->inner.user_data, collection, id, value);
}
static void store_free(void *user) {
  probe_t *p = user;
  p->inner.user_data_free(p->inner.user_data);
  atomic_fetch_add(&p->frees, 1);
}
static int transport(const char *request, char **response, void *user) {
  (void)request; (void)response; (void)user; return -1;
}
static int inspect(const json_value_t *args, json_value_t **out, void *user) {
  probe_t *p = user;
  (void)args;
  atomic_fetch_add(&p->calls, 1);
  block(&p->callback);
  *out = json_create_object();
  return *out && !turbo_runtime_json_object_set_string(*out, "answer", "canonical") ? 0 : -1;
}
static int verify_source(const json_value_t *snapshot, void *user) {
  probe_t *p = user;
  (void)snapshot;
  int call = atomic_fetch_add(&p->sources, 1) + 1;
  if (p->source_fail_at && call == p->source_fail_at) return SALTS_EPROTO;
  if (p->nested) {
    turbo_agent_job_execution_status_t status;
    turbo_agent_job_execution_status_init(&status);
    p->nested_status = turbo_agent_job_execution_get_status(p->nested, &status);
  }
  return p->source_error;
}
static int verify_evidence(const json_value_t *snapshot, const json_value_t *reference,
    const json_value_t *result, turbo_agent_job_acceptance_decision_t decision,
    const char *ref, void *user) {
  probe_t *p = user;
  (void)snapshot; (void)reference; (void)result; (void)decision; (void)ref;
  atomic_fetch_add(&p->evidences, 1); return SALTS_OK;
}
static int policy(turbo_agent_harness_t *h, int allow) {
  turbo_agent_policy_t p = turbo_agent_policy_default();
  p.allow_runtime_tools = allow;
  return turbo_agent_set_tool_policy(turbo_agent_session_agent(turbo_agent_app_session(
      turbo_agent_harness_app(h))), &p);
}
static int submit(turbo_agent_harness_t *h, turbo_agent_job_execution_t **out, uint64_t deadline) {
  const char *caps[] = {"runtime_tools"};
  turbo_agent_compiler_config_t config;
  turbo_agent_compiler_config_init(&config);
  config.allowed_capabilities = caps; config.allowed_capability_count = 1;
  return turbo_agent_harness_submit_job_inspect(h, "job", 2, &config, deadline, out);
}
typedef struct observer_s { turbo_agent_job_execution_t *execution; atomic_int ready, done, errors; } observer_t;
typedef struct contender_s {
  turbo_agent_job_execution_t *execution;
  const char *request;
  atomic_int ready;
  atomic_int *go;
  int rc;
} contender_t;
static void cancel_contender(void *user) {
  contender_t *c = user;
  atomic_store(&c->ready, 1);
  if (await(c->go)) c->rc = turbo_agent_job_execution_cancel(c->execution, c->request, "reason");
}
static void observe(void *user) {
  observer_t *o = user;
  turbo_agent_job_execution_status_t status;
  turbo_agent_job_execution_status_init(&status);
  atomic_store(&o->ready, 1);
  do {
    if (turbo_agent_job_execution_get_status(o->execution, &status) ||
        (status.state < TURBO_AGENT_EXECUTION_COMPLETED && status.operation_status != SALTS_EBUSY))
      atomic_fetch_add(&o->errors, 1);
    cmeta_thread_yield();
  } while (!atomic_load(&o->done));
}

spec("Bound Job asynchronous supervision") {
  static probe_t p;
  static cmeta_threadpool_t *pool;
  static turbo_tool_registry_t *registry;
  static turbo_agent_harness_t *harness;
  static turbo_agent_job_t *job;
  static turbo_agent_job_execution_t *execution, *other;
  static json_value_t *result, *certificate, *reference, *record;
  static turbo_agent_compile_diagnostic_t diagnostic;
  static turbo_agent_job_execution_status_t status;

  before_each() {
    cmeta_threadpool_config_t pool_config = {1, 1};
    turbo_agent_session_config_t session = {0};
    turbo_agent_app_config_t app = {0};
    turbo_agent_harness_config_t config;
    turbo_agent_job_verifier_t verifier;
    turbo_tool_definition_v6_t tool = {0};
    const char *caps[] = {"runtime_tools"};
    memset(&p, 0, sizeof(p));
    atomic_init(&p.callback.entered, 0); atomic_init(&p.callback.open, 1);
    atomic_init(&p.publication.entered, 0); atomic_init(&p.publication.open, 1);
    atomic_init(&p.calls, 0); atomic_init(&p.reads, 0); atomic_init(&p.claims, 0);
    atomic_init(&p.cancellation_claims, 0); atomic_init(&p.frees, 0);
    atomic_init(&p.sources, 0); atomic_init(&p.evidences, 0);
    atomic_init(&p.reads_after_error, 0); atomic_init(&p.unacknowledged, 0);
    pool = cmeta_threadpool_create_with_config(&pool_config);
    registry = turbo_tool_registry_create();
    p.inner = turbo_agent_runtime_store_memory_create();
    tool.struct_size = sizeof(tool); tool.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
    tool.definition.name = "inspect"; tool.definition.description = "inspect";
    tool.definition.parameters_json = "{\"type\":\"object\",\"additionalProperties\":false}";
    tool.definition.json_value_handler = inspect; tool.definition.user_data = &p;
    tool.execution_policy = (turbo_tool_execution_policy_t){TURBO_TOOL_EXECUTION_SEQUENTIAL, TURBO_TOOL_IDEMPOTENCY_READ_ONLY};
    tool.required_capabilities = caps; tool.required_capability_count = 1;
    tool.result_schema_json = "{\"type\":\"object\",\"properties\":{\"answer\":{\"type\":\"string\"}},\"required\":[\"answer\"],\"additionalProperties\":false}";
    tool.strict_result = 1; tool.effect_flags = TURBO_TOOL_EFFECT_READ;
    check_equal(turbo_tool_registry_add_v6(registry, &tool), TURBO_TOOL_OK);
    session.thread_id = "async-job-thread";
    session.runtime_store = (turbo_agent_runtime_store_t){.get = get, .put = put, .list = list,
        .create_if_absent = claim, .user_data = &p, .user_data_free = store_free};
    session.agent_config.model = "offline"; session.agent_config.transport_fn = transport;
    session.agent_config.tool_registry = registry;
    app.session_config = &session;
    turbo_agent_harness_config_init(&config); config.app_config = &app; config.executor = pool;
    turbo_agent_job_verifier_init(&verifier);
    verifier.source = verify_source; verifier.evidence = verify_evidence; verifier.user_data = &p;
    check_equal(turbo_agent_harness_create_verified_with_store(&config, &verifier, &harness), SALTS_OK);
    p.harness = harness;
    p.runtime = turbo_agent_session_runtime(turbo_agent_app_session(turbo_agent_harness_app(harness)));
    check_equal(policy(harness, 1), SALTS_OK);
    check_equal(turbo_agent_job_create(p.runtime, "job", "goal", "criteria", &job), SALTS_OK);
    check_equal(turbo_agent_job_propose(job, 0, source, strlen(source), "source"), SALTS_OK);
    check_equal(turbo_agent_job_bind_compiled_inspect(job, 1, "async-job-thread", "attempt"), SALTS_OK);
    turbo_agent_job_execution_status_init(&status);
  }
  after_each() {
    atomic_store(&p.callback.open, 1); atomic_store(&p.publication.open, 1);
    cmeta_threadpool_wait(pool);
    turbo_agent_job_destroy(job); job = NULL;
    turbo_agent_job_execution_release(execution); turbo_agent_job_execution_release(other);
    execution = other = NULL;
    turbo_agent_harness_release(harness); harness = NULL;
    turbo_tool_registry_destroy(registry); cmeta_threadpool_destroy(pool);
    json_free(result); json_free(certificate); json_free(reference); json_free(record);
    result = certificate = reference = record = NULL;
  }

  it("keeps exclusive ownership until terminal result retirement and reads the canonical result once") {
    check_equal(submit(harness, &execution, 0), SALTS_OK);
    check_not_null(turbo_agent_job_execution_id(execution));
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_get_status(execution, &status), SALTS_OK);
    check_equal(status.state, TURBO_AGENT_EXECUTION_COMPLETED);
    check_equal(status.operation_status, SALTS_OK); check_equal(status.cancellation_status, SALTS_ENOENT);
    check_equal(submit(harness, &other, 0), SALTS_EBUSY); check_null(other);
    check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), SALTS_OK);
    check_equal(json_get_string(result, "answer"), "canonical");
    check_equal(json_get_string(reference, "job_id"), "job");
    check_not_equal(turbo_agent_job_execution_id(execution), json_get_string(reference, "execution_id"));
    check_equal(atomic_load(&p.calls), 1); check_equal(atomic_load(&p.evidences), 0);
    check_equal(turbo_agent_runtime_store_list_json(p.runtime, "runs", NULL, NULL, &record), SALTS_OK);
    check_equal(json_array_size(record), (size_t)1);
    json_free(result); json_free(certificate); json_free(reference);
    result = certificate = reference = NULL;
    check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), SALTS_EALREADY);
    check_null(result); check_null(certificate); check_null(reference);
    check_equal(submit(harness, &other, 0), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(other, 5000), SALTS_OK);
    check_equal(atomic_load(&p.calls), 1);
  }

  it("does not report terminal or write cancellation while a noncooperative callback remains active") {
    atomic_store(&p.callback.open, 0);
    check_equal(submit(harness, &execution, 0), SALTS_OK);
    check_true(await(&p.callback.entered));
    check_equal(turbo_agent_job_execution_cancel(execution, "stop-1", "reason"), SALTS_OK);
    check_equal(turbo_agent_job_execution_cancel(execution, "stop-2", "different"), SALTS_EALREADY);
    check_equal(turbo_agent_job_execution_wait(execution, 1), SALTS_ETIMEDOUT);
    check_equal(turbo_agent_job_execution_get_status(execution, &status), SALTS_OK);
    check_equal(status.state, TURBO_AGENT_EXECUTION_RUNNING);
    check_equal(status.operation_status, SALTS_EBUSY); check_equal(status.cancellation_status, SALTS_EBUSY);
    check_equal(atomic_load(&p.cancellation_claims), 0);
    check_equal(submit(harness, &other, 0), SALTS_EBUSY);
    atomic_store(&p.callback.open, 1);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_get_status(execution, &status), SALTS_OK);
    check_equal(status.state, TURBO_AGENT_EXECUTION_CANCELLED);
    check_equal(status.cancellation_status, SALTS_OK);
    check_equal(turbo_agent_job_get_cancellation(job, &record), SALTS_OK);
    check_equal(json_get_string(record, "request_id"), "stop-1");
    json_free(record); record = NULL;
    check_equal(turbo_agent_runtime_store_list_json(p.runtime, "compiled_inspect_attempts", NULL, NULL, &record), SALTS_OK);
    check_equal(json_get_string(json_array_get(record, 0), "phase"), "committed");
    check_equal(json_get_int(json_array_get(record, 0), "tool_status", -1), TURBO_TOOL_OK);
    check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), SALTS_ECANCELED);
    check_null(result); check_null(certificate); check_null(reference);
  }

  it("waits for cancellation publication and exposes closed admission during finalization") {
    atomic_store(&p.callback.open, 0); atomic_store(&p.publication.open, 0);
    check_equal(submit(harness, &execution, 0), SALTS_OK); check_true(await(&p.callback.entered));
    check_equal(turbo_agent_job_execution_cancel(execution, "stop-1", "reason"), SALTS_OK);
    atomic_store(&p.callback.open, 1); check_true(await(&p.publication.entered));
    check_equal(turbo_agent_job_execution_cancel(execution, "stop-2", "reason"), SALTS_EALREADY);
    check_equal(turbo_agent_job_execution_wait(execution, 1), SALTS_ETIMEDOUT);
    check_equal(turbo_agent_job_execution_get_status(execution, &status), SALTS_OK);
    check_equal(status.state, TURBO_AGENT_EXECUTION_RUNNING); check_equal(status.cancellation_status, SALTS_EBUSY);
    atomic_store(&p.publication.open, 1);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_get_status(execution, &status), SALTS_OK);
    check_equal(status.cancellation_status, SALTS_OK);
  }

  it("preserves cancellation write errors and lost acknowledgements without worker readback") {
    const int errors[] = {SALTS_ENOTSUP, SALTS_EIO};
    for (int i = 0; i < 2; ++i) {
      gate_t queue; atomic_init(&queue.entered, 0); atomic_init(&queue.open, 0);
      if (i) { turbo_agent_job_execution_release(execution); execution = NULL; }
      p.publication_error = errors[i]; p.after_commit = i;
      atomic_store(&p.unacknowledged, 0); atomic_store(&p.reads_after_error, 0);
      check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK);
      check_true(await(&queue.entered));
      check_equal(submit(harness, &execution, 0), SALTS_OK);
      check_equal(turbo_agent_job_execution_cancel(execution, "stop-1", "reason"), SALTS_OK);
      atomic_store(&queue.open, 1);
      check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
      check_equal(turbo_agent_job_execution_get_status(execution, &status), SALTS_OK);
      check_equal(status.cancellation_status, errors[i]);
      check_equal(atomic_load(&p.reads_after_error), 0);
      atomic_store(&p.unacknowledged, 0);
    }
    check_equal(turbo_agent_job_get_cancellation(job, &record), SALTS_OK);
    check_equal(json_get_string(record, "request_id"), "stop-1");
  }

  it("copies caller config and Job identity before queued admission") {
    gate_t queue; atomic_init(&queue.entered, 0); atomic_init(&queue.open, 0);
    check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK); check_true(await(&queue.entered));
    char id[] = "job", cap[] = "runtime_tools"; const char *caps[] = {cap};
    turbo_agent_compiler_config_t config; turbo_agent_compiler_config_init(&config);
    config.allowed_capabilities = caps; config.allowed_capability_count = 1;
    check_equal(turbo_agent_harness_submit_job_inspect(harness, id, 2, &config, 0, &execution), SALTS_OK);
    memset(id, 'x', 3); memset(cap, 'x', strlen(cap)); memset(&config, 0, sizeof(config));
    check_equal(turbo_agent_job_execution_get_status(execution, &status), SALTS_OK);
    check_equal(status.state, TURBO_AGENT_EXECUTION_QUEUED);
    check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), SALTS_EBUSY);
    atomic_store(&queue.open, 1);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), SALTS_OK);
  }

  it("cancels queued Jobs and materializes the durable request without starting a Run") {
    gate_t queue; atomic_init(&queue.entered, 0); atomic_init(&queue.open, 0);
    check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK); check_true(await(&queue.entered));
    check_equal(submit(harness, &execution, 0), SALTS_OK);
    check_equal(turbo_agent_job_execution_cancel(execution, "queued-stop", "reason"), SALTS_OK);
    atomic_store(&queue.open, 1);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_get_status(execution, &status), SALTS_OK);
    check_equal(status.state, TURBO_AGENT_EXECUTION_CANCELLED); check_equal(status.cancellation_status, SALTS_OK);
    check_equal(atomic_load(&p.calls), 0);
    check_equal(turbo_agent_runtime_store_list_json(p.runtime, "runs", NULL, NULL, &record), SALTS_OK);
    check_equal(json_array_size(record), (size_t)0);
  }

  it("counts queue delay against the deadline without fabricating a durable request") {
    check_equal(submit(harness, &execution, cmeta_monotonic_ms()), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_get_status(execution, &status), SALTS_OK);
    check_equal(status.state, TURBO_AGENT_EXECUTION_TIMED_OUT);
    check_equal(status.cancellation_requested, 0); check_equal(status.cancellation_status, SALTS_ENOENT);
    check_equal(atomic_load(&p.calls), 0);
    check_equal(turbo_agent_job_get_cancellation(job, &record), SALTS_ENOENT);
  }

  it("settles pool CANCEL_PENDING with explicit cancellation and releases its worker reference") {
    gate_t queue; atomic_init(&queue.entered, 0); atomic_init(&queue.open, 0);
    check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK); check_true(await(&queue.entered));
    check_equal(submit(harness, &execution, 0), SALTS_OK);
    check_equal(turbo_agent_job_execution_cancel(execution, "shutdown-stop", "reason"), SALTS_OK);
    check_equal(cmeta_threadpool_shutdown_with_policy(pool, SALTS_THREADPOOL_SHUTDOWN_CANCEL_PENDING), SALTS_OK);
    atomic_store(&queue.open, 1);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_get_status(execution, &status), SALTS_OK);
    check_equal(status.state, TURBO_AGENT_EXECUTION_CANCELLED); check_equal(status.cancellation_status, SALTS_OK);
    check_equal(atomic_load(&p.calls), 0);
    atomic_store(&queue.open, 1);
  }

  it("settles pool-only queued cancellation without inventing a durable request") {
    gate_t queue; atomic_init(&queue.entered, 0); atomic_init(&queue.open, 0);
    check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK); check_true(await(&queue.entered));
    check_equal(submit(harness, &execution, 0), SALTS_OK);
    check_equal(cmeta_threadpool_shutdown_with_policy(pool, SALTS_THREADPOOL_SHUTDOWN_CANCEL_PENDING), SALTS_OK);
    atomic_store(&queue.open, 1);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_get_status(execution, &status), SALTS_OK);
    check_equal(status.state, TURBO_AGENT_EXECUTION_CANCELLED);
    check_equal(status.cancellation_requested, 0); check_equal(status.cancellation_status, SALTS_ENOENT);
    check_equal(atomic_load(&p.cancellation_claims), 0); check_equal(atomic_load(&p.calls), 0);
    check_equal(turbo_agent_job_get_cancellation(job, &record), SALTS_ENOENT);
  }

  it("rejects saturated or closed pool admission without touching Store or leaking the reservation") {
    gate_t queue; atomic_init(&queue.entered, 0); atomic_init(&queue.open, 0);
    check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK); check_true(await(&queue.entered));
    check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK);
    int reads = atomic_load(&p.reads), claims = atomic_load(&p.claims);
    check_equal(submit(harness, &execution, 0), SALTS_ENOBUFS); check_null(execution);
    check_equal(atomic_load(&p.reads), reads); check_equal(atomic_load(&p.claims), claims);
    atomic_store(&queue.open, 1); cmeta_threadpool_wait(pool);
    check_equal(submit(harness, &execution, 0), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    turbo_agent_job_execution_release(execution); execution = NULL;
    /* Operation terminal precedes the pool finalize callback's last reference.
     * Drain that exact ownership boundary before asserting fresh admission. */
    cmeta_threadpool_wait(pool);
    cmeta_threadpool_shutdown(pool);
    reads = atomic_load(&p.reads);
    check_equal(submit(harness, &execution, 0), SALTS_ESHUTDOWN); check_null(execution);
    check_equal(atomic_load(&p.reads), reads);
  }

  it("keeps owners alive after early client releases until a blocked worker completes") {
    atomic_store(&p.callback.open, 0);
    check_equal(submit(harness, &execution, 0), SALTS_OK); check_true(await(&p.callback.entered));
    turbo_agent_job_destroy(job); job = NULL;
    turbo_agent_job_execution_release(execution); execution = NULL;
    turbo_agent_harness_release(harness); harness = NULL;
    check_equal(atomic_load(&p.frees), 0);
    atomic_store(&p.callback.open, 1); cmeta_threadpool_wait(pool);
    check_equal(atomic_load(&p.frees), 1);
  }

  it("rechecks sources and current authority when taking a terminal result") {
    check_equal(submit(harness, &execution, 0), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    p.source_error = SALTS_EPROTO;
    check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), SALTS_EPROTO);
    check_null(result); check_null(certificate); check_null(reference);
    p.source_error = 0;
    check_equal(submit(harness, &other, 0), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(other, 5000), SALTS_OK);
    check_equal(policy(harness, 1), SALTS_OK); /* Legal quiescent replacement invalidates generation. */
    check_equal(turbo_agent_job_execution_take_result(other, &diagnostic, &result, &certificate, &reference), SALTS_EPROTO);
    check_equal(atomic_load(&p.calls), 1);
  }

  it("rechecks durable cancellation after terminal and permits status observation from Host callbacks") {
    check_equal(submit(harness, &execution, 0), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    p.nested = execution;
    check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), SALTS_OK);
    check_equal(p.nested_status, SALTS_OK); p.nested = NULL;
    json_free(result); json_free(certificate); json_free(reference); result = certificate = reference = NULL;
    check_equal(submit(harness, &other, 0), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(other, 5000), SALTS_OK);
    check_equal(turbo_agent_job_request_cancel(job, 2, "late-stop", "reason"), SALTS_OK);
    check_equal(turbo_agent_job_execution_take_result(other, &diagnostic, &result, &certificate, &reference), SALTS_ECANCELED);
    check_null(result); check_null(certificate); check_null(reference);
  }

  it("does not hand out a buffered result after canonical records become unreadable") {
    check_equal(submit(harness, &execution, 0), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    p.read_failure = "compiled_inspect_attempts";
    check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), SALTS_EIO);
    check_null(result); check_null(certificate); check_null(reference);
  }

  it("checks source again after canonical joins at the actual take boundary") {
    check_equal(submit(harness, &execution, 0), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    p.source_fail_at = atomic_load(&p.sources) + 2;
    check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), SALTS_EPROTO);
    check_null(result); check_null(certificate); check_null(reference);
    check_equal(atomic_load(&p.calls), 1);
  }

  it("polls status concurrently without treating a cancellation request as callback completion") {
    observer_t observer; cmeta_thread_t thread = {0};
    atomic_store(&p.callback.open, 0);
    check_equal(submit(harness, &execution, 0), SALTS_OK); check_true(await(&p.callback.entered));
    observer.execution = execution;
    atomic_init(&observer.ready, 0); atomic_init(&observer.done, 0); atomic_init(&observer.errors, 0);
    check_equal(cmeta_thread_create(&thread, observe, &observer), SALTS_OK); check_true(await(&observer.ready));
    check_equal(turbo_agent_job_execution_cancel(execution, "stop", "reason"), SALTS_OK);
    atomic_store(&p.callback.open, 1);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    atomic_store(&observer.done, 1); check_equal(cmeta_thread_join(&thread), SALTS_OK);
    cmeta_thread_destroy(&thread);
    check_equal(atomic_load(&observer.errors), 0);
  }

  it("validates public ABI, bounds, distinct outputs and cancellation identities") {
    turbo_agent_compiler_config_t config; turbo_agent_compiler_config_init(&config);
    char long_name[66]; memset(long_name, 'x', 65); long_name[65] = 0;
    const char *caps[] = {long_name};
    check_equal(turbo_agent_harness_submit_job_inspect(harness, "../bad", 2, &config, 0, &execution), SALTS_EINVAL);
    config.allowed_capabilities = caps; config.allowed_capability_count = 1;
    check_equal(turbo_agent_harness_submit_job_inspect(harness, "job", 2, &config, 0, &execution), SALTS_EINVAL);
    config.allowed_capability_count = 33;
    check_equal(turbo_agent_harness_submit_job_inspect(harness, "job", 2, &config, 0, &execution), SALTS_EINVAL);
    check_equal(turbo_agent_job_execution_wait(NULL, 0), SALTS_EINVAL);
    check_null(turbo_agent_job_execution_retain(NULL));
    check_equal(submit(harness, &execution, 0), SALTS_OK);
    check_equal(turbo_agent_job_execution_cancel(execution, "../bad", "reason"), SALTS_EINVAL);
    check_equal(turbo_agent_job_execution_cancel(execution, "stop", ""), SALTS_EINVAL);
    status.abi_version++;
    check_equal(turbo_agent_job_execution_get_status(execution, &status), SALTS_EINVAL);
    check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &result, &reference), SALTS_EINVAL);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_cancel(execution, "late", "reason"), SALTS_EALREADY);
  }

  it("arbitrates concurrent cancellation callers to exactly one immutable request") {
    contender_t callers[2]; cmeta_thread_t threads[2] = {0}; atomic_int go;
    atomic_init(&go, 0); atomic_store(&p.callback.open, 0);
    check_equal(submit(harness, &execution, 0), SALTS_OK); check_true(await(&p.callback.entered));
    for (int i = 0; i < 2; ++i) {
      callers[i].execution = execution; callers[i].request = i ? "second" : "first";
      callers[i].go = &go; callers[i].rc = SALTS_EIO; atomic_init(&callers[i].ready, 0);
      check_equal(cmeta_thread_create(&threads[i], cancel_contender, &callers[i]), SALTS_OK);
      check_true(await(&callers[i].ready));
    }
    atomic_store(&go, 1);
    for (int i = 0; i < 2; ++i) {
      check_equal(cmeta_thread_join(&threads[i]), SALTS_OK);
      cmeta_thread_destroy(&threads[i]);
    }
    check_true((callers[0].rc == SALTS_OK && callers[1].rc == SALTS_EALREADY) ||
        (callers[1].rc == SALTS_OK && callers[0].rc == SALTS_EALREADY));
    atomic_store(&p.callback.open, 1);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_get_cancellation(job, &record), SALTS_OK);
    check_equal(json_get_string(record, "request_id"), callers[0].rc == SALTS_OK ? "first" : "second");
    check_equal(atomic_load(&p.cancellation_claims), 1);
  }
}
