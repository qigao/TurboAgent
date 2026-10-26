#include <salts/thread_pool.h>
#include "turbo_agent_harness.h"
#include "turbo_agent_job.h"
#include "turbo_agent_harness_internal.h"
#include "turbo_agent_job_internal.h"
#include <json_parser.h>

#include "turbo_agent_session_execution.h"
#include "turbo_agent_session.h"
#include "turbo_agent_state.h"
#include "turbo_agent_policy.h"
#include "turbo_tool_schema.h"
#include "turbo_runtime_json.h"
#include "turbo_runtime_segment.h"
#include "turbo_runtime_store.h"

#include <inttypes.h>
#include <limits.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct turbo_agent_harness_s {
  atomic_size_t ref_count;
  cmeta_mutex_t mutex;
  turbo_agent_app_t *app;
  char *compiled_attempt_thread_id;
  cmeta_threadpool_t *executor;
  turbo_agent_execution_t *active_execution;
  int submitting;
  turbo_agent_job_verifier_t job_verifier;
};

struct turbo_agent_harness_execution_s {
  atomic_size_t ref_count;
  turbo_agent_harness_t *harness;
  turbo_graph_t *graph;
  turbo_agent_execution_t *execution;
  int terminal_before_publish;
};

static int turbo_agent_harness_config_valid(const turbo_agent_harness_config_t *config) {
  return config && config->struct_size >= sizeof(*config) &&
         config->abi_version == TURBO_AGENT_HARNESS_ABI_VERSION && config->app_config &&
         config->app_config->session_config &&
         config->executor && cmeta_threadpool_capacity(config->executor) > 0 &&
         cmeta_threadpool_is_accepting(config->executor);
}

static int turbo_agent_harness_run_options_valid(const turbo_agent_harness_run_options_t *options) {
  return !options || (options->struct_size >= sizeof(*options) &&
                      options->abi_version == TURBO_AGENT_HARNESS_ABI_VERSION);
}

void turbo_agent_harness_config_init(turbo_agent_harness_config_t *config) {
  if (!config) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_AGENT_HARNESS_ABI_VERSION;
}

void turbo_agent_harness_run_options_init(turbo_agent_harness_run_options_t *options) {
  if (!options) {
    return;
  }
  memset(options, 0, sizeof(*options));
  options->struct_size = sizeof(*options);
  options->abi_version = TURBO_AGENT_HARNESS_ABI_VERSION;
}

static int turbo_agent_harness_construct(const turbo_agent_harness_config_t *config,
    int explicit_store, const turbo_agent_job_verifier_t *verifier,
    turbo_agent_harness_t **out_harness) {
  turbo_agent_harness_t *harness;
  const turbo_agent_session_config_t *session_config;
  const char *thread_id;
  int rc;

  if (!out_harness) return SALTS_EINVAL;
  *out_harness = NULL;
  if (!turbo_agent_harness_config_valid(config)) return SALTS_EINVAL;
  session_config = config->app_config->session_config;
  if (explicit_store && (!session_config->runtime_store.put ||
      !session_config->runtime_store.get || !session_config->runtime_store.list))
    return SALTS_EINVAL;
  harness = (turbo_agent_harness_t *)calloc(1, sizeof(*harness));
  if (!harness) return SALTS_ENOMEM;
  if (verifier) harness->job_verifier = *verifier;
  atomic_init(&harness->ref_count, 1);
  cmeta_mutex_init(&harness->mutex);
  if (!harness->mutex) {
    free(harness);
    return SALTS_ENOMEM;
  }
  /* Allocate the stable scope before App/Session consumes any store. There
   * must be no fallible construction left after App succeeds. */
  thread_id = session_config->thread_id;
  if (thread_id && thread_id[0]) {
    size_t size = strlen(thread_id) + 1;
    harness->compiled_attempt_thread_id = (char *)malloc(size);
    if (!harness->compiled_attempt_thread_id) {
      cmeta_mutex_destroy(&harness->mutex);
      free(harness);
      return SALTS_ENOMEM;
    }
    memcpy(harness->compiled_attempt_thread_id, thread_id, size);
  }
  if (explicit_store) {
    rc = turbo_agent_app_create_with_store(config->app_config, &harness->app);
  } else {
    harness->app = turbo_agent_app_create(config->app_config);
    rc = harness->app ? SALTS_OK : SALTS_EIO;
  }
  if (rc != SALTS_OK) {
    free(harness->compiled_attempt_thread_id);
    cmeta_mutex_destroy(&harness->mutex);
    free(harness);
    return rc;
  }
  /* Empty thread scope is still captured lazily by compiled Inspect. */
  harness->executor = config->executor;
  *out_harness = harness;
  return SALTS_OK;
}

turbo_agent_harness_t *turbo_agent_harness_create(const turbo_agent_harness_config_t *config) {
  turbo_agent_harness_t *harness = NULL;
  (void)turbo_agent_harness_construct(config, 0, NULL, &harness);
  return harness;
}

int turbo_agent_harness_create_with_store(const turbo_agent_harness_config_t *config,
    turbo_agent_harness_t **out_harness) {
  return turbo_agent_harness_construct(config, 1, NULL, out_harness);
}

void turbo_agent_job_verifier_init(turbo_agent_job_verifier_t *verifier) {
  if (!verifier) return;
  memset(verifier, 0, sizeof(*verifier));
  verifier->struct_size = sizeof(*verifier);
  verifier->abi_version = TURBO_AGENT_JOB_VERIFIER_ABI_VERSION;
}

int turbo_agent_harness_create_verified_with_store(const turbo_agent_harness_config_t *config,
    const turbo_agent_job_verifier_t *verifier, turbo_agent_harness_t **out_harness) {
  if (out_harness) *out_harness = NULL;
  if (!verifier || verifier->struct_size < sizeof(*verifier) ||
      verifier->abi_version != TURBO_AGENT_JOB_VERIFIER_ABI_VERSION ||
      !verifier->source || !verifier->evidence) return SALTS_EINVAL;
  return turbo_agent_harness_construct(config, 1, verifier, out_harness);
}

static int turbo_agent_harness_verification_status(int rc, const char *stage,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  if (rc != SALTS_OK && diagnostic && diagnostic->status == TURBO_AGENT_COMPILE_OK) {
    diagnostic->status = rc == SALTS_ENOMEM ? TURBO_AGENT_COMPILE_OUT_OF_MEMORY : TURBO_AGENT_COMPILE_SOURCE_INVALID;
    (void)snprintf(diagnostic->message, sizeof(diagnostic->message),
        "Host Job %s verification failed (status %d); no fallback or retry", stage, rc);
  }
  return rc;
}

int turbo_agent_harness_verify_job_source(turbo_agent_harness_t *harness,
    const json_value_t *snapshot, turbo_agent_compile_diagnostic_t *diagnostic) {
  int rc = harness->job_verifier.source ?
      harness->job_verifier.source(snapshot, harness->job_verifier.user_data) : SALTS_OK;
  return turbo_agent_harness_verification_status(rc, "source", diagnostic);
}

int turbo_agent_harness_verify_job_evidence(const turbo_agent_job_proof_t *proof,
    const json_value_t *reference, const json_value_t *result,
    turbo_agent_job_acceptance_decision_t decision, const char *evidence_ref,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  const turbo_agent_job_verifier_t *verifier = &proof->harness->job_verifier;
  int rc = verifier->evidence ? verifier->evidence(proof->snapshot, reference, result,
      decision, evidence_ref, verifier->user_data) : SALTS_OK;
  rc = turbo_agent_harness_verification_status(rc, "evidence", diagnostic);
  if (rc == SALTS_OK) rc = turbo_agent_harness_verify_job_source(proof->harness, proof->snapshot, diagnostic);
  return rc;
}

turbo_agent_harness_t *turbo_agent_harness_retain(turbo_agent_harness_t *harness) {
  size_t current;

  if (!harness) {
    return NULL;
  }
  current = atomic_load_explicit(&harness->ref_count, memory_order_relaxed);
  for (;;) {
    if (current == 0 || current == SIZE_MAX) {
      return NULL;
    }
    if (atomic_compare_exchange_weak_explicit(&harness->ref_count, &current, current + 1,
                                              memory_order_relaxed, memory_order_relaxed)) {
      return harness;
    }
  }
}

void turbo_agent_harness_release(turbo_agent_harness_t *harness) {
  if (!harness) {
    return;
  }
  if (atomic_fetch_sub_explicit(&harness->ref_count, 1, memory_order_acq_rel) != 1) {
    return;
  }
  atomic_thread_fence(memory_order_acquire);
  turbo_agent_execution_release(harness->active_execution);
  turbo_agent_app_destroy(harness->app);
  free(harness->compiled_attempt_thread_id);
  cmeta_mutex_destroy(&harness->mutex);
  free(harness);
}

turbo_agent_app_t *turbo_agent_harness_app(const turbo_agent_harness_t *harness) {
  return harness ? harness->app : NULL;
}

int turbo_agent_harness_get_capabilities(const turbo_agent_harness_t *harness,
                                         json_value_t **out_capabilities_json) {
  json_value_t *capabilities = NULL;
  int rc;

  if (!harness || !out_capabilities_json) {
    return SALTS_EINVAL;
  }
  *out_capabilities_json = NULL;
  rc = turbo_agent_app_get_capabilities(harness->app, &capabilities);
  if (rc != SALTS_OK || !capabilities) {
    turbo_runtime_json_destroy(capabilities);
    return rc != SALTS_OK ? rc : SALTS_EIO;
  }
  json_object_set_bool(capabilities, "has_async_execution", 1);
  json_object_set_bool(capabilities, "supports_start", 1);
  json_object_set_bool(capabilities, "supports_resume", 1);
  json_object_set_bool(capabilities, "supports_fork", 1);
  json_object_set_bool(capabilities, "supports_cancel", 1);
  json_object_set_bool(capabilities, "supports_deadline", 1);
  json_object_set_bool(capabilities, "supports_event_sink", 1);
  json_object_set_number(capabilities, "max_concurrent_executions", 1.0);
  json_object_set_number(capabilities, "executor_queue_capacity",
                               (double)cmeta_threadpool_capacity(harness->executor));
  *out_capabilities_json = capabilities;
  return SALTS_OK;
}

int turbo_agent_harness_get_tool_schemas(const turbo_agent_harness_t *harness,
                                         json_value_t **out_tool_schemas_json) {
  return harness ? turbo_agent_app_get_tool_schemas(harness->app, out_tool_schemas_json)
                 : SALTS_EINVAL;
}

