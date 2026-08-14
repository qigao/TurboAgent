#include "turbo_agent_tool_executor_internal.h"

#include "turbo_agent_core_internal.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_runtime_v1_internal.h"

#include <openssl/sha.h>
#include <turbo_thread.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define TURBO_AGENT_TOOL_JOURNAL_COLLECTION "agent_tool_journal"
#define TURBO_AGENT_TOOL_JOURNAL_SCHEMA_VERSION 1
#define TURBO_AGENT_TOOL_DEFAULT_WORKERS 4u
#define TURBO_AGENT_TOOL_DEFAULT_QUEUE_CAPACITY 8u
#define TURBO_AGENT_TOOL_DEFAULT_MAX_BATCH_CALLS 64u
#define TURBO_AGENT_TOOL_DEFAULT_MAX_ARGUMENT_BYTES (1024u * 1024u)
#define TURBO_AGENT_TOOL_DEFAULT_MAX_OUTPUT_BYTES (1024u * 1024u)

struct turbo_agent_tool_executor_s {
  turbo_agent_tool_executor_config_t config;
  turbo_threadpool_t *pool;
  turbo_mutex_t batch_mutex;
};

typedef struct turbo_agent_tool_task_s {
  turbo_agent_tool_executor_t *executor;
  turbo_agent_runtime_t *runtime;
  const turbo_cancel_token_t *cancel_token;
  const turbo_tool_registry_t *registry;
  turbo_agent_tool_execution_t *call;
  turbo_agent_execution_context_t context;
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
  turbo_threadpool_config_t pool_config;
  turbo_agent_tool_executor_t *executor;
  if (!out_executor) return TURBO_EINVAL;
  *out_executor = NULL;
  if (config) {
    effective = *config;
  } else {
    turbo_agent_tool_executor_config_init(&effective);
  }
  if (!turbo_agent_tool_executor_config_valid(&effective)) return TURBO_EINVAL;
  executor = (turbo_agent_tool_executor_t *)calloc(1, sizeof(*executor));
  if (!executor) return TURBO_ENOMEM;
  executor->config = effective;
  pool_config.num_threads = (int)effective.max_workers;
  pool_config.queue_capacity = effective.queue_capacity;
  executor->pool = turbo_threadpool_create_with_config(&pool_config);
  if (!executor->pool) {
    free(executor);
    return TURBO_ENOMEM;
  }
  turbo_mutex_init(&executor->batch_mutex);
  *out_executor = executor;
  return TURBO_OK;
}

void turbo_agent_tool_executor_destroy(turbo_agent_tool_executor_t *executor) {
  if (!executor) return;
  turbo_threadpool_destroy(executor->pool);
  turbo_mutex_destroy(&executor->batch_mutex);
  free(executor);
}

int turbo_agent_tool_executor_configure(turbo_agent_t *agent,
                                        const turbo_agent_tool_executor_config_t *config) {
  turbo_agent_tool_executor_t *replacement = NULL;
  turbo_agent_tool_executor_t *previous;
  int rc;
  if (!agent) return TURBO_EINVAL;
  rc = turbo_agent_tool_executor_create(config, &replacement);
  if (rc != TURBO_OK) return rc;
  previous = agent->tool_executor;
  agent->tool_executor = replacement;
  turbo_agent_tool_executor_destroy(previous);
  return TURBO_OK;
}

static char *turbo_agent_tool_sha256_parts(const char *first, const char *second) {
  static const char hex[] = "0123456789abcdef";
  SHA256_CTX sha;
  unsigned char digest[SHA256_DIGEST_LENGTH];
  char *id;
  size_t index;
  if (!first || !second || SHA256_Init(&sha) != 1 ||
      SHA256_Update(&sha, first, strlen(first) + 1) != 1 ||
      SHA256_Update(&sha, second, strlen(second)) != 1 || SHA256_Final(digest, &sha) != 1)
    return NULL;
  id = (char *)malloc(SHA256_DIGEST_LENGTH * 2 + 1);
  if (!id) return NULL;
  for (index = 0; index < SHA256_DIGEST_LENGTH; ++index) {
    id[index * 2] = hex[digest[index] >> 4];
    id[index * 2 + 1] = hex[digest[index] & 0x0f];
  }
  id[SHA256_DIGEST_LENGTH * 2] = '\0';
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
  int rc;
  if (!runtime) return TURBO_OK;
  if (!journal_id || !thread_id || !run_id || !call || !phase ||
      turbo_agent_runtime_make_timestamp(timestamp, sizeof(timestamp)) != 0)
    return TURBO_EINVAL;
  arguments_hash = turbo_agent_tool_sha256_parts(call->tool_name, call->arguments_json);
  record = turbo_json_create_object();
  if (!arguments_hash || !record) {
    free(arguments_hash);
    turbo_runtime_json_destroy(record);
    return TURBO_ENOMEM;
  }
  turbo_json_object_set_number(record, "schema_version", TURBO_AGENT_TOOL_JOURNAL_SCHEMA_VERSION);
  turbo_json_object_set_string(record, "journal_id", journal_id);
  turbo_json_object_set_string(record, "thread_id", thread_id);
  turbo_json_object_set_string(record, "run_id", run_id);
  turbo_json_object_set_string(record, "call_id", call->call_id);
  turbo_json_object_set_string(record, "turn_key", call->turn_key);
  turbo_json_object_set_string(record, "tool_name", call->tool_name);
  turbo_json_object_set_string(record, "arguments_hash", arguments_hash);
  turbo_json_object_set_number(record, "execution_mode", (double)call->policy.mode);
  turbo_json_object_set_number(record, "idempotency", (double)call->policy.idempotency);
  turbo_json_object_set_string(record, "phase", phase);
  turbo_json_object_set_string(record, "updated_at", timestamp);
  turbo_json_object_set_number(record, "status", (double)call->status);
  if (strcmp(phase, "committed") == 0 && call->output)
    turbo_json_object_set_string(record, "output", call->output);
  rc = turbo_agent_runtime_store_put_json(runtime, TURBO_AGENT_TOOL_JOURNAL_COLLECTION, journal_id,
                                          record);
  free(arguments_hash);
  turbo_runtime_json_destroy(record);
  return rc == 0 ? TURBO_OK : TURBO_EIO;
}

