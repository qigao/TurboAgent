#include "turbo_agent_app.h"

#include <stdlib.h>

struct turbo_agent_app_s {
  turbo_agent_session_t *session;
};

CXX_C_API turbo_agent_app_t *turbo_agent_app_create(const turbo_agent_app_config_t *config) {
  turbo_agent_app_t *app;

  if (!config || !config->session_config) {
    return NULL;
  }
  app = (turbo_agent_app_t *)calloc(1, sizeof(*app));
  if (!app) {
    return NULL;
  }
  app->session = turbo_agent_session_create(config->session_config);
  if (!app->session) {
    free(app);
    return NULL;
  }
  return app;
}

CXX_C_API void turbo_agent_app_destroy(turbo_agent_app_t *app) {
  if (!app) {
    return;
  }
  turbo_agent_session_destroy(app->session);
  free(app);
}

CXX_C_API turbo_agent_session_t *turbo_agent_app_session(const turbo_agent_app_t *app) {
  if (!app) {
    return NULL;
  }
  return app->session;
}

CXX_C_API const char *turbo_agent_app_thread_id(const turbo_agent_app_t *app) {
  return app ? turbo_agent_session_thread_id(app->session) : NULL;
}

CXX_C_API const char *turbo_agent_app_last_run_id(const turbo_agent_app_t *app) {
  return app ? turbo_agent_session_last_run_id(app->session) : NULL;
}

CXX_C_API const char *turbo_agent_app_last_checkpoint_id(const turbo_agent_app_t *app) {
  return app ? turbo_agent_session_last_checkpoint_id(app->session) : NULL;
}

CXX_C_API turbo_agent_session_workflow_kind_t
turbo_agent_app_workflow_kind(const turbo_agent_app_t *app) {
  return app ? turbo_agent_session_workflow_kind(app->session)
             : TURBO_AGENT_SESSION_WORKFLOW_LOOP;
}

CXX_C_API const char *turbo_agent_app_memory_namespace(const turbo_agent_app_t *app) {
  return app ? turbo_agent_session_memory_namespace(app->session) : NULL;
}

CXX_C_API const turbo_agent_memory_store_t *
turbo_agent_app_memory_store(const turbo_agent_app_t *app) {
  return app ? turbo_agent_session_memory_store(app->session) : NULL;
}

CXX_C_API const turbo_tool_registry_t *
turbo_agent_app_tool_registry(const turbo_agent_app_t *app) {
  return app ? turbo_agent_session_tool_registry(app->session) : NULL;
}

CXX_C_API size_t turbo_agent_app_tool_count(const turbo_agent_app_t *app) {
  return app ? turbo_agent_session_tool_count(app->session) : 0;
}

CXX_C_API int turbo_agent_app_get_capabilities(
    const turbo_agent_app_t *app, json_value_t **out_capabilities_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_capabilities(app->session, out_capabilities_json);
}

CXX_C_API int turbo_agent_app_get_tool_schemas(
    const turbo_agent_app_t *app, json_value_t **out_tool_schemas_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_tool_schemas(app->session, out_tool_schemas_json);
}

CXX_C_API int turbo_agent_app_get_startup_diagnostics(
    const turbo_agent_app_t *app, json_value_t **out_diagnostics_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_startup_diagnostics(app->session, out_diagnostics_json);
}

CXX_C_API int turbo_agent_app_add_trace_bind_sink(
    turbo_agent_app_t *app, const turbo_agent_trace_bind_sink_t *sink) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_add_trace_bind_sink(app->session, sink);
}

CXX_C_API int turbo_agent_app_add_observer_bind_sink(
    turbo_agent_app_t *app, const turbo_agent_observer_bind_sink_t *sink) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_add_observer_bind_sink(app->session, sink);
}

CXX_C_API int turbo_agent_app_set_trace_history_enabled(turbo_agent_app_t *app, int enabled) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_set_trace_history_enabled(app->session, enabled);
}

CXX_C_API int turbo_agent_app_get_thread(turbo_agent_app_t *app, json_value_t **out_thread_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_thread(app->session, out_thread_json);
}

CXX_C_API int turbo_agent_app_get_run(turbo_agent_app_t *app, const char *run_id,
                                      json_value_t **out_run_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_run(app->session, run_id, out_run_json);
}