int turbo_agent_harness_get_startup_diagnostics(const turbo_agent_harness_t *harness,
                                                json_value_t **out_diagnostics_json) {
  json_value_t *diagnostics = NULL;
  json_value_t *capabilities = NULL;
  int rc;

  if (!harness || !out_diagnostics_json) {
    return SALTS_EINVAL;
  }
  *out_diagnostics_json = NULL;
  rc = turbo_agent_app_get_startup_diagnostics(harness->app, &diagnostics);
  if (rc != SALTS_OK || !diagnostics) {
    turbo_runtime_json_destroy(diagnostics);
    return rc != SALTS_OK ? rc : SALTS_EIO;
  }
  rc = turbo_agent_harness_get_capabilities(harness, &capabilities);
  if (rc != SALTS_OK || !capabilities) {
    turbo_runtime_json_destroy(capabilities);
    turbo_runtime_json_destroy(diagnostics);
    return rc != SALTS_OK ? rc : SALTS_EIO;
  }
  if (turbo_runtime_json_object_set(diagnostics, "harness", capabilities) !=
      TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(capabilities);
    turbo_runtime_json_destroy(diagnostics);
    return SALTS_ENOMEM;
  }
  *out_diagnostics_json = diagnostics;
  return SALTS_OK;
}

turbo_agent_harness_execution_t *
turbo_agent_harness_execution_retain(turbo_agent_harness_execution_t *execution) {
  size_t current;

  if (!execution) {
    return NULL;
  }
  current = atomic_load_explicit(&execution->ref_count, memory_order_relaxed);
  for (;;) {
    if (current == 0 || current == SIZE_MAX) {
      return NULL;
    }
    if (atomic_compare_exchange_weak_explicit(&execution->ref_count, &current, current + 1,
                                              memory_order_relaxed, memory_order_relaxed)) {
      return execution;
    }
  }
}

void turbo_agent_harness_execution_release(turbo_agent_harness_execution_t *execution) {
  if (!execution) {
    return;
  }
  if (atomic_fetch_sub_explicit(&execution->ref_count, 1, memory_order_acq_rel) != 1) {
    return;
  }
  atomic_thread_fence(memory_order_acquire);
  turbo_agent_execution_release(execution->execution);
  turbo_graph_destroy(execution->graph);
  turbo_agent_harness_release(execution->harness);
  free(execution);
}

static void turbo_agent_harness_terminal(void *user_data,
                                         turbo_agent_execution_t *agent_execution) {
  turbo_agent_harness_execution_t *execution = (turbo_agent_harness_execution_t *)user_data;
  turbo_agent_harness_t *harness;
  turbo_agent_execution_t *active = NULL;

  if (!execution || !execution->harness) {
    return;
  }
  harness = execution->harness;
  cmeta_mutex_lock(&harness->mutex);
  if (harness->active_execution == agent_execution) {
    active = harness->active_execution;
    harness->active_execution = NULL;
  } else {
    execution->terminal_before_publish = 1;
  }
  cmeta_mutex_unlock(&harness->mutex);
  turbo_agent_execution_release(active);
  turbo_agent_harness_execution_release(execution);
}

int turbo_agent_harness_begin_submit(turbo_agent_harness_t *harness) {
  int rc = SALTS_OK;
  turbo_agent_execution_t *terminal = NULL;
  turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_QUEUED;

  cmeta_mutex_lock(&harness->mutex);
  if (harness->active_execution &&
      turbo_agent_execution_get_status(harness->active_execution, &status) == SALTS_OK &&
      status >= TURBO_AGENT_EXECUTION_COMPLETED) {
    terminal = harness->active_execution;
    harness->active_execution = NULL;
  }
  if (harness->submitting || harness->active_execution) {
    rc = SALTS_EBUSY;
  } else {
    harness->submitting = 1;
  }
  cmeta_mutex_unlock(&harness->mutex);
  turbo_agent_execution_release(terminal);
  return rc;
}

void turbo_agent_harness_end_submit(turbo_agent_harness_t *harness) {
  cmeta_mutex_lock(&harness->mutex);
  harness->submitting = 0;
  cmeta_mutex_unlock(&harness->mutex);
}

cmeta_threadpool_t *turbo_agent_harness_executor(turbo_agent_harness_t *harness) {
  return harness->executor;
}

static int turbo_agent_harness_compiled_reject(
    turbo_agent_compile_diagnostic_t *diagnostic,
    turbo_agent_compile_status_t status, const char *message) {
  if (diagnostic) {
    diagnostic->status = status;
    snprintf(diagnostic->message, sizeof(diagnostic->message), "%s",
             message ? message : "compiled Inspect admission rejected");
  }
  return SALTS_EPROTO;
}

/* Treat caller compiler capabilities as a restriction, never as authority.
 * The host grant is copied from the attached Agent and intersects that list. */
static int turbo_agent_harness_compiler_authority_configure(
    const turbo_agent_compiler_config_t *requested,
    const turbo_agent_tool_authority_snapshot_t *authority,
    turbo_agent_compiler_config_t *effective,
    const char ***out_capabilities) {
  const char **allowed = NULL;
  size_t count = 0;
  size_t i;
  if (!requested || !authority || !effective || !out_capabilities ||
      (requested->allowed_capability_count && !requested->allowed_capabilities) ||
      !requested->deny_unlisted_capabilities) return SALTS_EINVAL;
  *out_capabilities = NULL;
  allowed = (const char **)calloc(requested->allowed_capability_count ?
                                      requested->allowed_capability_count : 1,
                                  sizeof(*allowed));
  if (!allowed) return SALTS_ENOMEM;
  for (i = 0; i < requested->allowed_capability_count; ++i) {
    turbo_agent_policy_capability_t capability;
    const char *canonical_name;
    size_t prior;
    const char *name = requested->allowed_capabilities[i];
    if (!name || !name[0]) {
      free(allowed);
      return SALTS_EINVAL;
    }
    if (turbo_agent_policy_capability_from_name(name, &capability) != 0 ||
        !(authority->capability_mask &
          TURBO_AGENT_TOOL_AUTHORITY_CAPABILITY_BIT(capability))) continue;
    canonical_name = turbo_agent_policy_capability_name(capability);
    if (!canonical_name) continue;
    for (prior = 0; prior < count; ++prior) {
      if (strcmp(allowed[prior], canonical_name) == 0) break;
    }
    if (prior == count) allowed[count++] = canonical_name;
  }
  *effective = *requested;
  effective->allowed_capabilities = allowed;
  effective->allowed_capability_count = count;
  effective->deny_unlisted_capabilities = 1;
  *out_capabilities = allowed;
  return SALTS_OK;
}

#define TURBO_AGENT_COMPILED_EXECUTION_SCHEMA_VERSION 1
#define TURBO_AGENT_COMPILED_EXECUTION_KEY "compiled_execution"
#define TURBO_AGENT_COMPILED_EXECUTION_JOURNAL_SCHEMA_VERSION 1
#define TURBO_AGENT_COMPILED_EXECUTION_JOURNAL_COLLECTION "agent_compiled_execution_journal"
#define TURBO_AGENT_COMPILED_INSPECT_ATTEMPT_SCHEMA_VERSION 2
#define TURBO_AGENT_COMPILED_INSPECT_ATTEMPT_COLLECTION "compiled_inspect_attempts"

typedef struct turbo_agent_compiled_inspect_run_s {
  turbo_agent_t *agent;
  turbo_agent_runtime_t *runtime;
  turbo_agent_executable_plan_t *plan;
  const turbo_tool_registry_t *approved;
  const turbo_tool_execution_context_t *tool_context;
  turbo_agent_tool_authority_snapshot_t authority;
  const json_value_t *certificate;
  const char *execution_id;
  const char *thread_id;
  const char *tool_name;
  const char *attempt_id;
  const json_value_t *job_snapshot;
  const char *result_schema_json;
  turbo_tool_effect_flags_t effects;
  char attempt_record_id[192];
  char plan_hash[32];
  char created_at[32];
  turbo_tool_status_t tool_status;
  int callback_started;
  int journal_started;
  int committed;
  int replayed;
  int failure_rc;
  int store_status;
  char failure_message[256];
  turbo_agent_compile_diagnostic_t *diagnostic;
  json_value_t *result;
} turbo_agent_compiled_inspect_run_t;

static json_value_t *turbo_agent_compiled_execution_record_state(json_value_t *state) {
  json_value_t *record;
  if (!state || json_type(state) != JSON_OBJECT) return NULL;
  record = json_object_get(state, TURBO_AGENT_COMPILED_EXECUTION_KEY);
  return record && json_type(record) == JSON_OBJECT ? record : NULL;
}

static void turbo_agent_compiled_inspect_fail(
    turbo_agent_compiled_inspect_run_t *run,
    turbo_agent_compile_status_t diagnostic_status,
    turbo_tool_status_t tool_status,
    int failure_rc,
    const char *message) {
  if (!run) return;
  run->tool_status = tool_status;
  run->failure_rc = failure_rc;
  snprintf(run->failure_message, sizeof(run->failure_message), "%s",
           message ? message : "compiled Inspect execution failed");
  if (run->diagnostic) {
    run->diagnostic->status = diagnostic_status;
    snprintf(run->diagnostic->message, sizeof(run->diagnostic->message), "%s",
             run->failure_message);
  }
}

static int turbo_agent_compiled_inspect_set_phase(
    json_value_t *state, const char *phase, turbo_tool_status_t tool_status,
    const char *message) {
  json_value_t *record = turbo_agent_compiled_execution_record_state(state);
  if (!record || !phase ||
      turbo_runtime_json_object_set_string(record, "phase", phase) != TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_int64(record, "tool_status", (int64_t)tool_status) !=
          TURBO_RUNTIME_JSON_OK) return -1;
  if (message &&
      turbo_runtime_json_object_set_string(record, "diagnostic", message) !=
          TURBO_RUNTIME_JSON_OK) return -1;
  return 0;
}

