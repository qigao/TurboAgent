#include "turbo_agent_remote_app.h"

#include <stdlib.h>
#include <string.h>

struct turbo_agent_remote_app_s {
  turbo_agent_remote_session_t *session;
  char *graph_name;
};

static char *turbo_agent_remote_app_strdup_or_null(const char *value) {
  size_t len;
  char *copy;

  if (!value || !value[0]) {
    return NULL;
  }
  len = strlen(value);
  copy = (char *)malloc(len + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, value, len + 1);
  return copy;
}

CXX_C_API turbo_agent_remote_app_t *
turbo_agent_remote_app_create(const turbo_agent_remote_app_config_t *config) {
  turbo_agent_remote_app_t *app;

  if (!config || !config->session_config || !config->graph_name || !config->graph_name[0]) {
    return NULL;
  }

  app = (turbo_agent_remote_app_t *)calloc(1, sizeof(*app));
  if (!app) {
    return NULL;
  }

  app->session = turbo_agent_remote_session_create(config->session_config);
  if (!app->session) {
    free(app);
    return NULL;
  }

  app->graph_name = turbo_agent_remote_app_strdup_or_null(config->graph_name);
  if (!app->graph_name) {
    turbo_agent_remote_session_destroy(app->session);
    free(app);
    return NULL;
  }

  return app;
}

CXX_C_API void turbo_agent_remote_app_destroy(turbo_agent_remote_app_t *app) {
  if (!app) {
    return;
  }
  turbo_agent_remote_session_destroy(app->session);
  free(app->graph_name);
  free(app);
}

CXX_C_API turbo_agent_remote_session_t *
turbo_agent_remote_app_session(const turbo_agent_remote_app_t *app) {
  return app ? app->session : NULL;
}

CXX_C_API const char *turbo_agent_remote_app_graph_name(const turbo_agent_remote_app_t *app) {
  return app ? app->graph_name : NULL;
}

CXX_C_API const char *turbo_agent_remote_app_thread_id(const turbo_agent_remote_app_t *app) {
  return app ? turbo_agent_remote_session_thread_id(app->session) : NULL;
}

CXX_C_API const char *turbo_agent_remote_app_last_run_id(const turbo_agent_remote_app_t *app) {
  return app ? turbo_agent_remote_session_last_run_id(app->session) : NULL;
}

CXX_C_API const char *turbo_agent_remote_app_last_checkpoint_id(
    const turbo_agent_remote_app_t *app) {
  return app ? turbo_agent_remote_session_last_checkpoint_id(app->session) : NULL;
}

CXX_C_API int turbo_agent_remote_app_get_startup_diagnostics(
    turbo_agent_remote_app_t *app, json_value_t **out_diagnostics_json) {
  if (!app || !app->session || !app->graph_name || !out_diagnostics_json) {
    return -1;
  }
  return turbo_agent_remote_session_get_startup_diagnostics(
      app->session, app->graph_name, out_diagnostics_json);
}

