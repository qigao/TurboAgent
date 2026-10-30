#include "turbo_agent_tool_executor_internal.h"
#include "turbo_agent_tool_journal_internal.h"

#include "turbo_agent_core_internal.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_runtime_store.h"
#include "turbo_agent_util_internal.h"
#include "turbo_tool_schema.h"

#include <cmeta_crypto.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <salts/thread_pool.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define TURBO_AGENT_TOOL_DEFAULT_WORKERS 4u
#define TURBO_AGENT_TOOL_DEFAULT_QUEUE_CAPACITY 8u
#define TURBO_AGENT_TOOL_DEFAULT_MAX_BATCH_CALLS 64u
#define TURBO_AGENT_TOOL_DEFAULT_MAX_ARGUMENT_BYTES (1024u * 1024u)
#define TURBO_AGENT_TOOL_DEFAULT_MAX_OUTPUT_BYTES (1024u * 1024u)
#define TURBO_AGENT_TOOL_BATCH_WAIT_SLICE_MS UINT64_C(25)
#define TURBO_AGENT_TOOL_MILLISECONDS_TO_NANOSECONDS UINT64_C(1000000)

struct turbo_agent_tool_executor_s {
  turbo_agent_tool_executor_config_t config;
  cmeta_threadpool_t *pool;
  cmeta_mutex_t batch_mutex;
  cmeta_cond_t batch_changed;
  int batch_active;
  cmeta_mutex_t observation_mutex;
};

typedef struct turbo_agent_tool_task_s {
  turbo_agent_tool_executor_t *executor;
  turbo_agent_runtime_t *runtime;
  const turbo_cancel_token_t *cancel_token;
  const turbo_tool_registry_t *registry;
  turbo_agent_tool_execution_t *call;
  turbo_agent_execution_context_t context;
  turbo_event_sink_json_value_fn event_sink;
  void *event_sink_user_data;
} turbo_agent_tool_task_t;

static char *turbo_agent_tool_strdup(const char *text) {
  size_t length;
  char *copy;
  if (!text) return NULL;
  length = strlen(text) + 1;
  copy = (char *)malloc(length);
  if (copy) memcpy(copy, text, length);
  return copy;
}

static int
turbo_agent_tool_executor_config_valid(const turbo_agent_tool_executor_config_t *config) {
  return config && config->struct_size >= sizeof(*config) &&
         config->abi_version == TURBO_AGENT_TOOL_EXECUTOR_CONFIG_ABI_VERSION &&
         config->max_workers > 0 && config->max_workers <= (size_t)INT_MAX &&
         config->queue_capacity >= config->max_workers && config->max_batch_calls > 0 &&
         config->max_arguments_bytes > 0 && config->max_output_bytes > 0;
}

void turbo_agent_tool_executor_config_init(turbo_agent_tool_executor_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_AGENT_TOOL_EXECUTOR_CONFIG_ABI_VERSION;
  config->max_workers = TURBO_AGENT_TOOL_DEFAULT_WORKERS;
  config->queue_capacity = TURBO_AGENT_TOOL_DEFAULT_QUEUE_CAPACITY;
  config->max_batch_calls = TURBO_AGENT_TOOL_DEFAULT_MAX_BATCH_CALLS;
  config->max_arguments_bytes = TURBO_AGENT_TOOL_DEFAULT_MAX_ARGUMENT_BYTES;
  config->max_output_bytes = TURBO_AGENT_TOOL_DEFAULT_MAX_OUTPUT_BYTES;
}

int turbo_agent_tool_executor_create(const turbo_agent_tool_executor_config_t *config,
                                     turbo_agent_tool_executor_t **out_executor) {
  turbo_agent_tool_executor_config_t effective;
  cmeta_threadpool_config_t pool_config;
  turbo_agent_tool_executor_t *executor;
  if (!out_executor) return SALTS_EINVAL;
  *out_executor = NULL;
  if (config) {
    effective = *config;
  } else {
    turbo_agent_tool_executor_config_init(&effective);
  }
  if (!turbo_agent_tool_executor_config_valid(&effective)) return SALTS_EINVAL;
  executor = (turbo_agent_tool_executor_t *)calloc(1, sizeof(*executor));
  if (!executor) return SALTS_ENOMEM;
  executor->config = effective;
  pool_config.num_threads = (int)effective.max_workers;
  pool_config.queue_capacity = effective.queue_capacity;
  executor->pool = cmeta_threadpool_create_with_config(&pool_config);
  if (!executor->pool) {
    free(executor);
    return SALTS_ENOMEM;
  }
  cmeta_mutex_init(&executor->batch_mutex);
  if (!executor->batch_mutex) {
    cmeta_threadpool_destroy(executor->pool);
    free(executor);
    return SALTS_ENOMEM;
  }
  cmeta_cond_init(&executor->batch_changed);
  if (!executor->batch_changed) {
    cmeta_mutex_destroy(&executor->batch_mutex);
    cmeta_threadpool_destroy(executor->pool);
    free(executor);
    return SALTS_ENOMEM;
  }
  cmeta_mutex_init(&executor->observation_mutex);
  if (!executor->observation_mutex) {
    cmeta_cond_destroy(&executor->batch_changed);
    cmeta_mutex_destroy(&executor->batch_mutex);
    cmeta_threadpool_destroy(executor->pool);
    free(executor);
    return SALTS_ENOMEM;
  }
  *out_executor = executor;
  return SALTS_OK;
}