static int turbo_agent_compiled_inspect_key_component_valid(
    const char *value, size_t max_len) {
  size_t i;
  if (!value || !value[0]) return 0;
  for (i = 0; value[i]; ++i) {
    const char ch = value[i];
    if (i >= max_len ||
        !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return 0;
  }
  return i > 0;
}

static int turbo_agent_compiled_inspect_attempt_id_valid(const char *attempt_id) {
  return turbo_agent_compiled_inspect_key_component_valid(attempt_id, 64);
}

/* The attempt record is the sole durable authority for the typed result.
 * Run records below contain only a reference and execution/authority metadata. */
static int turbo_agent_compiled_inspect_attempt_write(
    turbo_agent_compiled_inspect_run_t *run, const char *phase,
    turbo_tool_status_t tool_status, const char *message, const json_value_t *result) {
  json_value_t *existing = NULL;
  json_value_t *record = NULL;
  const char *existing_phase;
  char updated_at[32];
  int read_status;
  int store_status = SALTS_OK;
  int rc = -1;
  int is_start;

  if (!run || !run->runtime || !run->attempt_record_id[0] ||
      !run->attempt_id || !run->thread_id || !run->tool_name ||
      !run->result_schema_json || !run->plan_hash[0] || !run->created_at[0] ||
      !run->certificate || !phase ||
      turbo_runtime_record_timestamp(updated_at, sizeof(updated_at)) != 0) return -1;

  is_start = strcmp(phase, "started") == 0;
  read_status = turbo_agent_runtime_store_get_json_status(
      run->runtime, TURBO_AGENT_COMPILED_INSPECT_ATTEMPT_COLLECTION,
      run->attempt_record_id, &existing);
  if (is_start) {
    if (read_status != SALTS_ENOENT) {
      /* An existing durable claim is a duplicate, not a generic I/O error.
       * Other lookup failures retain their store status where available. */
      run->store_status = read_status == SALTS_OK ? SALTS_EALREADY : read_status;
      goto cleanup;
    }
  } else {
    if (read_status != SALTS_OK || !existing || json_type(existing) != JSON_OBJECT)
      goto cleanup;
    existing_phase = json_get_string(existing, "phase");
    if (!existing_phase || strcmp(existing_phase, "started") != 0 ||
        json_get_int(existing, "schema_version", -1) !=
            TURBO_AGENT_COMPILED_INSPECT_ATTEMPT_SCHEMA_VERSION ||
        strcmp(json_get_string(existing, "id") ? json_get_string(existing, "id") : "",
               run->attempt_record_id) != 0 ||
        strcmp(json_get_string(existing, "thread_id") ? json_get_string(existing, "thread_id") : "",
               run->thread_id) != 0 ||
        strcmp(json_get_string(existing, "attempt_id") ? json_get_string(existing, "attempt_id") : "",
               run->attempt_id) != 0 ||
        strcmp(json_get_string(existing, "plan_hash") ? json_get_string(existing, "plan_hash") : "",
               run->plan_hash) != 0 ||
        strcmp(json_get_string(existing, "tool_name") ? json_get_string(existing, "tool_name") : "",
               run->tool_name) != 0 ||
        strcmp(json_get_string(existing, "result_schema") ?
                   json_get_string(existing, "result_schema") : "",
               run->result_schema_json) != 0 ||
        json_get_int(existing, "effects", INT_MIN) != (int)run->effects ||
        json_get_double(existing, "authority_generation", -1.0) !=
            (double)run->authority.generation) goto cleanup;
  }

  record = json_create_object();
  if (!record ||
      turbo_runtime_json_object_set_int64(record, "schema_version",
          TURBO_AGENT_COMPILED_INSPECT_ATTEMPT_SCHEMA_VERSION) != TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "id", run->attempt_record_id) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "thread_id", run->thread_id) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "attempt_id", run->attempt_id) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "execution_id", run->execution_id) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "plan_hash", run->plan_hash) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "tool_name", run->tool_name) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "result_schema", run->result_schema_json) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_int64(record, "effects", (int64_t)run->effects) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_int64(record, "authority_generation",
          (int64_t)run->authority.generation) != TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "backend", "runtime_tools") !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "execution_mode", "compiled_inspect") !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "approval_decision",
          "not_required_read_only_inspect") != TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "phase", phase) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_int64(record, "tool_status", (int64_t)tool_status) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "created_at", run->created_at) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "updated_at", updated_at) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_clone(record, "certificate", run->certificate) !=
          TURBO_RUNTIME_JSON_OK ||
      (message && turbo_runtime_json_object_set_string(record, "error", message) !=
          TURBO_RUNTIME_JSON_OK) ||
      (result && turbo_runtime_json_object_set_clone(record, "result", result) !=
          TURBO_RUNTIME_JSON_OK)) goto cleanup;

  if (is_start) {
    /* The read above validates the common path, but it is not the claim.
     * Only the backend's conditional insert can arbitrate competing Harnesses.
     * Unsupported backends fail closed; never emulate this with get + put. */
    store_status = turbo_agent_runtime_store_create_json_if_absent(
        run->runtime, TURBO_AGENT_COMPILED_INSPECT_ATTEMPT_COLLECTION,
        run->attempt_record_id, record);
    run->store_status = store_status;
    /* Preserve the Runtime Store status for the caller's failure diagnostic;
     * every non-success remains fail-closed and prevents callback dispatch. */
    rc = store_status == SALTS_OK ? 0 : store_status;
  } else {
    rc = turbo_agent_runtime_store_put_json(
        run->runtime, TURBO_AGENT_COMPILED_INSPECT_ATTEMPT_COLLECTION,
        run->attempt_record_id, record) == SALTS_OK ? 0 : -1;
  }

cleanup:
  turbo_runtime_json_destroy(record);
  turbo_runtime_json_destroy(existing);
  return rc;
}

/* Per-Run journal rows deliberately omit the typed result. They point to the
 * stable attempt identity and can be recreated on a retry after Session failure. */
static int turbo_agent_compiled_inspect_run_journal_write(
    turbo_agent_compiled_inspect_run_t *run, const char *phase,
    turbo_tool_status_t tool_status, const char *message) {
  json_value_t *existing = NULL;
  json_value_t *record = NULL;
  const char *existing_phase;
  char updated_at[32];
  int read_status;
  int rc = -1;
  int is_start;

  if (!run || !run->runtime || !run->execution_id || !run->thread_id ||
      !run->attempt_id || !run->attempt_record_id[0] || !run->tool_name ||
      !run->plan_hash[0] || !run->created_at[0] || !run->certificate || !phase ||
      turbo_runtime_record_timestamp(updated_at, sizeof(updated_at)) != 0) return -1;

  is_start = strcmp(phase, "started") == 0;
  read_status = turbo_agent_runtime_store_get_json_status(
      run->runtime, TURBO_AGENT_COMPILED_EXECUTION_JOURNAL_COLLECTION,
      run->execution_id, &existing);
  if (is_start) {
    if (read_status != SALTS_ENOENT) goto cleanup;
  } else {
    if (read_status != SALTS_OK || !existing || json_type(existing) != JSON_OBJECT)
      goto cleanup;
    existing_phase = json_get_string(existing, "phase");
    if (!existing_phase || strcmp(existing_phase, "started") != 0 ||
        json_get_int(existing, "schema_version", -1) !=
            TURBO_AGENT_COMPILED_EXECUTION_JOURNAL_SCHEMA_VERSION ||
        strcmp(json_get_string(existing, "execution_id") ?
                   json_get_string(existing, "execution_id") : "", run->execution_id) != 0 ||
        strcmp(json_get_string(existing, "thread_id") ?
                   json_get_string(existing, "thread_id") : "", run->thread_id) != 0 ||
        strcmp(json_get_string(existing, "attempt_id") ?
                   json_get_string(existing, "attempt_id") : "", run->attempt_id) != 0 ||
        strcmp(json_get_string(existing, "attempt_record_id") ?
                   json_get_string(existing, "attempt_record_id") : "",
               run->attempt_record_id) != 0 ||
        strcmp(json_get_string(existing, "plan_hash") ?
                   json_get_string(existing, "plan_hash") : "", run->plan_hash) != 0 ||
        json_get_double(existing, "authority_generation", -1.0) !=
            (double)run->authority.generation) goto cleanup;
  }

  record = json_create_object();
  if (!record ||
      turbo_runtime_json_object_set_int64(record, "schema_version",
          TURBO_AGENT_COMPILED_EXECUTION_JOURNAL_SCHEMA_VERSION) != TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "execution_id", run->execution_id) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "thread_id", run->thread_id) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "attempt_id", run->attempt_id) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "attempt_record_id", run->attempt_record_id) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "tool_name", run->tool_name) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "plan_hash", run->plan_hash) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_int64(record, "authority_generation",
          (int64_t)run->authority.generation) != TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "backend", "runtime_tools") !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "execution_mode", "compiled_inspect") !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "approval_decision",
          "not_required_read_only_inspect") != TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "phase", phase) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_int64(record, "tool_status", (int64_t)tool_status) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "created_at", run->created_at) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "updated_at", updated_at) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_clone(record, "certificate", run->certificate) !=
          TURBO_RUNTIME_JSON_OK ||
      (message && turbo_runtime_json_object_set_string(record, "diagnostic", message) !=
          TURBO_RUNTIME_JSON_OK)) goto cleanup;

  rc = turbo_agent_runtime_store_put_json(
      run->runtime, TURBO_AGENT_COMPILED_EXECUTION_JOURNAL_COLLECTION,
      run->execution_id, record) == SALTS_OK ? 0 : -1;

cleanup:
  turbo_runtime_json_destroy(record);
  turbo_runtime_json_destroy(existing);
  return rc;
}

/* If terminal journal persistence fails, the prior durable "started" marker
 * remains the recovery signal. Never reinterpret that uncertainty as success. */
static void turbo_agent_compiled_inspect_mark_unknown(
    turbo_agent_compiled_inspect_run_t *run, const char *message) {
  char original[256];
  if (!run || !run->journal_started) return;
  snprintf(original, sizeof(original), "%s",
           message ? message : run->failure_message);
  if (turbo_agent_compiled_inspect_attempt_write(
          run, "outcome_unknown", run->tool_status, original, NULL) != 0 ||
      turbo_agent_compiled_inspect_run_journal_write(
          run, "outcome_unknown", run->tool_status, original) != 0) {
    snprintf(run->failure_message, sizeof(run->failure_message),
             "%s; journal update failed, leaving the prior durable phase as-is",
             original);
    if (run->diagnostic) {
      snprintf(run->diagnostic->message, sizeof(run->diagnostic->message), "%s",
               run->failure_message);
    }
  }
}

static int turbo_agent_compiled_inspect_admit_node(
    turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_agent_compiled_inspect_run_t *run = (turbo_agent_compiled_inspect_run_t *)user_data;
  if (!ctx || !ctx->json_value_state || !run) return -1;
  if (turbo_agent_tool_authority_snapshot_is_current(run->agent, &run->authority) != 1) {
    turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_CAPABILITY_DENIED,
        TURBO_TOOL_ERROR, SALTS_EPROTO,
        "Host Tool Authority changed before the compiled Run began");
    (void)turbo_agent_compiled_inspect_set_phase(
        ctx->json_value_state, "rejected", run->tool_status, run->failure_message);
    return -1;
  }
  if (turbo_agent_compiled_execution_record_state(ctx->json_value_state) == NULL) {
    turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_SOURCE_INVALID,
        TURBO_TOOL_ERROR, SALTS_EPROTO, "compiled execution record is missing from Run state");
    return -1;
  }
  return 0;
}

