#include <salts/thread_pool.h>
#include "turbo_agent_harness.h"
#include <json_parser.h>

#include "turbo_agent_session_execution.h"
#include "turbo_tool_schema.h"
#include "turbo_runtime_store.h"

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

#define TURBO_HARNESS_INSPECT_COLLECTION "compiled_inspect_attempts"
#define TURBO_HARNESS_INSPECT_MAX_RECORD_BYTES (1024u * 1024u)

/* SQLite Store treats IDs as SQL values, but validate both identity components
 * as path-safe for the RuntimeStore file backend as well. A file backend is
 * NOT claimed crash-safe; these records are still useful for local inspection. */
static int turbo_harness_inspect_key_component(const char *s, size_t max_len) {
  size_t n;
  if (!s || !s[0]) return 0;
  for (n = 0; s[n]; ++n) {
    const char c = s[n];
    if (n >= max_len ||
        !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
      return 0;
  }
  return n > 0;
}

/* The record contains only identity, declarative contracts and JSON outcomes.
 * Never persist function pointers, Runtime state, provider handles or leases. */
static int turbo_harness_inspect_record_write(
    turbo_agent_runtime_t *runtime, const char *key, const char *thread_id,
    const char *attempt_id, const char *hash_hex, const char *tool,
    const char *result_schema, turbo_tool_effect_flags_t effects,
    const char *phase, turbo_tool_status_t status, const char *error,
    const json_value_t *result) {
  json_value_t *record = json_create_object();
  char *serialized = NULL;
  int rc = SALTS_ENOMEM;
  if (!runtime || !record) goto cleanup;
  if (turbo_runtime_json_object_set_int64(record, "schema_version", 1) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "id", key) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "thread_id", thread_id) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "attempt_id", attempt_id) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "plan_hash", hash_hex) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "tool_name", tool) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "result_schema", result_schema) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_int64(record, "effects", (int64_t)effects) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "phase", phase) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_int64(record, "tool_status", status) !=
          TURBO_RUNTIME_JSON_OK ||
      (error && turbo_runtime_json_object_set_string(record, "error", error) !=
          TURBO_RUNTIME_JSON_OK) ||
      (result && turbo_runtime_json_object_set_clone(record, "result", result) !=
          TURBO_RUNTIME_JSON_OK))
    goto cleanup;
  serialized = json_serialize(record, NULL);
  if (!serialized) goto cleanup;
  if (strlen(serialized) > TURBO_HARNESS_INSPECT_MAX_RECORD_BYTES) {
    rc = SALTS_EMSGSIZE;
    goto cleanup;
  }
  rc = turbo_agent_runtime_store_put_json(
      runtime, TURBO_HARNESS_INSPECT_COLLECTION, key, record) == SALTS_OK
           ? SALTS_OK : SALTS_EIO;
cleanup:
  json_serialize_free(serialized);
  turbo_runtime_json_destroy(record);
  return rc;
}

static int turbo_harness_inspect_record_string_equals(
    const json_value_t *record, const char *field, const char *expected) {
  const char *value = record ? json_get_string(record, field) : NULL;
  return value && expected && strcmp(value, expected) == 0;
}

/* One synchronous, read-only slice of compiled Harness execution. The async
 * Session Run/Checkpoint/Journal integration is tracked separately in #72. */
