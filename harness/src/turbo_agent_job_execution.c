#include "turbo_agent_job_execution.h"
#include "turbo_agent_job_cancel.h"
#include "turbo_agent_harness_internal.h"
#include "turbo_agent_job_observation_internal.h"
#include "turbo_agent_job_admission_internal.h"

#include <cmeta_uuid.h>
#include <salts/clock.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct turbo_agent_job_execution_s {
  atomic_size_t refs;
  cmeta_mutex_t mutex;
  cmeta_cond_t changed;
  turbo_agent_harness_t *harness;
  turbo_cancel_source_t *source;
  turbo_cancel_token_t *token;
  turbo_agent_compiler_config_t config;
  char capability_bytes[32][65];
  const char *capabilities[32];
  char id[SALTS_UUID_STRING_SIZE], job_id[65];
  char request_id[65], reason_ref[1025];
  char observation_id[65];
  int observation_status;
  int queue_receipted, queue_receipt_status;
  uint32_t revision;
  uint64_t deadline;
  int reserved, closing;
  turbo_agent_job_execution_status_t status;
  json_value_t *reference;
};

static int bounded(const char *s, size_t max) {
  size_t i;
  if (!s || !s[0]) return 0;
  for (i = 0; i <= max; ++i) if (!s[i]) return 1;
  return 0;
}
static int identifier(const char *s, size_t max) {
  if (!bounded(s, max)) return 0;
  for (size_t i = 0; s[i]; ++i) {
    unsigned char c = (unsigned char)s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
  }
  return 1;
}
static int terminal(turbo_agent_execution_status_t state) {
  return state >= TURBO_AGENT_EXECUTION_COMPLETED && state <= TURBO_AGENT_EXECUTION_FAILED;
}
void turbo_agent_job_execution_status_init(turbo_agent_job_execution_status_t *status) {
  if (!status) return;
  memset(status, 0, sizeof(*status));
  status->struct_size = sizeof(*status);
  status->abi_version = TURBO_AGENT_JOB_EXECUTION_ABI_VERSION;
  status->state = TURBO_AGENT_EXECUTION_QUEUED;
  status->operation_status = SALTS_EBUSY;
  status->tool_status = TURBO_TOOL_INVALID_ARGUMENT;
  status->cancellation_status = SALTS_ENOENT;
}

turbo_agent_job_execution_t *turbo_agent_job_execution_retain(turbo_agent_job_execution_t *e) {
  size_t refs;
  if (!e) return NULL;
  refs = atomic_load_explicit(&e->refs, memory_order_relaxed);
  for (;;) {
    if (!refs || refs == SIZE_MAX) return NULL;
    if (atomic_compare_exchange_weak_explicit(&e->refs, &refs, refs + 1,
        memory_order_relaxed, memory_order_relaxed)) return e;
  }
}
/* Final release is possible only after the worker relinquishes its reference,
 * or on rejected submission. Neither path has a live callback/Store operation. */