static int turbo_agent_compiled_inspect_start_node(
    turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_agent_compiled_inspect_run_t *run = (turbo_agent_compiled_inspect_run_t *)user_data;
  int cancel_status;
  if (!ctx || !ctx->json_value_state || !run) return -1;
  if (turbo_agent_tool_authority_snapshot_is_current(run->agent, &run->authority) != 1) {
    turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_CAPABILITY_DENIED,
        TURBO_TOOL_ERROR, SALTS_EPROTO,
        "Host Tool Authority was revoked before the compiled tool callback");
    (void)turbo_agent_compiled_inspect_set_phase(
        ctx->json_value_state, "rejected", run->tool_status, run->failure_message);
    return -1;
  }
  cancel_status = run->tool_context && run->tool_context->cancel_token
      ? turbo_cancel_token_check(run->tool_context->cancel_token) : SALTS_OK;
  if (cancel_status != SALTS_OK) {
    run->tool_status = cancel_status == SALTS_ETIMEDOUT
        ? TURBO_TOOL_DEADLINE_EXCEEDED : TURBO_TOOL_CANCELLED;
    run->failure_rc = run->job_snapshot ? cancel_status : SALTS_EIO;
    snprintf(run->failure_message, sizeof(run->failure_message), "%s",
             "compiled Inspect was cancelled before callback dispatch");
    if (run->diagnostic) {
      snprintf(run->diagnostic->message, sizeof(run->diagnostic->message), "%s",
               run->failure_message);
    }
    (void)turbo_agent_compiled_inspect_set_phase(
        ctx->json_value_state, "cancelled", run->tool_status, run->failure_message);
    return -1;
  }
  if (turbo_agent_compiled_inspect_set_phase(
          ctx->json_value_state, "started", TURBO_TOOL_OK, NULL) != 0) {
    turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
        TURBO_TOOL_ERROR, SALTS_ENOMEM, "could not persist compiled execution start state");
    return -1;
  }
  return 0;
}

static int turbo_agent_compiled_inspect_execute_node(
    turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_agent_compiled_inspect_run_t *run = (turbo_agent_compiled_inspect_run_t *)user_data;
  json_value_t *result = NULL;
  json_value_t *record;
  turbo_tool_status_t status;
  int cancel_status;
  int attempt_read_status;
  int64_t saved_status;
  const char *saved_phase;
  const json_value_t *saved_result;
  json_value_t *stored_attempt = NULL;
  if (!ctx || !ctx->json_value_state || !run) return -1;
  record = turbo_agent_compiled_execution_record_state(ctx->json_value_state);
  if (!record || !json_get_string(record, "phase") ||
      strcmp(json_get_string(record, "phase"), "started") != 0) {
    turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_SOURCE_INVALID,
        TURBO_TOOL_ERROR, SALTS_EPROTO, "compiled execution did not reach its in-memory started phase");
    return -1;
  }
  if (turbo_agent_tool_authority_snapshot_is_current(run->agent, &run->authority) != 1) {
    turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_CAPABILITY_DENIED,
        TURBO_TOOL_ERROR, SALTS_EPROTO,
        "Host Tool Authority changed at the final pre-callback gate");
    (void)turbo_agent_compiled_inspect_set_phase(
        ctx->json_value_state, "rejected", run->tool_status, run->failure_message);
    return -1;
  }
  cancel_status = run->tool_context && run->tool_context->cancel_token
      ? turbo_cancel_token_check(run->tool_context->cancel_token) : SALTS_OK;
  if (cancel_status != SALTS_OK) {
    run->tool_status = cancel_status == SALTS_ETIMEDOUT
        ? TURBO_TOOL_DEADLINE_EXCEEDED : TURBO_TOOL_CANCELLED;
    run->failure_rc = run->job_snapshot ? cancel_status : SALTS_EIO;
    snprintf(run->failure_message, sizeof(run->failure_message), "%s",
             "compiled Inspect was cancelled before journaled callback dispatch");
    if (run->diagnostic) {
      snprintf(run->diagnostic->message, sizeof(run->diagnostic->message), "%s",
               run->failure_message);
    }
    (void)turbo_agent_compiled_inspect_set_phase(
        ctx->json_value_state, "cancelled", run->tool_status, run->failure_message);
    return -1;
  }

  /* Look up the stable attempt only after a fresh authority check. A COMMITTED
   * attempt is replayed through this Run's Session finalization; STARTED and
   * outcome_unknown never redispatch a callback. */
  if (run->job_snapshot) {
    int active = turbo_agent_harness_require_job_active(run->runtime,
        run->job_snapshot, run->diagnostic);
    if (active != SALTS_OK) {
      run->tool_status = active == SALTS_ECANCELED ? TURBO_TOOL_CANCELLED : TURBO_TOOL_ERROR;
      run->failure_rc = active;
      return -1;
    }
  }
  attempt_read_status = turbo_agent_runtime_store_get_json_status(
      run->runtime, TURBO_AGENT_COMPILED_INSPECT_ATTEMPT_COLLECTION,
      run->attempt_record_id, &stored_attempt);
  if (attempt_read_status == SALTS_OK) {
    if (!stored_attempt || json_type(stored_attempt) != JSON_OBJECT ||
        json_get_int(stored_attempt, "schema_version", -1) !=
            TURBO_AGENT_COMPILED_INSPECT_ATTEMPT_SCHEMA_VERSION ||
        !json_get_string(stored_attempt, "phase") ||
        strcmp(json_get_string(stored_attempt, "id") ?
                   json_get_string(stored_attempt, "id") : "", run->attempt_record_id) != 0 ||
        strcmp(json_get_string(stored_attempt, "thread_id") ?
                   json_get_string(stored_attempt, "thread_id") : "", run->thread_id) != 0 ||
        strcmp(json_get_string(stored_attempt, "attempt_id") ?
                   json_get_string(stored_attempt, "attempt_id") : "", run->attempt_id) != 0 ||
        strcmp(json_get_string(stored_attempt, "plan_hash") ?
                   json_get_string(stored_attempt, "plan_hash") : "", run->plan_hash) != 0 ||
        strcmp(json_get_string(stored_attempt, "tool_name") ?
                   json_get_string(stored_attempt, "tool_name") : "", run->tool_name) != 0 ||
        strcmp(json_get_string(stored_attempt, "result_schema") ?
                   json_get_string(stored_attempt, "result_schema") : "",
               run->result_schema_json) != 0 ||
        json_get_int(stored_attempt, "effects", INT_MIN) != (int)run->effects ||
        json_get_double(stored_attempt, "authority_generation", -1.0) !=
            (double)run->authority.generation) {
      turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_SOURCE_INVALID,
          TURBO_TOOL_ERROR, SALTS_EPROTO,
          "durable Inspect attempt identity or contract does not match the admitted Run");
      (void)turbo_agent_compiled_inspect_set_phase(
          ctx->json_value_state, "rejected", run->tool_status, run->failure_message);
      turbo_runtime_json_destroy(stored_attempt);
      return -1;
    }
    if (turbo_agent_tool_authority_snapshot_is_current(run->agent, &run->authority) != 1) {
      turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_CAPABILITY_DENIED,
          TURBO_TOOL_ERROR, SALTS_EPROTO,
          "Host Tool Authority changed before durable attempt replay");
      (void)turbo_agent_compiled_inspect_set_phase(
          ctx->json_value_state, "rejected", run->tool_status, run->failure_message);
      turbo_runtime_json_destroy(stored_attempt);
      return -1;
    }
    saved_phase = json_get_string(stored_attempt, "phase");
    saved_status = turbo_runtime_json_value_as_int64(
        json_object_get(stored_attempt, "tool_status"), INT64_MIN);
    saved_result = json_object_get(stored_attempt, "result");
    if (strcmp(saved_phase, "committed") == 0) {
      if (saved_status != TURBO_TOOL_OK || !saved_result ||
          turbo_tool_schema_validate_result_json_value(
              run->approved, run->tool_name, saved_result, NULL, 0) !=
              TURBO_TOOL_SCHEMA_VALID ||
          !(run->result = json_clone(saved_result))) {
        turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_TOOL_SCHEMA_INVALID,
            TURBO_TOOL_ERROR, SALTS_EPROTO,
            "saved durable Inspect result failed the current result contract");
        (void)turbo_agent_compiled_inspect_set_phase(
            ctx->json_value_state, "rejected", run->tool_status, run->failure_message);
        turbo_runtime_json_destroy(stored_attempt);
        return -1;
      }
      if (turbo_agent_compiled_inspect_set_phase(
              ctx->json_value_state, "committed", TURBO_TOOL_OK, NULL) != 0 ||
          turbo_agent_compiled_inspect_run_journal_write(
              run, "started", TURBO_TOOL_OK, NULL) != 0) {
        turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
            TURBO_TOOL_ERROR, SALTS_EIO,
            "could not restore Run journal metadata for committed attempt replay");
        turbo_runtime_json_destroy(stored_attempt);
        return -1;
      }
      run->journal_started = 1;
      run->committed = 1;
      run->replayed = 1;
      /* Report the status persisted with the canonical attempt result, rather
       * than leaking the ERROR default used before this Run reaches a result. */
      run->tool_status = (turbo_tool_status_t)saved_status;
      run->failure_rc = SALTS_OK;
      if (turbo_agent_compiled_inspect_run_journal_write(
              run, "committed", TURBO_TOOL_OK, NULL) != 0) {
        turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_OK,
            TURBO_TOOL_ERROR, SALTS_EIO,
            "committed attempt is retained, but this Run's journal reference could not be committed");
        turbo_runtime_json_destroy(stored_attempt);
        return -1;
      }
      turbo_runtime_json_destroy(stored_attempt);
      return 0;
    }
    if (strcmp(saved_phase, "started") == 0 ||
        strcmp(saved_phase, "outcome_unknown") == 0) {
      turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_OK,
          TURBO_TOOL_UNKNOWN_SIDE_EFFECT, SALTS_EBUSY,
          "durable Inspect attempt has an uncertain outcome; automatic redispatch is forbidden");
      (void)turbo_agent_compiled_inspect_set_phase(
          ctx->json_value_state, "outcome_unknown", run->tool_status, run->failure_message);
      if (turbo_agent_compiled_inspect_run_journal_write(
              run, "started", run->tool_status, NULL) == 0) {
        run->journal_started = 1;
        (void)turbo_agent_compiled_inspect_run_journal_write(
            run, "outcome_unknown", run->tool_status, run->failure_message);
      }
      turbo_runtime_json_destroy(stored_attempt);
      return -1;
    }
    turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_SOURCE_INVALID,
        TURBO_TOOL_ERROR, SALTS_EPROTO, "durable Inspect attempt has an unknown phase");
    (void)turbo_agent_compiled_inspect_set_phase(
        ctx->json_value_state, "rejected", run->tool_status, run->failure_message);
    turbo_runtime_json_destroy(stored_attempt);
    return -1;
  }
  if (attempt_read_status != SALTS_ENOENT) {
    turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_SOURCE_INVALID,
        TURBO_TOOL_ERROR, attempt_read_status,
        "durable Inspect attempt lookup failed; storage errors are not treated as absence");
    (void)turbo_agent_compiled_inspect_set_phase(
        ctx->json_value_state, "journal_failed", run->tool_status, run->failure_message);
    turbo_runtime_json_destroy(stored_attempt);
    return -1;
  }

  /* Persist the authoritative STARTED attempt first, then the metadata-only
   * Run journal reference. Neither failure may permit callback dispatch. */
  if (turbo_agent_compiled_inspect_attempt_write(
          run, "started", TURBO_TOOL_UNKNOWN_SIDE_EFFECT, NULL, NULL) != 0) {
    const char *claim_message = run->store_status == SALTS_EALREADY
        ? "durable Inspect attempt already exists; callback was not invoked"
        : run->store_status == SALTS_ENOTSUP
            ? "Runtime Store does not support atomic Inspect attempt claims; callback was not invoked"
            : "could not persist durable Inspect STARTED attempt; callback was not invoked";
    turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_OK,
        TURBO_TOOL_ERROR,
        run->store_status != SALTS_OK ? run->store_status : SALTS_EIO,
        claim_message);
    (void)turbo_agent_compiled_inspect_set_phase(
        ctx->json_value_state, "journal_failed", run->tool_status, run->failure_message);
    return -1;
  }
  run->journal_started = 1;
  if (turbo_agent_compiled_inspect_run_journal_write(
          run, "started", TURBO_TOOL_UNKNOWN_SIDE_EFFECT, NULL) != 0) {
    turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_OK,
        TURBO_TOOL_ERROR, SALTS_EIO,
        "attempt is durably STARTED but Run journal reference failed; callback was not invoked");
    (void)turbo_agent_compiled_inspect_set_phase(
        ctx->json_value_state, "journal_failed", run->tool_status, run->failure_message);
    return -1;
  }

  run->callback_started = 1;
  status = turbo_agent_execute_compiled_plan(run->plan, run->tool_context, &result);
  run->tool_status = status;
  if (status != TURBO_TOOL_OK) {
    turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_OK,
        status, SALTS_EIO, "compiled tool callback failed; no automatic retry was attempted");
    (void)turbo_agent_compiled_inspect_set_phase(
        ctx->json_value_state, "outcome_unknown", run->tool_status, run->failure_message);
    turbo_agent_compiled_inspect_mark_unknown(run, run->failure_message);
    turbo_runtime_json_destroy(result);
    return -1;
  }
  if (!result || turbo_tool_schema_validate_result_json_value(
          run->approved, run->tool_name, result, NULL, 0) != TURBO_TOOL_SCHEMA_VALID) {
    turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_TOOL_SCHEMA_INVALID,
        TURBO_TOOL_ERROR, SALTS_EPROTO,
        "compiled tool returned a result violating its admitted schema; no retry was attempted");
    (void)turbo_agent_compiled_inspect_set_phase(
        ctx->json_value_state, "outcome_unknown", run->tool_status, run->failure_message);
    turbo_agent_compiled_inspect_mark_unknown(run, run->failure_message);
    turbo_runtime_json_destroy(result);
    return -1;
  }
  if (turbo_agent_tool_authority_snapshot_is_current(run->agent, &run->authority) != 1) {
    turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_CAPABILITY_DENIED,
        TURBO_TOOL_ERROR, SALTS_EPROTO,
        "Host Tool Authority changed during callback; result was not committed");
    (void)turbo_agent_compiled_inspect_set_phase(
        ctx->json_value_state, "outcome_unknown", run->tool_status, run->failure_message);
    turbo_agent_compiled_inspect_mark_unknown(run, run->failure_message);
    turbo_runtime_json_destroy(result);
    return -1;
  }

  /* The result payload is written only to compiled_inspect_attempts. The Run
   * checkpoint retains phase/identity metadata and references that attempt. */
  if (turbo_agent_compiled_inspect_set_phase(
          ctx->json_value_state, "committed", TURBO_TOOL_OK, NULL) != 0) {
    turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
        TURBO_TOOL_ERROR, SALTS_ENOMEM,
        "could not construct the committed compiled Run state after callback");
    (void)turbo_agent_compiled_inspect_set_phase(
        ctx->json_value_state, "outcome_unknown", run->tool_status, run->failure_message);
    turbo_agent_compiled_inspect_mark_unknown(run, run->failure_message);
    turbo_runtime_json_destroy(result);
    return -1;
  }
  if (turbo_agent_compiled_inspect_attempt_write(
          run, "committed", TURBO_TOOL_OK, NULL, result) != 0) {
    turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_OK,
        TURBO_TOOL_ERROR, SALTS_EIO,
        "tool returned a valid result but durable attempt commit failed; result delivery was withheld and no retry was attempted");
    (void)turbo_agent_compiled_inspect_set_phase(
        ctx->json_value_state, "outcome_unknown", run->tool_status, run->failure_message);
    turbo_agent_compiled_inspect_mark_unknown(run, run->failure_message);
    turbo_runtime_json_destroy(result);
    return -1;
  }

  /* Once this point is crossed, a retry must reuse the saved result even if
   * Run journal or Session finalization fails. */
  run->committed = 1;
  if (turbo_agent_compiled_inspect_run_journal_write(
          run, "committed", TURBO_TOOL_OK, NULL) != 0) {
    turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_OK,
        TURBO_TOOL_ERROR, SALTS_EIO,
        "attempt result is committed but Run journal finalization failed; retry with the same attempt ID");
    turbo_runtime_json_destroy(result);
    return -1;
  }
  run->result = result;
  run->failure_rc = SALTS_OK;
  run->failure_message[0] = '\0';
  return 0;
}

