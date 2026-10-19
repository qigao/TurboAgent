#include <salts/thread_pool.h>
#include "turbo_agent_harness.h"
#include <json_parser.h>

#include "turbo_agent_session_execution.h"
#include "turbo_agent_session.h"
#include "turbo_agent_policy.h"
#include "turbo_tool_schema.h"
#include "turbo_runtime_json.h"
#include "turbo_runtime_segment.h"

#include <inttypes.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct turbo_agent_harness_s {
  atomic_size_t ref_count;
  cmeta_mutex_t mutex;
  turbo_agent_app_t *app;
  cmeta_threadpool_t *executor;
  turbo_agent_execution_t *active_execution;
  int submitting;
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

turbo_agent_harness_t *turbo_agent_harness_create(const turbo_agent_harness_config_t *config) {
  turbo_agent_harness_t *harness;

  if (!turbo_agent_harness_config_valid(config)) {
    return NULL;
  }
  harness = (turbo_agent_harness_t *)calloc(1, sizeof(*harness));
  if (!harness) {
    return NULL;
  }
  atomic_init(&harness->ref_count, 1);
  cmeta_mutex_init(&harness->mutex);
  if (!harness->mutex) {
    free(harness);
    return NULL;
  }
  harness->app = turbo_agent_app_create(config->app_config);
  if (!harness->app) {
    cmeta_mutex_destroy(&harness->mutex);
    free(harness);
    return NULL;
  }
  harness->executor = config->executor;
  return harness;
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

static int turbo_agent_harness_begin_submit(turbo_agent_harness_t *harness) {
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

static void turbo_agent_harness_end_failed_submit(turbo_agent_harness_t *harness) {
  cmeta_mutex_lock(&harness->mutex);
  harness->submitting = 0;
  cmeta_mutex_unlock(&harness->mutex);
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
  char plan_hash[32];
  turbo_tool_status_t tool_status;
  int callback_started;
  int committed;
  int failure_rc;
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

/* This node is deliberately separate from the tool callback. Runtime persists
 * a checkpoint for the next node before entering it, so failed checkpoint
 * persistence prevents the callback from starting. */
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
    run->failure_rc = SALTS_EIO;
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
  if (!ctx || !ctx->json_value_state || !run) return -1;
  record = turbo_agent_compiled_execution_record_state(ctx->json_value_state);
  if (!record || !json_get_string(record, "phase") ||
      strcmp(json_get_string(record, "phase"), "started") != 0) {
    turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_SOURCE_INVALID,
        TURBO_TOOL_ERROR, SALTS_EPROTO, "compiled execution did not reach its persisted started checkpoint");
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
    run->failure_rc = SALTS_EIO;
    snprintf(run->failure_message, sizeof(run->failure_message), "%s",
             "compiled Inspect was cancelled at the final pre-callback gate");
    (void)turbo_agent_compiled_inspect_set_phase(
        ctx->json_value_state, "cancelled", run->tool_status, run->failure_message);
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
    turbo_runtime_json_destroy(result);
    return -1;
  }
  if (turbo_agent_tool_authority_snapshot_is_current(run->agent, &run->authority) != 1) {
    turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_CAPABILITY_DENIED,
        TURBO_TOOL_ERROR, SALTS_EPROTO,
        "Host Tool Authority changed during callback; result was not committed");
    (void)turbo_agent_compiled_inspect_set_phase(
        ctx->json_value_state, "outcome_unknown", run->tool_status, run->failure_message);
    turbo_runtime_json_destroy(result);
    return -1;
  }
  if (turbo_runtime_json_object_set_clone(record, "result", result) != TURBO_RUNTIME_JSON_OK ||
      turbo_agent_compiled_inspect_set_phase(
          ctx->json_value_state, "committed", TURBO_TOOL_OK, NULL) != 0) {
    turbo_agent_compiled_inspect_fail(run, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
        TURBO_TOOL_ERROR, SALTS_ENOMEM,
        "tool result was returned but its committed Run state could not be constructed");
    (void)turbo_agent_compiled_inspect_set_phase(
        ctx->json_value_state, "outcome_unknown", run->tool_status, run->failure_message);
    turbo_runtime_json_destroy(result);
    return -1;
  }
  run->result = result;
  run->committed = 1;
  run->failure_rc = SALTS_OK;
  run->failure_message[0] = '\0';
  return 0;
}