CXX_C_API int turbo_agent_app_get_latest_run(turbo_agent_app_t *app, json_value_t **out_run_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_latest_run(app->session, out_run_json);
}

CXX_C_API int turbo_agent_app_get_pending_run(turbo_agent_app_t *app, json_value_t **out_run_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_pending_run(app->session, out_run_json);
}

CXX_C_API int turbo_agent_app_get_checkpoint(turbo_agent_app_t *app, const char *checkpoint_id,
                                             json_value_t **out_checkpoint_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_checkpoint(app->session, checkpoint_id, out_checkpoint_json);
}

CXX_C_API int turbo_agent_app_get_latest_checkpoint(turbo_agent_app_t *app, const char *run_id,
                                                    json_value_t **out_checkpoint_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_latest_checkpoint(app->session, run_id, out_checkpoint_json);
}

CXX_C_API int turbo_agent_app_get_thread_state_bind(
    turbo_agent_app_t *app, turbo_runtime_data_bind_value_t **out_state) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_thread_state_bind(app->session, out_state);
}

CXX_C_API int turbo_agent_app_get_thread_head_state_bind(
    turbo_agent_app_t *app, turbo_runtime_data_bind_value_t **out_state) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_thread_head_state_bind(app->session, out_state);
}

CXX_C_API int turbo_agent_app_get_thread_trace_events_bind(
    turbo_agent_app_t *app, turbo_runtime_data_bind_value_t **out_events) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_thread_trace_events_bind(app->session, out_events);
}

CXX_C_API int turbo_agent_app_get_thread_head_trace_events_bind(
    turbo_agent_app_t *app, turbo_runtime_data_bind_value_t **out_events) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_thread_head_trace_events_bind(app->session, out_events);
}

CXX_C_API int turbo_agent_app_get_run_state_bind(
    turbo_agent_app_t *app, const char *run_id,
    turbo_runtime_data_bind_value_t **out_state) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_run_state_bind(app->session, run_id, out_state);
}

CXX_C_API int turbo_agent_app_get_run_trace_events_bind(
    turbo_agent_app_t *app, const char *run_id,
    turbo_runtime_data_bind_value_t **out_events) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_run_trace_events_bind(app->session, run_id, out_events);
}

CXX_C_API int turbo_agent_app_get_checkpoint_state_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_state) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_checkpoint_state_bind(app->session, checkpoint_id, out_state);
}

CXX_C_API int turbo_agent_app_prepare_checkpoint_state_override_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_prepare_checkpoint_state_override_bind(app->session, checkpoint_id,
                                                                   state_patch,
                                                                   out_state_override);
}

CXX_C_API int turbo_agent_app_apply_checkpoint_state_patch_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_apply_checkpoint_state_patch_bind(app->session, checkpoint_id, state_patch,
                                                          out_state_override);
}

CXX_C_API int turbo_agent_app_prepare_thread_state_override_bind(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_prepare_thread_state_override_bind(app->session, state_patch,
                                                                out_state_override);
}

CXX_C_API int turbo_agent_app_apply_thread_state_patch_bind(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_apply_thread_state_patch_bind(app->session, state_patch,
                                                      out_state_override);
}

CXX_C_API int turbo_agent_app_get_supervisor_inbox(
    turbo_agent_app_t *app, json_value_t **out_inbox_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_supervisor_inbox(app->session, out_inbox_json);
}

CXX_C_API int turbo_agent_app_get_supervisor_handoff_history(
    turbo_agent_app_t *app, json_value_t **out_history_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_supervisor_handoff_history(app->session, out_history_json);
}

CXX_C_API int turbo_agent_app_get_supervisor_inspect(
    turbo_agent_app_t *app, json_value_t **out_inspect_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_supervisor_inspect(app->session, out_inspect_json);
}

CXX_C_API int turbo_agent_app_get_orchestration_inspect(
    turbo_agent_app_t *app, json_value_t **out_inspect_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_orchestration_inspect(app->session, out_inspect_json);
}

CXX_C_API int turbo_agent_app_get_observability_index(
    turbo_agent_app_t *app, json_value_t **out_index_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_observability_index(app->session, out_index_json);
}