static int turbo_agent_tool_journal_load(turbo_agent_runtime_t *runtime, const char *journal_id,
                                         const char *thread_id, const char *run_id,
                                         const turbo_agent_tool_execution_t *call,
                                         json_value_t **out_record) {
  json_value_t *records = NULL;
  const json_value_t *record;
  const char *stored_thread;
  const char *stored_run;
  const char *stored_call;
  const char *stored_turn;
  const char *stored_tool;
  const char *stored_arguments_hash;
  char *arguments_hash = NULL;
  *out_record = NULL;
  if (!runtime) return TURBO_ENOENT;
  if (turbo_agent_runtime_store_list_json(runtime, TURBO_AGENT_TOOL_JOURNAL_COLLECTION,
                                          "journal_id", journal_id, &records) != 0)
    return TURBO_EIO;
  if (turbo_json_array_size(records) == 0) {
    turbo_runtime_json_destroy(records);
    return TURBO_ENOENT;
  }
  if (turbo_json_array_size(records) != 1) {
    turbo_runtime_json_destroy(records);
    return TURBO_EPROTO;
  }
  record = turbo_json_array_get(records, 0);
  stored_thread = turbo_json_get_string(record, "thread_id");
  stored_run = turbo_json_get_string(record, "run_id");
  stored_call = turbo_json_get_string(record, "call_id");
  stored_turn = turbo_json_get_string(record, "turn_key");
  stored_tool = turbo_json_get_string(record, "tool_name");
  stored_arguments_hash = turbo_json_get_string(record, "arguments_hash");
  arguments_hash = turbo_agent_tool_sha256_parts(call->tool_name, call->arguments_json);
  if (turbo_json_get_double(record, "schema_version", -1.0) !=
          (double)TURBO_AGENT_TOOL_JOURNAL_SCHEMA_VERSION ||
      !stored_thread || strcmp(stored_thread, thread_id) != 0 || !stored_run ||
      strcmp(stored_run, run_id) != 0 || !stored_call || strcmp(stored_call, call->call_id) != 0 ||
      !stored_turn || strcmp(stored_turn, call->turn_key) != 0 || !stored_tool ||
      strcmp(stored_tool, call->tool_name) != 0 || !stored_arguments_hash || !arguments_hash ||
      strcmp(stored_arguments_hash, arguments_hash) != 0 ||
      turbo_json_get_double(record, "execution_mode", -1.0) != (double)call->policy.mode ||
      turbo_json_get_double(record, "idempotency", -1.0) != (double)call->policy.idempotency) {
    free(arguments_hash);
    turbo_runtime_json_destroy(records);
    return TURBO_EPROTO;
  }
  free(arguments_hash);
  *out_record = turbo_json_clone(record);
  turbo_runtime_json_destroy(records);
  return *out_record ? TURBO_OK : TURBO_ENOMEM;
}

static turbo_tool_status_t
turbo_agent_tool_cancel_status(const turbo_cancel_token_t *cancel_token) {
  int rc;
  if (!cancel_token) return TURBO_TOOL_OK;
  rc = turbo_cancel_token_check(cancel_token);
  if (rc == TURBO_OK) return TURBO_TOOL_OK;
  return rc == TURBO_ETIMEDOUT ? TURBO_TOOL_DEADLINE_EXCEEDED : TURBO_TOOL_CANCELLED;
}