CXX_C_API int turbo_agent_remote_app_start_json_value_graph(
    turbo_agent_remote_app_t *app, const json_value_t *state,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state) {
  if (!app || !app->session || !app->graph_name) {
    return -1;
  }
  return turbo_agent_remote_session_start_json_value_graph(app->session, app->graph_name, state,
                                                     options, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_remote_app_start_text(
    turbo_agent_remote_app_t *app, const char *user_text,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state) {
  if (!app || !app->session || !app->graph_name) {
    return -1;
  }
  return turbo_agent_remote_session_start_text(app->session, app->graph_name, user_text, options,
                                               out_summary_json, out_state);
}

CXX_C_API int turbo_agent_remote_app_start_messages(
    turbo_agent_remote_app_t *app, const json_value_t *messages,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state) {
  if (!app || !app->session || !app->graph_name) {
    return -1;
  }
  return turbo_agent_remote_session_start_messages(app->session, app->graph_name, messages,
                                                   options, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_remote_app_resume_json_value_graph(
    turbo_agent_remote_app_t *app, const char *checkpoint_id,
    const json_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state) {
  if (!app || !app->session || !app->graph_name) {
    return -1;
  }
  return turbo_agent_remote_session_resume_json_value_graph(app->session, app->graph_name,
                                                      checkpoint_id, state_override, options,
                                                      out_summary_json, out_state);
}

CXX_C_API int turbo_agent_remote_app_fork_json_value_graph(
    turbo_agent_remote_app_t *app, const char *checkpoint_id,
    const json_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state) {
  if (!app || !app->session || !app->graph_name) {
    return -1;
  }
  return turbo_agent_remote_session_fork_json_value_graph(app->session, app->graph_name, checkpoint_id,
                                                    state_override, options, out_summary_json,
                                                    out_state);
}

CXX_C_API int turbo_agent_remote_app_invoke_text(
    turbo_agent_remote_app_t *app, const char *user_text,
    const turbo_graph_run_options_t *options, char **out_text, json_value_t **out_summary_json) {
  if (!app || !app->session || !app->graph_name) {
    return -1;
  }
  return turbo_agent_remote_session_invoke_text(app->session, app->graph_name, user_text, options,
                                                out_text, out_summary_json);
}

CXX_C_API int turbo_agent_remote_app_invoke_messages_text(
    turbo_agent_remote_app_t *app, const json_value_t *messages,
    const turbo_graph_run_options_t *options, char **out_text, json_value_t **out_summary_json) {
  if (!app || !app->session || !app->graph_name) {
    return -1;
  }
  return turbo_agent_remote_session_invoke_messages_text(app->session, app->graph_name, messages,
                                                         options, out_text, out_summary_json);
}

CXX_C_API int turbo_agent_remote_app_invoke_json(
    turbo_agent_remote_app_t *app, const char *user_text,
    const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json) {
  if (!app || !app->session || !app->graph_name) {
    return -1;
  }
  return turbo_agent_remote_session_invoke_json(app->session, app->graph_name, user_text, options,
                                                out_json, out_summary_json);
}

CXX_C_API int turbo_agent_remote_app_invoke_messages_json(
    turbo_agent_remote_app_t *app, const json_value_t *messages,
    const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json) {
  if (!app || !app->session || !app->graph_name) {
    return -1;
  }
  return turbo_agent_remote_session_invoke_messages_json(app->session, app->graph_name, messages,
                                                         options, out_json, out_summary_json);
}

CXX_C_API int turbo_agent_remote_app_get_thread_state_json_value(
    turbo_agent_remote_app_t *app, json_value_t **out_state) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_thread_state_json_value(app->session, out_state);
}

CXX_C_API int turbo_agent_remote_app_memory_delete_record(
    const turbo_agent_remote_app_t *app, const char *memory_namespace, const char *key) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_memory_delete_record(app->session, memory_namespace, key);
}

CXX_C_API int turbo_agent_remote_app_memory_list_records(
    const turbo_agent_remote_app_t *app, const char *namespace_prefix,
    json_value_t **out_records_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_memory_list_records(app->session, namespace_prefix,
                                                        out_records_json);
}

CXX_C_API int turbo_agent_remote_app_memory_get_record(
    const turbo_agent_remote_app_t *app, const char *memory_namespace, const char *key,
    json_value_t **out_record_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_memory_get_record(app->session, memory_namespace, key,
                                                      out_record_json);
}

CXX_C_API int turbo_agent_remote_app_memory_put_record(
    const turbo_agent_remote_app_t *app, const json_value_t *record_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_memory_put_record(app->session, record_json);
}

CXX_C_API int turbo_agent_remote_app_memory_validate_record(
    const turbo_agent_remote_app_t *app, const json_value_t *record_json, int *out_valid) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_memory_validate_record(app->session, record_json, out_valid);
}

CXX_C_API int turbo_agent_remote_app_memory_query_records(
    const turbo_agent_remote_app_t *app, const char *namespace_prefix, const char *kind,
    const char *key_prefix, const char *text_substring, json_value_t **out_records_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_memory_query_records(app->session, namespace_prefix, kind,
                                                          key_prefix, text_substring,
                                                          out_records_json);
}

CXX_C_API int turbo_agent_remote_app_memory_query_records_ex(
    const turbo_agent_remote_app_t *app, const turbo_agent_memory_query_options_t *options,
    json_value_t **out_records_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_memory_query_records_ex(app->session, options,
                                                            out_records_json);
}

CXX_C_API int turbo_agent_remote_app_get_thread(turbo_agent_remote_app_t *app,
                                                json_value_t **out_thread_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_thread(app->session, out_thread_json);
}

CXX_C_API int turbo_agent_remote_app_get_latest_run(turbo_agent_remote_app_t *app,
                                                    json_value_t **out_run_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_latest_run(app->session, out_run_json);
}

CXX_C_API int turbo_agent_remote_app_get_pending_run(turbo_agent_remote_app_t *app,
                                                     json_value_t **out_run_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_pending_run(app->session, out_run_json);
}

CXX_C_API int turbo_agent_remote_app_get_checkpoint_context(
    turbo_agent_remote_app_t *app, const char *checkpoint_id, json_value_t **out_context_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_checkpoint_context(app->session, checkpoint_id,
                                                           out_context_json);
}

CXX_C_API int turbo_agent_remote_app_get_observability_index(
    turbo_agent_remote_app_t *app, json_value_t **out_index_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_observability_index(app->session, out_index_json);
}

CXX_C_API int turbo_agent_remote_app_get_thread_timeline_json_value(
    turbo_agent_remote_app_t *app, json_value_t **out_timeline) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_thread_timeline_json_value(app->session, out_timeline);
}