void turbo_agent_tool_executor_destroy(turbo_agent_tool_executor_t *executor) {
  if (!executor) return;
  cmeta_threadpool_destroy(executor->pool);
  cmeta_mutex_destroy(&executor->observation_mutex);
  cmeta_cond_destroy(&executor->batch_changed);
  cmeta_mutex_destroy(&executor->batch_mutex);
  free(executor);
}

int turbo_agent_tool_executor_configure(turbo_agent_t *agent,
                                        const turbo_agent_tool_executor_config_t *config) {
  turbo_agent_tool_executor_t *replacement = NULL;
  turbo_agent_tool_executor_t *previous;
  int rc;
  if (!agent) return SALTS_EINVAL;
  rc = turbo_agent_tool_executor_create(config, &replacement);
  if (rc != SALTS_OK) return rc;
  previous = agent->tool_executor;
  agent->tool_executor = replacement;
  turbo_agent_tool_executor_destroy(previous);
  return SALTS_OK;
}

static char *turbo_agent_tool_sha256_parts(const char *first, const char *second) {
  static const char hex[] = "0123456789abcdef";
  cmeta_sha256_stream *sha = NULL;
  unsigned char digest[SALTS_SHA256_DIGEST_BYTES];
  char *id;
  size_t index;
  if (!first || !second || cmeta_sha256_stream_create(&sha) != SALTS_OK)
    return NULL;
  /* Persisted IDs include the first string's NUL as a field separator. */
  if (cmeta_sha256_stream_update(sha, first, strlen(first) + 1) != SALTS_OK ||
      cmeta_sha256_stream_update(sha, second, strlen(second)) != SALTS_OK ||
      cmeta_sha256_stream_finish(sha, digest) != SALTS_OK) {
    cmeta_sha256_stream_destroy(sha);
    return NULL;
  }
  cmeta_sha256_stream_destroy(sha);
  id = (char *)malloc(SALTS_SHA256_DIGEST_BYTES * 2 + 1);
  if (!id) return NULL;
  for (index = 0; index < SALTS_SHA256_DIGEST_BYTES; ++index) {
    id[index * 2] = hex[digest[index] >> 4];
    id[index * 2 + 1] = hex[digest[index] & 0x0f];
  }
  id[SALTS_SHA256_DIGEST_BYTES * 2] = '\0';
  return id;
}

static char *turbo_agent_tool_journal_id(const char *run_id, const char *turn_key,
                                         const char *call_id) {
  char *run_turn_hash = turbo_agent_tool_sha256_parts(run_id, turn_key);
  char *journal_id;
  if (!run_turn_hash) return NULL;
  journal_id = turbo_agent_tool_sha256_parts(run_turn_hash, call_id);
  free(run_turn_hash);
  return journal_id;
}

static int turbo_agent_tool_journal_record(const char *journal_id, const char *thread_id,
                                           const char *run_id,
                                           const turbo_agent_tool_execution_t *call,
                                           const char *phase, json_value_t **out_record) {
  json_value_t *record;
  char *arguments_hash;
  char timestamp[32];
  int rc = SALTS_ENOMEM;
  if (out_record) *out_record = NULL;
  if (!journal_id || !thread_id || !run_id || !call || !phase || !out_record ||
      turbo_agent_runtime_make_timestamp(timestamp, sizeof(timestamp)) != 0)
    return SALTS_EINVAL;
  arguments_hash = turbo_agent_tool_sha256_parts(call->tool_name, call->arguments_json);
  record = json_create_object();
  if (!arguments_hash || !record) {
    free(arguments_hash);
    turbo_runtime_json_destroy(record);
    return SALTS_ENOMEM;
  }
  /* Never publish a phase marker without the identity and result it describes. */
  if (turbo_runtime_json_object_set_int64(record, "schema_version",
                                          TURBO_AGENT_TOOL_JOURNAL_SCHEMA_VERSION) != 0 ||
      turbo_runtime_json_object_set_string(record, "journal_id", journal_id) != 0 ||
      turbo_runtime_json_object_set_string(record, "thread_id", thread_id) != 0 ||
      turbo_runtime_json_object_set_string(record, "run_id", run_id) != 0 ||
      turbo_runtime_json_object_set_string(record, "call_id", call->call_id) != 0 ||
      turbo_runtime_json_object_set_string(record, "turn_key", call->turn_key) != 0 ||
      turbo_runtime_json_object_set_string(record, "tool_name", call->tool_name) != 0 ||
      turbo_runtime_json_object_set_string(record, "arguments_hash", arguments_hash) != 0 ||
      turbo_runtime_json_object_set_int64(record, "execution_mode", call->policy.mode) != 0 ||
      turbo_runtime_json_object_set_int64(record, "idempotency", call->policy.idempotency) != 0 ||
      turbo_runtime_json_object_set_string(record, "phase", phase) != 0 ||
      turbo_runtime_json_object_set_string(record, "updated_at", timestamp) != 0 ||
      turbo_runtime_json_object_set_int64(record, "status", call->status) != 0)
    goto cleanup;
  if (strcmp(phase, "committed") == 0 &&
      ((call->output &&
        turbo_runtime_json_object_set_string(record, "output", call->output) != 0) ||
       (call->detail && turbo_runtime_json_object_set_clone(record, "detail", call->detail) != 0)))
    goto cleanup;
  *out_record = record;
  record = NULL;
  rc = SALTS_OK;
cleanup:
  free(arguments_hash);
  turbo_runtime_json_destroy(record);
  return rc;
}

