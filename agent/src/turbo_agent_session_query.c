#include "turbo_agent_session.h"

static const char *session_query_checkpoint_id(const turbo_agent_session_t *session,
                                               const char *checkpoint_id) {
  return checkpoint_id && checkpoint_id[0] ? checkpoint_id
                                           : turbo_agent_session_last_checkpoint_id(session);
}

static const char *session_query_run_id(const turbo_agent_session_t *session, const char *run_id) {
  return run_id && run_id[0] ? run_id : turbo_agent_session_last_run_id(session);
}

/* Select exactly one source before invoking Runtime. Selection does not retry a
 * failed query with another source, and it does not update Session's cached ids. */
static int session_history_source(const turbo_agent_session_t *session, const char *run_id,
                                  const char *checkpoint_id, const char **out_run_id,
                                  const char **out_checkpoint_id) {
  *out_run_id = NULL;
  *out_checkpoint_id = NULL;
  if (run_id && run_id[0]) {
    *out_run_id = run_id;
  } else if (checkpoint_id && checkpoint_id[0]) {
    *out_checkpoint_id = checkpoint_id;
  } else {
    run_id = turbo_agent_session_last_run_id(session);
    checkpoint_id = turbo_agent_session_last_checkpoint_id(session);
    if (run_id && run_id[0]) *out_run_id = run_id;
    else if (checkpoint_id && checkpoint_id[0]) *out_checkpoint_id = checkpoint_id;
    else return -1;
  }
  return 0;
}

CXX_C_API int turbo_agent_session_get_thread(turbo_agent_session_t *session,
                                             json_value_t **out_thread_json) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *thread_id = turbo_agent_session_thread_id(session);
  if (!session || !runtime || !thread_id || !out_thread_json) {
    return -1;
  }
  return turbo_agent_runtime_get_thread(runtime, thread_id, out_thread_json);
}

CXX_C_API int turbo_agent_session_get_run(turbo_agent_session_t *session, const char *run_id,
                                          json_value_t **out_run_json) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *resolved_run_id = session_query_run_id(session, run_id);

  if (!session || !runtime || !resolved_run_id || !out_run_json) {
    return -1;
  }
  return turbo_agent_runtime_get_run(runtime, resolved_run_id, out_run_json);
}

CXX_C_API int turbo_agent_session_get_latest_run(turbo_agent_session_t *session,
                                                 json_value_t **out_run_json) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *thread_id = turbo_agent_session_thread_id(session);
  if (!session || !runtime || !thread_id || !out_run_json) {
    return -1;
  }
  return turbo_agent_runtime_get_latest_run(runtime, thread_id, out_run_json);
}

CXX_C_API int turbo_agent_session_get_pending_run(turbo_agent_session_t *session,
                                                  json_value_t **out_run_json) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *thread_id = turbo_agent_session_thread_id(session);
  if (!session || !runtime || !thread_id || !out_run_json) {
    return -1;
  }
  return turbo_agent_runtime_get_pending_run(runtime, thread_id, out_run_json);
}

CXX_C_API int turbo_agent_session_get_checkpoint(turbo_agent_session_t *session,
                                                 const char *checkpoint_id,
                                                 json_value_t **out_checkpoint_json) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *resolved_checkpoint_id = session_query_checkpoint_id(session, checkpoint_id);

  if (!session || !runtime || !resolved_checkpoint_id || !out_checkpoint_json) {
    return -1;
  }
  return turbo_agent_runtime_get_checkpoint(runtime, resolved_checkpoint_id, out_checkpoint_json);
}

CXX_C_API int turbo_agent_session_get_latest_checkpoint(turbo_agent_session_t *session,
                                                        const char *run_id,
                                                        json_value_t **out_checkpoint_json) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *resolved_run_id = session_query_run_id(session, run_id);

  if (!session || !runtime || !resolved_run_id || !out_checkpoint_json) {
    return -1;
  }
  return turbo_agent_runtime_get_latest_checkpoint(runtime, resolved_run_id, out_checkpoint_json);
}

CXX_C_API int turbo_agent_session_get_thread_state_json_value(turbo_agent_session_t *session,
                                                              json_value_t **out_state) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *thread_id = turbo_agent_session_thread_id(session);
  if (!session || !runtime || !thread_id || !out_state) {
    return -1;
  }
  return turbo_agent_runtime_get_thread_state_json_value(runtime, thread_id, out_state);
}