CXX_C_API int turbo_agent_remote_app_load_thread_history_events_json_value(
    turbo_agent_remote_app_t *app, json_value_t **out_events) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_load_thread_history_events_json_value(app->session, out_events);
}

CXX_C_API int turbo_agent_remote_app_replay_thread_history_json_value(
    turbo_agent_remote_app_t *app, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_replay_thread_history_json_value(app->session, event_sink,
                                                               event_sink_user_data);
}

CXX_C_API int turbo_agent_remote_app_observe_thread_history_json_value(
    turbo_agent_remote_app_t *app, const turbo_agent_observer_json_value_sink_t *sink) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_observe_thread_history_json_value(app->session, sink);
}

CXX_C_API int turbo_agent_remote_app_get_thread_trace_events_json_value(
    turbo_agent_remote_app_t *app, json_value_t **out_events) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_thread_trace_events_json_value(app->session, out_events);
}

CXX_C_API int turbo_agent_remote_app_get_branch_tree(
    turbo_agent_remote_app_t *app, json_value_t **out_branch_tree_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_branch_tree(app->session, out_branch_tree_json);
}

CXX_C_API int turbo_agent_remote_app_list_thread_lineage(
    turbo_agent_remote_app_t *app, json_value_t **out_lineage_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_list_thread_lineage(app->session, out_lineage_json);
}

CXX_C_API int turbo_agent_remote_app_get_supervisor_inbox(
    turbo_agent_remote_app_t *app, json_value_t **out_inbox_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_supervisor_inbox(app->session, out_inbox_json);
}

CXX_C_API int turbo_agent_remote_app_get_supervisor_handoff_history(
    turbo_agent_remote_app_t *app, json_value_t **out_history_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_supervisor_handoff_history(app->session, out_history_json);
}

CXX_C_API int turbo_agent_remote_app_get_supervisor_inspect(
    turbo_agent_remote_app_t *app, json_value_t **out_inspect_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_supervisor_inspect(app->session, out_inspect_json);
}

CXX_C_API int turbo_agent_remote_app_list_child_runs(
    turbo_agent_remote_app_t *app, const char *parent_agent_run_id,
    json_value_t **out_runs_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_list_child_runs(app->session, parent_agent_run_id,
                                                    out_runs_json);
}

