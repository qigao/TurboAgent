#include "turbo_agent_harness.h"

#include "turbo_agent_execution_internal.h"
#include "turbo_agent_session_internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef enum turbo_agent_harness_operation_e {
  TURBO_AGENT_HARNESS_START = 0,
  TURBO_AGENT_HARNESS_RESUME = 1,
  TURBO_AGENT_HARNESS_FORK = 2
} turbo_agent_harness_operation_t;

struct turbo_agent_harness_s {
  atomic_size_t ref_count;
  salts_mutex_t mutex;
  turbo_agent_app_t *app;
  turbo_threadpool_t *executor;
  turbo_agent_execution_t *active_execution;
  int submitting;
};

struct turbo_agent_harness_execution_s {
  atomic_size_t ref_count;
  turbo_agent_harness_t *harness;
  turbo_graph_t *graph;
  turbo_agent_execution_t *execution;
  turbo_event_sink_json_value_fn event_sink;
  void *event_sink_user_data;
  int terminal_before_publish;
};

static int turbo_agent_harness_config_valid(const turbo_agent_harness_config_t *config) {
  return config && config->struct_size >= sizeof(*config) &&
         config->abi_version == TURBO_AGENT_HARNESS_ABI_VERSION && config->app_config &&
         config->executor && turbo_threadpool_capacity(config->executor) > 0 &&
         turbo_threadpool_is_accepting(config->executor);
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
  salts_mutex_init(&harness->mutex);
  if (!harness->mutex) {
    free(harness);
    return NULL;
  }
  harness->app = turbo_agent_app_create(config->app_config);
  if (!harness->app) {
    salts_mutex_destroy(&harness->mutex);
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
  salts_mutex_destroy(&harness->mutex);
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
  turbo_json_object_set_bool(capabilities, "has_async_execution", 1);
  turbo_json_object_set_bool(capabilities, "supports_start", 1);
  turbo_json_object_set_bool(capabilities, "supports_resume", 1);
  turbo_json_object_set_bool(capabilities, "supports_fork", 1);
  turbo_json_object_set_bool(capabilities, "supports_cancel", 1);
  turbo_json_object_set_bool(capabilities, "supports_deadline", 1);
  turbo_json_object_set_bool(capabilities, "supports_event_sink", 1);
  turbo_json_object_set_number(capabilities, "max_concurrent_executions", 1.0);
  turbo_json_object_set_number(capabilities, "executor_queue_capacity",
                               (double)turbo_threadpool_capacity(harness->executor));
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

static int turbo_agent_harness_complete(void *user_data, turbo_graph_t *graph,
                                        const turbo_graph_run_options_t *graph_options,
                                        turbo_cancel_token_t *cancel_token, json_value_t **summary,
                                        json_value_t **state) {
  turbo_agent_harness_execution_t *execution = (turbo_agent_harness_execution_t *)user_data;
  turbo_agent_session_t *session;

  if (!execution || !execution->harness) {
    return SALTS_EINVAL;
  }
  session = turbo_agent_app_session(execution->harness->app);
  return turbo_agent_session_complete_execution_internal(
      session, graph, graph_options, execution->event_sink, execution->event_sink_user_data,
      cancel_token, summary, state);
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
  salts_mutex_lock(&harness->mutex);
  if (harness->active_execution == agent_execution) {
    active = harness->active_execution;
    harness->active_execution = NULL;
  } else {
    execution->terminal_before_publish = 1;
  }
  salts_mutex_unlock(&harness->mutex);
  turbo_agent_execution_release(active);
  turbo_agent_harness_execution_release(execution);
}

static int turbo_agent_harness_begin_submit(turbo_agent_harness_t *harness) {
  int rc = SALTS_OK;
  turbo_agent_execution_t *terminal = NULL;
  turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_QUEUED;

  salts_mutex_lock(&harness->mutex);
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
  salts_mutex_unlock(&harness->mutex);
  turbo_agent_execution_release(terminal);
  return rc;
}

static void turbo_agent_harness_end_failed_submit(turbo_agent_harness_t *harness) {
  salts_mutex_lock(&harness->mutex);
  harness->submitting = 0;
  salts_mutex_unlock(&harness->mutex);
}

static int turbo_agent_harness_submit(turbo_agent_harness_t *harness,
                                      turbo_agent_harness_operation_t operation,
                                      const json_value_t *input,
                                      const turbo_agent_harness_run_options_t *options,
                                      turbo_agent_harness_execution_t **out_execution) {
  turbo_agent_harness_execution_t *execution = NULL;
  turbo_agent_session_t *session;
  turbo_graph_t *graph = NULL;
  turbo_agent_runtime_t *runtime;
  turbo_agent_runtime_parent_link_t parent_link = {0};
  turbo_agent_runtime_exec_options_t runtime_options = {0};
  turbo_agent_execution_options_t execution_options = {sizeof(execution_options),
                                                       TURBO_AGENT_EXECUTION_ABI_VERSION, 0};
  turbo_agent_execution_hooks_t hooks = {0};
  turbo_agent_execution_t *active_ref = NULL;
  int rc;

  if (!out_execution) {
    return SALTS_EINVAL;
  }
  *out_execution = NULL;
  if (!harness || !turbo_agent_harness_run_options_valid(options) ||
      (operation == TURBO_AGENT_HARNESS_START && !input) ||
      (operation != TURBO_AGENT_HARNESS_START && (!options || !options->session_options))) {
    return SALTS_EINVAL;
  }
  if (operation == TURBO_AGENT_HARNESS_START && options && options->session_options) {
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
  execution->event_sink = options ? options->event_sink : NULL;
  execution->event_sink_user_data = options ? options->event_sink_user_data : NULL;
  if (!execution->harness) {
    rc = SALTS_ERANGE;
    goto fail_execution;
  }

  if (operation == TURBO_AGENT_HARNESS_START) {
    rc = turbo_agent_session_prepare_start_options_internal(session, execution->event_sink,
                                                            execution->event_sink_user_data,
                                                            &parent_link, &runtime_options);
  } else {
    rc = turbo_agent_session_prepare_resume_options_internal(
        session, options->session_options, execution->event_sink, execution->event_sink_user_data,
        &runtime_options);
  }
  if (rc != SALTS_OK) {
    goto fail_execution;
  }
  if (options) {
    execution_options.deadline_mono_ms = options->deadline_mono_ms;
  }
  hooks.complete = turbo_agent_harness_complete;
  hooks.terminal = turbo_agent_harness_terminal;
  hooks.user_data = execution;

  if (operation == TURBO_AGENT_HARNESS_START) {
    rc = turbo_agent_execution_start_internal(
        harness->executor, runtime, graph, input, options ? options->graph_options : NULL,
        &runtime_options, &execution_options, &hooks, &execution->execution);
  } else if (operation == TURBO_AGENT_HARNESS_RESUME) {
    rc = turbo_agent_execution_resume_internal(harness->executor, runtime, graph, input,
                                               options->graph_options, &runtime_options,
                                               &execution_options, &hooks, &execution->execution);
  } else {
    rc = turbo_agent_execution_fork_internal(harness->executor, runtime, graph, input,
                                             options->graph_options, &runtime_options,
                                             &execution_options, &hooks, &execution->execution);
  }
  if (rc != SALTS_OK) {
    goto fail_execution;
  }

  active_ref = turbo_agent_execution_retain(execution->execution);
  if (!active_ref) {
    (void)turbo_agent_execution_cancel(execution->execution, TURBO_CANCEL_SHUTDOWN);
    rc = SALTS_ERANGE;
    goto fail_running_execution;
  }
  salts_mutex_lock(&harness->mutex);
  harness->submitting = 0;
  if (!execution->terminal_before_publish) {
    harness->active_execution = active_ref;
    active_ref = NULL;
  }
  salts_mutex_unlock(&harness->mutex);
  turbo_agent_execution_release(active_ref);

  *out_execution = execution;
  return SALTS_OK;

fail_running_execution:
  salts_mutex_lock(&harness->mutex);
  harness->submitting = 0;
  salts_mutex_unlock(&harness->mutex);
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
  return turbo_agent_harness_submit(harness, TURBO_AGENT_HARNESS_START, state, options,
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
  return turbo_agent_harness_submit(harness, TURBO_AGENT_HARNESS_RESUME, input, options,
                                    out_execution);
}

int turbo_agent_harness_fork(turbo_agent_harness_t *harness, const json_value_t *input,
                             const turbo_agent_harness_run_options_t *options,
                             turbo_agent_harness_execution_t **out_execution) {
  return turbo_agent_harness_submit(harness, TURBO_AGENT_HARNESS_FORK, input, options,
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