static int turbo_agent_harness_execute_compiled_inspect_impl(
    turbo_agent_harness_t *harness,
    const turbo_agent_compiler_config_t *compiler_config,
    const char *source_json, size_t source_json_size,
    const turbo_tool_execution_context_t *context,
    const char *attempt_id, int *out_replayed, int gate_held,
    turbo_agent_compile_diagnostic_t *out_diagnostic,
    turbo_tool_status_t *out_tool_status,
    json_value_t **out_result,
    json_value_t **out_certificate) {
  turbo_agent_executable_plan_t *plan = NULL;
  const turbo_tool_registry_t *registry;
  const turbo_tool_registry_t *approved;
  const char *name;
  const char *result_schema_json = NULL;
  const json_value_t *result_schema = NULL;
  turbo_tool_effect_flags_t effects = TURBO_TOOL_EFFECT_UNKNOWN;
  turbo_tool_status_t tool_status;
  turbo_agent_compile_status_t compiled;
  json_value_t *result = NULL;
  json_value_t *certificate = NULL;
  json_value_t *stored = NULL;
  turbo_agent_runtime_t *runtime = NULL;
  const char *thread_id = NULL;
  char record_id[192] = {0};
  char plan_hash_hex[17] = {0};
  int durable = attempt_id != NULL;
  int strict_result = 0;
  int rc;

  if (out_replayed) *out_replayed = 0;
  if (out_result) *out_result = NULL;
  if (out_certificate) *out_certificate = NULL;
  if (out_tool_status) *out_tool_status = TURBO_TOOL_INVALID_ARGUMENT;
  if (out_diagnostic) memset(out_diagnostic, 0, sizeof(*out_diagnostic));
  if (!harness || !compiler_config || !compiler_config->deny_unlisted_capabilities ||
      !source_json || !source_json_size || !out_result || !out_certificate ||
      !out_tool_status ||
      (durable && (!out_replayed ||
                    !turbo_harness_inspect_key_component(attempt_id, 64))) ||
      (context && (context->abi_version != TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION ||
                   context->struct_size < sizeof(*context)))) {
    return SALTS_EINVAL;
  }

  if (!gate_held) {
    rc = turbo_agent_harness_begin_submit(harness);
    if (rc != SALTS_OK) return rc;
  }

  registry = turbo_agent_app_tool_registry(harness->app);
  if (!registry) {
    rc = turbo_agent_harness_compiled_reject(
        out_diagnostic, TURBO_AGENT_COMPILE_UNRESOLVED_TOOL,
        "Harness has no configured RuntimeTools registry");
    goto done;
  }
  compiled = turbo_agent_compile_plan_json(
      compiler_config, registry, source_json, source_json_size, &plan,
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
  *out_certificate = certificate;
  certificate = NULL;

  if (durable) {
    turbo_agent_session_t *session = turbo_agent_app_session(harness->app);
    int read_rc;
    int width;
    const char *phase = NULL;
    const json_value_t *persisted_result;
    int64_t saved_status;

    if (!session || !(runtime = turbo_agent_session_runtime(session)) ||
        !(thread_id = turbo_agent_session_thread_id(session)) ||
        !turbo_harness_inspect_key_component(thread_id, 80)) {
      rc = SALTS_EINVAL;
      goto done;
    }
    width = snprintf(record_id, sizeof(record_id), "%03u_%s_%s",
                     (unsigned)strlen(thread_id), thread_id, attempt_id);
    if (width < 0 || (size_t)width >= sizeof(record_id)) {
      rc = SALTS_EINVAL;
      goto done;
    }
    snprintf(plan_hash_hex, sizeof(plan_hash_hex), "%016llx",
             (unsigned long long)turbo_agent_executable_plan_hash(plan));

    read_rc = turbo_agent_runtime_store_get_json_status(
        runtime, TURBO_HARNESS_INSPECT_COLLECTION, record_id, &stored);
    if (read_rc != SALTS_ENOENT) {
      if (read_rc != SALTS_OK) {
        rc = read_rc;
        goto done;  /* Never equate an I/O/corruption error with absence. */
      }
      if (!stored || json_type(stored) != JSON_OBJECT ||
          turbo_runtime_json_value_as_int64(
              json_object_get(stored, "schema_version"), INT64_MIN) != 1 ||
          !turbo_harness_inspect_record_string_equals(stored, "id", record_id) ||
          !turbo_harness_inspect_record_string_equals(stored, "thread_id", thread_id) ||
          !turbo_harness_inspect_record_string_equals(stored, "attempt_id", attempt_id) ||
          !turbo_harness_inspect_record_string_equals(stored, "plan_hash", plan_hash_hex) ||
          !turbo_harness_inspect_record_string_equals(stored, "tool_name", name) ||
          !turbo_harness_inspect_record_string_equals(
              stored, "result_schema", result_schema_json) ||
          turbo_runtime_json_value_as_int64(
              json_object_get(stored, "effects"), INT64_MIN) != (int64_t)effects) {
        rc = SALTS_EPROTO;
        goto done;
      }
      phase = json_get_string(stored, "phase");
      saved_status = turbo_runtime_json_value_as_int64(
          json_object_get(stored, "tool_status"), INT64_MAX);
      if (!phase || saved_status > TURBO_TOOL_OK ||
          saved_status < TURBO_TOOL_TRAPPED) {
        rc = SALTS_EPROTO;
        goto done;
      }
      if (strcmp(phase, "started") == 0) {
        *out_tool_status = TURBO_TOOL_UNKNOWN_SIDE_EFFECT;
        rc = SALTS_EBUSY; /* Uncertain external effect: never retry. */
        goto done;
      }
      if (strcmp(phase, "committed") != 0) {
        rc = SALTS_EPROTO;
        goto done;
      }
      *out_tool_status = (turbo_tool_status_t)saved_status;
      persisted_result = json_object_get(stored, "result");
      if (*out_tool_status == TURBO_TOOL_OK) {
        if (!persisted_result ||
            turbo_tool_schema_validate_result_json_value(
                approved, name, persisted_result, NULL, 0) != TURBO_TOOL_SCHEMA_VALID ||
            !(*out_result = json_clone(persisted_result))) {
          rc = SALTS_EPROTO;
          goto done;
        }
        *out_replayed = 1;
        rc = SALTS_OK;
      } else {
        if (persisted_result) {
          rc = SALTS_EPROTO;
          goto done;
        }
        *out_replayed = 1;
        rc = turbo_harness_inspect_record_string_equals(
                 stored, "error", "result_schema") ? SALTS_EPROTO : SALTS_EIO;
      }
      goto done;
    }
  }

  if (context && context->cancel_token) {
    rc = turbo_cancel_token_check(context->cancel_token);
    if (rc != SALTS_OK) {
      *out_tool_status = rc == SALTS_ETIMEDOUT ?
          TURBO_TOOL_DEADLINE_EXCEEDED : TURBO_TOOL_CANCELLED;
      goto done;
    }
  }

  if (durable) {
    rc = turbo_harness_inspect_record_write(
        runtime, record_id, thread_id, attempt_id, plan_hash_hex, name,
        result_schema_json, effects, "started", TURBO_TOOL_UNKNOWN_SIDE_EFFECT,
        NULL, NULL);
    if (rc != SALTS_OK) goto done; /* Never invoke after failed durable STARTED. */
  }

  tool_status = turbo_agent_execute_compiled_plan(plan, context, &result);
  *out_tool_status = tool_status;
  if (tool_status != TURBO_TOOL_OK) {
    rc = SALTS_EIO;
  } else if (!result ||
             turbo_tool_schema_validate_result_json_value(
                 approved, name, result, NULL, 0) != TURBO_TOOL_SCHEMA_VALID) {
    *out_tool_status = TURBO_TOOL_ERROR;
    rc = turbo_agent_harness_compiled_reject(
        out_diagnostic, TURBO_AGENT_COMPILE_TOOL_SCHEMA_INVALID,
        "compiled Inspect returned a result violating its admitted schema");
  } else {
    rc = SALTS_OK;
  }

  if (durable) {
    int write_rc = turbo_harness_inspect_record_write(
        runtime, record_id, thread_id, attempt_id, plan_hash_hex, name,
        result_schema_json, effects, "committed", *out_tool_status,
        rc == SALTS_EPROTO ? "result_schema" : NULL,
        rc == SALTS_OK ? result : NULL);
    if (write_rc != SALTS_OK) {
      /* STARTED survives: outcome is uncertain, even if we saw a callback
       * result locally. Never return it as committed or retry the callback. */
      *out_tool_status = TURBO_TOOL_UNKNOWN_SIDE_EFFECT;
      rc = SALTS_EIO;
    }
  }
  if (rc == SALTS_OK) {
    *out_result = result;
    result = NULL;
  }

done:
  turbo_runtime_json_destroy(stored);
  turbo_runtime_json_destroy(result);
  turbo_runtime_json_destroy(certificate);
  turbo_agent_executable_plan_destroy(plan);
  if (!gate_held) turbo_agent_harness_end_failed_submit(harness);
  return rc;
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
  return turbo_agent_harness_execute_compiled_inspect_impl(
      harness, compiler_config, source_json, source_json_size, context,
      NULL, NULL, 0, out_diagnostic, out_tool_status, out_result, out_certificate);
}

int turbo_agent_harness_execute_compiled_inspect_durable_json(
    turbo_agent_harness_t *harness,
    const turbo_agent_compiler_config_t *compiler_config,
    const char *source_json, size_t source_json_size,
    const turbo_tool_execution_context_t *context,
    const char *attempt_id, int *out_replayed,
    turbo_agent_compile_diagnostic_t *out_diagnostic,
    turbo_tool_status_t *out_tool_status,
    json_value_t **out_result,
    json_value_t **out_certificate) {
  if (!attempt_id) return SALTS_EINVAL;
  return turbo_agent_harness_execute_compiled_inspect_impl(
      harness, compiler_config, source_json, source_json_size, context,
      attempt_id, out_replayed, 0, out_diagnostic, out_tool_status,
      out_result, out_certificate);
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