static json_value_t *turbo_agent_compiled_inspect_initial_state(
    const char *execution_id, const char *thread_id, const char *attempt_id,
    const char *attempt_record_id, const char *tool_name, const char *plan_hash,
    uint64_t authority_generation, const json_value_t *certificate) {
  json_value_t *state = turbo_agent_state_create();
  json_value_t *record = json_create_object();
  if (!state || !record ||
      turbo_runtime_json_object_set_int64(record, "schema_version",
          TURBO_AGENT_COMPILED_EXECUTION_SCHEMA_VERSION) != TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "execution_id", execution_id) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "thread_id", thread_id) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "attempt_id", attempt_id) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "attempt_record_id", attempt_record_id) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "tool_name", tool_name) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "plan_hash", plan_hash) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_int64(record, "authority_generation",
          (int64_t)authority_generation) != TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "backend", "runtime_tools") !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "execution_mode", "compiled_inspect") !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "approval_decision",
          "not_required_read_only_inspect") != TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "phase", "admitted") !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_int64(record, "tool_status", TURBO_TOOL_ERROR) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_clone(record, "certificate", certificate) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set(state, TURBO_AGENT_COMPILED_EXECUTION_KEY, record) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(record);
    turbo_runtime_json_destroy(state);
    return NULL;
  }
  return state;
}

static void turbo_agent_harness_admission_destroy(turbo_agent_inspect_admission_t *admission) {
  json_free(admission->certificate);
  turbo_agent_executable_plan_destroy(admission->plan);
  memset(admission, 0, sizeof(*admission));
}

static int turbo_agent_harness_compile_inspect(turbo_agent_harness_t *harness,
    const turbo_agent_compiler_config_t *config, const char *source, size_t size,
    const turbo_agent_tool_authority_snapshot_t *authority,
    turbo_agent_inspect_admission_t *out, turbo_agent_compile_diagnostic_t *diagnostic) {
  turbo_agent_compiler_config_t effective;
  const char **capabilities = NULL;
  const turbo_tool_registry_t *registry;
  const json_value_t *schema = NULL;
  int strict = 0;
  int rc;
  memset(out, 0, sizeof(*out));
  if (!config || config->struct_size < sizeof(*config) ||
      config->abi_version != TURBO_AGENT_COMPILER_CONFIG_ABI_VERSION ||
      !config->deny_unlisted_capabilities ||
      (config->allowed_capability_count && !config->allowed_capabilities)) return SALTS_EINVAL;
  rc = turbo_agent_harness_compiler_authority_configure(config, authority, &effective, &capabilities);
  if (rc != SALTS_OK) return rc;
  registry = turbo_agent_app_tool_registry(harness->app);
  if (!registry) {
    rc = turbo_agent_harness_compiled_reject(diagnostic, TURBO_AGENT_COMPILE_UNRESOLVED_TOOL,
        "Harness has no configured RuntimeTools registry");
    goto done;
  }
  if (turbo_agent_compile_plan_json(&effective, registry, source, size, &out->plan, diagnostic) !=
          TURBO_AGENT_COMPILE_OK || !out->plan) { rc = SALTS_EPROTO; goto done; }
  out->approved = turbo_agent_executable_plan_approved_tools(out->plan);
  out->name = turbo_agent_executable_plan_tool_name(out->plan);
  if (!out->approved || !out->name ||
      turbo_tool_registry_get_effects(out->approved, out->name, &out->effects) != TURBO_TOOL_OK ||
      !(out->effects == TURBO_TOOL_EFFECT_PURE || ((out->effects & TURBO_TOOL_EFFECT_READ) &&
        (out->effects & ~(TURBO_TOOL_EFFECT_READ | TURBO_TOOL_EFFECT_NETWORK)) == 0))) {
    rc = turbo_agent_harness_compiled_reject(diagnostic, TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION,
        "Inspect requires declared PURE or READ(+NETWORK) effects");
    goto done;
  }
  if (turbo_tool_registry_get_result_contract(out->approved, out->name, &out->result_schema,
          &schema, &strict) != TURBO_TOOL_OK || !out->result_schema || !schema || !strict) {
    rc = turbo_agent_harness_compiled_reject(diagnostic, TURBO_AGENT_COMPILE_TOOL_SCHEMA_INVALID,
        "compiled Inspect requires a declared strict result schema");
    goto done;
  }
  out->certificate = turbo_agent_executable_plan_certificate_json_value(out->plan);
  if (!out->certificate) { rc = SALTS_ENOMEM; goto done; }
  rc = SALTS_OK;
done:
  free((void *)capabilities);
  if (rc != SALTS_OK) turbo_agent_harness_admission_destroy(out);
  return rc;
}