static int turbo_agent_tool_journal_write(turbo_agent_runtime_t *runtime, const char *journal_id,
                                          const char *thread_id, const char *run_id,
                                          const turbo_agent_tool_execution_t *call,
                                          const char *phase) {
  json_value_t *record = NULL;
  int rc;
  if (!runtime) return SALTS_OK;
  rc = turbo_agent_tool_journal_record(journal_id, thread_id, run_id, call, phase, &record);
  if (rc == SALTS_OK)
    rc = turbo_agent_runtime_store_put_json(runtime, TURBO_AGENT_TOOL_JOURNAL_COLLECTION,
                                            journal_id, record) == 0
             ? SALTS_OK
             : SALTS_EIO;
  turbo_runtime_json_destroy(record);
  return rc;
}

static int turbo_agent_tool_journal_create(turbo_agent_runtime_t *runtime,
                                           const char *collection, const char *journal_id,
                                           const char *thread_id, const char *run_id,
                                           const turbo_agent_tool_execution_t *call,
                                           const char *phase) {
  json_value_t *record = NULL;
  int rc;
  if (!runtime) return SALTS_OK;
  rc = turbo_agent_tool_journal_record(journal_id, thread_id, run_id, call, phase, &record);
  if (rc == SALTS_OK)
    rc = turbo_agent_runtime_store_create_json_if_absent(runtime, collection, journal_id, record);
  turbo_runtime_json_destroy(record);
  return rc;
}

static int turbo_agent_tool_journal_string_equals(const json_value_t *record, const char *key,
                                                  const char *expected) {
  const json_value_t *value = json_object_get(record, key);
  const size_t length = strlen(expected);
  return value && json_type(value) == JSON_STRING && json_string_len(value) == length &&
         memcmp(json_string(value), expected, length) == 0;
}

static int turbo_agent_tool_journal_load(turbo_agent_runtime_t *runtime, const char *journal_id,
                                         const char *thread_id, const char *run_id,
                                         const turbo_agent_tool_execution_t *call,
                                         json_value_t **out_record) {
  json_value_t *records = NULL;
  json_value_t *record = NULL;
  const json_value_t *output;
  char *arguments_hash = NULL;
  int rc;
  *out_record = NULL;
  if (!runtime) return SALTS_ENOENT;
  /* The store key is authoritative. Filtering only by an embedded identity
   * would mistake a damaged existing record for a never-started invocation. */
  rc = turbo_agent_runtime_store_get_json_status(runtime, TURBO_AGENT_TOOL_JOURNAL_COLLECTION,
                                                 journal_id, &record);
  if (rc == SALTS_ENOENT) return rc;
  if (rc != SALTS_OK && rc != -1) return rc;
  if (rc == -1) {
    /* Only legacy callbacks with unclassified errors need this compatibility
     * query. Typed I/O/corruption failures must never be treated as absence. */
    if (turbo_agent_runtime_store_list_json(runtime, TURBO_AGENT_TOOL_JOURNAL_COLLECTION,
                                            "journal_id", journal_id, &records) != 0)
      return SALTS_EIO;
    rc = json_array_size(records) == 0 ? SALTS_ENOENT : SALTS_EIO;
    turbo_runtime_json_destroy(records);
    return rc;
  }
  output = json_object_get(record, "output");
  arguments_hash = turbo_agent_tool_sha256_parts(call->tool_name, call->arguments_json);
  if (!arguments_hash) {
    turbo_runtime_json_destroy(record);
    return SALTS_ENOMEM;
  }
  if (turbo_runtime_json_value_as_int64(json_object_get(record, "schema_version"), INT64_MIN) !=
          TURBO_AGENT_TOOL_JOURNAL_SCHEMA_VERSION ||
      !turbo_agent_tool_journal_string_equals(record, "journal_id", journal_id) ||
      !turbo_agent_tool_journal_string_equals(record, "thread_id", thread_id) ||
      !turbo_agent_tool_journal_string_equals(record, "run_id", run_id) ||
      !turbo_agent_tool_journal_string_equals(record, "call_id", call->call_id) ||
      !turbo_agent_tool_journal_string_equals(record, "turn_key", call->turn_key) ||
      !turbo_agent_tool_journal_string_equals(record, "tool_name", call->tool_name) ||
      !turbo_agent_tool_journal_string_equals(record, "arguments_hash", arguments_hash) ||
      turbo_runtime_json_value_as_int64(json_object_get(record, "execution_mode"), INT64_MIN) !=
          call->policy.mode ||
      turbo_runtime_json_value_as_int64(json_object_get(record, "idempotency"), INT64_MIN) !=
          call->policy.idempotency ||
      !turbo_agent_tool_journal_status_valid(
          turbo_runtime_json_value_as_int64(json_object_get(record, "status"), INT64_MAX)) ||
      (output && (json_type(output) != JSON_STRING ||
                  json_string_len(output) != strlen(json_string(output))))) {
    free(arguments_hash);
    turbo_runtime_json_destroy(record);
    return SALTS_EPROTO;
  }
  free(arguments_hash);
  *out_record = record;
  return SALTS_OK;
}

