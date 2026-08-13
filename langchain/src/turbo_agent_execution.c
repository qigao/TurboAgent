#include "turbo_agent_execution.h"

#include "turbo_agent_util_internal.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <turbo_uuid.h>

#define TURBO_EXECUTION_WAIT_SLICE_MS (UINT32_MAX - UINT64_C(1))
#define TURBO_EXECUTION_MS_TO_NS UINT64_C(1000000)

typedef enum turbo_agent_execution_kind_e {
  TURBO_AGENT_EXECUTION_START = 0,
  TURBO_AGENT_EXECUTION_RESUME = 1,
  TURBO_AGENT_EXECUTION_FORK = 2
} turbo_agent_execution_kind_t;

struct turbo_agent_execution_s {
  atomic_size_t ref_count;
  turbo_mutex_t mutex;
  turbo_cond_t changed;
  turbo_agent_execution_status_t status;
  turbo_agent_execution_kind_t kind;
  int operation_rc;
  int result_taken;
  char id[TURBO_UUID_STRING_SIZE];
  turbo_agent_runtime_t *runtime;
  turbo_graph_t *graph;
  json_value_t *input;
  turbo_graph_run_options_t graph_options;
  turbo_agent_runtime_exec_options_t runtime_options;
  turbo_agent_runtime_parent_link_t parent_link;
  turbo_cancel_source_t *cancel_source;
  turbo_cancel_token_t *cancel_token;
  json_value_t *summary;
  json_value_t *state;
};

static int turbo_agent_execution_is_terminal(turbo_agent_execution_status_t status) {
  return status >= TURBO_AGENT_EXECUTION_COMPLETED && status <= TURBO_AGENT_EXECUTION_FAILED;
}