CXX_C_API int turbo_agent_session_get_thread_head_state_json_value(turbo_agent_session_t *session,
                                                                   json_value_t **out_state) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *thread_id = turbo_agent_session_thread_id(session);
  if (!session || !runtime || !thread_id || !out_state) {
    return -1;
  }
  return turbo_agent_runtime_get_thread_head_state_json_value(runtime, thread_id, out_state);
}

CXX_C_API int turbo_agent_session_get_thread_trace_events_json_value(turbo_agent_session_t *session,
                                                                     json_value_t **out_events) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *thread_id = turbo_agent_session_thread_id(session);
  if (!session || !runtime || !thread_id || !out_events) {
    return -1;
  }
  return turbo_agent_runtime_get_thread_trace_events_json_value(runtime, thread_id, out_events);
}

CXX_C_API int
turbo_agent_session_get_thread_head_trace_events_json_value(turbo_agent_session_t *session,
                                                            json_value_t **out_events) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *thread_id = turbo_agent_session_thread_id(session);
  if (!session || !runtime || !thread_id || !out_events) {
    return -1;
  }
  return turbo_agent_runtime_get_thread_head_trace_events_json_value(runtime, thread_id,
                                                                     out_events);
}

CXX_C_API int turbo_agent_session_get_run_state_json_value(turbo_agent_session_t *session,
                                                           const char *run_id,
                                                           json_value_t **out_state) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *resolved_run_id = session_query_run_id(session, run_id);

  if (!session || !runtime || !resolved_run_id || !out_state) {
    return -1;
  }
  return turbo_agent_runtime_get_run_state_json_value(runtime, resolved_run_id, out_state);
}

CXX_C_API int turbo_agent_session_get_run_trace_events_json_value(turbo_agent_session_t *session,
                                                                  const char *run_id,
                                                                  json_value_t **out_events) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *resolved_run_id = session_query_run_id(session, run_id);

  if (!session || !runtime || !resolved_run_id || !out_events) {
    return -1;
  }
  return turbo_agent_runtime_get_run_trace_events_json_value(runtime, resolved_run_id, out_events);
}

CXX_C_API int turbo_agent_session_get_checkpoint_state_json_value(turbo_agent_session_t *session,
                                                                  const char *checkpoint_id,
                                                                  json_value_t **out_state) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *resolved_checkpoint_id = session_query_checkpoint_id(session, checkpoint_id);

  if (!session || !runtime || !resolved_checkpoint_id || !out_state) {
    return -1;
  }
  return turbo_agent_runtime_get_checkpoint_state_json_value(runtime, resolved_checkpoint_id,
                                                             out_state);
}

CXX_C_API int turbo_agent_session_prepare_checkpoint_state_override_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id, const json_value_t *state_patch,
    json_value_t **out_state_override) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *resolved_checkpoint_id = session_query_checkpoint_id(session, checkpoint_id);

  if (!session || !runtime || !resolved_checkpoint_id || !state_patch || !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_prepare_checkpoint_state_override_json_value(
      runtime, resolved_checkpoint_id, state_patch, out_state_override);
}

CXX_C_API int
turbo_agent_session_prepare_thread_state_override_json_value(turbo_agent_session_t *session,
                                                             const json_value_t *state_patch,
                                                             json_value_t **out_state_override) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *thread_id = turbo_agent_session_thread_id(session);
  if (!session || !runtime || !thread_id || !state_patch || !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_prepare_thread_state_override_json_value(
      runtime, thread_id, state_patch, out_state_override);
}

CXX_C_API int turbo_agent_session_apply_checkpoint_state_patch_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id, const json_value_t *state_patch,
    json_value_t **out_state_override) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *resolved_checkpoint_id = session_query_checkpoint_id(session, checkpoint_id);

  if (!session || !runtime || !resolved_checkpoint_id || !state_patch || !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_prepare_checkpoint_state_override_json_value(
      runtime, resolved_checkpoint_id, state_patch, out_state_override);
}

CXX_C_API int
turbo_agent_session_apply_thread_state_patch_json_value(turbo_agent_session_t *session,
                                                        const json_value_t *state_patch,
                                                        json_value_t **out_state_override) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *thread_id = turbo_agent_session_thread_id(session);
  if (!session || !runtime || !thread_id || !state_patch || !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_prepare_thread_state_override_json_value(
      runtime, thread_id, state_patch, out_state_override);
}