static int turbo_agent_tool_prepare_journal(turbo_agent_tool_executor_t *executor,
                                            turbo_agent_runtime_t *runtime, const char *thread_id,
                                            const char *run_id,
                                            turbo_agent_tool_execution_t *call) {
  json_value_t *record = NULL;
  const char *phase;
  const char *output;
  char *journal_id;
  int rc;
  (void)executor;
  if (!runtime) return TURBO_OK;
  journal_id = turbo_agent_tool_journal_id(run_id, call->turn_key, call->call_id);
  if (!journal_id) return TURBO_ENOMEM;
  rc = turbo_agent_tool_journal_load(runtime, journal_id, thread_id, run_id, call, &record);
  if (rc == TURBO_ENOENT) {
    call->status = TURBO_TOOL_OK;
    rc = turbo_agent_tool_journal_write(runtime, journal_id, thread_id, run_id, call, "planned");
    free(journal_id);
    return rc;
  }
  free(journal_id);
  if (rc != TURBO_OK) return rc;
  phase = turbo_json_get_string(record, "phase");
  if (!phase) {
    turbo_runtime_json_destroy(record);
    return TURBO_EPROTO;
  }
  if (strcmp(phase, "started") == 0) {
    call->status = TURBO_TOOL_UNKNOWN_SIDE_EFFECT;
    call->replayed = 1;
  } else if (strcmp(phase, "committed") == 0) {
    call->status =
        (turbo_tool_status_t)(int)turbo_json_get_double(record, "status", TURBO_TOOL_ERROR);
    output = turbo_json_get_string(record, "output");
    if (output) call->output = turbo_agent_tool_strdup(output);
    if (output && !call->output) rc = TURBO_ENOMEM;
    call->replayed = 1;
  } else if (strcmp(phase, "planned") != 0) {
    rc = TURBO_EPROTO;
  }
  turbo_runtime_json_destroy(record);
  return rc;
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
                                       task->context.run_id, task->call, "started") != TURBO_OK) {
      task->call->status = TURBO_TOOL_ERROR;
      goto cleanup;
    }
  }
  task->call->status = turbo_tool_registry_execute(task->registry, task->call->tool_name,
                                                   task->call->arguments_json, &task->call->output);
  if (task->call->output && strlen(task->call->output) > task->executor->config.max_output_bytes) {
    free(task->call->output);
    task->call->output = NULL;
    task->call->status = TURBO_TOOL_OUTPUT_LIMIT;
  }
  if (task->runtime &&
      turbo_agent_tool_journal_write(task->runtime, journal_id, task->context.thread_id,
                                     task->context.run_id, task->call, "committed") != TURBO_OK) {
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
    const turbo_agent_execution_context_t *base_context) {
  size_t wave_begin;
  for (wave_begin = begin; wave_begin < end; wave_begin += executor->config.max_workers) {
    turbo_agent_tool_task_t *tasks;
    size_t wave_end = wave_begin + executor->config.max_workers;
    size_t index;
    if (wave_end > end) wave_end = end;
    tasks = (turbo_agent_tool_task_t *)calloc(wave_end - wave_begin, sizeof(*tasks));
    if (!tasks) return TURBO_ENOMEM;
    for (index = wave_begin; index < wave_end; ++index) {
      turbo_agent_tool_task_t *task = &tasks[index - wave_begin];
      task->executor = executor;
      task->runtime = runtime;
      task->cancel_token = cancel_token;
      task->registry = registry;
      task->call = &calls[index];
      task->context = *base_context;
      task->context.tool_call_id = calls[index].call_id;
      task->context.tool_name = calls[index].tool_name;
      if (turbo_threadpool_try_submit(executor->pool, turbo_agent_tool_worker, task) != 0) {
        calls[index].status = TURBO_TOOL_BACKPRESSURE;
        calls[index].replayed = 1;
      }
    }
    turbo_threadpool_wait(executor->pool);
    free(tasks);
  }
  return TURBO_OK;
}

int turbo_agent_tool_executor_execute(turbo_agent_tool_executor_t *executor,
                                      turbo_agent_runtime_t *runtime,
                                      const turbo_cancel_token_t *cancel_token,
                                      const char *thread_id, const char *run_id,
                                      const turbo_tool_registry_t *registry,
                                      const turbo_agent_policy_t *policy,
                                      turbo_agent_tool_execution_t *calls, size_t call_count) {
  turbo_agent_execution_context_t base_context = {0};
  size_t index;
  int rc = TURBO_OK;
  if (!executor || !registry || !calls || call_count == 0 ||
      call_count > executor->config.max_batch_calls || (runtime && (!thread_id || !run_id)))
    return TURBO_EINVAL;
  for (index = 0; index < call_count; ++index) {
    if (!calls[index].call_id || !calls[index].turn_key || !calls[index].tool_name ||
        !calls[index].arguments_json ||
        strlen(calls[index].arguments_json) > executor->config.max_arguments_bytes)
      return TURBO_EMSGSIZE;
  }
  turbo_mutex_lock(&executor->batch_mutex);
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
    if (rc != TURBO_OK) goto cleanup;
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
      rc = turbo_agent_tool_execute_parallel_group(executor, runtime, cancel_token, registry, calls,
                                                   index, end, &base_context);
      if (rc != TURBO_OK) goto cleanup;
      index = end;
    } else {
      turbo_agent_tool_task_t task = {executor, runtime,       cancel_token,
                                      registry, &calls[index], base_context};
      task.context.tool_call_id = calls[index].call_id;
      task.context.tool_name = calls[index].tool_name;
      turbo_agent_tool_execute_one(&task);
      ++index;
    }
  }
cleanup:
  turbo_mutex_unlock(&executor->batch_mutex);
  return rc;
}