static int turbo_agent_harness_require_scope(turbo_agent_harness_t *harness,
    turbo_agent_session_t *session, turbo_agent_compile_diagnostic_t *out_diagnostic) {
  int rc;
  if (!harness->compiled_attempt_thread_id && session) {
    const char *current_thread_id = turbo_agent_session_thread_id(session);
    if (current_thread_id && current_thread_id[0]) {
      const size_t thread_id_size = strlen(current_thread_id) + 1;
      char *thread_id_copy = (char *)malloc(thread_id_size);
      if (!thread_id_copy) {
        rc = SALTS_ENOMEM;
        return rc;
      }
      memcpy(thread_id_copy, current_thread_id, thread_id_size);
      /* Harness serializes compiled Inspect against Session operations. */
      harness->compiled_attempt_thread_id = thread_id_copy;
    }
  }
  if (!harness->compiled_attempt_thread_id || !harness->compiled_attempt_thread_id[0]) {
    rc = turbo_agent_harness_compiled_reject(
        out_diagnostic, TURBO_AGENT_COMPILE_SOURCE_INVALID,
        "durable compiled Inspect requires a stable Session thread ID; configure one or complete a Session Run first");
    return rc;
  }
  return SALTS_OK;
}

/* Job metadata selects an existing execution contract; it never grants one. */
static int turbo_agent_harness_job_snapshot(turbo_agent_runtime_t *runtime,
    const char *job_id, uint32_t expected, const char *thread_id,
    json_value_t **out, turbo_agent_compile_diagnostic_t *diagnostic) {
  turbo_agent_job_t *job = NULL;
  json_value_t *snapshot = NULL;
  const json_value_t *binding;
  int rc = turbo_agent_job_open(runtime, job_id, &job);
  if (rc == SALTS_OK) rc = turbo_agent_job_get_snapshot(job, &snapshot);
  turbo_agent_job_destroy(job);
  if (rc != SALTS_OK) {
    (void)turbo_agent_harness_compiled_reject(diagnostic,
        TURBO_AGENT_COMPILE_SOURCE_INVALID, "could not read the complete persisted Job binding");
    return rc;
  }
  binding = json_object_get(snapshot, "attempt");
  if (strcmp(json_get_string(snapshot, "state"), "attempt_bound") ||
      turbo_runtime_json_value_as_int64(json_object_get(snapshot, "revision"), -1) != expected) {
    (void)turbo_agent_harness_compiled_reject(diagnostic,
        TURBO_AGENT_COMPILE_SOURCE_INVALID, "Job is not bound at the expected revision");
    rc = SALTS_EBUSY;
  } else if (!binding || strcmp(json_get_string(binding, "thread_id"), thread_id)) {
    rc = turbo_agent_harness_compiled_reject(diagnostic,
        TURBO_AGENT_COMPILE_SOURCE_INVALID, "Job attempt scope does not match this Harness thread");
  }
  if (rc != SALTS_OK) { json_free(snapshot); return rc; }
  *out = snapshot;
  return SALTS_OK;
}

void turbo_agent_harness_job_proof_destroy(turbo_agent_job_proof_t *proof) {
  turbo_agent_harness_admission_destroy(&proof->admission);
  json_free(proof->snapshot);
  memset(proof, 0, sizeof(*proof));
}

int turbo_agent_harness_require_job_active(turbo_agent_runtime_t *runtime,
    const json_value_t *snapshot, turbo_agent_compile_diagnostic_t *diagnostic) {
  int rc = turbo_agent_job_check_active(runtime, json_get_string(snapshot, "id"),
      (uint32_t)turbo_runtime_json_value_as_int64(json_object_get(snapshot, "revision"), -1));
  if (rc != SALTS_OK && diagnostic) {
    diagnostic->status = TURBO_AGENT_COMPILE_SOURCE_INVALID;
    (void)snprintf(diagnostic->message, sizeof(diagnostic->message),
        "Job progress fenced by cancellation or unreadable control evidence (status %d)", rc);
  }
  return rc;
}

int turbo_agent_harness_prepare_job_proof(turbo_agent_harness_t *harness,
    const char *id, uint32_t expected, int require_active, const turbo_agent_compiler_config_t *config,
    turbo_agent_compile_diagnostic_t *diagnostic, turbo_agent_job_proof_t *out) {
  turbo_agent_session_t *session = turbo_agent_app_session(harness->app);
  const json_value_t *proposal;
  int rc;
  memset(out, 0, sizeof(*out));
  out->harness = harness;
  out->runtime = turbo_agent_session_runtime(session);
  out->agent = turbo_agent_session_agent(session);
  rc = turbo_agent_harness_require_scope(harness, session, diagnostic);
  if (rc != SALTS_OK) goto fail;
  if (!out->agent || !out->runtime ||
      turbo_agent_tool_authority_snapshot_get(out->agent, &out->authority)) {
    rc = turbo_agent_harness_compiled_reject(diagnostic, TURBO_AGENT_COMPILE_CAPABILITY_DENIED,
        "Harness has no readable Host Tool Authority or Runtime snapshot");
    goto fail;
  }
  rc = turbo_agent_harness_job_snapshot(out->runtime, id, expected,
      harness->compiled_attempt_thread_id, &out->snapshot, diagnostic);
  if (rc != SALTS_OK) goto fail;
  if (require_active) {
    rc = turbo_agent_harness_require_job_active(out->runtime, out->snapshot, diagnostic);
    if (rc != SALTS_OK) goto fail;
  }
  proposal = json_object_get(out->snapshot, "proposal");
  rc = turbo_agent_harness_verify_job_source(harness, out->snapshot, diagnostic);
  if (rc != SALTS_OK) goto fail;
  rc = turbo_agent_harness_compile_inspect(harness, config,
      json_get_string(proposal, "source_json"),
      json_string_len(json_object_get(proposal, "source_json")), &out->authority,
      &out->admission, diagnostic);
  if (rc != SALTS_OK) goto fail;
  if (snprintf(out->plan_hash, sizeof(out->plan_hash), "%016" PRIx64,
          (uint64_t)turbo_agent_executable_plan_hash(out->admission.plan)) < 0) {
    rc = SALTS_EIO;
    goto fail;
  }
  return SALTS_OK;
fail:
  turbo_agent_harness_job_proof_destroy(out);
  return rc;
}

static json_value_t *turbo_agent_harness_job_reference(
    const json_value_t *snapshot, const json_value_t *summary,
    const turbo_agent_compiled_inspect_run_t *run) {
  json_value_t *reference = json_create_object();
  const json_value_t *proposal = json_object_get(snapshot, "proposal");
  if (!reference ||
      turbo_runtime_json_object_set_int64(reference, "schema_version", 1) ||
      turbo_runtime_json_object_set_string(reference, "kind", "compiled_inspect") ||
      turbo_runtime_json_object_set_string(reference, "job_id", json_get_string(snapshot, "id")) ||
      turbo_runtime_json_object_set_int64(reference, "job_revision", json_get_int(snapshot, "revision", -1)) ||
      turbo_runtime_json_object_set_int64(reference, "proposal_version", json_get_int(proposal, "revision", -1)) ||
      turbo_runtime_json_object_set_string(reference, "thread_id", run->thread_id) ||
      turbo_runtime_json_object_set_string(reference, "attempt_id", run->attempt_id) ||
      turbo_runtime_json_object_set_string(reference, "attempt_record_id", run->attempt_record_id) ||
      turbo_runtime_json_object_set_string(reference, "run_id", json_get_string(summary, "run_id")) ||
      turbo_runtime_json_object_set_string(reference, "run_thread_id", json_get_string(summary, "thread_id")) ||
      turbo_runtime_json_object_set_string(reference, "execution_id", run->execution_id) ||
      turbo_runtime_json_object_set_string(reference, "plan_hash", run->plan_hash) ||
      turbo_runtime_json_object_set_int64(reference, "authority_generation", (int64_t)run->authority.generation)) {
    json_free(reference);
    return NULL;
  }
  return reference;
}

/* A synchronous compiled Inspect uses the existing Session/Runtime Run.
 * The graph separates admission/start/execution; the attempt and compiled
 * journal persist before callback dispatch. Normal completion persists a Run
 * state_snapshot, not a guaranteed checkpoint at every graph edge. */