CXX_C_API int turbo_agent_session_get_observability_index(turbo_agent_session_t *session,
                                                          json_value_t **out_index_json) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *thread_id = turbo_agent_session_thread_id(session);
  if (!session || !runtime || !thread_id || !out_index_json) {
    return -1;
  }
  return turbo_agent_runtime_get_thread_observability_index(runtime, thread_id, out_index_json);
}

CXX_C_API int turbo_agent_session_get_checkpoint_trace_events_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id, json_value_t **out_events) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *resolved_checkpoint_id = session_query_checkpoint_id(session, checkpoint_id);

  if (!session || !runtime || !resolved_checkpoint_id || !out_events) {
    return -1;
  }
  return turbo_agent_runtime_get_checkpoint_trace_events_json_value(runtime, resolved_checkpoint_id,
                                                                    out_events);
}

CXX_C_API int turbo_agent_session_list_runs(turbo_agent_session_t *session,
                                            json_value_t **out_runs_json) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *thread_id = turbo_agent_session_thread_id(session);
  if (!session || !runtime || !thread_id || !out_runs_json) {
    return -1;
  }
  return turbo_agent_runtime_list_runs(runtime, thread_id, out_runs_json);
}

CXX_C_API int turbo_agent_session_list_checkpoints(turbo_agent_session_t *session,
                                                   const char *run_id,
                                                   json_value_t **out_checkpoints_json) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *resolved_run_id = session_query_run_id(session, run_id);

  if (!session || !runtime || !resolved_run_id || !out_checkpoints_json) {
    return -1;
  }
  return turbo_agent_runtime_list_checkpoints(runtime, resolved_run_id, out_checkpoints_json);
}

CXX_C_API int turbo_agent_session_list_thread_lineage(turbo_agent_session_t *session,
                                                      json_value_t **out_lineage_json) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *thread_id = turbo_agent_session_thread_id(session);
  if (!session || !runtime || !thread_id || !out_lineage_json) {
    return -1;
  }
  return turbo_agent_runtime_list_thread_lineage(runtime, thread_id, out_lineage_json);
}

CXX_C_API int turbo_agent_session_get_branch_tree(turbo_agent_session_t *session,
                                                  json_value_t **out_branch_tree_json) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *thread_id = turbo_agent_session_thread_id(session);
  if (!session || !runtime || !thread_id || !out_branch_tree_json) {
    return -1;
  }
  return turbo_agent_runtime_get_branch_tree(runtime, thread_id, out_branch_tree_json);
}

CXX_C_API int turbo_agent_session_get_checkpoint_context(turbo_agent_session_t *session,
                                                         const char *checkpoint_id,
                                                         json_value_t **out_context_json) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  if (!session || !runtime || !checkpoint_id || !checkpoint_id[0] || !out_context_json) {
    return -1;
  }
  return turbo_agent_runtime_get_checkpoint_context(runtime, checkpoint_id, out_context_json);
}

CXX_C_API int turbo_agent_session_load_history_events_json_value(turbo_agent_session_t *session,
                                                                 const char *run_id,
                                                                 const char *checkpoint_id,
                                                                 json_value_t **out_events) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *resolved_run_id = NULL;
  const char *resolved_checkpoint_id = NULL;

  if (!session || !runtime || !out_events) {
    return -1;
  }
  if (session_history_source(session, run_id, checkpoint_id, &resolved_run_id,
                             &resolved_checkpoint_id) != 0)
    return -1;
  return turbo_agent_runtime_load_history_events_json_value(runtime, resolved_run_id,
                                                            resolved_checkpoint_id, out_events);
}

CXX_C_API int
turbo_agent_session_load_thread_history_events_json_value(turbo_agent_session_t *session,
                                                          json_value_t **out_events) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *thread_id = turbo_agent_session_thread_id(session);
  if (!session || !runtime || !thread_id || !out_events) {
    return -1;
  }
  return turbo_agent_runtime_load_thread_history_events_json_value(runtime, thread_id, out_events);
}

CXX_C_API int turbo_agent_session_replay_history_json_value(
    turbo_agent_session_t *session, const char *run_id, const char *checkpoint_id,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *resolved_run_id = NULL;
  const char *resolved_checkpoint_id = NULL;

  if (!session || !runtime || !event_sink) {
    return -1;
  }
  if (session_history_source(session, run_id, checkpoint_id, &resolved_run_id,
                             &resolved_checkpoint_id) != 0)
    return -1;
  return turbo_agent_runtime_replay_history_json_value(
      runtime, resolved_run_id, resolved_checkpoint_id, event_sink, event_sink_user_data);
}