static uint64_t turbo_agent_execution_saturating_add(uint64_t left, uint64_t right) {
  return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static void turbo_agent_execution_free_graph_options(turbo_graph_run_options_t *options) {
  size_t i;

  if (!options) {
    return;
  }
  tstr_free((tstr_t)options->start_node);
  for (i = 0; i < options->interrupt_before_count; ++i) {
    tstr_free((tstr_t)options->interrupt_before_nodes[i]);
  }
  free((void *)options->interrupt_before_nodes);
  memset(options, 0, sizeof(*options));
}

static int turbo_agent_execution_copy_graph_options(const turbo_graph_run_options_t *source,
                                                    turbo_graph_run_options_t *destination) {
  char **interrupts = NULL;
  size_t i;

  memset(destination, 0, sizeof(*destination));
  if (!source) {
    return TURBO_OK;
  }
  *destination = *source;
  destination->start_node = NULL;
  destination->interrupt_before_nodes = NULL;
  destination->interrupt_before_count = 0;

  if (source->start_node) {
    destination->start_node = turbo_agent_util_strdup(source->start_node);
    if (!destination->start_node) {
      return TURBO_ENOMEM;
    }
  }
  if (source->interrupt_before_count == 0) {
    return TURBO_OK;
  }
  if (!source->interrupt_before_nodes ||
      source->interrupt_before_count > SIZE_MAX / sizeof(*interrupts)) {
    turbo_agent_execution_free_graph_options(destination);
    return TURBO_EINVAL;
  }

  interrupts = (char **)calloc(source->interrupt_before_count, sizeof(*interrupts));
  if (!interrupts) {
    turbo_agent_execution_free_graph_options(destination);
    return TURBO_ENOMEM;
  }
  destination->interrupt_before_nodes = (const char *const *)interrupts;
  for (i = 0; i < source->interrupt_before_count; ++i) {
    if (!source->interrupt_before_nodes[i]) {
      turbo_agent_execution_free_graph_options(destination);
      return TURBO_EINVAL;
    }
    interrupts[i] = turbo_agent_util_strdup(source->interrupt_before_nodes[i]);
    if (!interrupts[i]) {
      destination->interrupt_before_count = i;
      turbo_agent_execution_free_graph_options(destination);
      return TURBO_ENOMEM;
    }
    destination->interrupt_before_count = i + 1;
  }
  return TURBO_OK;
}

static void turbo_agent_execution_free_runtime_options(turbo_agent_execution_t *execution) {
  tstr_free((tstr_t)execution->runtime_options.checkpoint_id);
  tstr_free((tstr_t)execution->runtime_options.thread_id);
  tstr_free((tstr_t)execution->parent_link.parent_agent_run_id);
  tstr_free((tstr_t)execution->parent_link.parent_tool_call_id);
  tstr_free((tstr_t)execution->parent_link.parent_tool_name);
  tstr_free((tstr_t)execution->parent_link.parent_graph_run_id);
  tstr_free((tstr_t)execution->parent_link.call_frame_id);
  memset(&execution->runtime_options, 0, sizeof(execution->runtime_options));
  memset(&execution->parent_link, 0, sizeof(execution->parent_link));
}

static int turbo_agent_execution_copy_optional_string(const char *source,
                                                      const char **destination) {
  if (!source) {
    *destination = NULL;
    return TURBO_OK;
  }
  *destination = turbo_agent_util_strdup(source);
  return *destination ? TURBO_OK : TURBO_ENOMEM;
}

static int
turbo_agent_execution_copy_runtime_options(const turbo_agent_runtime_exec_options_t *source,
                                           turbo_agent_execution_t *execution) {
  int rc;

  if (!source) {
    memset(&execution->runtime_options, 0, sizeof(execution->runtime_options));
    return TURBO_OK;
  }
  execution->runtime_options = *source;
  execution->runtime_options.checkpoint_id = NULL;
  execution->runtime_options.thread_id = NULL;
  execution->runtime_options.parent_link = NULL;
  memset(&execution->parent_link, 0, sizeof(execution->parent_link));

#define TURBO_COPY_EXECUTION_STRING(source_value, destination_value)                               \
  do {                                                                                             \
    rc = turbo_agent_execution_copy_optional_string((source_value), &(destination_value));         \
    if (rc != TURBO_OK) {                                                                          \
      turbo_agent_execution_free_runtime_options(execution);                                       \
      return rc;                                                                                   \
    }                                                                                              \
  } while (0)

  TURBO_COPY_EXECUTION_STRING(source->checkpoint_id, execution->runtime_options.checkpoint_id);
  TURBO_COPY_EXECUTION_STRING(source->thread_id, execution->runtime_options.thread_id);
  if (source->parent_link) {
    TURBO_COPY_EXECUTION_STRING(source->parent_link->parent_agent_run_id,
                                execution->parent_link.parent_agent_run_id);
    TURBO_COPY_EXECUTION_STRING(source->parent_link->parent_tool_call_id,
                                execution->parent_link.parent_tool_call_id);
    TURBO_COPY_EXECUTION_STRING(source->parent_link->parent_tool_name,
                                execution->parent_link.parent_tool_name);
    TURBO_COPY_EXECUTION_STRING(source->parent_link->parent_graph_run_id,
                                execution->parent_link.parent_graph_run_id);
    TURBO_COPY_EXECUTION_STRING(source->parent_link->call_frame_id,
                                execution->parent_link.call_frame_id);
    execution->runtime_options.parent_link = &execution->parent_link;
  }
#undef TURBO_COPY_EXECUTION_STRING
  return TURBO_OK;
}

static turbo_agent_execution_status_t
turbo_agent_execution_status_from_summary(const json_value_t *summary) {
  const char *status = summary ? turbo_json_get_string(summary, "status") : NULL;

  if (!status) {
    return TURBO_AGENT_EXECUTION_FAILED;
  }
  if (strcmp(status, "completed") == 0) {
    return TURBO_AGENT_EXECUTION_COMPLETED;
  }
  if (strcmp(status, "interrupted") == 0) {
    return TURBO_AGENT_EXECUTION_INTERRUPTED;
  }
  if (strcmp(status, "cancelled") == 0) {
    return TURBO_AGENT_EXECUTION_CANCELLED;
  }
  if (strcmp(status, "timed_out") == 0) {
    return TURBO_AGENT_EXECUTION_TIMED_OUT;
  }
  return TURBO_AGENT_EXECUTION_FAILED;
}

static void turbo_agent_execution_destroy(turbo_agent_execution_t *execution) {
  if (!execution) {
    return;
  }
  turbo_runtime_json_destroy(execution->summary);
  turbo_runtime_json_destroy(execution->state);
  turbo_runtime_json_destroy(execution->input);
  turbo_cancel_token_release(execution->cancel_token);
  turbo_cancel_source_destroy(execution->cancel_source);
  turbo_agent_execution_free_graph_options(&execution->graph_options);
  turbo_agent_execution_free_runtime_options(execution);
  turbo_cond_destroy(&execution->changed);
  turbo_mutex_destroy(&execution->mutex);
  free(execution);
}

turbo_agent_execution_t *turbo_agent_execution_retain(turbo_agent_execution_t *execution) {
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

void turbo_agent_execution_release(turbo_agent_execution_t *execution) {
  if (!execution) {
    return;
  }
  if (atomic_fetch_sub_explicit(&execution->ref_count, 1, memory_order_acq_rel) != 1) {
    return;
  }
  atomic_thread_fence(memory_order_acquire);
  turbo_agent_execution_destroy(execution);
}

static void turbo_agent_execution_worker(void *arg) {
  turbo_agent_execution_t *execution = (turbo_agent_execution_t *)arg;
  json_value_t *summary = NULL;
  json_value_t *state = NULL;
  int rc;

  turbo_mutex_lock(&execution->mutex);
  execution->status = TURBO_AGENT_EXECUTION_RUNNING;
  turbo_cond_broadcast(&execution->changed);
  turbo_mutex_unlock(&execution->mutex);

  if (execution->kind == TURBO_AGENT_EXECUTION_START) {
    rc = turbo_agent_runtime_exec_start_controlled(
        execution->runtime, execution->graph, execution->input, &execution->graph_options,
        &execution->runtime_options, execution->cancel_token, &summary, &state);
  } else if (execution->kind == TURBO_AGENT_EXECUTION_RESUME) {
    rc = turbo_agent_runtime_exec_resume_controlled(
        execution->runtime, execution->graph, execution->input, &execution->graph_options,
        &execution->runtime_options, execution->cancel_token, &summary, &state);
  } else {
    rc = turbo_agent_runtime_exec_fork_controlled(
        execution->runtime, execution->graph, execution->input, &execution->graph_options,
        &execution->runtime_options, execution->cancel_token, &summary, &state);
  }

  turbo_mutex_lock(&execution->mutex);
  execution->operation_rc = rc;
  execution->summary = summary;
  execution->state = state;
  execution->status =
      rc == 0 ? turbo_agent_execution_status_from_summary(summary) : TURBO_AGENT_EXECUTION_FAILED;
  turbo_cond_broadcast(&execution->changed);
  turbo_mutex_unlock(&execution->mutex);
  turbo_agent_execution_release(execution);
}

static int turbo_agent_execution_options_valid(const turbo_agent_execution_options_t *options) {
  return !options || (options->struct_size >= sizeof(*options) &&
                      options->abi_version == TURBO_AGENT_EXECUTION_ABI_VERSION);
}

static int turbo_agent_execution_submit(turbo_agent_execution_kind_t kind,
                                        turbo_threadpool_t *executor,
                                        turbo_agent_runtime_t *runtime, turbo_graph_t *graph,
                                        const json_value_t *input,
                                        const turbo_graph_run_options_t *graph_options,
                                        const turbo_agent_runtime_exec_options_t *runtime_options,
                                        const turbo_agent_execution_options_t *execution_options,
                                        turbo_agent_execution_t **out_execution) {
  turbo_agent_execution_t *execution = NULL;
  turbo_cancel_source_config_t cancel_config = {sizeof(cancel_config),
                                                TURBO_RUNTIME_CONTROL_ABI_VERSION, 0, NULL, NULL};
  turbo_uuid_t uuid;
  int rc;

  if (!out_execution) {
    return TURBO_EINVAL;
  }
  *out_execution = NULL;
  if (!executor || !runtime || !graph || (kind == TURBO_AGENT_EXECUTION_START && !input) ||
      !turbo_agent_execution_options_valid(execution_options)) {
    return TURBO_EINVAL;
  }

  execution = (turbo_agent_execution_t *)calloc(1, sizeof(*execution));
  if (!execution) {
    return TURBO_ENOMEM;
  }
  atomic_init(&execution->ref_count, 1);
  execution->kind = kind;
  execution->status = TURBO_AGENT_EXECUTION_QUEUED;
  execution->operation_rc = TURBO_EBUSY;
  execution->runtime = runtime;
  execution->graph = graph;
  turbo_mutex_init(&execution->mutex);
  turbo_cond_init(&execution->changed);
  if (!execution->mutex || !execution->changed) {
    turbo_agent_execution_destroy(execution);
    return TURBO_ENOMEM;
  }

  if (input) {
    execution->input = turbo_json_clone(input);
    if (!execution->input) {
      turbo_agent_execution_release(execution);
      return TURBO_ENOMEM;
    }
  }
  rc = turbo_agent_execution_copy_graph_options(graph_options, &execution->graph_options);
  if (rc != TURBO_OK) {
    turbo_agent_execution_release(execution);
    return rc;
  }
  rc = turbo_agent_execution_copy_runtime_options(runtime_options, execution);
  if (rc != TURBO_OK) {
    turbo_agent_execution_release(execution);
    return rc;
  }

  if (execution_options) {
    cancel_config.deadline_mono_ms = execution_options->deadline_mono_ms;
  }
  rc = turbo_cancel_source_create(&cancel_config, &execution->cancel_source);
  if (rc != TURBO_OK ||
      turbo_cancel_source_token(execution->cancel_source, &execution->cancel_token) != TURBO_OK) {
    turbo_agent_execution_release(execution);
    return rc != TURBO_OK ? rc : TURBO_ENOMEM;
  }
  rc = turbo_uuid_v7_generate(&uuid);
  if (rc != TURBO_OK ||
      turbo_uuid_format(&uuid, execution->id, sizeof(execution->id)) != TURBO_OK) {
    turbo_agent_execution_release(execution);
    return rc != TURBO_OK ? rc : TURBO_EIO;
  }

  if (!turbo_agent_execution_retain(execution)) {
    turbo_agent_execution_release(execution);
    return TURBO_ERANGE;
  }
  if (turbo_threadpool_try_submit(executor, turbo_agent_execution_worker, execution) != 0) {
    turbo_agent_execution_release(execution);
    turbo_agent_execution_release(execution);
    return TURBO_EBUSY;
  }

  *out_execution = execution;
  return TURBO_OK;
}

int turbo_agent_execution_start(turbo_threadpool_t *executor, turbo_agent_runtime_t *runtime,
                                turbo_graph_t *graph, const json_value_t *state,
                                const turbo_graph_run_options_t *graph_options,
                                const turbo_agent_runtime_exec_options_t *runtime_options,
                                const turbo_agent_execution_options_t *execution_options,
                                turbo_agent_execution_t **out_execution) {
  return turbo_agent_execution_submit(TURBO_AGENT_EXECUTION_START, executor, runtime, graph, state,
                                      graph_options, runtime_options, execution_options,
                                      out_execution);
}

int turbo_agent_execution_resume(turbo_threadpool_t *executor, turbo_agent_runtime_t *runtime,
                                 turbo_graph_t *graph, const json_value_t *input,
                                 const turbo_graph_run_options_t *graph_options,
                                 const turbo_agent_runtime_exec_options_t *runtime_options,
                                 const turbo_agent_execution_options_t *execution_options,
                                 turbo_agent_execution_t **out_execution) {
  if (!runtime_options) {
    if (out_execution) {
      *out_execution = NULL;
    }
    return TURBO_EINVAL;
  }
  return turbo_agent_execution_submit(TURBO_AGENT_EXECUTION_RESUME, executor, runtime, graph, input,
                                      graph_options, runtime_options, execution_options,
                                      out_execution);
}

int turbo_agent_execution_fork(turbo_threadpool_t *executor, turbo_agent_runtime_t *runtime,
                               turbo_graph_t *graph, const json_value_t *input,
                               const turbo_graph_run_options_t *graph_options,
                               const turbo_agent_runtime_exec_options_t *runtime_options,
                               const turbo_agent_execution_options_t *execution_options,
                               turbo_agent_execution_t **out_execution) {
  if (!runtime_options) {
    if (out_execution) {
      *out_execution = NULL;
    }
    return TURBO_EINVAL;
  }
  return turbo_agent_execution_submit(TURBO_AGENT_EXECUTION_FORK, executor, runtime, graph, input,
                                      graph_options, runtime_options, execution_options,
                                      out_execution);
}

const char *turbo_agent_execution_id(const turbo_agent_execution_t *execution) {
  return execution ? execution->id : NULL;
}

int turbo_agent_execution_cancel(turbo_agent_execution_t *execution, turbo_cancel_reason_t reason) {
  int terminal;

  if (!execution) {
    return TURBO_EINVAL;
  }
  turbo_mutex_lock(&execution->mutex);
  terminal = turbo_agent_execution_is_terminal(execution->status);
  turbo_mutex_unlock(&execution->mutex);
  return terminal ? TURBO_EALREADY : turbo_cancel_source_cancel(execution->cancel_source, reason);
}

int turbo_agent_execution_wait(turbo_agent_execution_t *execution, uint64_t timeout_ms) {
  uint64_t deadline_ms = UINT64_MAX;
  int has_timeout = timeout_ms != UINT64_MAX;
  int rc = TURBO_OK;

  if (!execution) {
    return TURBO_EINVAL;
  }
  if (has_timeout) {
    deadline_ms = turbo_agent_execution_saturating_add(turbo_monotonic_ms(), timeout_ms);
  }

  turbo_mutex_lock(&execution->mutex);
  while (!turbo_agent_execution_is_terminal(execution->status)) {
    uint64_t now_ms;
    uint64_t wait_ms;

    if (!has_timeout) {
      turbo_cond_wait(&execution->changed, &execution->mutex);
      continue;
    }
    now_ms = turbo_monotonic_ms();
    if (now_ms >= deadline_ms) {
      rc = TURBO_ETIMEDOUT;
      break;
    }
    wait_ms = deadline_ms - now_ms;
    if (wait_ms > TURBO_EXECUTION_WAIT_SLICE_MS) {
      wait_ms = TURBO_EXECUTION_WAIT_SLICE_MS;
    }
    (void)turbo_cond_timedwait(&execution->changed, &execution->mutex,
                               wait_ms * TURBO_EXECUTION_MS_TO_NS);
  }
  turbo_mutex_unlock(&execution->mutex);
  return rc;
}

int turbo_agent_execution_get_status(const turbo_agent_execution_t *execution,
                                     turbo_agent_execution_status_t *out_status) {
  if (!execution || !out_status) {
    return TURBO_EINVAL;
  }
  turbo_mutex_lock((turbo_mutex_t *)&execution->mutex);
  *out_status = execution->status;
  turbo_mutex_unlock((turbo_mutex_t *)&execution->mutex);
  return TURBO_OK;
}

int turbo_agent_execution_result_code(const turbo_agent_execution_t *execution,
                                      int *out_result_code) {
  if (!execution || !out_result_code) {
    return TURBO_EINVAL;
  }
  turbo_mutex_lock((turbo_mutex_t *)&execution->mutex);
  if (!turbo_agent_execution_is_terminal(execution->status)) {
    turbo_mutex_unlock((turbo_mutex_t *)&execution->mutex);
    return TURBO_EBUSY;
  }
  *out_result_code = execution->operation_rc;
  turbo_mutex_unlock((turbo_mutex_t *)&execution->mutex);
  return TURBO_OK;
}

int turbo_agent_execution_take_result(turbo_agent_execution_t *execution,
                                      json_value_t **out_summary, json_value_t **out_state) {
  if (!execution || !out_summary || !out_state) {
    return TURBO_EINVAL;
  }
  turbo_mutex_lock(&execution->mutex);
  if (!turbo_agent_execution_is_terminal(execution->status)) {
    turbo_mutex_unlock(&execution->mutex);
    return TURBO_EBUSY;
  }
  if (execution->result_taken) {
    turbo_mutex_unlock(&execution->mutex);
    return TURBO_EALREADY;
  }
  *out_summary = NULL;
  *out_state = NULL;
  execution->result_taken = 1;
  *out_summary = execution->summary;
  *out_state = execution->state;
  execution->summary = NULL;
  execution->state = NULL;
  turbo_mutex_unlock(&execution->mutex);
  return TURBO_OK;
}