CXX_C_API int turbo_agent_app_append_supervisor_inbox_message_bind(
    turbo_agent_app_t *app, const char *source_agent, const char *text,
    turbo_runtime_data_bind_value_t **out_state_override) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_append_supervisor_inbox_message_bind(app->session, source_agent, text,
                                                                   out_state_override);
}

CXX_C_API int turbo_agent_app_get_checkpoint_trace_events_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_events) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_checkpoint_trace_events_bind(app->session, checkpoint_id,
                                                               out_events);
}

CXX_C_API int turbo_agent_app_get_child_run(turbo_agent_app_t *app,
                                            const json_value_t *output_item,
                                            json_value_t **out_run_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_child_run(app->session, output_item, out_run_json);
}

CXX_C_API int turbo_agent_app_get_child_checkpoint(turbo_agent_app_t *app,
                                                   const json_value_t *output_item,
                                                   json_value_t **out_checkpoint_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_child_checkpoint(app->session, output_item,
                                                  out_checkpoint_json);
}

CXX_C_API int turbo_agent_app_get_child_checkpoint_context(turbo_agent_app_t *app,
                                                           const json_value_t *output_item,
                                                           json_value_t **out_context_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_child_checkpoint_context(app->session, output_item,
                                                          out_context_json);
}

CXX_C_API int turbo_agent_app_get_child_thread_timeline_bind(
    turbo_agent_app_t *app, const json_value_t *output_item,
    turbo_runtime_data_bind_value_t **out_timeline) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_child_thread_timeline_bind(app->session, output_item,
                                                            out_timeline);
}

CXX_C_API int turbo_agent_app_get_child_branch_tree(
    turbo_agent_app_t *app, const json_value_t *output_item,
    json_value_t **out_branch_tree_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_child_branch_tree(app->session, output_item,
                                                   out_branch_tree_json);
}

CXX_C_API int turbo_agent_app_get_child_inspect(turbo_agent_app_t *app,
                                                const json_value_t *output_item,
                                                json_value_t **out_inspect_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_child_inspect(app->session, output_item, out_inspect_json);
}

CXX_C_API int turbo_agent_app_get_child_orchestration_inspect(
    turbo_agent_app_t *app, const json_value_t *output_item, json_value_t **out_inspect_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_child_orchestration_inspect(app->session, output_item,
                                                             out_inspect_json);
}

CXX_C_API int turbo_agent_app_get_child_multi_agent_inspect(
    turbo_agent_app_t *app, const json_value_t *output_item, json_value_t **out_inspect_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_child_multi_agent_inspect(app->session, output_item,
                                                           out_inspect_json);
}

CXX_C_API int turbo_agent_app_list_runs(turbo_agent_app_t *app, json_value_t **out_runs_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_list_runs(app->session, out_runs_json);
}

CXX_C_API int turbo_agent_app_list_child_runs(turbo_agent_app_t *app,
                                              const char *parent_agent_run_id,
                                              json_value_t **out_runs_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_list_child_runs(app->session, parent_agent_run_id, out_runs_json);
}

CXX_C_API int turbo_agent_app_list_child_checkpoints(turbo_agent_app_t *app,
                                                     const json_value_t *output_item,
                                                     json_value_t **out_checkpoints_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_list_child_checkpoints(app->session, output_item, out_checkpoints_json);
}

CXX_C_API int turbo_agent_app_list_checkpoints(turbo_agent_app_t *app, const char *run_id,
                                               json_value_t **out_checkpoints_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_list_checkpoints(app->session, run_id, out_checkpoints_json);
}

CXX_C_API int turbo_agent_app_list_thread_lineage(turbo_agent_app_t *app,
                                                  json_value_t **out_lineage_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_list_thread_lineage(app->session, out_lineage_json);
}

CXX_C_API int turbo_agent_app_get_branch_tree(turbo_agent_app_t *app,
                                              json_value_t **out_branch_tree_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_branch_tree(app->session, out_branch_tree_json);
}

CXX_C_API int turbo_agent_app_get_checkpoint_context(turbo_agent_app_t *app,
                                                     const char *checkpoint_id,
                                                     json_value_t **out_context_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_get_checkpoint_context(app->session, checkpoint_id, out_context_json);
}

CXX_C_API int turbo_agent_app_load_history_events_bind(
    turbo_agent_app_t *app, const char *run_id, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_events) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_load_history_events_bind(app->session, run_id, checkpoint_id,
                                                      out_events);
}