static turbo_tool_status_t
turbo_agent_tool_cancel_status(const turbo_cancel_token_t *cancel_token) {
  int rc;
  if (!cancel_token) return TURBO_TOOL_OK;
  rc = turbo_cancel_token_check(cancel_token);
  if (rc == SALTS_OK) return TURBO_TOOL_OK;
  return rc == SALTS_ETIMEDOUT ? TURBO_TOOL_DEADLINE_EXCEEDED : TURBO_TOOL_CANCELLED;
}

static uint64_t
turbo_agent_tool_batch_wait_ns(const turbo_cancel_token_t *cancel_token) {
  uint64_t wait_ms = TURBO_AGENT_TOOL_BATCH_WAIT_SLICE_MS;
  uint64_t deadline_ms;
  uint64_t now_ms;
  uint64_t remaining_ms;
  if (!cancel_token) return 0;
  deadline_ms = turbo_cancel_token_deadline_mono_ms(cancel_token);
  if (!deadline_ms) return wait_ms * TURBO_AGENT_TOOL_MILLISECONDS_TO_NANOSECONDS;
  now_ms = cmeta_monotonic_ms();
  if (now_ms >= deadline_ms) return 0;
  remaining_ms = deadline_ms - now_ms;
  if (remaining_ms < wait_ms) wait_ms = remaining_ms;
  return wait_ms * TURBO_AGENT_TOOL_MILLISECONDS_TO_NANOSECONDS;
}

static turbo_tool_status_t turbo_agent_tool_batch_acquire(
    turbo_agent_tool_executor_t *executor,
    const turbo_cancel_token_t *cancel_token) {
  int waited = 0;
  cmeta_mutex_lock(&executor->batch_mutex);
  for (;;) {
    turbo_tool_status_t status;
    uint64_t wait_ns;
    /* Preserve the established uncontended path: a token that was already
     * stopped still reaches durable planning, then the callback boundary
     * records cancellation. Only a caller that actually queued can leave the
     * gate before Journal admission. */
    if (waited) {
      status = turbo_agent_tool_cancel_status(cancel_token);
      if (status != TURBO_TOOL_OK) {
        /* A cancelled waiter may have consumed the owner's signal after the
         * gate became idle. Pass that wake-up to the next waiter so a
         * token-free caller cannot remain asleep with no active owner. */
        if (!executor->batch_active)
          cmeta_cond_signal(&executor->batch_changed);
        cmeta_mutex_unlock(&executor->batch_mutex);
        return status;
      }
    }
    if (!executor->batch_active) {
      executor->batch_active = 1;
      cmeta_mutex_unlock(&executor->batch_mutex);
      return TURBO_TOOL_OK;
    }
    waited = 1;
    if (!cancel_token) {
      cmeta_cond_wait(&executor->batch_changed, &executor->batch_mutex);
      continue;
    }
    wait_ns = turbo_agent_tool_batch_wait_ns(cancel_token);
    if (!wait_ns) continue;
    (void)cmeta_cond_timedwait(&executor->batch_changed,
                               &executor->batch_mutex, wait_ns);
  }
}

static void turbo_agent_tool_batch_release(
    turbo_agent_tool_executor_t *executor) {
  cmeta_mutex_lock(&executor->batch_mutex);
  executor->batch_active = 0;
  cmeta_cond_signal(&executor->batch_changed);
  cmeta_mutex_unlock(&executor->batch_mutex);
}

/* Check the entire model-originating batch before publishing any Journal record.
 * Schema/Capability admission is handled separately; this structural gate
 * prevents invalid JSON from reaching a callback or entering the durable log.
 * Callbacks still consume the original, unchanged arguments text. */