CXX_C_API int turbo_agent_remote_app_get_orchestration_inspect(
    turbo_agent_remote_app_t *app, json_value_t **out_inspect_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_orchestration_inspect(app->session, out_inspect_json);
}

CXX_C_API int turbo_agent_remote_app_get_child_run(
    turbo_agent_remote_app_t *app, const json_value_t *output_item,
    json_value_t **out_run_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_child_run(app->session, output_item, out_run_json);
}

CXX_C_API int turbo_agent_remote_app_get_child_checkpoint(
    turbo_agent_remote_app_t *app, const json_value_t *output_item,
    json_value_t **out_checkpoint_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_child_checkpoint(app->session, output_item,
                                                         out_checkpoint_json);
}

CXX_C_API int turbo_agent_remote_app_get_child_checkpoint_context(
    turbo_agent_remote_app_t *app, const json_value_t *output_item,
    json_value_t **out_context_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_child_checkpoint_context(app->session, output_item,
                                                                 out_context_json);
}

CXX_C_API int turbo_agent_remote_app_get_child_thread_timeline_json_value(
    turbo_agent_remote_app_t *app, const json_value_t *output_item,
    json_value_t **out_timeline) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_child_thread_timeline_json_value(app->session, output_item,
                                                                   out_timeline);
}

CXX_C_API int turbo_agent_remote_app_get_child_branch_tree(
    turbo_agent_remote_app_t *app, const json_value_t *output_item,
    json_value_t **out_branch_tree_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_child_branch_tree(app->session, output_item,
                                                          out_branch_tree_json);
}

CXX_C_API int turbo_agent_remote_app_list_child_checkpoints(
    turbo_agent_remote_app_t *app, const json_value_t *output_item,
    json_value_t **out_checkpoints_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_list_child_checkpoints(app->session, output_item,
                                                           out_checkpoints_json);
}

CXX_C_API int turbo_agent_remote_app_load_child_history_events_json_value(
    turbo_agent_remote_app_t *app, const json_value_t *output_item,
    json_value_t **out_events) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_load_child_history_events_json_value(app->session, output_item,
                                                                   out_events);
}

CXX_C_API int turbo_agent_remote_app_get_child_trace_events_json_value(
    turbo_agent_remote_app_t *app, const json_value_t *output_item,
    json_value_t **out_events) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_child_trace_events_json_value(app->session, output_item,
                                                                out_events);
}

CXX_C_API int turbo_agent_remote_app_get_child_inspect(
    turbo_agent_remote_app_t *app, const json_value_t *output_item,
    json_value_t **out_inspect_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_child_inspect(app->session, output_item, out_inspect_json);
}

CXX_C_API int turbo_agent_remote_app_get_child_orchestration_inspect(
    turbo_agent_remote_app_t *app, const json_value_t *output_item,
    json_value_t **out_inspect_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_child_orchestration_inspect(app->session, output_item,
                                                                    out_inspect_json);
}

CXX_C_API int turbo_agent_remote_app_get_child_multi_agent_inspect(
    turbo_agent_remote_app_t *app, const json_value_t *output_item,
    json_value_t **out_inspect_json) {
  if (!app || !app->session) {
    return -1;
  }
  return turbo_agent_remote_session_get_child_multi_agent_inspect(app->session, output_item,
                                                                  out_inspect_json);
}

CXX_C_API int turbo_agent_remote_app_resume_thread_command_json_value(
    turbo_agent_remote_app_t *app, const json_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state) {
  if (!app || !app->session || !app->graph_name) {
    return -1;
  }
  return turbo_agent_remote_session_resume_thread_command_json_value(
      app->session, app->graph_name, command, options, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_remote_app_fork_thread_command_json_value(
    turbo_agent_remote_app_t *app, const json_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state) {
  if (!app || !app->session || !app->graph_name) {
    return -1;
  }
  return turbo_agent_remote_session_fork_thread_command_json_value(
      app->session, app->graph_name, command, options, out_summary_json, out_state);
}