CXX_C_API int turbo_agent_app_load_thread_history_events_bind(
    turbo_agent_app_t *app, turbo_runtime_data_bind_value_t **out_events) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_load_thread_history_events_bind(app->session, out_events);
}

CXX_C_API int turbo_agent_app_replay_history_bind(
    turbo_agent_app_t *app, const char *run_id, const char *checkpoint_id,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_replay_history_bind(app->session, run_id, checkpoint_id, event_sink,
                                                 event_sink_user_data);
}

CXX_C_API int turbo_agent_app_replay_thread_history_bind(
    turbo_agent_app_t *app, turbo_event_sink_bind_fn event_sink, void *event_sink_user_data) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_replay_thread_history_bind(app->session, event_sink,
                                                        event_sink_user_data);
}

CXX_C_API int turbo_agent_app_observe_history_bind(
    turbo_agent_app_t *app, const char *run_id, const char *checkpoint_id,
    const turbo_agent_observer_bind_sink_t *sink) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_observe_history_bind(app->session, run_id, checkpoint_id, sink);
}

CXX_C_API int turbo_agent_app_observe_thread_history_bind(
    turbo_agent_app_t *app, const turbo_agent_observer_bind_sink_t *sink) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_observe_thread_history_bind(app->session, sink);
}

/* ── Core execution ────────────────────────────────────────────────────────── */