static int turbo_agent_tool_arguments_valid(const turbo_tool_registry_t *registry,
                                            const char *canonical_tool_name,
                                            const char *arguments_json) {
  json_value_t *arguments = NULL;
  turbo_tool_definition_t definition = {0};
  size_t index;
  int valid = 0;
  if (!registry || !canonical_tool_name || !arguments_json) return 0;
  arguments = json_parse(arguments_json, strlen(arguments_json));
  if (!arguments || json_type(arguments) != JSON_OBJECT) goto done;
  /* Use the same RuntimeTools schema validator as AgentCompiler; the lookup
   * consumes only borrowed canonical registry metadata, never a new authority.
   * All arguments are checked after middleware and before any journal write. */
  for (index = 0; index < turbo_tool_registry_count(registry); ++index) {
    if (turbo_tool_registry_get_definition(registry, index, &definition) != TURBO_TOOL_OK)
      goto done;
    if (definition.name && strcmp(definition.name, canonical_tool_name) == 0) {
      valid = turbo_tool_schema_validate_arguments_json_value(
          &definition, arguments, NULL, 0) == TURBO_TOOL_SCHEMA_VALID;
      break;
    }
  }
done:
  json_free(arguments);
  return valid;
}

static int turbo_agent_tool_prepare_journal(turbo_agent_tool_executor_t *executor,
                                            turbo_agent_runtime_t *runtime, const char *thread_id,
                                            const char *run_id,
                                            turbo_agent_tool_execution_t *call) {
  json_value_t *record = NULL;
  const char *output;
  char *journal_id;
  int rc;
  if (!runtime) return SALTS_OK;
  journal_id = turbo_agent_tool_journal_id(run_id, call->turn_key, call->call_id);
  if (!journal_id) return SALTS_ENOMEM;
  rc = turbo_agent_tool_journal_load(runtime, journal_id, thread_id, run_id, call, &record);
  if (rc == SALTS_ENOENT && call->legacy_turn_key) {
    turbo_agent_tool_execution_t legacy_call = *call;
    char *legacy_id;
    legacy_call.turn_key = call->legacy_turn_key;
    legacy_id = turbo_agent_tool_journal_id(run_id, legacy_call.turn_key, call->call_id);
    if (!legacy_id) { free(journal_id); return SALTS_ENOMEM; }
    rc = turbo_agent_tool_journal_load(runtime, legacy_id, thread_id, run_id, &legacy_call, &record);
    free(legacy_id);
    /* A local event offset cannot establish which nested state produced an old
     * record. Never migrate/replay it into a guessed scope or repeat its effects. */
    if (rc == SALTS_OK) rc = SALTS_EBUSY;
    turbo_runtime_json_destroy(record); record = NULL;
  }
  if (rc == SALTS_ENOENT) {
    call->status = TURBO_TOOL_OK;
    rc = turbo_agent_tool_journal_create(runtime, TURBO_AGENT_TOOL_JOURNAL_COLLECTION,
                                         journal_id, thread_id, run_id, call, "planned");
    /* Runtime-bound effect admission requires an atomic planned claim.
     * A plain put cannot arbitrate independent executors or processes. */
    if (rc == SALTS_OK) {
      free(journal_id);
      return SALTS_OK;
    }
    if (rc != SALTS_EALREADY) {
      free(journal_id);
      return rc;
    }
    /* Another owner published the same identity after our initial read. Load
     * and validate its record instead of overwriting it. */
    rc = turbo_agent_tool_journal_load(runtime, journal_id, thread_id, run_id, call, &record);
  }
  free(journal_id);
  if (rc != SALTS_OK) return rc;
  if (turbo_agent_tool_journal_string_equals(record, "phase", "started")) {
    call->status = TURBO_TOOL_UNKNOWN_SIDE_EFFECT;
    call->replayed = 1;
  } else if (turbo_agent_tool_journal_string_equals(record, "phase", "committed")) {
    const json_value_t *detail;
    call->status = (turbo_tool_status_t)turbo_runtime_json_value_as_int64(
        json_object_get(record, "status"), INT64_MAX);
    output = json_get_string(record, "output");
    if (output) {
      if (json_string_len(json_object_get(record, "output")) > executor->config.max_output_bytes) {
        call->status = TURBO_TOOL_OUTPUT_LIMIT;
      } else {
        call->output = turbo_agent_tool_strdup(output);
        if (!call->output) rc = SALTS_ENOMEM;
      }
    }
    detail = json_object_get(record, "detail");
    if (rc == SALTS_OK && detail) {
      call->detail = json_clone(detail);
      if (!call->detail) rc = SALTS_ENOMEM;
    }
    call->replayed = 1;
  } else if (!turbo_agent_tool_journal_string_equals(record, "phase", "planned")) {
    rc = SALTS_EPROTO;
  }
  turbo_runtime_json_destroy(record);
  return rc;
}