void turbo_agent_job_execution_release(turbo_agent_job_execution_t *e) {
  if (!e || atomic_fetch_sub_explicit(&e->refs, 1, memory_order_acq_rel) != 1) return;
  if (e->reserved) turbo_agent_harness_end_submit(e->harness);
  json_free(e->reference);
  turbo_cancel_token_release(e->token);
  turbo_cancel_source_destroy(e->source);
  turbo_agent_harness_release(e->harness);
  cmeta_cond_destroy(&e->changed);
  cmeta_mutex_destroy(&e->mutex);
  free(e);
}
const char *turbo_agent_job_execution_id(const turbo_agent_job_execution_t *e) {
  return e ? e->id : NULL;
}
int turbo_agent_job_execution_get_status(const turbo_agent_job_execution_t *e,
    turbo_agent_job_execution_status_t *out) {
  if (!e || !out || out->struct_size < sizeof(*out) ||
      out->abi_version != TURBO_AGENT_JOB_EXECUTION_ABI_VERSION) return SALTS_EINVAL;
  cmeta_mutex_lock((cmeta_mutex_t *)&e->mutex);
  *out = e->status;
  cmeta_mutex_unlock((cmeta_mutex_t *)&e->mutex);
  return SALTS_OK;
}
const char *turbo_agent_job_execution_observation_id(const turbo_agent_job_execution_t *e) {
  return e && e->observation_id[0] ? e->observation_id : NULL;
}
int turbo_agent_job_execution_observation_status(const turbo_agent_job_execution_t *e, int *out) {
  int rc;
  if (!out) return SALTS_EINVAL;
  *out = SALTS_EBUSY;
  if (!e) return SALTS_EINVAL;
  cmeta_mutex_lock((cmeta_mutex_t *)&e->mutex);
  rc = terminal(e->status.state) ? SALTS_OK : SALTS_EBUSY;
  if (rc == SALTS_OK) *out = e->observation_status;
  cmeta_mutex_unlock((cmeta_mutex_t *)&e->mutex);
  return rc;
}
int turbo_agent_job_execution_cancel(turbo_agent_job_execution_t *e,
    const char *request, const char *reason) {
  int rc;
  if (!e || !identifier(request, 64) || !bounded(reason, 1024)) return SALTS_EINVAL;
  cmeta_mutex_lock(&e->mutex);
  if (e->closing || e->status.cancellation_requested) rc = SALTS_EALREADY;
  else {
    /* No allocation or Store operation in this concurrent control path. A
     * deadline may already have won the token; still retain this explicit
     * persistent request. The first explicit request identity remains fixed. */
    rc = turbo_cancel_source_cancel(e->source, TURBO_CANCEL_USER);
    if (rc == SALTS_OK || rc == SALTS_EALREADY) {
      memcpy(e->request_id, request, strlen(request) + 1);
      memcpy(e->reason_ref, reason, strlen(reason) + 1);
      e->status.cancellation_requested = 1;
      e->status.cancellation_status = SALTS_EBUSY;
      rc = SALTS_OK;
    }
  }
  cmeta_mutex_unlock(&e->mutex);
  return rc;
}
int turbo_agent_job_execution_queue_receipt_status(const turbo_agent_job_execution_t *e, int *out) {
  int rc;
  if (!out) return SALTS_EINVAL;
  *out = SALTS_EBUSY;
  if (!e) return SALTS_EINVAL;
  cmeta_mutex_lock((cmeta_mutex_t *)&e->mutex);
  rc = terminal(e->status.state) ? SALTS_OK : SALTS_EBUSY;
  if (rc == SALTS_OK) *out = e->queue_receipt_status;
  cmeta_mutex_unlock((cmeta_mutex_t *)&e->mutex);
  return rc;
}
int turbo_agent_job_execution_wait(turbo_agent_job_execution_t *e, uint64_t timeout) {
  uint64_t now, deadline = UINT64_MAX;
  int rc = SALTS_OK;
  if (!e) return SALTS_EINVAL;
  if (timeout != UINT64_MAX) {
    now = cmeta_monotonic_ms();
    deadline = UINT64_MAX - now < timeout ? UINT64_MAX : now + timeout;
  }
  cmeta_mutex_lock(&e->mutex);
  while (!terminal(e->status.state)) {
    uint64_t wait;
    if (timeout == UINT64_MAX) { cmeta_cond_wait(&e->changed, &e->mutex); continue; }
    now = cmeta_monotonic_ms();
    if (now >= deadline) { rc = SALTS_ETIMEDOUT; break; }
    wait = deadline - now;
    if (wait > UINT32_MAX - UINT64_C(1)) wait = UINT32_MAX - UINT64_C(1);
    (void)cmeta_cond_timedwait(&e->changed, &e->mutex, wait * UINT64_C(1000000));
  }
  cmeta_mutex_unlock(&e->mutex);
  return rc;
}