CXX_C_API int turbo_agent_app_exec_start(
    turbo_agent_app_t *app, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state, const turbo_graph_run_options_t *options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_start_graph(app->session, graph, state, options, event_sink,
                                         event_sink_user_data, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_app_exec_resume(
    turbo_agent_app_t *app, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_app_exec_options_t *exec_options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_resume_graph(app->session, graph, input, options, exec_options,
                                          event_sink, event_sink_user_data, out_summary_json,
                                          out_state);
}

CXX_C_API int turbo_agent_app_exec_fork(
    turbo_agent_app_t *app, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_app_exec_options_t *exec_options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_fork_graph(app->session, graph, input, options, exec_options,
                                        event_sink, event_sink_user_data, out_summary_json,
                                        out_state);
}

/* ── Preset execution ──────────────────────────────────────────────────────── */

CXX_C_API int turbo_agent_app_start_preset(
    turbo_agent_app_t *app, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *state, const turbo_graph_run_options_t *options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_start_preset(app->session, kind, state, options, event_sink,
                                          event_sink_user_data, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_app_resume_preset(
    turbo_agent_app_t *app, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_app_exec_options_t *exec_options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_resume_preset(app->session, kind, input, options, exec_options,
                                           event_sink, event_sink_user_data, out_summary_json,
                                           out_state);
}

CXX_C_API int turbo_agent_app_fork_preset(
    turbo_agent_app_t *app, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_app_exec_options_t *exec_options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_fork_preset(app->session, kind, input, options, exec_options,
                                         event_sink, event_sink_user_data, out_summary_json,
                                         out_state);
}

/* ── High-level convenience entry points ───────────────────────────────────── */

CXX_C_API int turbo_agent_app_start_text(turbo_agent_app_t *app, const char *user_text,
                                         const turbo_graph_run_options_t *options,
                                         json_value_t **out_summary_json,
                                         turbo_runtime_data_bind_value_t **out_state) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_start_text(app->session, user_text, options, out_summary_json,
                                        out_state);
}

CXX_C_API int turbo_agent_app_start_text_stream(
    turbo_agent_app_t *app, const char *user_text,
    const turbo_graph_run_options_t *options, turbo_event_stream_mode_t stream_mode,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_start_text_stream(
      app->session, user_text, options, stream_mode, event_sink,
      event_sink_user_data, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_app_start_messages(turbo_agent_app_t *app,
                                             const turbo_runtime_data_bind_value_t *messages,
                                             const turbo_graph_run_options_t *options,
                                             json_value_t **out_summary_json,
                                             turbo_runtime_data_bind_value_t **out_state) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_start_messages(app->session, messages, options, out_summary_json,
                                            out_state);
}

CXX_C_API int turbo_agent_app_start_messages_stream(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *messages,
    const turbo_graph_run_options_t *options, turbo_event_stream_mode_t stream_mode,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_start_messages_stream(
      app->session, messages, options, stream_mode, event_sink,
      event_sink_user_data, out_summary_json, out_state);
}

CXX_C_API char *turbo_agent_app_result_text(const turbo_runtime_data_bind_value_t *state) {
  return turbo_agent_session_result_text(state);
}

CXX_C_API int turbo_agent_app_invoke_text(turbo_agent_app_t *app, const char *user_text,
                                          const turbo_graph_run_options_t *options,
                                          char **out_text, json_value_t **out_summary_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_invoke_text(app->session, user_text, options, out_text,
                                         out_summary_json);
}

CXX_C_API int turbo_agent_app_invoke_messages_text(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *messages,
    const turbo_graph_run_options_t *options, char **out_text, json_value_t **out_summary_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_invoke_messages_text(app->session, messages, options, out_text,
                                                  out_summary_json);
}

CXX_C_API int turbo_agent_app_invoke_json(turbo_agent_app_t *app, const char *user_text,
                                          const turbo_graph_run_options_t *options,
                                          json_value_t **out_json,
                                          json_value_t **out_summary_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_invoke_json(app->session, user_text, options, out_json,
                                         out_summary_json);
}

CXX_C_API int turbo_agent_app_invoke_messages_json(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *messages,
    const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_invoke_messages_json(app->session, messages, options, out_json,
                                                  out_summary_json);
}

CXX_C_API int turbo_agent_app_batch_text(
    turbo_agent_app_t *app, const char *const *user_texts, size_t count,
    const turbo_graph_run_options_t *options, json_value_t **out_results_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_batch_text(app->session, user_texts, count, options,
                                        out_results_json);
}

CXX_C_API int turbo_agent_app_memory_get(const turbo_agent_app_t *app, const char *memory_namespace,
                                         const char *key, char **out_value_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_memory_get(app->session, memory_namespace, key, out_value_json);
}

CXX_C_API int turbo_agent_app_memory_put(const turbo_agent_app_t *app, const char *memory_namespace,
                                         const char *key, const char *value_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_memory_put(app->session, memory_namespace, key, value_json);
}

CXX_C_API int turbo_agent_app_memory_put_context(const turbo_agent_app_t *app,
                                                 const char *memory_namespace, const char *key,
                                                 const char *scope, const char *path,
                                                 const char *text) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_memory_put_context(app->session, memory_namespace, key, scope, path,
                                                text);
}

CXX_C_API int turbo_agent_app_memory_delete(const turbo_agent_app_t *app,
                                            const char *memory_namespace, const char *key) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_memory_delete(app->session, memory_namespace, key);
}

CXX_C_API int turbo_agent_app_memory_list(const turbo_agent_app_t *app,
                                          const char *namespace_prefix,
                                          json_value_t **out_records_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_memory_list(app->session, namespace_prefix, out_records_json);
}

CXX_C_API int turbo_agent_app_memory_list_records(const turbo_agent_app_t *app,
                                                  const char *namespace_prefix,
                                                  json_value_t **out_records_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_memory_list_records(app->session, namespace_prefix, out_records_json);
}

CXX_C_API int turbo_agent_app_memory_get_record(const turbo_agent_app_t *app,
                                                const char *memory_namespace, const char *key,
                                                json_value_t **out_record_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_memory_get_record(app->session, memory_namespace, key,
                                               out_record_json);
}

CXX_C_API int turbo_agent_app_memory_put_record(const turbo_agent_app_t *app,
                                                const json_value_t *record_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_memory_put_record(app->session, record_json);
}

CXX_C_API int turbo_agent_app_memory_validate_record(const json_value_t *record_json) {
  return turbo_agent_session_memory_validate_record(record_json);
}

CXX_C_API int turbo_agent_app_memory_query_records(const turbo_agent_app_t *app,
                                                   const char *namespace_prefix,
                                                   const char *kind,
                                                   const char *key_prefix,
                                                   const char *text_substring,
                                                   json_value_t **out_records_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_memory_query_records(app->session, namespace_prefix, kind,
                                                  key_prefix, text_substring, out_records_json);
}

CXX_C_API int turbo_agent_app_memory_query_records_ex(
    const turbo_agent_app_t *app, const turbo_agent_memory_query_options_t *options,
    json_value_t **out_records_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_session_memory_query_records_ex(app->session, options, out_records_json);
}
