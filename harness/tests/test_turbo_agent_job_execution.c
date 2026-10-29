#include "tinytest.h"
#include "turbo_agent_job_execution.h"
#include "turbo_agent_job_observation.h"
#include "turbo_agent_job_admission.h"
#include "turbo_agent_job_queue_receipt.h"
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
  gate_t callback, publication, observation, admission, queue_receipt;
  atomic_int calls, reads, claims, cancellation_claims, frees, sources, evidences;
  atomic_int reads_after_error, unacknowledged;
  atomic_int observation_claims, observation_gets, observation_puts, observation_unacknowledged;
  atomic_int reads_after_observation_error, cancel_gets_after_error;
  atomic_int admission_claims, admission_gets, admission_puts, admission_unacknowledged;
  atomic_int reads_after_admission_error;
  int admission_error, admission_after_commit;
  atomic_int queue_receipt_claims, queue_receipt_gets, queue_receipt_puts;
  atomic_int queue_receipt_unacknowledged, queue_receipt_error_gets;
  int queue_receipt_error, queue_receipt_after_commit, queue_receipt_nested_status;
  int observation_error, observation_after_commit, observation_nested_status;
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
  if (!strcmp(collection, "agent_job_queue_receipts")) {
    atomic_fetch_add(&p->queue_receipt_gets, 1);
    if (atomic_load(&p->queue_receipt_unacknowledged)) atomic_fetch_add(&p->queue_receipt_error_gets, 1);
  }
  if (!strcmp(collection, "agent_job_admissions")) atomic_fetch_add(&p->admission_gets, 1);
  if (atomic_load(&p->admission_unacknowledged)) atomic_fetch_add(&p->reads_after_admission_error, 1);
  if (!strcmp(collection, "agent_job_observations")) atomic_fetch_add(&p->observation_gets, 1);
  if (!strcmp(collection, "agent_job_cancellations") && atomic_load(&p->unacknowledged))
    atomic_fetch_add(&p->cancel_gets_after_error, 1);
  if (atomic_load(&p->observation_unacknowledged)) atomic_fetch_add(&p->reads_after_observation_error, 1);
  if (atomic_load(&p->unacknowledged)) atomic_fetch_add(&p->reads_after_error, 1);
  if (p->read_failure && !strcmp(collection, p->read_failure)) { *out = NULL; return SALTS_EIO; }
  return p->inner.get(p->inner.user_data, collection, id, out);
}
static int put(void *user, const char *collection, const char *id, const char *value) {
  probe_t *p = user;
  if (!strcmp(collection, "agent_job_queue_receipts")) atomic_fetch_add(&p->queue_receipt_puts, 1);
  if (!strcmp(collection, "agent_job_admissions")) atomic_fetch_add(&p->admission_puts, 1);
  if (!strcmp(collection, "agent_job_observations")) atomic_fetch_add(&p->observation_puts, 1);
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
  if (!strcmp(collection, "agent_job_queue_receipts")) {
    atomic_fetch_add(&p->queue_receipt_claims, 1);
    if (p->nested) {
      int status;
      p->queue_receipt_nested_status = turbo_agent_job_execution_queue_receipt_status(p->nested, &status);
    }
    block(&p->queue_receipt);
    if (p->queue_receipt_error && !p->queue_receipt_after_commit) {
      atomic_store(&p->queue_receipt_unacknowledged, 1); return p->queue_receipt_error;
    }
    rc = p->inner.create_if_absent(p->inner.user_data, collection, id, value);
    if (rc == SALTS_OK && p->queue_receipt_error) rc = p->queue_receipt_error;
    if (rc != SALTS_OK) atomic_store(&p->queue_receipt_unacknowledged, 1);
    return rc;
  }
  if (!strcmp(collection, "agent_job_admissions")) {
    atomic_fetch_add(&p->admission_claims, 1);
    block(&p->admission);
    if (p->admission_error && !p->admission_after_commit) {
      atomic_store(&p->admission_unacknowledged, 1); return p->admission_error;
    }
    rc = p->inner.create_if_absent(p->inner.user_data, collection, id, value);
    if (rc == SALTS_OK && p->admission_error) rc = p->admission_error;
    if (rc != SALTS_OK) atomic_store(&p->admission_unacknowledged, 1);
    return rc;
  }
  if (!strcmp(collection, "agent_job_observations")) {
    atomic_fetch_add(&p->observation_claims, 1);
    if (p->nested) {
      int status;
      p->observation_nested_status = turbo_agent_job_execution_observation_status(p->nested, &status);
    }
    block(&p->observation);
    if (p->observation_error && !p->observation_after_commit) {
      atomic_store(&p->observation_unacknowledged, 1); return p->observation_error;
    }
    rc = p->inner.create_if_absent(p->inner.user_data, collection, id, value);
    if (rc == SALTS_OK && p->observation_error) rc = p->observation_error;
    if (rc != SALTS_OK) atomic_store(&p->observation_unacknowledged, 1);
    return rc;
  }
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
static int submit_recorded(turbo_agent_harness_t *h, const char *observation,
    turbo_agent_job_execution_t **out, uint64_t deadline) {
  const char *caps[] = {"runtime_tools"};
  turbo_agent_compiler_config_t config;
  turbo_agent_compiler_config_init(&config);
  config.allowed_capabilities = caps; config.allowed_capability_count = 1;
  return turbo_agent_harness_submit_recorded_job_inspect(h, "job", 2, &config, deadline, observation, out);
}
static int submit_admitted(turbo_agent_harness_t *h, const char *admission,
    turbo_agent_job_execution_t **out) {
  const char *caps[] = {"runtime_tools"};
  turbo_agent_compiler_config_t config;
  turbo_agent_compiler_config_init(&config);
  config.allowed_capabilities = caps; config.allowed_capability_count = 1;
  return turbo_agent_harness_submit_admitted_job_inspect(h, "job", 2, &config, 0, admission, out);
}
typedef struct admission_submit_s {
  turbo_agent_harness_t *harness;
  turbo_agent_job_execution_t *execution;
  int rc;
} admission_submit_t;
static int submit_receipted(turbo_agent_harness_t *h, const char *id,
    int *out_rejection, turbo_agent_job_execution_t **out) {
  const char *caps[] = {"runtime_tools"};
  turbo_agent_compiler_config_t config;
  turbo_agent_compiler_config_init(&config);
  config.allowed_capabilities = caps; config.allowed_capability_count = 1;
  return turbo_agent_harness_submit_receipted_job_inspect(h, "job", 2, &config, 0, id, out_rejection, out);
}
typedef struct receipt_submit_s { turbo_agent_harness_t *harness; int rc, rejection; } receipt_submit_t;
static void submit_receipt_thread(void *user) {
  receipt_submit_t *s = user;
  turbo_agent_job_execution_t *e = NULL;
  s->rc = submit_receipted(s->harness, "shutdown-overlap", &s->rejection, &e);
  turbo_agent_job_execution_release(e);
}
static void submit_admission_thread(void *user) {
  admission_submit_t *s = user;
  s->rc = submit_admitted(s->harness, "blocked-admission", &s->execution);
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
    atomic_init(&p.observation.entered, 0); atomic_init(&p.observation.open, 1);
    atomic_init(&p.admission.entered, 0); atomic_init(&p.admission.open, 1);
    atomic_init(&p.queue_receipt.entered, 0); atomic_init(&p.queue_receipt.open, 1);
    atomic_init(&p.calls, 0); atomic_init(&p.reads, 0); atomic_init(&p.claims, 0);
    atomic_init(&p.cancellation_claims, 0); atomic_init(&p.frees, 0);
    atomic_init(&p.sources, 0); atomic_init(&p.evidences, 0);
    atomic_init(&p.reads_after_error, 0); atomic_init(&p.unacknowledged, 0);
    atomic_init(&p.observation_claims, 0); atomic_init(&p.observation_gets, 0);
    atomic_init(&p.observation_puts, 0); atomic_init(&p.observation_unacknowledged, 0);
    atomic_init(&p.reads_after_observation_error, 0); atomic_init(&p.cancel_gets_after_error, 0);
    atomic_init(&p.admission_claims, 0); atomic_init(&p.admission_gets, 0);
    atomic_init(&p.admission_puts, 0); atomic_init(&p.admission_unacknowledged, 0);
    atomic_init(&p.reads_after_admission_error, 0);
    atomic_init(&p.queue_receipt_claims, 0); atomic_init(&p.queue_receipt_gets, 0);
    atomic_init(&p.queue_receipt_puts, 0); atomic_init(&p.queue_receipt_unacknowledged, 0);
    atomic_init(&p.queue_receipt_error_gets, 0);
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
    atomic_store(&p.queue_receipt.open, 1);
    atomic_store(&p.admission.open, 1);
    atomic_store(&p.callback.open, 1); atomic_store(&p.publication.open, 1); atomic_store(&p.observation.open, 1);
    cmeta_threadpool_wait(pool);
    turbo_agent_job_destroy(job); job = NULL;
    turbo_agent_job_execution_release(execution); turbo_agent_job_execution_release(other);
    execution = other = NULL;
    turbo_agent_harness_release(harness); harness = NULL;
    turbo_tool_registry_destroy(registry); cmeta_threadpool_destroy(pool);
    json_free(result); json_free(certificate); json_free(reference); json_free(record);
    result = certificate = reference = record = NULL;
  }

  it("acknowledges the selected native queue callback before Inspect without holding the status mutex") {
    int publication, rejection;
    gate_t queue; atomic_init(&queue.entered, 0); atomic_init(&queue.open, 0);
    check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK); check_true(await(&queue.entered));
    atomic_store(&p.queue_receipt.open, 0);
    check_equal(submit_receipted(harness, "queue-run", &rejection, &execution), SALTS_OK);
    check_equal(rejection, SALTS_ENOENT);
    check_equal(turbo_agent_job_execution_queue_receipt_status(execution, &publication), SALTS_EBUSY);
    check_equal(publication, SALTS_EBUSY);
    check_equal(turbo_agent_job_queue_receipt_get(p.runtime, "job", "queue-run", &record), SALTS_ENOENT);
    p.nested = execution; atomic_store(&queue.open, 1);
    check_true(await(&p.queue_receipt.entered));
    check_equal(p.queue_receipt_nested_status, SALTS_EBUSY);
    check_equal(atomic_load(&p.calls), 0); check_equal(atomic_load(&p.sources), 0);
    check_equal(turbo_agent_job_execution_wait(execution, 1), SALTS_ETIMEDOUT);
    atomic_store(&p.queue_receipt.open, 1);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK); p.nested = NULL;
    check_equal(turbo_agent_job_execution_queue_receipt_status(execution, &publication), SALTS_OK);
    check_equal(publication, SALTS_OK);
    check_equal(turbo_agent_job_queue_receipt_get(p.runtime, "job", "queue-run", &record), SALTS_OK);
    check_equal(json_object_size(record), (size_t)12);
    check_equal(json_get_string(record, "kind"), "job_inspect_queue_receipt");
    check_equal(json_get_string(record, "selection"), "run"); check_equal(json_get_int(record, "queue_status", 1), SALTS_OK);
    check_equal(json_get_string(record, "supervisor_id"), turbo_agent_job_execution_id(execution));
    check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), SALTS_OK);
    check_equal(atomic_load(&p.calls), 1); check_equal(atomic_load(&p.queue_receipt_claims), 1);
    check_equal(atomic_load(&p.queue_receipt_puts), 0);
  }

  it("withholds Inspect and returns original queue-receipt errors including lost acknowledgements") {
    const int errors[] = {SALTS_ENOTSUP, SALTS_EIO, SALTS_EIO, SALTS_EBUSY, SALTS_EALREADY};
    const char *ids[] = {"q-unsupported", "q-before", "q-lost", "q-busy", "q-conflict"};
    for (int i = 0; i < 5; ++i) {
      int rejection, publication;
      p.queue_receipt_error = errors[i]; p.queue_receipt_after_commit = i == 2;
      atomic_store(&p.queue_receipt_unacknowledged, 0);
      check_equal(submit_receipted(harness, ids[i], &rejection, &execution), SALTS_OK);
      check_equal(rejection, SALTS_ENOENT);
      check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
      check_equal(turbo_agent_job_execution_queue_receipt_status(execution, &publication), SALTS_OK);
      check_equal(publication, errors[i]);
      check_equal(turbo_agent_job_execution_get_status(execution, &status), SALTS_OK);
      check_equal(status.state, TURBO_AGENT_EXECUTION_FAILED); check_equal(status.operation_status, SALTS_EPROTO);
      check_equal(status.tool_status, TURBO_TOOL_ERROR);
      check_equal(turbo_agent_job_execution_observation_status(execution, &publication), SALTS_OK);
      check_equal(publication, SALTS_ENOENT);
      check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), errors[i]);
      check_null(result); check_null(certificate); check_null(reference);
      check_equal(atomic_load(&p.calls), 0); check_equal(atomic_load(&p.sources), 0);
      check_equal(atomic_load(&p.queue_receipt_error_gets), 0);
      atomic_store(&p.queue_receipt_unacknowledged, 0);
      check_equal(turbo_agent_job_queue_receipt_get(p.runtime, "job", ids[i], &record), i == 2 ? SALTS_OK : SALTS_ENOENT);
      json_free(record); record = NULL; turbo_agent_job_execution_release(execution); execution = NULL;
    }
    check_equal(atomic_load(&p.observation_claims), 0); check_equal(atomic_load(&p.queue_receipt_puts), 0);
  }

  it("keeps native rejection and rejection-receipt publication as separate outcomes") {
    int rejection;
    gate_t queue; atomic_init(&queue.entered, 0); atomic_init(&queue.open, 0);
    check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK); check_true(await(&queue.entered));
    check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK);
    p.queue_receipt_error = SALTS_EIO; p.queue_receipt_after_commit = 1;
    check_equal(submit_receipted(harness, "q-full", &rejection, &execution), SALTS_ENOBUFS);
    check_null(execution); check_equal(rejection, SALTS_EIO);
    check_equal(atomic_load(&p.queue_receipt_error_gets), 0);
    atomic_store(&p.queue_receipt_unacknowledged, 0);
    check_equal(turbo_agent_job_queue_receipt_get(p.runtime, "job", "q-full", &record), SALTS_OK);
    check_equal(json_get_string(record, "selection"), "rejected");
    check_equal(json_get_int(record, "queue_status", 0), SALTS_ENOBUFS);
    json_free(record); record = NULL;
    atomic_store(&queue.open, 1); cmeta_threadpool_wait(pool); cmeta_threadpool_shutdown(pool);
    p.queue_receipt_error = 0;
    check_equal(submit_receipted(harness, "q-closed", &rejection, &execution), SALTS_ESHUTDOWN);
    check_null(execution); check_equal(rejection, SALTS_OK);
    check_equal(turbo_agent_job_queue_receipt_get(p.runtime, "job", "q-closed", &record), SALTS_OK);
    check_equal(json_get_int(record, "queue_status", 0), SALTS_ESHUTDOWN);
    check_equal(atomic_load(&p.calls), 0); check_equal(atomic_load(&p.observation_claims), 0);
  }

  it("publishes an accepted Host cancellation even when its preceding queue receipt loses acknowledgement") {
    int rejection, publication;
    atomic_store(&p.queue_receipt.open, 0); p.queue_receipt_error = SALTS_EIO; p.queue_receipt_after_commit = 1;
    check_equal(submit_receipted(harness, "q-cancel-error", &rejection, &execution), SALTS_OK);
    check_true(await(&p.queue_receipt.entered));
    check_equal(turbo_agent_job_execution_cancel(execution, "stop", "reason"), SALTS_OK);
    atomic_store(&p.queue_receipt.open, 1);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_get_status(execution, &status), SALTS_OK);
    check_equal(status.cancellation_status, SALTS_OK); check_equal(status.operation_status, SALTS_EPROTO);
    check_equal(turbo_agent_job_execution_queue_receipt_status(execution, &publication), SALTS_OK);
    check_equal(publication, SALTS_EIO); check_equal(atomic_load(&p.queue_receipt_error_gets), 0);
    check_equal(atomic_load(&p.cancellation_claims), 1); check_equal(atomic_load(&p.calls), 0);
    check_equal(turbo_agent_job_get_cancellation(job, &record), SALTS_OK);
    check_equal(json_get_string(record, "request_id"), "stop");
  }

  it("receipts pool cancellation without selecting Inspect or inventing a Host request") {
    int rejection, publication;
    gate_t queue; atomic_init(&queue.entered, 0); atomic_init(&queue.open, 0);
    check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK); check_true(await(&queue.entered));
    check_equal(submit_receipted(harness, "q-pool", &rejection, &execution), SALTS_OK);
    check_equal(cmeta_threadpool_shutdown_with_policy(pool, SALTS_THREADPOOL_SHUTDOWN_CANCEL_PENDING), SALTS_OK);
    atomic_store(&queue.open, 1);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_queue_receipt_status(execution, &publication), SALTS_OK); check_equal(publication, SALTS_OK);
    check_equal(turbo_agent_job_queue_receipt_get(p.runtime, "job", "q-pool", &record), SALTS_OK);
    check_equal(json_get_string(record, "selection"), "pool_cancelled");
    check_equal(json_get_int(record, "queue_status", 1), SALTS_OK);
    check_equal(atomic_load(&p.calls), 0); check_equal(atomic_load(&p.cancellation_claims), 0);
  }

  it("does not let a native acceptance receipt bypass current authority") {
    int rejection;
    gate_t queue; atomic_init(&queue.entered, 0); atomic_init(&queue.open, 0);
    check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK); check_true(await(&queue.entered));
    check_equal(submit_receipted(harness, "q-policy", &rejection, &execution), SALTS_OK);
    check_equal(policy(harness, 0), SALTS_OK); atomic_store(&queue.open, 1);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_queue_receipt_get(p.runtime, "job", "q-policy", &record), SALTS_OK);
    check_equal(json_get_string(record, "selection"), "run");
    check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), SALTS_EPROTO);
    check_null(result); check_null(certificate); check_null(reference); check_equal(atomic_load(&p.calls), 0);
  }

  it("rejects malformed receipts and intent mismatches and preserves historical read errors") {
    const char *keys[] = {"kind", "admission_id", "observation_id", "supervisor_id", "job_id", "thread_id", "attempt_id", "selection"};
    const char *numbers[] = {"job_revision", "proposal_version", "queue_status"};
    json_value_t *original = NULL, *bad = NULL, *intent = NULL;
    int rejection;
    check_equal(submit_receipted(harness, "q-shape", &rejection, &execution), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_queue_receipt_get(p.runtime, "job", "q-shape", &original), SALTS_OK);
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
      bad = json_clone(original); check_equal(turbo_runtime_json_object_set_string(bad, keys[i], "wrong"), 0);
      check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_queue_receipts", "job:q-shape", bad), SALTS_OK);
      check_equal(turbo_agent_job_queue_receipt_get(p.runtime, "job", "q-shape", &record), SALTS_EPROTO); check_null(record); json_free(bad);
    }
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); ++i) {
      bad = json_clone(original); check_equal(turbo_runtime_json_object_set_int64(bad, numbers[i], INT64_MAX), 0);
      check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_queue_receipts", "job:q-shape", bad), SALTS_OK);
      check_equal(turbo_agent_job_queue_receipt_get(p.runtime, "job", "q-shape", &record), SALTS_EPROTO); check_null(record); json_free(bad);
    }
    bad = json_clone(original); check_equal(turbo_runtime_json_object_set_int64(bad, "queue_status", SALTS_EIO), 0);
    check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_queue_receipts", "job:q-shape", bad), SALTS_OK);
    check_equal(turbo_agent_job_queue_receipt_get(p.runtime, "job", "q-shape", &record), SALTS_EPROTO); json_free(bad);
    bad = json_clone(original); check_equal(turbo_runtime_json_object_set_int64(bad, "schema_version", INT64_MAX), 0);
    check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_queue_receipts", "job:q-shape", bad), SALTS_OK);
    check_equal(turbo_agent_job_queue_receipt_get(p.runtime, "job", "q-shape", &record), SALTS_ENOTSUP); json_free(bad);
    bad = json_clone(original); check_equal(turbo_runtime_json_object_set_string(bad, "result", "forbidden"), 0);
    check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_queue_receipts", "job:q-shape", bad), SALTS_OK);
    check_equal(turbo_agent_job_queue_receipt_get(p.runtime, "job", "q-shape", &record), SALTS_EPROTO); json_free(bad);
    check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_queue_receipts", "job:q-shape", original), SALTS_OK);
    check_equal(turbo_agent_job_admission_get(p.runtime, "job", "q-shape", &intent), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_string(intent, "supervisor_id", "00000000-0000-0000-0000-000000000000"), 0);
    check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_admissions", "job:q-shape", intent), SALTS_OK);
    check_equal(turbo_agent_job_queue_receipt_get(p.runtime, "job", "q-shape", &record), SALTS_EPROTO);
    p.read_failure = "agent_job_queue_receipts";
    check_equal(turbo_agent_job_queue_receipt_get(p.runtime, "job", "q-shape", &record), SALTS_EIO); check_null(record);
    json_free(original); json_free(intent);
  }

  it("validates required receipt outputs and keeps older handles unreceipted") {
    int rejection = 0, publication;
    check_equal(submit_receipted(harness, "bad:id", &rejection, &execution), SALTS_EINVAL);
    check_null(execution); check_equal(rejection, SALTS_ENOENT);
    check_equal(submit_receipted(harness, "q-missing-output", NULL, &execution), SALTS_EINVAL); check_null(execution);
    check_equal(submit_receipted(harness, NULL, &rejection, &execution), SALTS_EINVAL);
    check_equal(turbo_agent_job_execution_queue_receipt_status(NULL, &publication), SALTS_EINVAL);
    check_equal(turbo_agent_job_queue_receipt_get(p.runtime, "job", "q-missing", &record), SALTS_ENOENT);
    check_equal(turbo_agent_job_queue_receipt_get(p.runtime, "../bad", "q", &record), SALTS_EINVAL);
    check_equal(turbo_agent_job_queue_receipt_get(p.runtime, "job", "q", NULL), SALTS_EINVAL);
    check_equal(submit_admitted(harness, "q-old", &execution), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_queue_receipt_status(execution, &publication), SALTS_OK);
    check_equal(publication, SALTS_ENOENT);
    check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), SALTS_OK);
    check_equal(submit_receipted(harness, "q-old", &rejection, &other), SALTS_EALREADY);
    check_null(other); check_equal(rejection, SALTS_ENOENT); check_equal(atomic_load(&p.queue_receipt_claims), 0);
  }

  it("retains Runtime owners until a blocked queue receipt and its selected worker settle") {
    int rejection;
    atomic_store(&p.queue_receipt.open, 0);
    check_equal(submit_receipted(harness, "q-owners", &rejection, &execution), SALTS_OK);
    check_true(await(&p.queue_receipt.entered));
    turbo_agent_job_destroy(job); job = NULL;
    turbo_agent_job_execution_release(execution); execution = NULL;
    turbo_agent_harness_release(harness); harness = NULL;
    check_equal(atomic_load(&p.frees), 0);
    atomic_store(&p.queue_receipt.open, 1); cmeta_threadpool_wait(pool);
    check_equal(atomic_load(&p.frees), 1);
  }

  it("receipts shutdown overlapping pre-queue intent without retaining a rejected task") {
    receipt_submit_t s = {harness, SALTS_EBUSY, SALTS_EBUSY}; cmeta_thread_t thread = {0};
    atomic_store(&p.admission.open, 0);
    check_equal(cmeta_thread_create(&thread, submit_receipt_thread, &s), SALTS_OK); check_true(await(&p.admission.entered));
    check_equal(cmeta_threadpool_shutdown_with_policy(pool, SALTS_THREADPOOL_SHUTDOWN_CANCEL_PENDING), SALTS_OK);
    turbo_agent_job_destroy(job); job = NULL;
    turbo_agent_harness_release(harness); harness = NULL;
    check_equal(atomic_load(&p.frees), 0);
    atomic_store(&p.admission.open, 1);
    check_equal(cmeta_thread_join(&thread), SALTS_OK); cmeta_thread_destroy(&thread);
    check_equal(s.rc, SALTS_ESHUTDOWN); check_equal(s.rejection, SALTS_OK);
    check_equal(atomic_load(&p.queue_receipt_claims), 1); check_equal(atomic_load(&p.calls), 0);
    check_equal(atomic_load(&p.frees), 1);
  }

  it("persists admission before enqueue and joins the eventual observation to the same supervisor") {
    gate_t queue; atomic_init(&queue.entered, 0); atomic_init(&queue.open, 0);
    check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK); check_true(await(&queue.entered));
    char id[] = "intent-1";
    check_equal(submit_admitted(harness, id, &execution), SALTS_OK);
    memset(id, 'x', strlen(id));
    check_equal(atomic_load(&p.admission_claims), 1); check_equal(atomic_load(&p.admission_gets), 0);
    check_equal(turbo_agent_job_admission_get(p.runtime, "job", "intent-1", &record), SALTS_OK);
    check_equal(json_object_size(record), (size_t)10);
    check_equal(json_get_string(record, "kind"), "job_inspect_admission_intent");
    check_equal(json_get_string(record, "observation_id"), "intent-1");
    check_equal(json_get_string(record, "supervisor_id"), turbo_agent_job_execution_id(execution));
    check_equal(json_get_string(record, "thread_id"), "async-job-thread");
    check_equal(atomic_load(&p.calls), 0); check_equal(atomic_load(&p.observation_claims), 0);
    json_free(record); record = NULL;
    atomic_store(&queue.open, 1);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "intent-1", &record), SALTS_OK);
    check_equal(json_get_string(record, "supervisor_id"), turbo_agent_job_execution_id(execution));
    check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), SALTS_OK);
    check_equal(atomic_load(&p.calls), 1); check_equal(atomic_load(&p.admission_puts), 0);
    check_equal(submit_admitted(harness, "intent-1", &other), SALTS_EALREADY); check_null(other);
    check_equal(atomic_load(&p.reads_after_admission_error), 0);
  }

  it("never queues after admission errors or lost acknowledgements and never reads them back") {
    const int errors[] = {SALTS_ENOTSUP, SALTS_EIO, SALTS_EIO};
    const char *ids[] = {"unsupported-admission", "failed-admission", "uncertain-admission"};
    for (int i = 0; i < 3; ++i) {
      p.admission_error = errors[i]; p.admission_after_commit = i == 2;
      atomic_store(&p.admission_unacknowledged, 0); atomic_store(&p.reads_after_admission_error, 0);
      check_equal(submit_admitted(harness, ids[i], &execution), errors[i]); check_null(execution);
      cmeta_threadpool_wait(pool);
      check_equal(atomic_load(&p.calls), 0); check_equal(atomic_load(&p.sources), 0);
      check_equal(atomic_load(&p.observation_claims), 0); check_equal(atomic_load(&p.admission_gets), i);
      check_equal(atomic_load(&p.reads_after_admission_error), 0);
      atomic_store(&p.admission_unacknowledged, 0);
      check_equal(turbo_agent_job_admission_get(p.runtime, "job", ids[i], &record), i == 2 ? SALTS_OK : SALTS_ENOENT);
      json_free(record); record = NULL;
    }
    p.admission_error = 0;
    check_equal(submit_admitted(harness, "uncertain-admission", &execution), SALTS_EALREADY); check_null(execution);
    check_equal(atomic_load(&p.reads_after_admission_error), 0);
    check_equal(atomic_load(&p.admission_puts), 0); check_equal(atomic_load(&p.calls), 0);
  }

  it("preserves intent and the original queue rejection without inventing a terminal observation") {
    gate_t queue; atomic_init(&queue.entered, 0); atomic_init(&queue.open, 0);
    check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK); check_true(await(&queue.entered));
    check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK);
    check_equal(submit_admitted(harness, "full-admission", &execution), SALTS_ENOBUFS); check_null(execution);
    check_equal(turbo_agent_job_admission_get(p.runtime, "job", "full-admission", &record), SALTS_OK);
    json_free(record); record = NULL;
    check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "full-admission", &record), SALTS_ENOENT);
    atomic_store(&queue.open, 1); cmeta_threadpool_wait(pool);
    check_equal(submit_admitted(harness, "full-admission", &execution), SALTS_EALREADY); check_null(execution);
    atomic_store(&p.admission_unacknowledged, 0);
    cmeta_threadpool_shutdown(pool);
    check_equal(submit_admitted(harness, "closed-admission", &execution), SALTS_ESHUTDOWN); check_null(execution);
    check_equal(turbo_agent_job_admission_get(p.runtime, "job", "closed-admission", &record), SALTS_OK);
    check_equal(atomic_load(&p.observation_claims), 0); check_equal(atomic_load(&p.calls), 0);
  }

  it("rejects invalid inputs, stale bindings, read failures and cancellation before creating an intent") {
    turbo_agent_compiler_config_t config; turbo_agent_compiler_config_init(&config);
    int reads = atomic_load(&p.reads);
    check_equal(submit_admitted(harness, "bad:id", &execution), SALTS_EINVAL); check_null(execution);
    check_equal(submit_admitted(harness, NULL, &execution), SALTS_EINVAL);
    check_equal(atomic_load(&p.reads), reads);
    check_equal(turbo_agent_harness_submit_admitted_job_inspect(harness, "job", 1, &config, 0, "stale", &execution), SALTS_EBUSY);
    p.read_failure = "agent_job_revisions";
    check_equal(submit_admitted(harness, "unreadable", &execution), SALTS_EIO); check_null(execution);
    p.read_failure = NULL;
    check_equal(turbo_agent_job_request_cancel(job, 2, "stop", "reason"), SALTS_OK);
    check_equal(submit_admitted(harness, "cancelled-admission", &execution), SALTS_ECANCELED); check_null(execution);
    check_equal(atomic_load(&p.admission_claims), 0); check_equal(atomic_load(&p.calls), 0);
  }

  it("retains the reservation and owners while pre-queue admission publication is blocked") {
    admission_submit_t s = {harness, NULL, SALTS_EBUSY}; cmeta_thread_t thread = {0};
    atomic_store(&p.admission.open, 0);
    check_equal(cmeta_thread_create(&thread, submit_admission_thread, &s), SALTS_OK);
    check_true(await(&p.admission.entered));
    check_equal(atomic_load(&p.calls), 0); check_equal(atomic_load(&p.sources), 0);
    check_equal(submit(harness, &other, 0), SALTS_EBUSY); check_null(other);
    turbo_agent_job_destroy(job); job = NULL;
    turbo_agent_harness_release(harness); harness = NULL;
    check_equal(atomic_load(&p.frees), 0);
    atomic_store(&p.admission.open, 1);
    check_equal(cmeta_thread_join(&thread), SALTS_OK); cmeta_thread_destroy(&thread);
    check_equal(s.rc, SALTS_OK); execution = s.execution;
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    turbo_agent_job_execution_release(execution); execution = NULL;
    cmeta_threadpool_wait(pool); check_equal(atomic_load(&p.frees), 1);
  }

  it("validates every admission field, unsupported schemas and complete Job history") {
    const char *keys[] = {"kind", "admission_id", "observation_id", "supervisor_id", "job_id", "thread_id", "attempt_id"};
    const char *numbers[] = {"job_revision", "proposal_version"};
    json_value_t *original = NULL, *bad = NULL;
    check_equal(submit_admitted(harness, "admission-shape", &execution), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_admission_get(p.runtime, "job", "admission-shape", &original), SALTS_OK);
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
      bad = json_clone(original);
      check_equal(turbo_runtime_json_object_set_string(bad, keys[i], "wrong"), 0);
      check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_admissions", "job:admission-shape", bad), SALTS_OK);
      check_equal(turbo_agent_job_admission_get(p.runtime, "job", "admission-shape", &record), SALTS_EPROTO);
      check_null(record); json_free(bad);
    }
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); ++i) {
      bad = json_clone(original);
      check_equal(turbo_runtime_json_object_set_int64(bad, numbers[i], INT64_MAX), 0);
      check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_admissions", "job:admission-shape", bad), SALTS_OK);
      check_equal(turbo_agent_job_admission_get(p.runtime, "job", "admission-shape", &record), SALTS_EPROTO);
      check_null(record); json_free(bad);
    }
    bad = json_clone(original);
    check_equal(turbo_runtime_json_object_set_int64(bad, "schema_version", INT64_MAX), 0);
    check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_admissions", "job:admission-shape", bad), SALTS_OK);
    check_equal(turbo_agent_job_admission_get(p.runtime, "job", "admission-shape", &record), SALTS_ENOTSUP);
    json_free(bad); bad = json_clone(original);
    check_equal(turbo_runtime_json_object_set_string(bad, "result", "forbidden"), 0);
    check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_admissions", "job:admission-shape", bad), SALTS_OK);
    check_equal(turbo_agent_job_admission_get(p.runtime, "job", "admission-shape", &record), SALTS_EPROTO);
    check_null(record); json_free(bad);
    check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_admissions", "job:admission-shape", original), SALTS_OK);
    p.read_failure = "agent_job_revisions";
    check_equal(turbo_agent_job_admission_get(p.runtime, "job", "admission-shape", &record), SALTS_EIO);
    check_null(record); json_free(original);
  }

  it("reads historical intent without authority or source checks and preserves backend query errors") {
    check_equal(submit_admitted(harness, "historical-admission", &execution), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(policy(harness, 0), SALTS_OK); p.source_error = SALTS_EIO;
    check_equal(turbo_agent_job_request_cancel(job, 2, "late-stop", "reason"), SALTS_OK);
    int sources = atomic_load(&p.sources), claims = atomic_load(&p.claims);
    check_equal(turbo_agent_job_admission_get(p.runtime, "job", "historical-admission", &record), SALTS_OK);
    check_equal(atomic_load(&p.sources), sources); check_equal(atomic_load(&p.claims), claims);
    json_free(record); record = (json_value_t *)1;
    check_equal(turbo_agent_job_admission_get(p.runtime, "job", "missing-admission", &record), SALTS_ENOENT); check_null(record);
    p.read_failure = "agent_job_admissions";
    check_equal(turbo_agent_job_admission_get(p.runtime, "job", "historical-admission", &record), SALTS_EIO); check_null(record);
    check_equal(turbo_agent_job_admission_get(p.runtime, "../bad", "x", &record), SALTS_EINVAL); check_null(record);
    check_equal(turbo_agent_job_admission_get(p.runtime, "job", "x", NULL), SALTS_EINVAL);
    /* Durable cancellation is checked before source verification at take. */
    check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), SALTS_ECANCELED);
    check_null(result); check_null(certificate); check_null(reference);
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

  it("records settled execution metadata and canonical reference while preserving the v1 status layout") {
    int publication;
    check_equal(submit_recorded(harness, "observation-1", &execution, 0), SALTS_OK);
    check_equal(turbo_agent_job_execution_observation_id(execution), "observation-1");
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_observation_status(execution, &publication), SALTS_OK);
    check_equal(publication, SALTS_OK);
    check_equal(turbo_agent_job_execution_get_status(execution, &status), SALTS_OK);
    check_equal(status.state, TURBO_AGENT_EXECUTION_COMPLETED); check_equal(status.operation_status, SALTS_OK);
    check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "observation-1", &record), SALTS_OK);
    check_equal(json_object_size(record), (size_t)17);
    check_equal(json_get_string(record, "supervisor_id"), turbo_agent_job_execution_id(execution));
    check_equal(json_get_string(record, "state"), "completed");
    check_equal(json_get_string(record, "dispatch"), "inspect");
    check_equal(json_get_string(json_object_get(record, "execution_reference"), "job_id"), "job");
    check_equal(json_type(json_object_get(record, "cancellation_request")), JSON_NULL);
    check_null(json_object_get(record, "result")); check_null(json_object_get(record, "certificate"));
    check_null(json_object_get(record, "diagnostic"));
    check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), SALTS_OK);
    check_equal(json_get_string(result, "answer"), "canonical");
    check_equal(json_get_string(reference, "run_id"), json_get_string(json_object_get(record, "execution_reference"), "run_id"));
    check_equal(atomic_load(&p.observation_claims), 1); check_equal(atomic_load(&p.observation_puts), 0);
    check_equal(atomic_load(&p.calls), 1);
  }

  it("keeps local wait nonterminal until observation publication and permits concurrent status queries") {
    int publication;
    gate_t queue; atomic_init(&queue.entered, 0); atomic_init(&queue.open, 0);
    check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK); check_true(await(&queue.entered));
    atomic_store(&p.observation.open, 0);
    check_equal(submit_recorded(harness, "observed-cancel", &execution, 0), SALTS_OK);
    p.nested = execution;
    check_equal(turbo_agent_job_execution_observation_status(execution, &publication), SALTS_EBUSY);
    check_equal(publication, SALTS_EBUSY);
    check_equal(turbo_agent_job_execution_cancel(execution, "stop-1", "reason"), SALTS_OK);
    atomic_store(&queue.open, 1); check_true(await(&p.observation.entered));
    check_equal(turbo_agent_job_execution_wait(execution, 1), SALTS_ETIMEDOUT);
    check_equal(turbo_agent_job_execution_get_status(execution, &status), SALTS_OK);
    check_equal(status.state, TURBO_AGENT_EXECUTION_RUNNING);
    check_equal(p.observation_nested_status, SALTS_EBUSY);
    check_equal(turbo_agent_job_execution_cancel(execution, "too-late", "reason"), SALTS_EALREADY);
    atomic_store(&p.observation.open, 1);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK); p.nested = NULL;
    check_equal(turbo_agent_job_execution_observation_status(execution, &publication), SALTS_OK);
    check_equal(publication, SALTS_OK);
    check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "observed-cancel", &record), SALTS_OK);
    check_equal(json_get_string(record, "state"), "cancelled");
    check_equal(json_get_string(json_object_get(record, "cancellation_request"), "request_id"), "stop-1");
    check_equal(json_type(json_object_get(record, "execution_reference")), JSON_NULL);
    check_equal(atomic_load(&p.calls), 0);
  }

  it("reports observation write errors independently and never returns unacknowledged results") {
    const int errors[] = {SALTS_ENOTSUP, SALTS_EIO, SALTS_EIO};
    const char *ids[] = {"unsupported", "failed-before", "lost-ack"};
    for (int i = 0; i < 3; ++i) {
      int publication;
      p.observation_error = errors[i]; p.observation_after_commit = i == 2;
      atomic_store(&p.observation_unacknowledged, 0); atomic_store(&p.reads_after_observation_error, 0);
      check_equal(submit_recorded(harness, ids[i], &execution, 0), SALTS_OK);
      check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
      check_equal(turbo_agent_job_execution_observation_status(execution, &publication), SALTS_OK);
      check_equal(publication, errors[i]);
      check_equal(turbo_agent_job_execution_get_status(execution, &status), SALTS_OK);
      check_equal(status.state, TURBO_AGENT_EXECUTION_COMPLETED); check_equal(status.operation_status, SALTS_OK);
      check_equal(atomic_load(&p.reads_after_observation_error), 0);
      check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), errors[i]);
      check_null(result); check_null(certificate); check_null(reference);
      check_equal(atomic_load(&p.reads_after_observation_error), 0);
      atomic_store(&p.observation_unacknowledged, 0);
      check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", ids[i], &record), i == 2 ? SALTS_OK : SALTS_ENOENT);
      json_free(record); record = NULL;
      turbo_agent_job_execution_release(execution); execution = NULL;
    }
    check_equal(atomic_load(&p.observation_claims), 3); check_equal(atomic_load(&p.observation_puts), 0);
    check_equal(atomic_load(&p.calls), 1);
  }

  it("preserves duplicate observation identity and requires explicit supervisor reconciliation") {
    int publication;
    char first[37];
    check_equal(submit_recorded(harness, "same-id", &execution, 0), SALTS_OK);
    memcpy(first, turbo_agent_job_execution_id(execution), sizeof(first));
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), SALTS_OK);
    json_free(result); json_free(certificate); json_free(reference); result = certificate = reference = NULL;
    check_equal(submit_recorded(harness, "same-id", &other, 0), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(other, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_observation_status(other, &publication), SALTS_OK);
    check_equal(publication, SALTS_EALREADY); check_equal(atomic_load(&p.reads_after_observation_error), 0);
    check_equal(turbo_agent_job_execution_take_result(other, &diagnostic, &result, &certificate, &reference), SALTS_EALREADY);
    check_null(result); check_null(certificate); check_null(reference);
    atomic_store(&p.observation_unacknowledged, 0);
    check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "same-id", &record), SALTS_OK);
    check_equal(json_get_string(record, "supervisor_id"), first);
    check_not_equal(json_get_string(record, "supervisor_id"), turbo_agent_job_execution_id(other));
    check_equal(atomic_load(&p.calls), 1);
  }

  it("retains the original cancellation error in a separately acknowledged terminal observation") {
    int publication;
    gate_t queue; atomic_init(&queue.entered, 0); atomic_init(&queue.open, 0);
    check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK); check_true(await(&queue.entered));
    p.publication_error = SALTS_EIO; p.after_commit = 1;
    check_equal(submit_recorded(harness, "cancel-lost-ack", &execution, 0), SALTS_OK);
    check_equal(turbo_agent_job_execution_cancel(execution, "stop-1", "reason"), SALTS_OK);
    atomic_store(&queue.open, 1);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_observation_status(execution, &publication), SALTS_OK);
    check_equal(publication, SALTS_OK); check_equal(atomic_load(&p.cancel_gets_after_error), 0);
    atomic_store(&p.unacknowledged, 0);
    check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "cancel-lost-ack", &record), SALTS_OK);
    check_equal(json_get_int(record, "cancellation_status", 0), SALTS_EIO);
    json_free(record); record = NULL;
    check_equal(turbo_agent_job_get_cancellation(job, &record), SALTS_OK);
    check_equal(json_get_string(record, "request_id"), "stop-1");
  }

  it("validates every observation identity, status join and future schema") {
    const char *keys[] = {"kind", "observation_id", "supervisor_id", "job_id", "thread_id", "attempt_id", "dispatch", "state"};
    const char *numbers[] = {"job_revision", "proposal_version", "operation_status", "tool_status", "cancellation_status"};
    json_value_t *original = NULL, *bad = NULL;
    check_equal(submit_recorded(harness, "shape", &execution, 0), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "shape", &original), SALTS_OK);
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
      bad = json_clone(original);
      check_equal(turbo_runtime_json_object_set_string(bad, keys[i], "wrong"), 0);
      check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_observations", "job:shape", bad), SALTS_OK);
      check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "shape", &record), SALTS_EPROTO);
      check_null(record); json_free(bad);
    }
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); ++i) {
      bad = json_clone(original);
      check_equal(turbo_runtime_json_object_set_int64(bad, numbers[i], INT64_C(2147483648)), 0);
      check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_observations", "job:shape", bad), SALTS_OK);
      check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "shape", &record), SALTS_EPROTO);
      check_null(record); json_free(bad);
    }
    bad = json_clone(original);
    check_equal(turbo_runtime_json_object_set_int64(bad, "schema_version", 2), 0);
    check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_observations", "job:shape", bad), SALTS_OK);
    check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "shape", &record), SALTS_ENOTSUP);
    json_free(bad); bad = json_clone(original);
    check_equal(turbo_runtime_json_object_set_string(bad, "result", "forbidden"), 0);
    check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_observations", "job:shape", bad), SALTS_OK);
    check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "shape", &record), SALTS_EPROTO);
    json_free(bad); json_free(original);
  }

  it("treats historical metadata as an observation rather than current result authority") {
    check_equal(submit_recorded(harness, "historical", &execution, 0), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(policy(harness, 0), SALTS_OK); p.source_error = SALTS_EIO;
    int sources = atomic_load(&p.sources);
    check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "historical", &record), SALTS_OK);
    check_equal(json_get_string(record, "state"), "completed");
    check_equal(atomic_load(&p.sources), sources);
    check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), SALTS_EIO);
    check_null(result); check_null(certificate); check_null(reference);
  }

  it("records failed and timed-out local operations without inventing execution references") {
    check_equal(submit_recorded(harness, "expired", &execution, cmeta_monotonic_ms()), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "expired", &record), SALTS_OK);
    check_equal(json_get_string(record, "state"), "timed_out");
    check_equal(json_type(json_object_get(record, "execution_reference")), JSON_NULL);
    check_equal(turbo_agent_job_execution_take_result(execution, &diagnostic, &result, &certificate, &reference), SALTS_ETIMEDOUT);
    json_free(record); record = NULL;
    p.source_error = SALTS_EIO;
    check_equal(submit_recorded(harness, "failed", &other, 0), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(other, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "failed", &record), SALTS_OK);
    check_equal(json_get_string(record, "state"), "failed");
    check_equal(json_get_int(record, "operation_status", 0), SALTS_EIO);
    check_equal(json_type(json_object_get(record, "execution_reference")), JSON_NULL);
    check_equal(atomic_load(&p.calls), 0);
  }

  it("keeps the legacy entry unrecorded and validates observation API inputs before Store access") {
    int publication;
    const char *caps[] = {"runtime_tools"};
    turbo_agent_compiler_config_t config; turbo_agent_compiler_config_init(&config);
    config.allowed_capabilities = caps; config.allowed_capability_count = 1;
    int reads = atomic_load(&p.reads);
    check_equal(submit_recorded(harness, "../bad", &execution, 0), SALTS_EINVAL);
    check_equal(submit_recorded(harness, NULL, &execution, 0), SALTS_EINVAL);
    check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "../bad", &record), SALTS_EINVAL);
    check_null(record); check_equal(atomic_load(&p.reads), reads);
    check_equal(turbo_agent_harness_submit_recorded_job_inspect(harness, "job", 1, &config, 0, "stale", &execution), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_observation_status(execution, &publication), SALTS_OK);
    check_equal(publication, SALTS_EBUSY);
    turbo_agent_job_execution_release(execution); execution = NULL; cmeta_threadpool_wait(pool);
    check_equal(submit(harness, &execution, 0), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_null(turbo_agent_job_execution_observation_id(execution));
    check_equal(turbo_agent_job_execution_observation_status(execution, &publication), SALTS_OK);
    check_equal(publication, SALTS_ENOENT);
    check_equal(atomic_load(&p.observation_claims), 0);
  }

  it("rejects recorded queue admission without reading or publishing an observation") {
    gate_t queue; atomic_init(&queue.entered, 0); atomic_init(&queue.open, 0);
    check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK); check_true(await(&queue.entered));
    check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK);
    int reads = atomic_load(&p.reads), claims = atomic_load(&p.claims);
    check_equal(submit_recorded(harness, "rejected", &execution, 0), SALTS_ENOBUFS);
    check_null(execution); check_equal(atomic_load(&p.reads), reads); check_equal(atomic_load(&p.claims), claims);
    check_equal(atomic_load(&p.observation_claims), 0);
    atomic_store(&queue.open, 1); cmeta_threadpool_wait(pool);
    cmeta_threadpool_shutdown(pool);
    check_equal(submit_recorded(harness, "closed", &execution, 0), SALTS_ESHUTDOWN);
    check_null(execution); check_equal(atomic_load(&p.reads), reads);
  }

  it("records pool cancellation as local non-dispatch without a Host cancellation request") {
    gate_t queue; atomic_init(&queue.entered, 0); atomic_init(&queue.open, 0);
    check_equal(cmeta_threadpool_try_submit(pool, block, &queue), SALTS_OK); check_true(await(&queue.entered));
    check_equal(submit_recorded(harness, "pool-cancelled", &execution, 0), SALTS_OK);
    check_equal(cmeta_threadpool_shutdown_with_policy(pool, SALTS_THREADPOOL_SHUTDOWN_CANCEL_PENDING), SALTS_OK);
    atomic_store(&queue.open, 1);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "pool-cancelled", &record), SALTS_OK);
    check_equal(json_get_string(record, "dispatch"), "pool_cancelled");
    check_equal(json_get_string(record, "state"), "cancelled");
    check_equal(json_get_bool(record, "cancellation_requested", 1), 0);
    check_equal(json_get_int(record, "cancellation_status", 0), SALTS_ENOENT);
    check_equal(atomic_load(&p.calls), 0); check_equal(atomic_load(&p.cancellation_claims), 0);
  }

  it("retains Runtime and Store through observation publication after all client owners release") {
    atomic_store(&p.callback.open, 0); atomic_store(&p.observation.open, 0);
    check_equal(submit_recorded(harness, "released", &execution, 0), SALTS_OK);
    check_true(await(&p.callback.entered));
    turbo_agent_job_destroy(job); job = NULL;
    turbo_agent_job_execution_release(execution); execution = NULL;
    turbo_agent_harness_release(harness); harness = NULL;
    atomic_store(&p.callback.open, 1); check_true(await(&p.observation.entered));
    check_equal(atomic_load(&p.frees), 0); check_equal(atomic_load(&p.observation_claims), 1);
    atomic_store(&p.observation.open, 1); cmeta_threadpool_wait(pool);
    check_equal(atomic_load(&p.frees), 1);
  }

  it("rejects malformed nested reference and cancellation status fields and preserves read errors") {
    json_value_t *original = NULL, *bad = NULL;
    const char *keys[] = {"job_id", "thread_id", "attempt_id", "attempt_record_id"};
    check_equal(submit_recorded(harness, "nested", &execution, 0), SALTS_OK);
    check_equal(turbo_agent_job_execution_wait(execution, 5000), SALTS_OK);
    check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "nested", &original), SALTS_OK);
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
      bad = json_clone(original);
      check_equal(turbo_runtime_json_object_set_string(json_object_get(bad, "execution_reference"), keys[i], "wrong"), 0);
      check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_observations", "job:nested", bad), SALTS_OK);
      check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "nested", &record), SALTS_EPROTO);
      check_null(record); json_free(bad);
    }
    bad = json_clone(original);
    check_equal(turbo_runtime_json_object_set_int64(json_object_get(bad, "execution_reference"), "schema_version", 2), 0);
    check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_observations", "job:nested", bad), SALTS_OK);
    check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "nested", &record), SALTS_ENOTSUP);
    json_free(bad); bad = json_clone(original);
    check_equal(turbo_runtime_json_object_set_string(bad, "cancellation_requested", "false"), 0);
    check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_observations", "job:nested", bad), SALTS_OK);
    check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "nested", &record), SALTS_EPROTO);
    json_free(bad); bad = json_clone(original);
    check_equal(turbo_runtime_json_object_set_clone(bad, "execution_reference", json_object_get(original, "cancellation_request")), 0);
    check_equal(turbo_agent_runtime_store_put_json(p.runtime, "agent_job_observations", "job:nested", bad), SALTS_OK);
    check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "nested", &record), SALTS_EPROTO);
    json_free(bad);
    p.read_failure = "agent_job_observations";
    check_equal(turbo_agent_job_execution_observation_get(p.runtime, "job", "nested", &record), SALTS_EIO);
    check_null(record); json_free(original);
  }
}