static void turbo_agent_tool_event_sink(const json_value_t *event, void *user_data) {
  turbo_agent_tool_task_t *task = (turbo_agent_tool_task_t *)user_data;
  if (!task || !event || !task->event_sink) return;
  cmeta_mutex_lock(&task->executor->observation_mutex);
  task->event_sink(event, task->event_sink_user_data);
  cmeta_mutex_unlock(&task->executor->observation_mutex);
}

static void turbo_agent_tool_detail_sink(const json_value_t *detail, void *user_data) {
  turbo_agent_tool_task_t *task = (turbo_agent_tool_task_t *)user_data;
  json_value_t *copy;
  if (!task || !detail || !task->call) return;
  copy = json_clone(detail);
  if (!copy) {
    task->call->detail_capture_failed = 1;
    return;
  }
  cmeta_mutex_lock(&task->executor->observation_mutex);
  turbo_runtime_json_destroy(task->call->detail);
  task->call->detail = copy;
  cmeta_mutex_unlock(&task->executor->observation_mutex);
}

static void turbo_agent_tool_execute_one(turbo_agent_tool_task_t *task) {
  turbo_agent_execution_context_t saved = {0};
  turbo_tool_status_t cancel_status;
  char *journal_id = NULL;
  int claim_rc;
  if (!task || !task->call || task->call->replayed) return;
  turbo_agent_execution_context_get(&saved);
  turbo_agent_execution_context_set(&task->context);
  turbo_agent_current_tool_approval_set(task->call->approval_granted);
  cancel_status = turbo_agent_tool_cancel_status(task->cancel_token);
  if (cancel_status != TURBO_TOOL_OK) {
    task->call->status = cancel_status;
    goto cleanup;
  }
  if (task->runtime) {
    journal_id = turbo_agent_tool_journal_id(task->context.run_id, task->call->turn_key,
                                             task->call->call_id);
    if (!journal_id) {
      task->call->status = TURBO_TOOL_OUT_OF_MEMORY;
      goto cleanup;
    }
    claim_rc = turbo_agent_tool_journal_create(
        task->runtime, TURBO_AGENT_TOOL_CLAIM_COLLECTION, journal_id,
        task->context.thread_id, task->context.run_id, task->call, "admitted");
    if (claim_rc == SALTS_EALREADY) {
      /* A durable backend-entry fence may race the main Journal transition.
       * Reuse a committed/started outcome when visible; a still-planned record
       * is conservatively UNKNOWN because the winning owner may enter later. */
      int replay_rc = turbo_agent_tool_prepare_journal(
          task->executor, task->runtime, task->context.thread_id,
          task->context.run_id, task->call);
      if (replay_rc != SALTS_OK || !task->call->replayed) {
        free(task->call->output);
        task->call->output = NULL;
        turbo_runtime_json_destroy(task->call->detail);
        task->call->detail = NULL;
        task->call->status = TURBO_TOOL_UNKNOWN_SIDE_EFFECT;
        task->call->replayed = 1;
      }
      goto cleanup;
    }
    if (claim_rc != SALTS_OK) {
      /* Unsupported atomic admission enters no callback. Other create errors
       * can hide a committed fence, so their disposition remains UNKNOWN. */
      task->call->status = claim_rc == SALTS_ENOTSUP
                               ? TURBO_TOOL_ERROR
                               : TURBO_TOOL_UNKNOWN_SIDE_EFFECT;
      task->call->replayed = 1;
      goto cleanup;
    }
    /* The claim is already durable, but backend entry has not happened.
     * A cancellation published by Store admission must not enter the tool.
     * Leave the PLANNED record and ADMITTED claim untouched: a second owner
     * conservatively reports UNKNOWN rather than redispatching this identity. */
    cancel_status = turbo_agent_tool_cancel_status(task->cancel_token);
    if (cancel_status != TURBO_TOOL_OK) {
      task->call->status = cancel_status;
      goto cleanup;
    }
    if (turbo_agent_tool_journal_write(task->runtime, journal_id,
                                       task->context.thread_id, task->context.run_id,
                                       task->call, "started") != SALTS_OK) {
      /* The durable claim may already be visible to another owner. */
      task->call->status = TURBO_TOOL_UNKNOWN_SIDE_EFFECT;
      task->call->replayed = 1;
      goto cleanup;
    }
  }
  /* Recheck after durable STARTED and directly before synchronous backend
   * dispatch. Cancellation during journal persistence previously passed the
   * initial check and could enter a callback despite a stopped token.
   * A STARTED+ADMITTED identity remains conservatively UNKNOWN on recovery;
   * never erase the persisted fence or retry the effect automatically. */
  cancel_status = turbo_agent_tool_cancel_status(task->cancel_token);
  if (cancel_status != TURBO_TOOL_OK) {
    task->call->status = cancel_status;
    goto cleanup;
  }
  {
    turbo_tool_execution_context_t tool_context = {0};
    tool_context.struct_size = sizeof(tool_context);
    tool_context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
    tool_context.cancel_token = task->cancel_token;
    tool_context.deadline_mono_ms =
        task->cancel_token ? turbo_cancel_token_deadline_mono_ms(task->cancel_token) : 0;
    tool_context.thread_id = task->context.thread_id;
    tool_context.run_id = task->context.run_id;
    tool_context.turn_id = task->call->turn_key;
    tool_context.tool_call_id = task->call->call_id;
    tool_context.event_sink = task->event_sink ? turbo_agent_tool_event_sink : NULL;
    tool_context.event_sink_user_data = task->event_sink ? task : NULL;
    tool_context.detail_sink = turbo_agent_tool_detail_sink;
    tool_context.detail_sink_user_data = task;
    task->call->status = turbo_tool_registry_execute_with_context(
        task->registry, task->call->tool_name, task->call->arguments_json, &tool_context,
        &task->call->output);
  }
  if (task->call->detail_capture_failed) {
    free(task->call->output);
    task->call->output = NULL;
    task->call->status = TURBO_TOOL_OUT_OF_MEMORY;
  }
  if (task->call->output && strlen(task->call->output) > task->executor->config.max_output_bytes) {
    free(task->call->output);
    task->call->output = NULL;
    task->call->status = TURBO_TOOL_OUTPUT_LIMIT;
  }
  if (task->runtime &&
      turbo_agent_tool_journal_write(task->runtime, journal_id, task->context.thread_id,
                                     task->context.run_id, task->call, "committed") != SALTS_OK) {
    free(task->call->output);
    task->call->output = NULL;
    task->call->status = TURBO_TOOL_UNKNOWN_SIDE_EFFECT;
  }
cleanup:
  free(journal_id);
  turbo_agent_execution_context_set(&saved);
}

