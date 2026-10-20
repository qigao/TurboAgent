#include "turbo_agent_session_execution.h"

#include "turbo_agent_execution_internal.h"
#include "turbo_agent_session_internal.h"

#include <stdlib.h>
#include <string.h>

/* The synchronous API predates versioned submit admission. Preserve its
 * NULL-only checkpoint default and leave input-kind validation to Runtime. */
static int turbo_agent_session_prepare_sync_resume(
    const turbo_agent_session_t *session, const turbo_agent_session_exec_options_t *options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    turbo_agent_runtime_exec_options_t *out_options) {
  if (turbo_agent_session_has_pending_bound_claim_commit_internal(session))
    return SALTS_EBUSY;
  out_options->scope = (turbo_agent_runtime_scope_t)options->scope;
  out_options->input_kind = (turbo_agent_runtime_input_kind_t)options->input_kind;
  if (out_options->scope == TURBO_RUNTIME_SCOPE_THREAD) {
    out_options->thread_id = turbo_agent_session_thread_id(session);
    if (!out_options->thread_id) return -1;
  } else {
    out_options->checkpoint_id = options->checkpoint_id
                                    ? options->checkpoint_id
                                    : turbo_agent_session_last_checkpoint_id(session);
    if (!out_options->checkpoint_id) return -1;
  }
  out_options->event_sink = event_sink;
  out_options->event_sink_user_data = event_sink_user_data;
  return 0;
}

static int turbo_agent_session_execute_sync(
    turbo_agent_session_operation_t operation, turbo_agent_session_t *session,
    turbo_graph_t *graph, const json_value_t *input, const turbo_graph_run_options_t *graph_options,
    const turbo_agent_session_exec_options_t *session_options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary, json_value_t **out_state) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  turbo_agent_runtime_parent_link_t parent_link = {0};
  turbo_agent_runtime_exec_options_t runtime_options = {0};
  json_value_t *local_summary = NULL;
  json_value_t **summary_slot = out_summary ? out_summary : &local_summary;
  int rc;

  if (!runtime || !graph || !out_state ||
      (operation == TURBO_AGENT_SESSION_START ? !input : !session_options)) return -1;
  if (out_summary) *out_summary = NULL;

  if (operation == TURBO_AGENT_SESSION_START) {
    rc = turbo_agent_session_prepare_start_options_internal(
        session, event_sink, event_sink_user_data, &parent_link, &runtime_options);
  } else {
    rc = turbo_agent_session_prepare_sync_resume(
        session, session_options, event_sink, event_sink_user_data, &runtime_options);
  }
  if (rc != 0) return rc;

  switch (operation) {
  case TURBO_AGENT_SESSION_START:
    rc = turbo_agent_runtime_exec_start(runtime, graph, input, graph_options, &runtime_options,
                                         summary_slot, out_state);
    break;
  case TURBO_AGENT_SESSION_RESUME:
    rc = turbo_agent_runtime_exec_resume(runtime, graph, input, graph_options, &runtime_options,
                                          summary_slot, out_state);
    break;
  case TURBO_AGENT_SESSION_FORK:
    rc = turbo_agent_runtime_exec_fork(runtime, graph, input, graph_options, &runtime_options,
                                        summary_slot, out_state);
    break;
  default:
    return -1;
  }

  if (rc == 0) {
    rc = turbo_agent_session_complete_execution_internal(
        session, graph, graph_options, event_sink, event_sink_user_data, NULL,
        summary_slot, out_state);
  }
  json_free(local_summary);
  return rc;
}