static void settle(void *user, int dispatch) {
  turbo_agent_job_execution_t *e = user;
  turbo_agent_compile_diagnostic_t diagnostic = {0};
  turbo_tool_execution_context_t context = {0};
  turbo_tool_status_t tool_status = TURBO_TOOL_INVALID_ARGUMENT;
  json_value_t *result = NULL, *certificate = NULL, *reference = NULL;
  turbo_agent_job_t *job = NULL;
  turbo_agent_job_execution_status_t observed;
  int observation_rc = SALTS_ENOENT;
  int receipt_rc = SALTS_ENOENT;
  int operation, cancel_rc = SALTS_ENOENT, delivery;
  cmeta_mutex_lock(&e->mutex);
  e->status.state = TURBO_AGENT_EXECUTION_RUNNING;
  cmeta_cond_broadcast(&e->changed);
  cmeta_mutex_unlock(&e->mutex);
  context.struct_size = sizeof(context); context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
  context.cancel_token = e->token; context.deadline_mono_ms = e->deadline;
  if (e->queue_receipted) {
    turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(
        turbo_agent_app_session(turbo_agent_harness_app(e->harness)));
    receipt_rc = turbo_agent_job_queue_receipt_write(runtime, e->job_id, e->revision,
        e->observation_id, e->id, SALTS_OK, dispatch);
  }
  if (e->queue_receipted && receipt_rc != SALTS_OK) {
    operation = SALTS_EPROTO;
    tool_status = TURBO_TOOL_ERROR;
  } else if (dispatch) {
    operation = turbo_agent_harness_execute_job_reserved(e->harness, e->job_id,
        e->revision, &e->config, &context, &diagnostic, &tool_status, &result, &certificate, &reference);
  } else {
    (void)turbo_cancel_source_cancel(e->source, TURBO_CANCEL_SHUTDOWN);
    operation = SALTS_ECANCELED;
    tool_status = TURBO_TOOL_CANCELLED;
  }
  /* The canonical attempt owns the result. Do not buffer a second payload
   * while a terminal handle waits for its Host to take or discard. */
  json_free(result); json_free(certificate);
  cmeta_mutex_lock(&e->mutex);
  e->closing = 1;
  cmeta_mutex_unlock(&e->mutex);
  /* Only the worker accesses Runtime, after synchronous Inspect has exited.
   * closing makes accepted request bytes immutable across this Store operation. */
  if (e->status.cancellation_requested) {
    turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(
        turbo_agent_app_session(turbo_agent_harness_app(e->harness)));
    cancel_rc = turbo_agent_job_open(runtime, e->job_id, &job);
    if (cancel_rc == SALTS_OK)
      cancel_rc = turbo_agent_job_request_cancel(job, e->revision, e->request_id, e->reason_ref);
    turbo_agent_job_destroy(job);
  }
  delivery = operation == SALTS_OK ? turbo_cancel_token_check(e->token) : operation;
  turbo_agent_job_execution_status_init(&observed);
  observed.operation_status = operation;
  observed.tool_status = tool_status;
  observed.diagnostic = diagnostic;
  observed.cancellation_requested = e->status.cancellation_requested;
  observed.cancellation_status = cancel_rc;
  observed.state = delivery == SALTS_OK ? TURBO_AGENT_EXECUTION_COMPLETED :
      delivery == SALTS_ECANCELED ? TURBO_AGENT_EXECUTION_CANCELLED :
      delivery == SALTS_ETIMEDOUT ? TURBO_AGENT_EXECUTION_TIMED_OUT : TURBO_AGENT_EXECUTION_FAILED;
  if (e->observation_id[0] && (!e->queue_receipted || receipt_rc == SALTS_OK)) {
    turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(
        turbo_agent_app_session(turbo_agent_harness_app(e->harness)));
    observation_rc = turbo_agent_job_observation_write(runtime, e->job_id, e->revision,
        e->observation_id, e->id, dispatch, &observed, e->request_id, e->reason_ref, reference);
  }
  if (delivery != SALTS_OK) { json_free(reference); reference = NULL; }
  cmeta_mutex_lock(&e->mutex);
  e->status = observed;
  e->observation_status = observation_rc;
  e->queue_receipt_status = receipt_rc;
  e->reference = reference;
  cmeta_cond_broadcast(&e->changed);
  cmeta_mutex_unlock(&e->mutex);
}
static void worker(void *user) { settle(user, 1); }
static void cancel_pending(void *user) { settle(user, 0); }
static void finalize(void *user) { turbo_agent_job_execution_release(user); }