static json_value_t *turbo_agent_compiled_inspect_initial_state(
    const char *execution_id, const char *thread_id, const char *tool_name,
    const char *plan_hash, uint64_t authority_generation,
    const json_value_t *certificate) {
  json_value_t *state = json_create_object();
  json_value_t *record = json_create_object();
  if (!state || !record ||
      turbo_runtime_json_object_set_int64(record, "schema_version",
          TURBO_AGENT_COMPILED_EXECUTION_SCHEMA_VERSION) != TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "execution_id", execution_id) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "thread_id", thread_id) !=
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

/* A synchronous compiled Inspect is executed inside the existing Session/Runtime
 * Run. Three graph nodes force Runtime to persist an admitted checkpoint and
 * then a started checkpoint BEFORE the tool callback node is entered. */
int turbo_agent_harness_execute_compiled_inspect_json(
    turbo_agent_harness_t *harness,
    const turbo_agent_compiler_config_t *compiler_config,
    const char *source_json, size_t source_json_size,
    const turbo_tool_execution_context_t *context,
    turbo_agent_compile_diagnostic_t *out_diagnostic,
    turbo_tool_status_t *out_tool_status,
    json_value_t **out_result,
    json_value_t **out_certificate) {
  turbo_agent_executable_plan_t *plan = NULL;
  const turbo_tool_registry_t *registry;
  const turbo_tool_registry_t *approved;
  turbo_agent_session_t *session = NULL;
  turbo_agent_t *agent = NULL;
  turbo_agent_runtime_t *runtime = NULL;
  turbo_agent_tool_authority_snapshot_t authority = {0};
  turbo_agent_compiler_config_t effective_config;
  const char **effective_capabilities = NULL;
  const char *name;
  const char *result_schema_json = NULL;
  const json_value_t *result_schema = NULL;
  turbo_tool_effect_flags_t effects = TURBO_TOOL_EFFECT_UNKNOWN;
  turbo_agent_compile_status_t compiled;
  turbo_agent_compiled_inspect_run_t compiled_run = {0};
  turbo_graph_t *graph = NULL;
  json_value_t *initial_state = NULL;
  json_value_t *summary = NULL;
  json_value_t *result_state = NULL;
  json_value_t *certificate = NULL;
  char *execution_id = NULL;
  char plan_hash[32];
  const char *thread_id;
  int strict_result = 0;
  int rc = SALTS_EIO;

  if (out_result) *out_result = NULL;
  if (out_certificate) *out_certificate = NULL;
  if (out_tool_status) *out_tool_status = TURBO_TOOL_INVALID_ARGUMENT;
  if (out_diagnostic) memset(out_diagnostic, 0, sizeof(*out_diagnostic));
  if (!harness || !compiler_config ||
      compiler_config->struct_size < sizeof(*compiler_config) ||
      compiler_config->abi_version != TURBO_AGENT_COMPILER_CONFIG_ABI_VERSION ||
      !compiler_config->deny_unlisted_capabilities ||
      (compiler_config->allowed_capability_count && !compiler_config->allowed_capabilities) ||
      !source_json || !source_json_size || !out_result || !out_certificate ||
      !out_tool_status ||
      (context && (context->abi_version != TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION ||
                   context->struct_size < sizeof(*context)))) {
    return SALTS_EINVAL;
  }

  rc = turbo_agent_harness_begin_submit(harness);
  if (rc != SALTS_OK) return rc;

  session = turbo_agent_app_session(harness->app);
  agent = turbo_agent_session_agent(session);
  runtime = turbo_agent_session_runtime(session);
  if (!agent || !runtime ||
      turbo_agent_tool_authority_snapshot_get(agent, &authority) != 0) {
    rc = turbo_agent_harness_compiled_reject(
        out_diagnostic, TURBO_AGENT_COMPILE_CAPABILITY_DENIED,
        "Harness has no readable Host Tool Authority or Runtime snapshot");
    goto done;
  }
  rc = turbo_agent_harness_compiler_authority_configure(
      compiler_config, &authority, &effective_config,
      &effective_capabilities);
  if (rc != SALTS_OK) goto done;

  registry = turbo_agent_app_tool_registry(harness->app);
  if (!registry) {
    rc = turbo_agent_harness_compiled_reject(
        out_diagnostic, TURBO_AGENT_COMPILE_UNRESOLVED_TOOL,
        "Harness has no configured RuntimeTools registry");
    goto done;
  }
  compiled = turbo_agent_compile_plan_json(
      &effective_config, registry, source_json, source_json_size, &plan,
      out_diagnostic);
  if (compiled != TURBO_AGENT_COMPILE_OK || !plan) {
    rc = SALTS_EPROTO;
    goto done;
  }

  approved = turbo_agent_executable_plan_approved_tools(plan);
  name = turbo_agent_executable_plan_tool_name(plan);
  if (!approved || !name ||
      turbo_tool_registry_get_effects(approved, name, &effects) != TURBO_TOOL_OK ||
      !(effects == TURBO_TOOL_EFFECT_PURE ||
        ((effects & TURBO_TOOL_EFFECT_READ) &&
         (effects & ~(TURBO_TOOL_EFFECT_READ | TURBO_TOOL_EFFECT_NETWORK)) == 0))) {
    rc = turbo_agent_harness_compiled_reject(
        out_diagnostic, TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION,
        "Inspect requires declared PURE or READ(+NETWORK) effects");
    goto done;
  }
  if (turbo_tool_registry_get_result_contract(
          approved, name, &result_schema_json, &result_schema, &strict_result) !=
          TURBO_TOOL_OK || !result_schema_json || !result_schema || !strict_result) {
    rc = turbo_agent_harness_compiled_reject(
        out_diagnostic, TURBO_AGENT_COMPILE_TOOL_SCHEMA_INVALID,
        "compiled Inspect requires a declared strict result schema");
    goto done;
  }

  certificate = turbo_agent_executable_plan_certificate_json_value(plan);
  if (!certificate) {
    rc = SALTS_ENOMEM;
    goto done;
  }
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

  thread_id = turbo_agent_app_thread_id(harness->app);
  execution_id = turbo_runtime_record_uuid("compiled");
  if (!thread_id || !thread_id[0] || !execution_id ||
      snprintf(plan_hash, sizeof(plan_hash), "%" PRIu64,
               (uint64_t)turbo_agent_executable_plan_hash(plan)) < 0) {
    rc = SALTS_ENOMEM;
    goto done;
  }
  initial_state = turbo_agent_compiled_inspect_initial_state(
      execution_id, thread_id, name, plan_hash, authority.generation, certificate);
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
    if (compiled_run.failure_rc != SALTS_OK) rc = compiled_run.failure_rc;
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

  *out_result = compiled_run.result;
  compiled_run.result = NULL;
  *out_tool_status = compiled_run.tool_status;
  rc = SALTS_OK;

done:
  if (out_tool_status && compiled_run.plan &&
      compiled_run.tool_status != TURBO_TOOL_INVALID_ARGUMENT)
    *out_tool_status = compiled_run.tool_status;
  turbo_runtime_json_destroy(compiled_run.result);
  turbo_runtime_json_destroy(result_state);
  turbo_runtime_json_destroy(summary);
  turbo_runtime_json_destroy(initial_state);
  turbo_runtime_json_destroy(certificate);
  turbo_graph_destroy(graph);
  free(execution_id);
  turbo_agent_executable_plan_destroy(plan);
  free((void *)effective_capabilities);
  turbo_agent_harness_end_failed_submit(harness);
  return rc;
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
  turbo_agent_harness_end_failed_submit(harness);
  return rc;

fail:
  turbo_graph_destroy(graph);
  turbo_agent_harness_end_failed_submit(harness);
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