CXX_C_API int
turbo_agent_session_replay_thread_history_json_value(turbo_agent_session_t *session,
                                                     turbo_event_sink_json_value_fn event_sink,
                                                     void *event_sink_user_data) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *thread_id = turbo_agent_session_thread_id(session);
  if (!session || !runtime || !thread_id || !event_sink) {
    return -1;
  }
  return turbo_agent_runtime_replay_thread_history_json_value(runtime, thread_id, event_sink,
                                                              event_sink_user_data);
}

CXX_C_API int
turbo_agent_session_observe_history_json_value(turbo_agent_session_t *session, const char *run_id,
                                               const char *checkpoint_id,
                                               const turbo_agent_observer_json_value_sink_t *sink) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *resolved_run_id = NULL;
  const char *resolved_checkpoint_id = NULL;

  if (!session || !runtime || !sink || !sink->callback) {
    return -1;
  }
  if (session_history_source(session, run_id, checkpoint_id, &resolved_run_id,
                             &resolved_checkpoint_id) != 0)
    return -1;
  return turbo_agent_runtime_observe_history_json_value(runtime, resolved_run_id,
                                                        resolved_checkpoint_id, sink);
}

CXX_C_API int turbo_agent_session_observe_thread_history_json_value(
    turbo_agent_session_t *session, const turbo_agent_observer_json_value_sink_t *sink) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *thread_id = turbo_agent_session_thread_id(session);
  if (!session || !runtime || !thread_id || !sink || !sink->callback) {
    return -1;
  }
  return turbo_agent_runtime_observe_thread_history_json_value(runtime, thread_id, sink);
}

CXX_C_API int turbo_agent_session_get_thread_timeline_json_value(turbo_agent_session_t *session,
                                                                 json_value_t **out_timeline) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *thread_id = turbo_agent_session_thread_id(session);
  if (!session || !runtime || !thread_id || !out_timeline) {
    return -1;
  }
  return turbo_agent_runtime_get_thread_timeline_json_value(runtime, thread_id, out_timeline);
}

CXX_C_API int turbo_agent_session_apply_command_json_value(turbo_agent_session_t *session,
                                                           const char *checkpoint_id,
                                                           const json_value_t *command,
                                                           json_value_t **out_state_override) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *resolved_checkpoint_id = session_query_checkpoint_id(session, checkpoint_id);

  if (!session || !runtime || !resolved_checkpoint_id || !command || !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_prepare_checkpoint_command_override_json_value(
      runtime, resolved_checkpoint_id, command, out_state_override);
}

CXX_C_API int turbo_agent_session_prepare_checkpoint_command_override_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id, const json_value_t *command,
    json_value_t **out_state_override) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *resolved_checkpoint_id = session_query_checkpoint_id(session, checkpoint_id);

  if (!session || !runtime || !resolved_checkpoint_id || !command || !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_prepare_checkpoint_command_override_json_value(
      runtime, resolved_checkpoint_id, command, out_state_override);
}

CXX_C_API int turbo_agent_session_apply_checkpoint_command_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id, const json_value_t *command,
    json_value_t **out_state_override) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  if (!session || !runtime || !checkpoint_id || !checkpoint_id[0] || !command ||
      !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_prepare_checkpoint_command_override_json_value(
      runtime, checkpoint_id, command, out_state_override);
}

CXX_C_API int
turbo_agent_session_prepare_thread_command_override_json_value(turbo_agent_session_t *session,
                                                               const json_value_t *command,
                                                               json_value_t **out_state_override) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *thread_id = turbo_agent_session_thread_id(session);
  if (!session || !runtime || !thread_id || !command || !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_prepare_thread_command_override_json_value(runtime, thread_id, command,
                                                                        out_state_override);
}

CXX_C_API int
turbo_agent_session_apply_thread_command_json_value(turbo_agent_session_t *session,
                                                    const json_value_t *command,
                                                    json_value_t **out_state_override) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *thread_id = turbo_agent_session_thread_id(session);
  if (!session || !runtime || !thread_id || !command || !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_prepare_thread_command_override_json_value(runtime, thread_id, command,
                                                                        out_state_override);
}