typedef enum submit_mode_e { SUBMIT_VOLATILE, SUBMIT_ADMITTED, SUBMIT_RECEIPTED } submit_mode_t;
static int submit(turbo_agent_harness_t *harness,
    const char *id, uint32_t revision, const turbo_agent_compiler_config_t *config,
    uint64_t deadline, const char *observation, submit_mode_t mode,
    int *out_rejection_receipt_status, turbo_agent_job_execution_t **out) {
  turbo_agent_job_execution_t *e;
  turbo_cancel_source_config_t cancel_config = {sizeof(cancel_config), TURBO_RUNTIME_CONTROL_ABI_VERSION, 0, NULL, NULL};
  cmeta_uuid_t uuid;
  cmeta_threadpool_task_t task = {worker, cancel_pending, finalize, NULL};
  int rc;
  if (out_rejection_receipt_status) *out_rejection_receipt_status = SALTS_ENOENT;
  if (!out) return SALTS_EINVAL;
  *out = NULL;
  if (!harness || !identifier(id, 64) || revision > TURBO_AGENT_JOB_MAX_REVISIONS ||
      (observation && !identifier(observation, 64)) ||
      !config || config->struct_size < sizeof(*config) ||
      config->abi_version != TURBO_AGENT_COMPILER_CONFIG_ABI_VERSION || !config->deny_unlisted_capabilities ||
      config->allowed_capability_count > 32 ||
      (config->allowed_capability_count && !config->allowed_capabilities)) return SALTS_EINVAL;
  for (size_t i = 0; i < config->allowed_capability_count; ++i)
    if (!bounded(config->allowed_capabilities[i], 64)) return SALTS_EINVAL;
  e = calloc(1, sizeof(*e));
  if (!e) return SALTS_ENOMEM;
  atomic_init(&e->refs, 1);
  cmeta_mutex_init(&e->mutex); cmeta_cond_init(&e->changed);
  e->harness = turbo_agent_harness_retain(harness);
  if (!e->mutex || !e->changed || !e->harness) { rc = SALTS_ENOMEM; goto fail; }
  turbo_agent_job_execution_status_init(&e->status);
  e->observation_status = observation ? SALTS_EBUSY : SALTS_ENOENT;
  e->queue_receipted = mode == SUBMIT_RECEIPTED;
  e->queue_receipt_status = e->queue_receipted ? SALTS_EBUSY : SALTS_ENOENT;
  if (observation) memcpy(e->observation_id, observation, strlen(observation) + 1);
  memcpy(e->job_id, id, strlen(id) + 1);
  e->revision = revision; e->deadline = deadline; e->config = *config;
  for (size_t i = 0; i < config->allowed_capability_count; ++i) {
    memcpy(e->capability_bytes[i], config->allowed_capabilities[i], strlen(config->allowed_capabilities[i]) + 1);
    e->capabilities[i] = e->capability_bytes[i];
  }
  e->config.allowed_capabilities = e->capabilities;
  cancel_config.deadline_mono_ms = deadline;
  rc = turbo_cancel_source_create(&cancel_config, &e->source);
  if (rc != SALTS_OK) goto fail;
  rc = turbo_cancel_source_token(e->source, &e->token);
  if (rc != SALTS_OK) goto fail;
  rc = cmeta_uuid_v7_generate(&uuid);
  if (rc == SALTS_OK) rc = cmeta_uuid_format(&uuid, e->id, sizeof(e->id));
  if (rc != SALTS_OK) goto fail;
  rc = turbo_agent_harness_begin_submit(harness);
  if (rc != SALTS_OK) goto fail;
  e->reserved = 1;
  if (mode != SUBMIT_VOLATILE) {
    turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(
        turbo_agent_app_session(turbo_agent_harness_app(harness)));
    rc = turbo_agent_job_admission_write(runtime, e->job_id, e->revision, e->observation_id, e->id);
    if (rc != SALTS_OK) goto fail;
  }
  if (!turbo_agent_job_execution_retain(e)) { rc = SALTS_ERANGE; goto fail; }
  task.arg = e;
  rc = cmeta_threadpool_try_submit_task(turbo_agent_harness_executor(harness), &task);
  if (rc != SALTS_OK) {
    if (e->queue_receipted) {
      turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(
          turbo_agent_app_session(turbo_agent_harness_app(e->harness)));
      *out_rejection_receipt_status = turbo_agent_job_queue_receipt_write(runtime, e->job_id,
          e->revision, e->observation_id, e->id, rc, 0);
    }
    turbo_agent_job_execution_release(e); goto fail;
  }
  *out = e;
  return SALTS_OK;
fail:
  turbo_agent_job_execution_release(e);
  return rc;
}

int turbo_agent_harness_submit_job_inspect(turbo_agent_harness_t *harness,
    const char *id, uint32_t revision, const turbo_agent_compiler_config_t *config,
    uint64_t deadline, turbo_agent_job_execution_t **out) {
  return submit(harness, id, revision, config, deadline, NULL, SUBMIT_VOLATILE, NULL, out);
}
int turbo_agent_harness_submit_recorded_job_inspect(turbo_agent_harness_t *harness,
    const char *id, uint32_t revision, const turbo_agent_compiler_config_t *config,
    uint64_t deadline, const char *observation, turbo_agent_job_execution_t **out) {
  if (!observation) { if (out) *out = NULL; return SALTS_EINVAL; }
  return submit(harness, id, revision, config, deadline, observation, SUBMIT_VOLATILE, NULL, out);
}