CXX_C_API int turbo_agent_session_start_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const json_value_t *state,
    const turbo_graph_run_options_t *options, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data, json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_session_execute_sync(TURBO_AGENT_SESSION_START, session, graph, state,
      options, NULL, event_sink, event_sink_user_data, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_session_resume_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const json_value_t *input,
    const turbo_graph_run_options_t *options,
    const turbo_agent_session_exec_options_t *exec_options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_session_execute_sync(TURBO_AGENT_SESSION_RESUME, session, graph, input,
      options, exec_options, event_sink, event_sink_user_data, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_session_fork_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const json_value_t *input,
    const turbo_graph_run_options_t *options,
    const turbo_agent_session_exec_options_t *exec_options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_session_execute_sync(TURBO_AGENT_SESSION_FORK, session, graph, input,
      options, exec_options, event_sink, event_sink_user_data, out_summary_json, out_state);
}

typedef struct turbo_agent_session_completion_s {
  turbo_agent_session_t *session;
  turbo_event_sink_json_value_fn event_sink;
  void *event_sink_user_data;
  turbo_agent_session_terminal_fn terminal;
  void *terminal_user_data;
} turbo_agent_session_completion_t;

static int turbo_agent_session_complete(void *user_data, turbo_graph_t *graph,
    const turbo_graph_run_options_t *graph_options, turbo_cancel_token_t *cancel_token,
    json_value_t **summary, json_value_t **state) {
  turbo_agent_session_completion_t *completion = user_data;
  return turbo_agent_session_complete_execution_internal(completion->session, graph,
      graph_options, completion->event_sink, completion->event_sink_user_data,
      cancel_token, summary, state);
}

static void turbo_agent_session_terminal(void *user_data, turbo_agent_execution_t *execution) {
  turbo_agent_session_completion_t *completion = user_data;
  if (completion->terminal) {
    completion->terminal(completion->terminal_user_data, execution);
  }
  /* A callback may release Session/graph; do not access them after notification. */
  free(completion);
}

void turbo_agent_session_submit_options_init(turbo_agent_session_submit_options_t *options) {
  if (!options) return;
  memset(options, 0, sizeof(*options));
  options->struct_size = sizeof(*options);
  options->abi_version = TURBO_AGENT_SESSION_SUBMIT_ABI_VERSION;
  options->operation = TURBO_AGENT_SESSION_START;
}

int turbo_agent_session_submit(cmeta_threadpool_t *executor,
    turbo_agent_session_t *session, turbo_graph_t *graph, const json_value_t *input,
    const turbo_agent_session_submit_options_t *options,
    turbo_agent_execution_t **out_execution) {
  turbo_agent_session_submit_options_t defaults;
  turbo_agent_session_completion_t *completion;
  turbo_agent_runtime_t *runtime;
  turbo_agent_runtime_parent_link_t parent_link = {0};
  turbo_agent_runtime_exec_options_t runtime_options = {0};
  turbo_agent_execution_options_t execution_options = {
      sizeof(execution_options), TURBO_AGENT_EXECUTION_ABI_VERSION, 0};
  turbo_agent_execution_hooks_t hooks = {0};
  int rc;

  if (!out_execution) return SALTS_EINVAL;
  *out_execution = NULL;
  if (!options) {
    turbo_agent_session_submit_options_init(&defaults);
    options = &defaults;
  }
  if (!executor || !session || !graph || cmeta_threadpool_capacity(executor) == 0 ||
      options->struct_size < sizeof(*options) ||
      options->abi_version != TURBO_AGENT_SESSION_SUBMIT_ABI_VERSION ||
      options->operation < TURBO_AGENT_SESSION_START || options->operation > TURBO_AGENT_SESSION_FORK ||
      (options->operation == TURBO_AGENT_SESSION_START && (!input || options->session_options)) ||
      (options->operation != TURBO_AGENT_SESSION_START && !options->session_options)) return SALTS_EINVAL;

  runtime = turbo_agent_session_runtime(session);
  if (!runtime) return SALTS_EINVAL;
  if (options->operation == TURBO_AGENT_SESSION_START) {
    rc = turbo_agent_session_prepare_start_options_internal(session, options->event_sink,
        options->event_sink_user_data, &parent_link, &runtime_options);
  } else {
    rc = turbo_agent_session_prepare_resume_options_internal(session, options->session_options,
        options->event_sink, options->event_sink_user_data, &runtime_options);
  }
  if (rc != SALTS_OK) return rc;

  completion = calloc(1, sizeof(*completion));
  if (!completion) return SALTS_ENOMEM;
  completion->session = session;
  completion->event_sink = options->event_sink;
  completion->event_sink_user_data = options->event_sink_user_data;
  completion->terminal = options->terminal;
  completion->terminal_user_data = options->terminal_user_data;
  execution_options.deadline_mono_ms = options->deadline_mono_ms;
  hooks.complete = turbo_agent_session_complete;
  hooks.terminal = turbo_agent_session_terminal;
  hooks.user_data = completion;

  if (options->operation == TURBO_AGENT_SESSION_START) {
    rc = turbo_agent_execution_start_internal(executor, runtime, graph, input,
        options->graph_options, &runtime_options, &execution_options, &hooks, out_execution);
  } else if (options->operation == TURBO_AGENT_SESSION_RESUME) {
    rc = turbo_agent_execution_resume_internal(executor, runtime, graph, input,
        options->graph_options, &runtime_options, &execution_options, &hooks, out_execution);
  } else {
    rc = turbo_agent_execution_fork_internal(executor, runtime, graph, input,
        options->graph_options, &runtime_options, &execution_options, &hooks, out_execution);
  }
  /* Accepted work may already have freed completion before submit returns. */
  if (rc != SALTS_OK) free(completion);
  return rc;
}

/* === Preset execution =============================================== */

CXX_C_API int turbo_agent_session_start_preset(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const json_value_t *state, const turbo_graph_run_options_t *options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  turbo_graph_t *graph;
  int rc;

  graph = turbo_agent_session_create_preset_graph(session, kind);
  if (!graph) {
    return -1;
  }
  rc = turbo_agent_session_start_graph(session, graph, state, options, event_sink,
                                       event_sink_user_data, out_summary_json, out_state);
  turbo_graph_destroy(graph);
  return rc;
}

CXX_C_API int turbo_agent_session_resume_preset(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const json_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_session_exec_options_t *exec_options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  turbo_graph_t *graph;
  int rc;

  graph = turbo_agent_session_create_preset_graph(session, kind);
  if (!graph) {
    return -1;
  }
  rc = turbo_agent_session_resume_graph(session, graph, input, options, exec_options, event_sink,
                                        event_sink_user_data, out_summary_json, out_state);
  turbo_graph_destroy(graph);
  return rc;
}

CXX_C_API int turbo_agent_session_fork_preset(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const json_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_session_exec_options_t *exec_options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  turbo_graph_t *graph;
  int rc;

  graph = turbo_agent_session_create_preset_graph(session, kind);
  if (!graph) {
    return -1;
  }
  rc = turbo_agent_session_fork_graph(session, graph, input, options, exec_options, event_sink,
                                      event_sink_user_data, out_summary_json, out_state);
  turbo_graph_destroy(graph);
  return rc;
}