static int turbo_agent_harness_execute_compiled_inspect_attempt_impl(
    turbo_agent_harness_t *harness,
    const turbo_agent_compiler_config_t *compiler_config,
    const char *source_json, size_t source_json_size,
    const turbo_tool_execution_context_t *context,
    const char *attempt_id, const char *job_id, uint32_t expected_job_revision,
    turbo_agent_compile_diagnostic_t *out_diagnostic,
    turbo_tool_status_t *out_tool_status,
    json_value_t **out_result,
    json_value_t **out_certificate, json_value_t **out_reference, int reserved) {
  turbo_agent_inspect_admission_t admission = {0};
  json_value_t *job_snapshot = NULL;
  json_value_t *job_reference = NULL;
  turbo_agent_executable_plan_t *plan = NULL;
  const turbo_tool_registry_t *approved;
  turbo_agent_session_t *session = NULL;
  turbo_agent_t *agent = NULL;
  turbo_agent_runtime_t *runtime = NULL;
  turbo_agent_tool_authority_snapshot_t authority = {0};
  const char *name;
  const char *result_schema_json = NULL;
  turbo_tool_effect_flags_t effects = TURBO_TOOL_EFFECT_UNKNOWN;
  turbo_agent_compiled_inspect_run_t compiled_run = {0};
  turbo_graph_t *graph = NULL;
  json_value_t *initial_state = NULL;
  json_value_t *summary = NULL;
  json_value_t *result_state = NULL;
  json_value_t *certificate = NULL;
  char *execution_id = NULL;
  char *owned_thread_id = NULL;
  char plan_hash[32];
  int attempt_key_width;
  const char *thread_id;
  int rc = SALTS_EIO;

  if (out_reference) *out_reference = NULL;
  if (out_result) *out_result = NULL;
  if (out_certificate) *out_certificate = NULL;
  if (out_tool_status) *out_tool_status = TURBO_TOOL_INVALID_ARGUMENT;
  if (out_diagnostic) memset(out_diagnostic, 0, sizeof(*out_diagnostic));
  if (!harness || !compiler_config ||
      compiler_config->struct_size < sizeof(*compiler_config) ||
      compiler_config->abi_version != TURBO_AGENT_COMPILER_CONFIG_ABI_VERSION ||
      !compiler_config->deny_unlisted_capabilities ||
      (compiler_config->allowed_capability_count && !compiler_config->allowed_capabilities) ||
      (job_id ? (!turbo_agent_compiled_inspect_key_component_valid(job_id, 64) ||
          expected_job_revision > TURBO_AGENT_JOB_MAX_REVISIONS || !out_reference ||
          out_reference == out_result || out_reference == out_certificate ||
          source_json || source_json_size || attempt_id) :
          (!source_json || !source_json_size ||
           !turbo_agent_compiled_inspect_attempt_id_valid(attempt_id))) ||
      !out_result || !out_certificate || out_result == out_certificate || !out_tool_status ||
      (context && (context->abi_version != TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION ||
                   context->struct_size < sizeof(*context)))) {
    return SALTS_EINVAL;
  }

  if (!reserved) {
    rc = turbo_agent_harness_begin_submit(harness);
    if (rc != SALTS_OK) return rc;
  }

  session = turbo_agent_app_session(harness->app);
  agent = turbo_agent_session_agent(session);
  runtime = turbo_agent_session_runtime(session);
  rc = turbo_agent_harness_require_scope(harness, session, out_diagnostic);
  if (rc != SALTS_OK) goto done;
  if (!agent || !runtime ||
      turbo_agent_tool_authority_snapshot_get(agent, &authority) != 0) {
    rc = turbo_agent_harness_compiled_reject(
        out_diagnostic, TURBO_AGENT_COMPILE_CAPABILITY_DENIED,
        "Harness has no readable Host Tool Authority or Runtime snapshot");
    goto done;
  }
  if (job_id) {
    const json_value_t *proposal;
    const json_value_t *binding;
    rc = turbo_agent_harness_job_snapshot(runtime, job_id, expected_job_revision,
        harness->compiled_attempt_thread_id, &job_snapshot, out_diagnostic);
    if (rc != SALTS_OK) goto done;
    rc = turbo_agent_harness_require_job_active(runtime, job_snapshot, out_diagnostic);
    if (rc != SALTS_OK) goto done;
    rc = turbo_agent_harness_verify_job_source(harness, job_snapshot, out_diagnostic);
    if (rc != SALTS_OK) goto done;
    proposal = json_object_get(job_snapshot, "proposal");
    binding = json_object_get(job_snapshot, "attempt");
    source_json = json_get_string(proposal, "source_json");
    source_json_size = json_string_len(json_object_get(proposal, "source_json"));
    attempt_id = json_get_string(binding, "attempt_id");
  }
  rc = turbo_agent_harness_compile_inspect(harness, compiler_config, source_json,
      source_json_size, &authority, &admission, out_diagnostic);
  if (rc != SALTS_OK) goto done;
  plan = admission.plan;
  certificate = admission.certificate;
  approved = admission.approved;
  name = admission.name;
  result_schema_json = admission.result_schema;
  effects = admission.effects;
  *out_certificate = json_clone(certificate);
  if (!*out_certificate) {
    rc = SALTS_ENOMEM;
    goto done;
  }

  if (context && context->cancel_token) {
    rc = turbo_cancel_token_check(context->cancel_token);
    if (rc != SALTS_OK) {
      *out_tool_status = rc == SALTS_ETIMEDOUT
          ? TURBO_TOOL_DEADLINE_EXCEEDED : TURBO_TOOL_CANCELLED;
      goto done;
    }
  }
  if (turbo_agent_tool_authority_snapshot_is_current(agent, &authority) != 1) {
    *out_tool_status = TURBO_TOOL_ERROR;
    rc = turbo_agent_harness_compiled_reject(
        out_diagnostic, TURBO_AGENT_COMPILE_CAPABILITY_DENIED,
        "Host Tool Authority changed after admission; stale plan rejected before Run creation");
    goto done;
  }

  thread_id = harness->compiled_attempt_thread_id;
  execution_id = turbo_runtime_record_uuid("compiled");
  if (!thread_id || !thread_id[0] || !execution_id ||
      snprintf(plan_hash, sizeof(plan_hash), "%016" PRIx64,
               (uint64_t)turbo_agent_executable_plan_hash(plan)) < 0 ||
      turbo_runtime_record_timestamp(compiled_run.created_at,
                                     sizeof(compiled_run.created_at)) != 0) {
    rc = SALTS_EIO;
    goto done;
  }
  /* This identity was copied when Harness was created. Session summary
   * finalization may replace App-owned thread ID storage between retries. */
  owned_thread_id = (char *)malloc(strlen(thread_id) + 1);
  if (!owned_thread_id) {
    rc = SALTS_ENOMEM;
    goto done;
  }
  memcpy(owned_thread_id, thread_id, strlen(thread_id) + 1);
  thread_id = owned_thread_id;
  if (!turbo_agent_compiled_inspect_key_component_valid(thread_id, 80)) {
    rc = SALTS_EINVAL;
    goto done;
  }
  attempt_key_width = snprintf(compiled_run.attempt_record_id,
      sizeof(compiled_run.attempt_record_id), "%03u_%s_%s",
      (unsigned)strlen(thread_id), thread_id, attempt_id);
  if (attempt_key_width < 0 ||
      (size_t)attempt_key_width >= sizeof(compiled_run.attempt_record_id)) {
    rc = SALTS_EINVAL;
    goto done;
  }
  initial_state = turbo_agent_compiled_inspect_initial_state(
      execution_id, thread_id, attempt_id, compiled_run.attempt_record_id,
      name, plan_hash, authority.generation, certificate);
  if (!initial_state) {
    rc = SALTS_ENOMEM;
    goto done;
  }

  compiled_run.agent = agent;
  compiled_run.runtime = runtime;
  compiled_run.plan = plan;
  compiled_run.approved = approved;
  compiled_run.tool_context = context;
  compiled_run.authority = authority;
  compiled_run.certificate = certificate;
  compiled_run.execution_id = execution_id;
  compiled_run.thread_id = thread_id;
  compiled_run.tool_name = name;
  compiled_run.attempt_id = attempt_id;
  compiled_run.job_snapshot = job_snapshot;
  compiled_run.result_schema_json = result_schema_json;
  compiled_run.effects = effects;
  snprintf(compiled_run.plan_hash, sizeof(compiled_run.plan_hash), "%s", plan_hash);
  compiled_run.diagnostic = out_diagnostic;
  compiled_run.tool_status = TURBO_TOOL_ERROR;
  compiled_run.failure_rc = SALTS_EIO;

  graph = turbo_graph_create("turbo-agent-compiled-inspect-v1");
  if (!graph ||
      turbo_graph_add_json_value_node_ex(
          graph, "admit", "turbo-agent.compiled-inspect.admit.v1",
          turbo_agent_compiled_inspect_admit_node, &compiled_run) != TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_json_value_node_ex(
          graph, "start", "turbo-agent.compiled-inspect.start.v1",
          turbo_agent_compiled_inspect_start_node, &compiled_run) != TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_json_value_node_ex(
          graph, "execute", "turbo-agent.compiled-inspect.execute.v1",
          turbo_agent_compiled_inspect_execute_node, &compiled_run) != TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_json_value_edge(graph, "admit", "start", NULL, NULL) !=
          TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_json_value_edge(graph, "start", "execute", NULL, NULL) !=
          TURBO_GRAPH_EXEC_OK ||
      turbo_graph_set_entry(graph, "admit") != TURBO_GRAPH_EXEC_OK) {
    rc = SALTS_ENOMEM;
    goto done;
  }

  rc = turbo_agent_session_start_graph(
      session, graph, initial_state, NULL, NULL, NULL, &summary, &result_state);
  if (rc != SALTS_OK) {
    if (compiled_run.failure_rc != SALTS_OK) {
      rc = compiled_run.failure_rc;
    } else if (compiled_run.committed) {
      /* The compiled tool result is durably committed, but Session-level
       * finalization (for example, inbox applied-state persistence) failed.
       * Keep the journal as the source of truth and return the API's stable
       * I/O failure instead of leaking a backend-specific Session error. */
      rc = SALTS_EIO;
    }
    goto done;
  }
  if (!compiled_run.committed || !compiled_run.result) {
    rc = compiled_run.failure_rc != SALTS_OK ? compiled_run.failure_rc : SALTS_EIO;
    if (out_diagnostic && !out_diagnostic->message[0]) {
      snprintf(out_diagnostic->message, sizeof(out_diagnostic->message), "%s",
               "compiled Inspect Run did not reach a committed result; no retry was attempted");
    }
    goto done;
  }

  /* Authority must still be current at the actual delivery boundary. A
   * committed attempt remains auditable if delivery is denied. */
  if (turbo_agent_tool_authority_snapshot_is_current(agent, &compiled_run.authority) != 1) {
    turbo_agent_compiled_inspect_fail(&compiled_run,
        TURBO_AGENT_COMPILE_CAPABILITY_DENIED, TURBO_TOOL_ERROR, SALTS_EPROTO,
        "Host Tool Authority changed before committed Inspect result delivery");
    *out_tool_status = TURBO_TOOL_ERROR;
    rc = SALTS_EPROTO;
    goto done;
  }
  if (job_snapshot) {
    rc = turbo_agent_harness_verify_job_source(harness, job_snapshot, out_diagnostic);
    if (rc != SALTS_OK) goto done;
    rc = turbo_agent_harness_require_job_active(runtime, job_snapshot, out_diagnostic);
    if (rc != SALTS_OK) goto done;
    if (context && context->cancel_token) {
      rc = turbo_cancel_token_check(context->cancel_token);
      if (rc != SALTS_OK) {
        if (out_diagnostic) {
          out_diagnostic->status = TURBO_AGENT_COMPILE_SOURCE_INVALID;
          (void)snprintf(out_diagnostic->message, sizeof(out_diagnostic->message),
              "Job result withheld after cancellation/deadline; canonical attempt remains unchanged");
        }
        goto done;
      }
    }
    const char *run_id = json_get_string(summary, "run_id");
    const char *run_thread_id = json_get_string(summary, "thread_id");
    const char *run_status = json_get_string(summary, "status");
    if (!run_id || !run_id[0] || !run_thread_id || !run_thread_id[0] ||
        !run_status || strcmp(run_status, "completed")) {
      rc = turbo_agent_harness_compiled_reject(out_diagnostic,
          TURBO_AGENT_COMPILE_SOURCE_INVALID, "Job delivery has no completed Runtime Run summary");
      goto done;
    }
    job_reference = turbo_agent_harness_job_reference(job_snapshot, summary, &compiled_run);
    if (!job_reference) {
      (void)turbo_agent_harness_compiled_reject(out_diagnostic,
          TURBO_AGENT_COMPILE_OUT_OF_MEMORY, "could not allocate Job execution reference after result commit");
      rc = SALTS_ENOMEM;
      goto done;
    }
    *out_reference = job_reference;
    job_reference = NULL;
  }
  *out_result = compiled_run.result;
  compiled_run.result = NULL;
  *out_tool_status = compiled_run.tool_status;
  rc = SALTS_OK;

done:
  if (out_tool_status && compiled_run.plan &&
      compiled_run.tool_status != TURBO_TOOL_INVALID_ARGUMENT)
    *out_tool_status = compiled_run.tool_status;
  if (job_id && out_tool_status && (rc == SALTS_ECANCELED || rc == SALTS_ETIMEDOUT))
    *out_tool_status = rc == SALTS_ETIMEDOUT ? TURBO_TOOL_DEADLINE_EXCEEDED : TURBO_TOOL_CANCELLED;
  json_free(job_reference);
  turbo_runtime_json_destroy(compiled_run.result);
  turbo_runtime_json_destroy(result_state);
  turbo_runtime_json_destroy(summary);
  turbo_runtime_json_destroy(initial_state);
  turbo_runtime_json_destroy(certificate);
  turbo_graph_destroy(graph);
  free(owned_thread_id);
  free(execution_id);
  turbo_agent_executable_plan_destroy(plan);
  json_free(job_snapshot);
  if (!reserved) turbo_agent_harness_end_submit(harness);
  return rc;
}

