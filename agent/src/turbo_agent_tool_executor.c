#include "turbo_agent_tool_executor_internal.h"
#include "turbo_agent_tool_journal_internal.h"

#include "turbo_agent_core_internal.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_runtime_store.h"
#include "turbo_agent_util_internal.h"

#include <cmeta_crypto.h>
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

struct turbo_agent_tool_executor_s {
  turbo_agent_tool_executor_config_t config;
  cmeta_threadpool_t *pool;
  cmeta_mutex_t batch_mutex;
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
  cmeta_mutex_init(&executor->observation_mutex);
  *out_executor = executor;
  return SALTS_OK;
}

void turbo_agent_tool_executor_destroy(turbo_agent_tool_executor_t *executor) {
  if (!executor) return;
  cmeta_threadpool_destroy(executor->pool);
  cmeta_mutex_destroy(&executor->observation_mutex);
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

static int turbo_agent_tool_journal_write(turbo_agent_runtime_t *runtime, const char *journal_id,
                                          const char *thread_id, const char *run_id,
                                          const turbo_agent_tool_execution_t *call,
                                          const char *phase) {
  json_value_t *record;
  char *arguments_hash;
  char timestamp[32];
  int rc = SALTS_ENOMEM;
  if (!runtime) return SALTS_OK;
  if (!journal_id || !thread_id || !run_id || !call || !phase ||
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
  rc = turbo_agent_runtime_store_put_json(runtime, TURBO_AGENT_TOOL_JOURNAL_COLLECTION, journal_id,
                                          record) == 0
           ? SALTS_OK
           : SALTS_EIO;
cleanup:
  free(arguments_hash);
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
    rc = turbo_agent_tool_journal_write(runtime, journal_id, thread_id, run_id, call, "planned");
    free(journal_id);
    return rc;
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
    if (!journal_id ||
        turbo_agent_tool_journal_write(task->runtime, journal_id, task->context.thread_id,
                                       task->context.run_id, task->call, "started") != SALTS_OK) {
      task->call->status = TURBO_TOOL_ERROR;
      goto cleanup;
    }
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
  }
  cmeta_mutex_lock(&executor->batch_mutex);
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
  cmeta_mutex_unlock(&executor->batch_mutex);
  return rc;
}