static void turbo_agent_tool_worker(void *arg) {
  turbo_agent_tool_execute_one((turbo_agent_tool_task_t *)arg);
}

static int turbo_agent_tool_execute_parallel_group(
    turbo_agent_tool_executor_t *executor, turbo_agent_runtime_t *runtime,
    const turbo_cancel_token_t *cancel_token, const turbo_tool_registry_t *registry,
    turbo_agent_tool_execution_t *calls, size_t begin, size_t end,
    const turbo_agent_execution_context_t *base_context,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data) {
  size_t wave_begin;
  for (wave_begin = begin; wave_begin < end; wave_begin += executor->config.max_workers) {
    turbo_agent_tool_task_t *tasks;
    size_t wave_end = wave_begin + executor->config.max_workers;
    size_t index;
    if (wave_end > end) wave_end = end;
    tasks = (turbo_agent_tool_task_t *)calloc(wave_end - wave_begin, sizeof(*tasks));
    if (!tasks) return SALTS_ENOMEM;
    for (index = wave_begin; index < wave_end; ++index) {
      turbo_agent_tool_task_t *task = &tasks[index - wave_begin];
      task->executor = executor;
      task->runtime = runtime;
      task->cancel_token = cancel_token;
      task->registry = registry;
      task->call = &calls[index];
      task->context = *base_context;
      task->event_sink = event_sink;
      task->event_sink_user_data = event_sink_user_data;
      task->context.tool_call_id = calls[index].call_id;
      task->context.tool_name = calls[index].tool_name;
      if (cmeta_threadpool_try_submit(executor->pool, turbo_agent_tool_worker, task) != 0) {
        calls[index].status = TURBO_TOOL_BACKPRESSURE;
        calls[index].replayed = 1;
      }
    }
    cmeta_threadpool_wait(executor->pool);
    free(tasks);
  }
  return SALTS_OK;
}