int turbo_agent_harness_execute_compiled_inspect_attempt_json(
    turbo_agent_harness_t *harness,
    const turbo_agent_compiler_config_t *compiler_config,
    const char *source_json, size_t source_json_size,
    const turbo_tool_execution_context_t *context,
    const char *attempt_id,
    turbo_agent_compile_diagnostic_t *out_diagnostic,
    turbo_tool_status_t *out_tool_status,
    json_value_t **out_result,
    json_value_t **out_certificate) {
  return turbo_agent_harness_execute_compiled_inspect_attempt_impl(
      harness, compiler_config, source_json, source_json_size, context, attempt_id, NULL, 0,
      out_diagnostic, out_tool_status, out_result, out_certificate, NULL, 0);
}

int turbo_agent_harness_execute_compiled_inspect_json(
    turbo_agent_harness_t *harness,
    const turbo_agent_compiler_config_t *compiler_config,
    const char *source_json, size_t source_json_size,
    const turbo_tool_execution_context_t *context,
    turbo_agent_compile_diagnostic_t *out_diagnostic,
    turbo_tool_status_t *out_tool_status,
    json_value_t **out_result,
    json_value_t **out_certificate) {
  char *attempt_id = turbo_runtime_record_uuid("compiled-attempt");
  int rc;
  if (!attempt_id) return SALTS_ENOMEM;
  rc = turbo_agent_harness_execute_compiled_inspect_attempt_impl(
      harness, compiler_config, source_json, source_json_size, context, attempt_id, NULL, 0,
      out_diagnostic, out_tool_status, out_result, out_certificate, NULL, 0);
  free(attempt_id);
  return rc;
}

int turbo_agent_harness_execute_job_inspect_json(
    turbo_agent_harness_t *harness, const char *job_id, uint32_t expected_job_revision,
    const turbo_agent_compiler_config_t *compiler_config,
    const turbo_tool_execution_context_t *context,
    turbo_agent_compile_diagnostic_t *out_diagnostic, turbo_tool_status_t *out_tool_status,
    json_value_t **out_result, json_value_t **out_certificate, json_value_t **out_reference) {
  /* Empty strings keep invalid job_id on Job validation, never on the legacy
   * source/attempt path. All outputs are initialized by the common entry. */
  return turbo_agent_harness_execute_compiled_inspect_attempt_impl(
      harness, compiler_config, NULL, 0, context, NULL, job_id ? job_id : "",
      expected_job_revision, out_diagnostic, out_tool_status, out_result,
      out_certificate, out_reference, 0);
}

int turbo_agent_harness_execute_job_reserved(turbo_agent_harness_t *harness,
    const char *id, uint32_t revision, const turbo_agent_compiler_config_t *config,
    const turbo_tool_execution_context_t *context, turbo_agent_compile_diagnostic_t *diagnostic,
    turbo_tool_status_t *status, json_value_t **result, json_value_t **certificate, json_value_t **reference) {
  return turbo_agent_harness_execute_compiled_inspect_attempt_impl(harness, config,
      NULL, 0, context, NULL, id, revision, diagnostic, status, result, certificate, reference, 1);
}

static int turbo_agent_harness_submit(turbo_agent_harness_t *harness,
                                      turbo_agent_session_operation_t operation,
                                      const json_value_t *input,
                                      const turbo_agent_harness_run_options_t *options,
                                      turbo_agent_harness_execution_t **out_execution) {
  turbo_agent_harness_execution_t *execution = NULL;
  turbo_agent_session_t *session;
  turbo_graph_t *graph = NULL;
  turbo_agent_runtime_t *runtime;
  turbo_agent_session_submit_options_t submit_options;
  turbo_agent_execution_t *active_ref = NULL;
  int rc;

  if (!out_execution) {
    return SALTS_EINVAL;
  }
  *out_execution = NULL;
  if (!harness || !turbo_agent_harness_run_options_valid(options) ||
      (operation == TURBO_AGENT_SESSION_START && !input) ||
      (operation != TURBO_AGENT_SESSION_START && (!options || !options->session_options))) {
    return SALTS_EINVAL;
  }
  if (operation == TURBO_AGENT_SESSION_START && options && options->session_options) {
    return SALTS_EINVAL;
  }
  rc = turbo_agent_harness_begin_submit(harness);
  if (rc != SALTS_OK) {
    return rc;
  }

  session = turbo_agent_app_session(harness->app);
  runtime = turbo_agent_session_runtime(session);
  graph =
      turbo_agent_session_create_preset_graph(session, turbo_agent_session_workflow_kind(session));
  if (!runtime || !graph) {
    rc = SALTS_EINVAL;
    goto fail;
  }

  execution = (turbo_agent_harness_execution_t *)calloc(1, sizeof(*execution));
  if (!execution) {
    rc = SALTS_ENOMEM;
    goto fail;
  }
  atomic_init(&execution->ref_count, 2);
  execution->harness = turbo_agent_harness_retain(harness);
  execution->graph = graph;
  if (!execution->harness) {
    rc = SALTS_ERANGE;
    goto fail_execution;
  }

  turbo_agent_session_submit_options_init(&submit_options);
  submit_options.operation = operation;
  submit_options.terminal = turbo_agent_harness_terminal;
  submit_options.terminal_user_data = execution;
  if (options) {
    submit_options.graph_options = options->graph_options;
    submit_options.session_options = options->session_options;
    submit_options.deadline_mono_ms = options->deadline_mono_ms;
    submit_options.event_sink = options->event_sink;
    submit_options.event_sink_user_data = options->event_sink_user_data;
  }
  rc = turbo_agent_session_submit(harness->executor, session, graph, input,
      &submit_options, &execution->execution);
  if (rc != SALTS_OK) {
    goto fail_execution;
  }

  active_ref = turbo_agent_execution_retain(execution->execution);
  if (!active_ref) {
    (void)turbo_agent_execution_cancel(execution->execution, TURBO_CANCEL_SHUTDOWN);
    rc = SALTS_ERANGE;
    goto fail_running_execution;
  }
  cmeta_mutex_lock(&harness->mutex);
  harness->submitting = 0;
  if (!execution->terminal_before_publish) {
    harness->active_execution = active_ref;
    active_ref = NULL;
  }
  cmeta_mutex_unlock(&harness->mutex);
  turbo_agent_execution_release(active_ref);

  *out_execution = execution;
  return SALTS_OK;

fail_running_execution:
  cmeta_mutex_lock(&harness->mutex);
  harness->submitting = 0;
  cmeta_mutex_unlock(&harness->mutex);
  turbo_agent_harness_execution_release(execution);
  return rc;

fail_execution:
  turbo_agent_harness_execution_release(execution);
  turbo_agent_harness_execution_release(execution);
  turbo_agent_harness_end_submit(harness);
  return rc;

fail:
  turbo_graph_destroy(graph);
  turbo_agent_harness_end_submit(harness);
  return rc;
}

int turbo_agent_harness_start_state(turbo_agent_harness_t *harness, const json_value_t *state,
                                    const turbo_agent_harness_run_options_t *options,
                                    turbo_agent_harness_execution_t **out_execution) {
  return turbo_agent_harness_submit(harness, TURBO_AGENT_SESSION_START, state, options,
                                    out_execution);
}

int turbo_agent_harness_start_text(turbo_agent_harness_t *harness, const char *user_text,
                                   const turbo_agent_harness_run_options_t *options,
                                   turbo_agent_harness_execution_t **out_execution) {
  json_value_t *state;
  int rc;

  if (!user_text || !out_execution) {
    return SALTS_EINVAL;
  }
  *out_execution = NULL;
  state = turbo_agent_session_create_input_state_json_value(user_text);
  if (!state) {
    return SALTS_ENOMEM;
  }
  rc = turbo_agent_harness_start_state(harness, state, options, out_execution);
  turbo_runtime_json_destroy(state);
  return rc;
}

int turbo_agent_harness_resume(turbo_agent_harness_t *harness, const json_value_t *input,
                               const turbo_agent_harness_run_options_t *options,
                               turbo_agent_harness_execution_t **out_execution) {
  return turbo_agent_harness_submit(harness, TURBO_AGENT_SESSION_RESUME, input, options,
                                    out_execution);
}

int turbo_agent_harness_fork(turbo_agent_harness_t *harness, const json_value_t *input,
                             const turbo_agent_harness_run_options_t *options,
                             turbo_agent_harness_execution_t **out_execution) {
  return turbo_agent_harness_submit(harness, TURBO_AGENT_SESSION_FORK, input, options,
                                    out_execution);
}

const char *turbo_agent_harness_execution_id(const turbo_agent_harness_execution_t *execution) {
  return execution ? turbo_agent_execution_id(execution->execution) : NULL;
}

int turbo_agent_harness_execution_cancel(turbo_agent_harness_execution_t *execution,
                                         turbo_cancel_reason_t reason) {
  return execution ? turbo_agent_execution_cancel(execution->execution, reason) : SALTS_EINVAL;
}

int turbo_agent_harness_execution_wait(turbo_agent_harness_execution_t *execution,
                                       uint64_t timeout_ms) {
  return execution ? turbo_agent_execution_wait(execution->execution, timeout_ms) : SALTS_EINVAL;
}

int turbo_agent_harness_execution_get_status(const turbo_agent_harness_execution_t *execution,
                                             turbo_agent_execution_status_t *out_status) {
  return execution ? turbo_agent_execution_get_status(execution->execution, out_status)
                   : SALTS_EINVAL;
}

int turbo_agent_harness_execution_result_code(const turbo_agent_harness_execution_t *execution,
                                              int *out_result_code) {
  return execution ? turbo_agent_execution_result_code(execution->execution, out_result_code)
                   : SALTS_EINVAL;
}

int turbo_agent_harness_execution_take_result(turbo_agent_harness_execution_t *execution,
                                              json_value_t **out_summary,
                                              json_value_t **out_state) {
  return execution ? turbo_agent_execution_take_result(execution->execution, out_summary, out_state)
                   : SALTS_EINVAL;
}