int turbo_agent_harness_submit_admitted_job_inspect(turbo_agent_harness_t *harness,
    const char *id, uint32_t revision, const turbo_agent_compiler_config_t *config,
    uint64_t deadline, const char *admission, turbo_agent_job_execution_t **out) {
  if (!admission) { if (out) *out = NULL; return SALTS_EINVAL; }
  return submit(harness, id, revision, config, deadline, admission, SUBMIT_ADMITTED, NULL, out);
}

int turbo_agent_harness_submit_receipted_job_inspect(turbo_agent_harness_t *harness,
    const char *id, uint32_t revision, const turbo_agent_compiler_config_t *config,
    uint64_t deadline, const char *admission, int *out_rejection, turbo_agent_job_execution_t **out) {
  if (out_rejection) *out_rejection = SALTS_ENOENT;
  if (!admission || !out_rejection) { if (out) *out = NULL; return SALTS_EINVAL; }
  return submit(harness, id, revision, config, deadline, admission, SUBMIT_RECEIPTED, out_rejection, out);
}

int turbo_agent_job_execution_take_result(turbo_agent_job_execution_t *e,
    turbo_agent_compile_diagnostic_t *diagnostic, json_value_t **out_result,
    json_value_t **out_certificate, json_value_t **out_reference) {
  turbo_agent_job_proof_t proof = {0};
  json_value_t *result = NULL, *certificate = NULL, *reference = NULL;
  int rc;
  if (out_result) *out_result = NULL;
  if (out_certificate) *out_certificate = NULL;
  if (out_reference) *out_reference = NULL;
  if (diagnostic) memset(diagnostic, 0, sizeof(*diagnostic));
  if (!e || !out_result || !out_certificate || !out_reference || out_result == out_certificate ||
      out_result == out_reference || out_certificate == out_reference) return SALTS_EINVAL;
  cmeta_mutex_lock(&e->mutex);
  if (!terminal(e->status.state)) rc = SALTS_EBUSY;
  else if (e->status.result_taken) rc = SALTS_EALREADY;
  else {
    e->status.result_taken = 1;
    rc = SALTS_OK;
  }
  cmeta_mutex_unlock(&e->mutex);
  if (rc != SALTS_OK) return rc;
  /* No execution mutex is held across Host/Store callbacks. The Host owns this
   * take/release interval exclusively; the Harness reservation remains held. */
  rc = e->status.operation_status;
  if (diagnostic) *diagnostic = e->status.diagnostic;
  if (e->queue_receipted && e->queue_receipt_status != SALTS_OK) {
    rc = e->queue_receipt_status;
    goto done;
  }
  if (rc != SALTS_OK) goto done;
  if (e->observation_id[0] && e->observation_status != SALTS_OK) {
    rc = e->observation_status;
    goto done;
  }
  rc = turbo_cancel_token_check(e->token);
  if (rc != SALTS_OK) goto done;
  rc = turbo_agent_harness_prepare_job_proof(e->harness, e->job_id, e->revision,
      1, &e->config, diagnostic, &proof);
  if (rc != SALTS_OK) goto done;
  rc = turbo_agent_harness_load_job_result(&proof, e->reference, diagnostic, &result);
  if (rc != SALTS_OK) goto done;
  certificate = json_clone(proof.admission.certificate);
  reference = json_clone(e->reference);
  if (!certificate || !reference) { rc = SALTS_ENOMEM; goto done; }
  rc = turbo_agent_harness_verify_job_source(e->harness, proof.snapshot, diagnostic);
  if (rc == SALTS_OK) rc = turbo_agent_harness_require_job_active(proof.runtime, proof.snapshot, diagnostic);
  if (rc == SALTS_OK && turbo_agent_tool_authority_snapshot_is_current(proof.agent, &proof.authority) != 1)
    rc = SALTS_EPROTO;
  if (rc == SALTS_OK) rc = turbo_cancel_token_check(e->token);
  if (rc == SALTS_OK) {
    *out_result = result; result = NULL;
    *out_certificate = certificate; certificate = NULL;
    *out_reference = reference; reference = NULL;
  }
done:
  json_free(result); json_free(certificate); json_free(reference);
  turbo_agent_harness_job_proof_destroy(&proof);
  cmeta_mutex_lock(&e->mutex);
  e->reserved = 0;
  cmeta_mutex_unlock(&e->mutex);
  turbo_agent_harness_end_submit(e->harness);
  return rc;
}