int turbo_agent_tool_executor_execute(turbo_agent_tool_executor_t *executor,
                                      turbo_agent_runtime_t *runtime,
                                      const turbo_cancel_token_t *cancel_token,
                                      const char *thread_id, const char *run_id,
                                      turbo_event_sink_json_value_fn event_sink,
                                      void *event_sink_user_data,
                                      const turbo_tool_registry_t *registry,
                                      const turbo_agent_policy_t *policy,
                                      turbo_agent_tool_execution_t *calls, size_t call_count) {
  turbo_agent_execution_context_t base_context = {0};
  turbo_tool_status_t queue_status;
  size_t index;
  int rc = SALTS_OK;
  if (!executor || !registry || !calls || call_count == 0 ||
      call_count > executor->config.max_batch_calls || (runtime && (!thread_id || !run_id)))
    return SALTS_EINVAL;
  for (index = 0; index < call_count; ++index) {
    if (!calls[index].call_id || !calls[index].turn_key || !calls[index].tool_name ||
        !calls[index].arguments_json ||
        strlen(calls[index].arguments_json) > executor->config.max_arguments_bytes)
      return SALTS_EMSGSIZE;
    /* run_id is shared by this batch; turn_key + call_id completes the effect
     * identity. Reject collisions before any Journal preparation or callback,
     * even for denied/replayed entries or dispatch without a Runtime. Tool
     * names, aliases and arguments cannot distinguish the same effect key. */
    for (size_t previous = 0; previous < index; ++previous) {
      if (strcmp(calls[previous].turn_key, calls[index].turn_key) == 0 &&
          strcmp(calls[previous].call_id, calls[index].call_id) == 0)
        return SALTS_EPROTO;
    }
  }
  queue_status = turbo_agent_tool_batch_acquire(executor, cancel_token);
  if (queue_status != TURBO_TOOL_OK) {
    for (index = 0; index < call_count; ++index) {
      if (!calls[index].replayed) calls[index].status = queue_status;
    }
    return SALTS_OK;
  }
  /* The executor is also called directly by trusted host code. Never trust
   * a caller-supplied alias or execution policy as the durable identity:
   * RuntimeTools' registered canonical name and policy are authoritative. */
  for (index = 0; index < call_count; ++index) {
    const char *canonical_name = NULL;
    turbo_tool_execution_policy_t registered_policy = {0};
    turbo_tool_status_t status;
    if (calls[index].replayed) continue;
    status = turbo_tool_registry_resolve_name(
        registry, calls[index].tool_name, &canonical_name);
    if (status == TURBO_TOOL_NOT_FOUND) {
      calls[index].status = TURBO_TOOL_NOT_FOUND;
      calls[index].replayed = 1;
      continue;
    }
    if (status != TURBO_TOOL_OK || !canonical_name ||
        turbo_tool_registry_get_execution_policy(
            registry, canonical_name, &registered_policy) != TURBO_TOOL_OK) {
      rc = SALTS_EPROTO;
      goto cleanup;
    }
    calls[index].tool_name = canonical_name;
    calls[index].policy = registered_policy;
  }
  for (index = 0; index < call_count; ++index) {
    const char *reason = NULL;
    const char *const *required_capabilities = NULL;
    size_t required_capability_count = 0;
    if (calls[index].status == TURBO_TOOL_NOT_FOUND) continue;
    rc = turbo_tool_registry_get_required_capabilities(
        registry, calls[index].tool_name, &required_capabilities, &required_capability_count);
    if (rc != TURBO_TOOL_OK) {
      calls[index].status = TURBO_TOOL_ERROR;
      calls[index].replayed = 1;
      calls[index].policy_reason = "tool_capability_metadata_unavailable";
      continue;
    }
    if (turbo_agent_policy_check_tool(policy, registry, calls[index].tool_name, &reason) !=
        TURBO_AGENT_POLICY_ALLOW) {
      calls[index].status = TURBO_TOOL_ERROR;
      calls[index].replayed = 1;
      calls[index].policy_reason = reason ? reason : "tool_policy_denied";
    }
  }
  /* Do not create a partial planned batch if a later call has malformed
   * JSON. In particular, before_tool middleware may have changed arguments
   * after the Workflow node's initial preflight. */
  for (index = 0; index < call_count; ++index) {
    if (!calls[index].replayed &&
        !turbo_agent_tool_arguments_valid(
            registry, calls[index].tool_name, calls[index].arguments_json)) {
      rc = SALTS_EPROTO;
      goto cleanup;
    }
  }
  for (index = 0; index < call_count; ++index) {
    if (calls[index].replayed) continue;
    rc = turbo_agent_tool_prepare_journal(executor, runtime, thread_id, run_id, &calls[index]);
    if (rc != SALTS_OK) goto cleanup;
  }
  turbo_agent_execution_context_get(&base_context);
  if (runtime) {
    base_context.thread_id = thread_id;
    base_context.run_id = run_id;
    base_context.runtime = runtime;
  }
  base_context.cancel_token = cancel_token;
  index = 0;
  while (index < call_count) {
    if (calls[index].policy.mode == TURBO_TOOL_EXECUTION_PARALLEL_SAFE) {
      size_t end = index + 1;
      while (end < call_count && calls[end].policy.mode == TURBO_TOOL_EXECUTION_PARALLEL_SAFE)
        ++end;
      rc = turbo_agent_tool_execute_parallel_group(
          executor, runtime, cancel_token, registry, calls,
          index, end, &base_context, event_sink, event_sink_user_data);
      if (rc != SALTS_OK) goto cleanup;
      index = end;
    } else {
      turbo_agent_tool_task_t task = {0};
      task.executor = executor;
      task.runtime = runtime;
      task.cancel_token = cancel_token;
      task.registry = registry;
      task.call = &calls[index];
      task.context = base_context;
      task.event_sink = event_sink;
      task.event_sink_user_data = event_sink_user_data;
      task.context.tool_call_id = calls[index].call_id;
      task.context.tool_name = calls[index].tool_name;
      turbo_agent_tool_execute_one(&task);
      ++index;
    }
  }
cleanup:
  turbo_agent_tool_batch_release(executor);
  return rc;
}
