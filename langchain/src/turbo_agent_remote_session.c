#include "turbo_agent_remote_session.h"
#include "turbo_agent_session.h"
#include "turbo_agent_state.h"
#include "turbo_agent_util_internal.h"
#include <stdlib.h>
#include <string.h>

static char *turbo_agent_remote_session_strdup_or_null(const char *value) {
  return (char *)turbo_agent_util_strdup(value);
}

struct turbo_agent_remote_session_s {
  turbo_agent_runtime_remote_client_t *client;
  int owns_client;
  char *thread_id;
  char *last_run_id;
  char *last_checkpoint_id;
};



static void turbo_agent_remote_session_replace_string(char **slot, const char *value) {
  char *replacement;

  if (!slot) {
    return;
  }
  replacement = turbo_agent_remote_session_strdup_or_null(value);
  tstr_free(*slot);
  *slot = replacement;
}

static int turbo_agent_remote_session_capture_summary(turbo_agent_remote_session_t *session,
                                                      const json_value_t *summary) {
  const char *thread_id;
  const char *run_id;
  const json_value_t *checkpoint_value;
  const char *checkpoint_id = NULL;

  if (!session || !summary) {
    return -1;
  }
  thread_id = turbo_json_get_string(summary, "thread_id");
  run_id = turbo_json_get_string(summary, "run_id");
  checkpoint_value = turbo_json_object_get(summary, "checkpoint_id");
  if (!thread_id || !run_id) {
    return -1;
  }
  if (checkpoint_value && turbo_json_type(checkpoint_value) == TURBO_JSON_STRING) {
    checkpoint_id = turbo_json_get_string(summary, "checkpoint_id");
  }

  turbo_agent_remote_session_replace_string(&session->thread_id, thread_id);
  turbo_agent_remote_session_replace_string(&session->last_run_id, run_id);
  if (checkpoint_id && checkpoint_id[0] != '\0') {
    turbo_agent_remote_session_replace_string(&session->last_checkpoint_id, checkpoint_id);
  }
  return 0;
}

static const char *turbo_agent_remote_session_resolve_thread_id(
    const turbo_agent_remote_session_t *session) {
  return session ? session->thread_id : NULL;
}

static const char *turbo_agent_remote_session_resolve_checkpoint_id(
    const turbo_agent_remote_session_t *session, const char *checkpoint_id) {
  if (checkpoint_id && checkpoint_id[0] != '\0') {
    return checkpoint_id;
  }
  return session ? session->last_checkpoint_id : NULL;
}

static const char *turbo_agent_remote_session_resolve_parent_agent_run_id(
    const turbo_agent_remote_session_t *session, const char *parent_agent_run_id) {
  if (parent_agent_run_id && parent_agent_run_id[0] != '\0') {
    return parent_agent_run_id;
  }
  return session ? session->last_run_id : NULL;
}

static int turbo_agent_remote_session_call_start(
    turbo_agent_remote_session_t *session, const char *graph_name,
    const turbo_runtime_data_bind_value_t *state, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  char *thread_id = NULL;
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !graph_name || !graph_name[0] || !out_summary_json ||
      !out_state) {
    return -1;
  }
  thread_id = turbo_agent_remote_session_strdup_or_null(
      turbo_agent_remote_session_resolve_thread_id(session));
  rc = turbo_agent_runtime_remote_client_start_bind_graph(
      session->client, graph_name, state, options, thread_id, out_summary_json, out_state,
      &error_json);
  tstr_free(thread_id);
  turbo_free_json(&error_json);
  if (rc != 0) {
    return rc;
  }
  return turbo_agent_remote_session_capture_summary(session, *out_summary_json);
}

static int turbo_agent_remote_session_call_resume(
    turbo_agent_remote_session_t *session, const char *graph_name, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state, int fork_mode) {
  const char *resolved_checkpoint_id;
  char *owned_checkpoint_id = NULL;
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !graph_name || !graph_name[0] || !out_summary_json ||
      !out_state) {
    return -1;
  }
  resolved_checkpoint_id =
      turbo_agent_remote_session_resolve_checkpoint_id(session, checkpoint_id);
  if (!resolved_checkpoint_id) {
    return -1;
  }
  owned_checkpoint_id = turbo_agent_remote_session_strdup_or_null(resolved_checkpoint_id);
  if (!owned_checkpoint_id) {
    return -1;
  }
  if (fork_mode) {
    rc = turbo_agent_runtime_remote_client_fork_bind_graph(
        session->client, graph_name, owned_checkpoint_id, state_override, options,
        out_summary_json, out_state, &error_json);
  } else {
    rc = turbo_agent_runtime_remote_client_resume_bind_graph(
        session->client, graph_name, owned_checkpoint_id, state_override, options,
        out_summary_json, out_state, &error_json);
  }
  tstr_free(owned_checkpoint_id);
  turbo_free_json(&error_json);
  if (rc != 0) {
    return rc;
  }
  return turbo_agent_remote_session_capture_summary(session, *out_summary_json);
}

static int turbo_agent_remote_session_call_thread_command(
    turbo_agent_remote_session_t *session, const char *graph_name,
    const turbo_runtime_data_bind_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state, int fork_mode) {
  const char *thread_id;
  char *owned_thread_id = NULL;
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !graph_name || !graph_name[0] || !command ||
      !out_summary_json || !out_state) {
    return -1;
  }
  thread_id = turbo_agent_remote_session_resolve_thread_id(session);
  if (!thread_id || !thread_id[0]) {
    return -1;
  }
  owned_thread_id = turbo_agent_remote_session_strdup_or_null(thread_id);
  if (!owned_thread_id) {
    return -1;
  }
  if (fork_mode) {
    rc = turbo_agent_runtime_remote_client_fork_thread_command_bind(
        session->client, graph_name, owned_thread_id, command, options, out_summary_json, out_state,
        &error_json);
  } else {
    rc = turbo_agent_runtime_remote_client_resume_thread_command_bind(
        session->client, graph_name, owned_thread_id, command, options, out_summary_json, out_state,
        &error_json);
  }
  tstr_free(owned_thread_id);
  turbo_free_json(&error_json);
  if (rc != 0) {
    return rc;
  }
  return turbo_agent_remote_session_capture_summary(session, *out_summary_json);
}

static int turbo_agent_remote_session_get_observability_object(
    turbo_agent_remote_session_t *session, const char *field_name,
    json_value_t **out_object_json) {
  json_value_t *index_json = NULL;
  const json_value_t *field_json;
  int rc;

  if (out_object_json) {
    *out_object_json = NULL;
  }
  if (!session || !session->client || !field_name || !field_name[0] || !out_object_json) {
    return -1;
  }

  rc = turbo_agent_remote_session_get_observability_index(session, &index_json);
  if (rc != 0 || !index_json) {
    turbo_free_json(&index_json);
    return rc;
  }

  field_json = turbo_json_object_get(index_json, field_name);
  if (!field_json || turbo_json_type(field_json) != TURBO_JSON_OBJECT) {
    turbo_free_json(&index_json);
    return -1;
  }

  *out_object_json = turbo_json_clone(field_json);
  turbo_free_json(&index_json);
  return *out_object_json ? 0 : -1;
}

static int turbo_agent_remote_session_get_supervisor_array_json_local(
    turbo_agent_remote_session_t *session,
    const json_value_t *(*selector)(const json_value_t *state),
    json_value_t **out_array_json) {
  turbo_runtime_data_bind_value_t *state_bind = NULL;
  json_value_t *state_json = NULL;
  json_value_t *array_json = NULL;
  const json_value_t *selected = NULL;
  int rc = -1;

  if (!session || !selector || !out_array_json) {
    return -1;
  }
  *out_array_json = NULL;
  if (turbo_agent_remote_session_get_thread_state_bind(session, &state_bind) != 0 || !state_bind) {
    goto cleanup;
  }
  state_json = turbo_runtime_data_bind_value_to_json(state_bind);
  if (!state_json) {
    goto cleanup;
  }
  selected = selector(state_json);
  if (selected && turbo_json_type(selected) == TURBO_JSON_ARRAY) {
    array_json = turbo_json_clone(selected);
  } else {
    array_json = turbo_json_create_array();
  }
  if (!array_json) {
    goto cleanup;
  }
  *out_array_json = array_json;
  array_json = NULL;
  rc = 0;

cleanup:
  turbo_free_json(&array_json);
  turbo_free_json(&state_json);
  turbo_runtime_data_bind_value_destroy(state_bind);
  return rc;
}

static int turbo_agent_remote_session_build_supervisor_inspect_local(
    turbo_agent_remote_session_t *session, json_value_t **out_inspect_json) {
  turbo_runtime_data_bind_value_t *state_bind = NULL;
  turbo_runtime_data_bind_value_t *control_bind = NULL;
  turbo_runtime_data_bind_value_t *workflow_bind = NULL;
  json_value_t *state_json = NULL;
  json_value_t *control_json = NULL;
  json_value_t *workflow_json = NULL;
  json_value_t *inspect_json = NULL;
  json_value_t *inbox_json = NULL;
  json_value_t *history_json = NULL;
  json_value_t *supervisor_json = NULL;
  const json_value_t *state_inbox = NULL;
  const json_value_t *state_history = NULL;
  const json_value_t *control_supervisor = NULL;
  int rc = -1;

  if (!session || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;
  if (turbo_agent_remote_session_get_thread_state_bind(session, &state_bind) != 0 || !state_bind) {
    goto cleanup;
  }
  state_json = turbo_runtime_data_bind_value_to_json(state_bind);
  if (!state_json) {
    goto cleanup;
  }
  control_bind = turbo_agent_state_control_snapshot_bind(state_bind);
  workflow_bind = turbo_agent_state_workflow_snapshot_bind(state_bind);
  if (!control_bind || !workflow_bind) {
    goto cleanup;
  }
  control_json = turbo_runtime_data_bind_value_to_json(control_bind);
  workflow_json = turbo_runtime_data_bind_value_to_json(workflow_bind);
  if (!control_json || !workflow_json) {
    goto cleanup;
  }

  state_inbox = turbo_agent_state_supervisor_inbox(state_json);
  state_history = turbo_agent_state_supervisor_handoff_history(state_json);
  control_supervisor = turbo_json_object_get(control_json, "supervisor");

  inbox_json = state_inbox && turbo_json_type(state_inbox) == TURBO_JSON_ARRAY
                   ? turbo_json_clone(state_inbox)
                   : turbo_json_create_array();
  history_json = state_history && turbo_json_type(state_history) == TURBO_JSON_ARRAY
                     ? turbo_json_clone(state_history)
                     : turbo_json_create_array();
  supervisor_json = control_supervisor && turbo_json_type(control_supervisor) == TURBO_JSON_OBJECT
                        ? turbo_json_clone(control_supervisor)
                        : turbo_json_create_object();
  inspect_json = turbo_json_create_object();
  if (!inbox_json || !history_json || !supervisor_json || !inspect_json) {
    goto cleanup;
  }

  turbo_json_object_add(inspect_json, "supervisor", supervisor_json);
  supervisor_json = NULL;
  turbo_json_object_add(inspect_json, "inbox", inbox_json);
  inbox_json = NULL;
  turbo_json_object_add(inspect_json, "handoff_history", history_json);
  history_json = NULL;
  turbo_json_object_add(inspect_json, "control_snapshot", control_json);
  control_json = NULL;
  turbo_json_object_add(inspect_json, "workflow_snapshot", workflow_json);
  workflow_json = NULL;

  *out_inspect_json = inspect_json;
  inspect_json = NULL;
  rc = 0;

cleanup:
  turbo_free_json(&supervisor_json);
  turbo_free_json(&history_json);
  turbo_free_json(&inbox_json);
  turbo_free_json(&inspect_json);
  turbo_free_json(&workflow_json);
  turbo_free_json(&control_json);
  turbo_free_json(&state_json);
  turbo_runtime_data_bind_value_destroy(workflow_bind);
  turbo_runtime_data_bind_value_destroy(control_bind);
  turbo_runtime_data_bind_value_destroy(state_bind);
  return rc;
}

static int turbo_agent_remote_session_build_orchestration_inspect_local(
    turbo_agent_remote_session_t *session, json_value_t **out_inspect_json) {
  json_value_t *inspect_json = NULL;
  json_value_t *supervisor_inspect_json = NULL;
  json_value_t *thread_lineage_json = NULL;
  json_value_t *branch_tree_json = NULL;
  json_value_t *child_runs_json = NULL;
  json_value_t *thread_timeline_json = NULL;
  turbo_runtime_data_bind_value_t *thread_timeline_bind = NULL;
  int rc = -1;

  if (!session || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;
  if (turbo_agent_remote_session_get_supervisor_inspect(session, &supervisor_inspect_json) != 0 ||
      !supervisor_inspect_json) {
    goto cleanup;
  }
  if (turbo_agent_remote_session_get_thread_timeline_bind(session, &thread_timeline_bind) == 0 &&
      thread_timeline_bind) {
    thread_timeline_json = turbo_runtime_data_bind_value_to_json(thread_timeline_bind);
    if (!thread_timeline_json) {
      goto cleanup;
    }
  }
  if (turbo_agent_remote_session_list_thread_lineage(session, &thread_lineage_json) != 0 ||
      !thread_lineage_json) {
    goto cleanup;
  }
  if (turbo_agent_remote_session_get_branch_tree(session, &branch_tree_json) != 0 ||
      !branch_tree_json) {
    goto cleanup;
  }
  if (session->last_run_id && session->last_run_id[0] != '\0') {
    if (turbo_agent_remote_session_list_child_runs(session, session->last_run_id, &child_runs_json) !=
            0 ||
        !child_runs_json) {
      goto cleanup;
    }
  } else {
    child_runs_json = turbo_json_create_array();
  }
  if (!child_runs_json) {
    goto cleanup;
  }

  inspect_json = turbo_json_create_object();
  if (!inspect_json) {
    goto cleanup;
  }
  turbo_json_object_add(inspect_json, "supervisor_inspect", supervisor_inspect_json);
  supervisor_inspect_json = NULL;
  if (thread_timeline_json) {
    turbo_json_object_add(inspect_json, "thread_timeline", thread_timeline_json);
    thread_timeline_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "thread_timeline");
  }
  turbo_json_object_add(inspect_json, "thread_lineage", thread_lineage_json);
  thread_lineage_json = NULL;
  turbo_json_object_add(inspect_json, "branch_tree", branch_tree_json);
  branch_tree_json = NULL;
  turbo_json_object_add(inspect_json, "child_runs", child_runs_json);
  child_runs_json = NULL;

  *out_inspect_json = inspect_json;
  inspect_json = NULL;
  rc = 0;

cleanup:
  turbo_free_json(&inspect_json);
  turbo_free_json(&child_runs_json);
  turbo_free_json(&branch_tree_json);
  turbo_free_json(&thread_lineage_json);
  turbo_free_json(&thread_timeline_json);
  turbo_runtime_data_bind_value_destroy(thread_timeline_bind);
  turbo_free_json(&supervisor_inspect_json);
  return rc;
}

static int turbo_agent_remote_session_clone_bind_field(
    const turbo_runtime_data_bind_value_t *object, const char *field_name,
    turbo_runtime_data_bind_value_t **out_value) {
  const turbo_runtime_data_bind_value_t *field_value;

  if (!object || !field_name || !field_name[0] || !out_value ||
      turbo_runtime_data_bind_value_kind(object) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    return -1;
  }
  field_value = turbo_runtime_data_bind_object_get(object, field_name);
  if (!field_value) {
    return -1;
  }
  *out_value = turbo_runtime_data_bind_value_clone(field_value);
  return *out_value ? 0 : -1;
}

static int turbo_agent_remote_session_call_get_run_json(
    turbo_agent_remote_session_t *session, const char *run_id, json_value_t **out_run_json) {
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !run_id || !run_id[0] || !out_run_json) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_get_run(session->client, run_id, out_run_json,
                                                 &error_json);
  turbo_free_json(&error_json);
  return rc;
}

static int turbo_agent_remote_session_call_get_checkpoint_json(
    turbo_agent_remote_session_t *session, const char *checkpoint_id,
    json_value_t **out_checkpoint_json) {
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !checkpoint_id || !checkpoint_id[0] ||
      !out_checkpoint_json) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_get_checkpoint(session->client, checkpoint_id,
                                                        out_checkpoint_json, &error_json);
  turbo_free_json(&error_json);
  return rc;
}

static int turbo_agent_remote_session_call_list_checkpoints_json(
    turbo_agent_remote_session_t *session, const char *run_id,
    json_value_t **out_checkpoints_json) {
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !run_id || !run_id[0] || !out_checkpoints_json) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_list_checkpoints(session->client, run_id,
                                                          out_checkpoints_json, &error_json);
  turbo_free_json(&error_json);
  return rc;
}

static int turbo_agent_remote_session_call_load_history_events_bind(
    turbo_agent_remote_session_t *session, const char *run_id, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_events) {
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client ||
      ((!run_id || !run_id[0]) && (!checkpoint_id || !checkpoint_id[0])) || !out_events) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_load_history_events_bind(session->client, run_id,
                                                                  checkpoint_id, out_events,
                                                                  &error_json);
  turbo_free_json(&error_json);
  return rc;
}

static int turbo_agent_remote_session_call_get_run_trace_events_bind(
    turbo_agent_remote_session_t *session, const char *run_id,
    turbo_runtime_data_bind_value_t **out_events) {
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !run_id || !run_id[0] || !out_events) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_get_run_trace_events_bind(session->client, run_id,
                                                                   out_events, &error_json);
  turbo_free_json(&error_json);
  return rc;
}

static int turbo_agent_remote_session_call_get_checkpoint_trace_events_bind(
    turbo_agent_remote_session_t *session, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_events) {
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !checkpoint_id || !checkpoint_id[0] || !out_events) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_get_checkpoint_trace_events_bind(session->client,
                                                                          checkpoint_id,
                                                                          out_events, &error_json);
  turbo_free_json(&error_json);
  return rc;
}

static int turbo_agent_remote_session_bind_object_set_string(
    turbo_runtime_data_bind_value_t *object, const char *key, const char *value) {
  turbo_runtime_data_bind_value_t *string_value;

  if (!object || !key || !key[0]) {
    return -1;
  }
  string_value = turbo_runtime_data_bind_value_create_string(value);
  if (!string_value ||
      turbo_runtime_data_bind_object_set(object, key, string_value) != TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(string_value);
    return -1;
  }
  return 0;
}

static int turbo_agent_remote_session_bind_object_set_clone(
    turbo_runtime_data_bind_value_t *object, const char *key,
    const turbo_runtime_data_bind_value_t *value) {
  turbo_runtime_data_bind_value_t *copy;

  if (!object || !key || !key[0] || !value) {
    return -1;
  }
  copy = turbo_runtime_data_bind_value_clone(value);
  if (!copy || turbo_runtime_data_bind_object_set(object, key, copy) != TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(copy);
    return -1;
  }
  return 0;
}

static const char *turbo_agent_remote_session_observer_type_for_trace_name(const char *name) {
  if (!name || name[0] == '\0') {
    return NULL;
  }
  if (strcmp(name, "model_request") == 0 || strcmp(name, "model_response") == 0) {
    return "model_delta";
  }
  if (strcmp(name, "tool_dispatch") == 0) {
    return "tool_call_started";
  }
  if (strcmp(name, "tool_result") == 0) {
    return "tool_result";
  }
  if (strcmp(name, "structured_retry") == 0 || strcmp(name, "replan_requested") == 0 ||
      strcmp(name, "review_required") == 0 || strcmp(name, "review_approved") == 0 ||
      strcmp(name, "guardrail_rejected") == 0 || strcmp(name, "memory_load") == 0 ||
      strcmp(name, "memory_save") == 0) {
    return "state_updated";
  }
  return NULL;
}

static int turbo_agent_remote_session_observer_event_from_bind(
    const turbo_runtime_data_bind_value_t *raw_event,
    turbo_runtime_data_bind_value_t **out_observer_event) {
  const turbo_runtime_data_bind_value_t *kind_value;
  const char *kind;
  const char *observer_type = NULL;
  turbo_runtime_data_bind_value_t *observer_event = NULL;

  if (!raw_event || !out_observer_event) {
    return -1;
  }
  *out_observer_event = NULL;

  kind_value = turbo_runtime_data_bind_object_get(raw_event, "kind");
  kind = turbo_runtime_data_bind_value_as_string(kind_value);
  if (kind && strcmp(kind, "model") == 0) {
    observer_type = "model_delta";
  } else if (kind && strcmp(kind, "tool_result") == 0) {
    observer_type = "tool_result";
  } else if (kind && strcmp(kind, "trace") == 0) {
    observer_type = turbo_agent_remote_session_observer_type_for_trace_name(
        turbo_runtime_data_bind_value_as_string(
            turbo_runtime_data_bind_object_get(raw_event, "name")));
  } else if (kind && strcmp(kind, "observer") == 0) {
    observer_type = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(raw_event, "type"));
  }

  if (!observer_type) {
    return 0;
  }

  observer_event = turbo_runtime_data_bind_value_create_object();
  if (!observer_event ||
      turbo_agent_remote_session_bind_object_set_string(observer_event, "kind", "observer") != 0 ||
      turbo_agent_remote_session_bind_object_set_string(observer_event, "type", observer_type) != 0 ||
      turbo_agent_remote_session_bind_object_set_clone(observer_event, "event", raw_event) != 0) {
    turbo_runtime_data_bind_value_destroy(observer_event);
    return -1;
  }
  *out_observer_event = observer_event;
  return 1;
}

static int turbo_agent_remote_session_events_bind_has_terminal_type(
    const turbo_runtime_data_bind_value_t *events_bind, const char *terminal_type) {
  size_t count;
  size_t i;

  if (!events_bind || !terminal_type || terminal_type[0] == '\0' ||
      turbo_runtime_data_bind_value_kind(events_bind) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    return 0;
  }
  count = turbo_runtime_data_bind_value_size(events_bind);
  for (i = 0; i < count; ++i) {
    const turbo_runtime_data_bind_value_t *raw_event =
        turbo_runtime_data_bind_array_get(events_bind, i);
    const char *type;

    if (!raw_event) {
      continue;
    }
    type = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(raw_event, "type"));
    if (type && strcmp(type, terminal_type) == 0) {
      return 1;
    }
  }
  return 0;
}

static int turbo_agent_remote_session_observe_events_bind(
    const turbo_runtime_data_bind_value_t *events_bind,
    const turbo_agent_observer_bind_sink_t *sink) {
  size_t count;
  size_t i;

  if (!events_bind || !sink || !sink->callback ||
      turbo_runtime_data_bind_value_kind(events_bind) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    return -1;
  }
  count = turbo_runtime_data_bind_value_size(events_bind);
  for (i = 0; i < count; ++i) {
    const turbo_runtime_data_bind_value_t *raw_event =
        turbo_runtime_data_bind_array_get(events_bind, i);
    turbo_runtime_data_bind_value_t *observer_event = NULL;
    int rc;

    if (!raw_event) {
      return -1;
    }
    rc = turbo_agent_remote_session_observer_event_from_bind(raw_event, &observer_event);
    if (rc < 0) {
      turbo_runtime_data_bind_value_destroy(observer_event);
      return -1;
    }
    if (rc > 0 && observer_event) {
      sink->callback(observer_event, sink->user_data);
    }
    turbo_runtime_data_bind_value_destroy(observer_event);
  }
  return 0;
}

static int turbo_agent_remote_session_observer_terminal_event_from_json(
    const json_value_t *record_json, turbo_runtime_data_bind_value_t **out_observer_event) {
  const char *status;
  turbo_runtime_data_bind_value_t *record_bind = NULL;
  turbo_runtime_data_bind_value_t *observer_event = NULL;

  if (!record_json || !out_observer_event) {
    return -1;
  }
  *out_observer_event = NULL;
  status = turbo_json_get_string(record_json, "status");
  if (!status || (strcmp(status, "interrupted") != 0 && strcmp(status, "completed") != 0)) {
    return 0;
  }
  record_bind = turbo_runtime_data_bind_value_from_json(record_json);
  if (!record_bind) {
    return -1;
  }
  observer_event = turbo_runtime_data_bind_value_create_object();
  if (!observer_event ||
      turbo_agent_remote_session_bind_object_set_string(observer_event, "kind", "observer") != 0 ||
      turbo_agent_remote_session_bind_object_set_string(observer_event, "type", status) != 0 ||
      turbo_agent_remote_session_bind_object_set_clone(observer_event, "event", record_bind) != 0) {
    turbo_runtime_data_bind_value_destroy(record_bind);
    turbo_runtime_data_bind_value_destroy(observer_event);
    return -1;
  }
  turbo_runtime_data_bind_value_destroy(record_bind);
  *out_observer_event = observer_event;
  return 1;
}

static int turbo_agent_remote_session_observe_terminal_record_json(
    const turbo_runtime_data_bind_value_t *events_bind, const json_value_t *record_json,
    const turbo_agent_observer_bind_sink_t *sink) {
  turbo_runtime_data_bind_value_t *observer_event = NULL;
  const char *type;
  int rc;

  if (!record_json || !sink || !sink->callback) {
    return -1;
  }
  rc = turbo_agent_remote_session_observer_terminal_event_from_json(record_json, &observer_event);
  if (rc <= 0) {
    turbo_runtime_data_bind_value_destroy(observer_event);
    return rc;
  }
  type = turbo_runtime_data_bind_value_as_string(
      turbo_runtime_data_bind_object_get(observer_event, "type"));
  if (!events_bind || !type ||
      !turbo_agent_remote_session_events_bind_has_terminal_type(events_bind, type)) {
    sink->callback(observer_event, sink->user_data);
  }
  turbo_runtime_data_bind_value_destroy(observer_event);
  return 1;
}

static int turbo_agent_remote_session_extract_result_json(
    turbo_runtime_data_bind_value_t *state, json_value_t **out_json) {
  json_value_t *json_state = NULL;
  json_value_t *result_json = NULL;
  int rc;

  if (!state || !out_json) {
    return -1;
  }
  *out_json = NULL;
  json_state = turbo_runtime_data_bind_value_to_json(state);
  if (!json_state) {
    return -1;
  }
  rc = turbo_agent_state_parse_final_output_json(json_state, &result_json);
  turbo_free_json(&json_state);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    return -1;
  }
  *out_json = result_json;
  return 0;
}

CXX_C_API turbo_agent_remote_session_t *
turbo_agent_remote_session_create(const turbo_agent_remote_session_config_t *config) {
  turbo_agent_remote_session_t *session;

  if (!config || (!config->client && (!config->client_config.rpc_client &&
                                      (!config->client_config.url ||
                                       !config->client_config.url[0])))) {
    return NULL;
  }

  session = (turbo_agent_remote_session_t *)calloc(1, sizeof(*session));
  if (!session) {
    return NULL;
  }

  if (config->client) {
    session->client = config->client;
    session->owns_client = 0;
  } else {
    session->client = turbo_agent_runtime_remote_client_create(&config->client_config);
    if (!session->client) {
      free(session);
      return NULL;
    }
    session->owns_client = 1;
  }

  session->thread_id = turbo_agent_remote_session_strdup_or_null(config->thread_id);
  return session;
}

CXX_C_API void turbo_agent_remote_session_destroy(turbo_agent_remote_session_t *session) {
  if (!session) {
    return;
  }
  if (session->owns_client && session->client) {
    turbo_agent_runtime_remote_client_destroy(session->client);
  }
  tstr_free(session->thread_id);
  tstr_free(session->last_run_id);
  tstr_free(session->last_checkpoint_id);
  free(session);
}

CXX_C_API turbo_agent_runtime_remote_client_t *
turbo_agent_remote_session_client(const turbo_agent_remote_session_t *session) {
  return session ? session->client : NULL;
}

CXX_C_API const char *turbo_agent_remote_session_thread_id(
    const turbo_agent_remote_session_t *session) {
  return session ? session->thread_id : NULL;
}

CXX_C_API const char *turbo_agent_remote_session_last_run_id(
    const turbo_agent_remote_session_t *session) {
  return session ? session->last_run_id : NULL;
}

CXX_C_API const char *turbo_agent_remote_session_last_checkpoint_id(
    const turbo_agent_remote_session_t *session) {
  return session ? session->last_checkpoint_id : NULL;
}

CXX_C_API int turbo_agent_remote_session_get_startup_diagnostics(
    turbo_agent_remote_session_t *session, const char *graph_name,
    json_value_t **out_diagnostics_json) {
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !graph_name || !graph_name[0] ||
      !out_diagnostics_json) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_get_startup_diagnostics(
      session->client, graph_name, out_diagnostics_json, &error_json);
  turbo_free_json(&error_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_start_bind_graph(
    turbo_agent_remote_session_t *session, const char *graph_name,
    const turbo_runtime_data_bind_value_t *state, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  if (!session || !session->client || !graph_name || !graph_name[0] || !out_summary_json ||
      !out_state) {
    return -1;
  }
  return turbo_agent_remote_session_call_start(session, graph_name, state, options,
                                               out_summary_json, out_state);
}

CXX_C_API int turbo_agent_remote_session_start_text(
    turbo_agent_remote_session_t *session, const char *graph_name, const char *user_text,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  turbo_runtime_data_bind_value_t *state;
  int rc;

  if (!session || !session->client || !graph_name || !graph_name[0] || !out_summary_json ||
      !out_state) {
    return -1;
  }
  state = turbo_agent_session_create_input_state_bind(user_text);
  if (!state) {
    return -1;
  }
  rc = turbo_agent_remote_session_start_bind_graph(session, graph_name, state, options,
                                                   out_summary_json, out_state);
  turbo_runtime_data_bind_value_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_start_messages(
    turbo_agent_remote_session_t *session, const char *graph_name,
    const turbo_runtime_data_bind_value_t *messages, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  turbo_runtime_data_bind_value_t *state;
  int rc;

  if (!session || !session->client || !graph_name || !graph_name[0] || !out_summary_json ||
      !out_state) {
    return -1;
  }
  state = turbo_agent_session_create_input_messages_state_bind(messages);
  if (!state) {
    return -1;
  }
  rc = turbo_agent_remote_session_start_bind_graph(session, graph_name, state, options,
                                                   out_summary_json, out_state);
  turbo_runtime_data_bind_value_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_resume_bind_graph(
    turbo_agent_remote_session_t *session, const char *graph_name, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  if (!session || !session->client || !graph_name || !graph_name[0] || !out_summary_json ||
      !out_state) {
    return -1;
  }
  return turbo_agent_remote_session_call_resume(session, graph_name, checkpoint_id, state_override,
                                                options, out_summary_json, out_state, 0);
}

CXX_C_API int turbo_agent_remote_session_fork_bind_graph(
    turbo_agent_remote_session_t *session, const char *graph_name, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  if (!session || !session->client || !graph_name || !graph_name[0] || !out_summary_json ||
      !out_state) {
    return -1;
  }
  return turbo_agent_remote_session_call_resume(session, graph_name, checkpoint_id, state_override,
                                                options, out_summary_json, out_state, 1);
}

CXX_C_API int turbo_agent_remote_session_invoke_text(
    turbo_agent_remote_session_t *session, const char *graph_name, const char *user_text,
    const turbo_graph_run_options_t *options, char **out_text, json_value_t **out_summary_json) {
  turbo_runtime_data_bind_value_t *out_state = NULL;
  char *text;
  int rc;

  if (!out_text || !out_summary_json) {
    return -1;
  }
  *out_text = NULL;
  *out_summary_json = NULL;
  rc = turbo_agent_remote_session_start_text(session, graph_name, user_text, options,
                                             out_summary_json, &out_state);
  if (rc != 0) {
    turbo_runtime_data_bind_value_destroy(out_state);
    turbo_free_json(out_summary_json);
    return rc;
  }
  text = turbo_agent_session_result_text(out_state);
  turbo_runtime_data_bind_value_destroy(out_state);
  if (!text) {
    turbo_free_json(out_summary_json);
    return -1;
  }
  *out_text = text;
  return 0;
}

CXX_C_API int turbo_agent_remote_session_invoke_messages_text(
    turbo_agent_remote_session_t *session, const char *graph_name,
    const turbo_runtime_data_bind_value_t *messages, const turbo_graph_run_options_t *options,
    char **out_text, json_value_t **out_summary_json) {
  turbo_runtime_data_bind_value_t *out_state = NULL;
  char *text;
  int rc;

  if (!out_text || !out_summary_json) {
    return -1;
  }
  *out_text = NULL;
  *out_summary_json = NULL;
  rc = turbo_agent_remote_session_start_messages(session, graph_name, messages, options,
                                                 out_summary_json, &out_state);
  if (rc != 0) {
    turbo_runtime_data_bind_value_destroy(out_state);
    turbo_free_json(out_summary_json);
    return rc;
  }
  text = turbo_agent_session_result_text(out_state);
  turbo_runtime_data_bind_value_destroy(out_state);
  if (!text) {
    turbo_free_json(out_summary_json);
    return -1;
  }
  *out_text = text;
  return 0;
}

CXX_C_API int turbo_agent_remote_session_invoke_json(
    turbo_agent_remote_session_t *session, const char *graph_name, const char *user_text,
    const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json) {
  turbo_runtime_data_bind_value_t *out_state = NULL;
  int rc;

  if (!out_json || !out_summary_json) {
    return -1;
  }
  *out_json = NULL;
  *out_summary_json = NULL;
  rc = turbo_agent_remote_session_start_text(session, graph_name, user_text, options,
                                             out_summary_json, &out_state);
  if (rc != 0) {
    turbo_runtime_data_bind_value_destroy(out_state);
    turbo_free_json(out_summary_json);
    return rc;
  }
  rc = turbo_agent_remote_session_extract_result_json(out_state, out_json);
  turbo_runtime_data_bind_value_destroy(out_state);
  if (rc != 0 || !*out_json) {
    turbo_free_json(out_summary_json);
    turbo_free_json(out_json);
    return -1;
  }
  return 0;
}

CXX_C_API int turbo_agent_remote_session_invoke_messages_json(
    turbo_agent_remote_session_t *session, const char *graph_name,
    const turbo_runtime_data_bind_value_t *messages, const turbo_graph_run_options_t *options,
    json_value_t **out_json, json_value_t **out_summary_json) {
  turbo_runtime_data_bind_value_t *out_state = NULL;
  int rc;

  if (!out_json || !out_summary_json) {
    return -1;
  }
  *out_json = NULL;
  *out_summary_json = NULL;
  rc = turbo_agent_remote_session_start_messages(session, graph_name, messages, options,
                                                 out_summary_json, &out_state);
  if (rc != 0) {
    turbo_runtime_data_bind_value_destroy(out_state);
    turbo_free_json(out_summary_json);
    return rc;
  }
  rc = turbo_agent_remote_session_extract_result_json(out_state, out_json);
  turbo_runtime_data_bind_value_destroy(out_state);
  if (rc != 0 || !*out_json) {
    turbo_free_json(out_summary_json);
    turbo_free_json(out_json);
    return -1;
  }
  return 0;
}

CXX_C_API int turbo_agent_remote_session_get_thread_state_bind(
    turbo_agent_remote_session_t *session, turbo_runtime_data_bind_value_t **out_state) {
  const char *thread_id;
  json_value_t *error_json = NULL;
  int rc;

  thread_id = turbo_agent_remote_session_resolve_thread_id(session);
  if (!session || !session->client || !thread_id || !thread_id[0] || !out_state) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_get_thread_state_bind(session->client, thread_id,
                                                               out_state, &error_json);
  turbo_free_json(&error_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_memory_delete_record(
    const turbo_agent_remote_session_t *session, const char *memory_namespace, const char *key) {
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !memory_namespace || !memory_namespace[0] || !key ||
      !key[0]) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_delete_memory_record(session->client, memory_namespace,
                                                              key, &error_json);
  turbo_free_json(&error_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_memory_list_records(
    const turbo_agent_remote_session_t *session, const char *namespace_prefix,
    json_value_t **out_records_json) {
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !out_records_json) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_list_memory_records(session->client, namespace_prefix,
                                                             out_records_json, &error_json);
  turbo_free_json(&error_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_memory_get_record(
    const turbo_agent_remote_session_t *session, const char *memory_namespace, const char *key,
    json_value_t **out_record_json) {
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !memory_namespace || !memory_namespace[0] || !key ||
      !key[0] || !out_record_json) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_get_memory_record(session->client, memory_namespace, key,
                                                           out_record_json, &error_json);
  turbo_free_json(&error_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_memory_put_record(
    const turbo_agent_remote_session_t *session, const json_value_t *record_json) {
  json_value_t *stored_record_json = NULL;
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !record_json) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_put_memory_record(session->client, record_json,
                                                           &stored_record_json, &error_json);
  turbo_free_json(&stored_record_json);
  turbo_free_json(&error_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_memory_validate_record(
    const turbo_agent_remote_session_t *session, const json_value_t *record_json, int *out_valid) {
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !record_json || !out_valid) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_validate_memory_record(session->client, record_json,
                                                                out_valid, &error_json);
  turbo_free_json(&error_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_memory_query_records(
    const turbo_agent_remote_session_t *session, const char *namespace_prefix, const char *kind,
    const char *key_prefix, const char *text_substring, json_value_t **out_records_json) {
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !out_records_json) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_query_memory_records(
      session->client, namespace_prefix, kind, key_prefix, text_substring, out_records_json,
      &error_json);
  turbo_free_json(&error_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_memory_query_records_ex(
    const turbo_agent_remote_session_t *session,
    const turbo_agent_memory_query_options_t *options, json_value_t **out_records_json) {
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !out_records_json) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_query_memory_records_ex(session->client, options,
                                                                  out_records_json,
                                                                  &error_json);
  turbo_free_json(&error_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_get_thread(turbo_agent_remote_session_t *session,
                                                    json_value_t **out_thread_json) {
  const char *thread_id;

  thread_id = turbo_agent_remote_session_resolve_thread_id(session);
  if (!session || !session->client || !thread_id || !thread_id[0] || !out_thread_json) {
    return -1;
  }
  return turbo_agent_remote_session_get_observability_object(session, "thread",
                                                             out_thread_json);
}

CXX_C_API int turbo_agent_remote_session_get_latest_run(turbo_agent_remote_session_t *session,
                                                        json_value_t **out_run_json) {
  const char *thread_id;

  thread_id = turbo_agent_remote_session_resolve_thread_id(session);
  if (!session || !session->client || !thread_id || !thread_id[0] || !out_run_json) {
    return -1;
  }
  return turbo_agent_remote_session_get_observability_object(session, "latest_run",
                                                             out_run_json);
}

CXX_C_API int turbo_agent_remote_session_get_pending_run(turbo_agent_remote_session_t *session,
                                                         json_value_t **out_run_json) {
  const char *thread_id;

  thread_id = turbo_agent_remote_session_resolve_thread_id(session);
  if (!session || !session->client || !thread_id || !thread_id[0] || !out_run_json) {
    return -1;
  }
  return turbo_agent_remote_session_get_observability_object(session, "pending_run",
                                                             out_run_json);
}

CXX_C_API int turbo_agent_remote_session_get_checkpoint_context(
    turbo_agent_remote_session_t *session, const char *checkpoint_id,
    json_value_t **out_context_json) {
  const char *resolved_checkpoint_id;
  json_value_t *error_json = NULL;
  int rc;

  resolved_checkpoint_id =
      turbo_agent_remote_session_resolve_checkpoint_id(session, checkpoint_id);
  if (!session || !session->client || !resolved_checkpoint_id || !out_context_json) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_get_checkpoint_context(session->client,
                                                                resolved_checkpoint_id,
                                                                out_context_json, &error_json);
  turbo_free_json(&error_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_get_observability_index(
    turbo_agent_remote_session_t *session, json_value_t **out_index_json) {
  const char *thread_id;
  json_value_t *error_json = NULL;
  int rc;

  thread_id = turbo_agent_remote_session_resolve_thread_id(session);
  if (!session || !session->client || !thread_id || !thread_id[0] || !out_index_json) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_get_thread_observability_index(session->client, thread_id,
                                                                        out_index_json,
                                                                        &error_json);
  turbo_free_json(&error_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_get_thread_timeline_bind(
    turbo_agent_remote_session_t *session, turbo_runtime_data_bind_value_t **out_timeline) {
  const char *thread_id;
  json_value_t *error_json = NULL;
  int rc;

  thread_id = turbo_agent_remote_session_resolve_thread_id(session);
  if (!session || !session->client || !thread_id || !thread_id[0] || !out_timeline) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_get_thread_timeline_bind(session->client, thread_id,
                                                                  out_timeline, &error_json);
  turbo_free_json(&error_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_load_thread_history_events_bind(
    turbo_agent_remote_session_t *session, turbo_runtime_data_bind_value_t **out_events) {
  turbo_runtime_data_bind_value_t *timeline = NULL;
  int rc;

  if (!session || !session->client || !out_events) {
    return -1;
  }
  rc = turbo_agent_remote_session_get_thread_timeline_bind(session, &timeline);
  if (rc != 0 || !timeline) {
    turbo_runtime_data_bind_value_destroy(timeline);
    return -1;
  }
  rc = turbo_agent_remote_session_clone_bind_field(timeline, "history_events", out_events);
  turbo_runtime_data_bind_value_destroy(timeline);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_replay_thread_history_bind(
    turbo_agent_remote_session_t *session, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data) {
  turbo_runtime_data_bind_value_t *events = NULL;
  const turbo_runtime_data_bind_value_t *event = NULL;
  size_t index;

  if (!session || !session->client || !event_sink) {
    return -1;
  }
  if (turbo_agent_remote_session_load_thread_history_events_bind(session, &events) != 0 ||
      !events ||
      turbo_runtime_data_bind_value_kind(events) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    turbo_runtime_data_bind_value_destroy(events);
    return -1;
  }
  for (index = 0; (event = turbo_runtime_data_bind_array_get(events, index)) != NULL; ++index) {
    event_sink(event, event_sink_user_data);
  }
  turbo_runtime_data_bind_value_destroy(events);
  return 0;
}

CXX_C_API int turbo_agent_remote_session_observe_thread_history_bind(
    turbo_agent_remote_session_t *session, const turbo_agent_observer_bind_sink_t *sink) {
  json_value_t *index_json = NULL;
  const json_value_t *events_json;
  const json_value_t *record_json = NULL;
  turbo_runtime_data_bind_value_t *events_bind = NULL;
  int rc;

  if (!session || !session->client || !sink || !sink->callback) {
    return -1;
  }
  rc = turbo_agent_remote_session_get_observability_index(session, &index_json);
  if (rc != 0 || !index_json) {
    turbo_free_json(&index_json);
    return -1;
  }
  events_json = turbo_json_object_get(index_json, "history_events");
  if (!events_json || turbo_json_type(events_json) != TURBO_JSON_ARRAY) {
    turbo_free_json(&index_json);
    return -1;
  }
  events_bind = turbo_runtime_data_bind_value_from_json(events_json);
  if (!events_bind) {
    turbo_free_json(&index_json);
    return -1;
  }
  rc = turbo_agent_remote_session_observe_events_bind(events_bind, sink);
  if (rc == 0) {
    record_json = turbo_json_object_get(index_json, "pending_run");
    if (!record_json || turbo_json_type(record_json) != TURBO_JSON_OBJECT) {
      record_json = turbo_json_object_get(index_json, "latest_run");
    }
    if (record_json && turbo_json_type(record_json) == TURBO_JSON_OBJECT) {
      rc = turbo_agent_remote_session_observe_terminal_record_json(events_bind, record_json, sink);
      if (rc > 0) {
        rc = 0;
      }
    }
  }
  turbo_runtime_data_bind_value_destroy(events_bind);
  turbo_free_json(&index_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_get_thread_trace_events_bind(
    turbo_agent_remote_session_t *session, turbo_runtime_data_bind_value_t **out_events) {
  json_value_t *index_json = NULL;
  const json_value_t *trace_events_json;
  int rc;

  if (!session || !session->client || !out_events) {
    return -1;
  }
  rc = turbo_agent_remote_session_get_observability_index(session, &index_json);
  if (rc != 0 || !index_json) {
    turbo_free_json(&index_json);
    return -1;
  }
  trace_events_json = turbo_json_object_get(index_json, "trace_events");
  if (!trace_events_json || turbo_json_type(trace_events_json) != TURBO_JSON_ARRAY) {
    turbo_free_json(&index_json);
    return -1;
  }
  *out_events = turbo_runtime_data_bind_value_from_json(trace_events_json);
  turbo_free_json(&index_json);
  return *out_events ? 0 : -1;
}

CXX_C_API int turbo_agent_remote_session_get_branch_tree(
    turbo_agent_remote_session_t *session, json_value_t **out_branch_tree_json) {
  const char *thread_id;
  json_value_t *error_json = NULL;
  int rc;

  thread_id = turbo_agent_remote_session_resolve_thread_id(session);
  if (!session || !session->client || !thread_id || !thread_id[0] || !out_branch_tree_json) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_get_branch_tree(session->client, thread_id,
                                                         out_branch_tree_json, &error_json);
  turbo_free_json(&error_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_list_thread_lineage(
    turbo_agent_remote_session_t *session, json_value_t **out_lineage_json) {
  return turbo_agent_remote_session_get_observability_object(session, "thread_lineage",
                                                             out_lineage_json);
}

CXX_C_API int turbo_agent_remote_session_get_supervisor_inbox(
    turbo_agent_remote_session_t *session, json_value_t **out_inbox_json) {
  return turbo_agent_remote_session_get_supervisor_array_json_local(
      session, turbo_agent_state_supervisor_inbox, out_inbox_json);
}

CXX_C_API int turbo_agent_remote_session_get_supervisor_handoff_history(
    turbo_agent_remote_session_t *session, json_value_t **out_history_json) {
  return turbo_agent_remote_session_get_supervisor_array_json_local(
      session, turbo_agent_state_supervisor_handoff_history, out_history_json);
}

CXX_C_API int turbo_agent_remote_session_get_supervisor_inspect(
    turbo_agent_remote_session_t *session, json_value_t **out_inspect_json) {
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !session->thread_id || !session->thread_id[0] ||
      !out_inspect_json) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_get_supervisor_inspect(session->client, session->thread_id,
                                                                out_inspect_json);
  turbo_free_json(&error_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_list_child_runs(
    turbo_agent_remote_session_t *session, const char *parent_agent_run_id,
    json_value_t **out_runs_json) {
  const char *resolved_parent_agent_run_id;
  json_value_t *error_json = NULL;
  int rc;

  resolved_parent_agent_run_id =
      turbo_agent_remote_session_resolve_parent_agent_run_id(session, parent_agent_run_id);
  if (!session || !session->client || !resolved_parent_agent_run_id || !out_runs_json) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_list_child_runs(session->client,
                                                         resolved_parent_agent_run_id, out_runs_json,
                                                         &error_json);
  turbo_free_json(&error_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_get_orchestration_inspect(
    turbo_agent_remote_session_t *session, json_value_t **out_inspect_json) {
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !session->thread_id || !session->thread_id[0] ||
      !out_inspect_json) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_get_orchestration_inspect(
      session->client, session->thread_id, out_inspect_json);
  turbo_free_json(&error_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_get_child_run(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_run_json) {
  const char *child_run_id = turbo_agent_state_tool_result_child_run_id(output_item);

  if (!session || !session->client || !child_run_id || child_run_id[0] == '\0' || !out_run_json) {
    return -1;
  }
  return turbo_agent_remote_session_call_get_run_json(session, child_run_id, out_run_json);
}

CXX_C_API int turbo_agent_remote_session_get_child_checkpoint(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_checkpoint_json) {
  const char *child_checkpoint_id =
      turbo_agent_state_tool_result_child_checkpoint_id(output_item);

  if (!session || !session->client || !child_checkpoint_id || child_checkpoint_id[0] == '\0' ||
      !out_checkpoint_json) {
    return -1;
  }
  return turbo_agent_remote_session_call_get_checkpoint_json(session, child_checkpoint_id,
                                                             out_checkpoint_json);
}

CXX_C_API int turbo_agent_remote_session_get_child_checkpoint_context(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_context_json) {
  const char *child_checkpoint_id =
      turbo_agent_state_tool_result_child_checkpoint_id(output_item);

  if (!session || !session->client || !child_checkpoint_id || child_checkpoint_id[0] == '\0' ||
      !out_context_json) {
    return -1;
  }
  return turbo_agent_runtime_remote_client_get_checkpoint_context(session->client,
                                                                  child_checkpoint_id,
                                                                  out_context_json, NULL);
}

CXX_C_API int turbo_agent_remote_session_get_child_thread_timeline_bind(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    turbo_runtime_data_bind_value_t **out_timeline) {
  const char *child_thread_id = turbo_agent_state_tool_result_child_thread_id(output_item);

  if (!session || !session->client || !child_thread_id || child_thread_id[0] == '\0' ||
      !out_timeline) {
    return -1;
  }
  return turbo_agent_runtime_remote_client_get_thread_timeline_bind(session->client, child_thread_id,
                                                                    out_timeline, NULL);
}

CXX_C_API int turbo_agent_remote_session_get_child_branch_tree(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_branch_tree_json) {
  const char *child_thread_id = turbo_agent_state_tool_result_child_thread_id(output_item);

  if (!session || !session->client || !child_thread_id || child_thread_id[0] == '\0' ||
      !out_branch_tree_json) {
    return -1;
  }
  return turbo_agent_runtime_remote_client_get_branch_tree(session->client, child_thread_id,
                                                           out_branch_tree_json, NULL);
}

CXX_C_API int turbo_agent_remote_session_list_child_checkpoints(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_checkpoints_json) {
  const char *child_run_id = turbo_agent_state_tool_result_child_run_id(output_item);

  if (!session || !session->client || !child_run_id || child_run_id[0] == '\0' ||
      !out_checkpoints_json) {
    return -1;
  }
  return turbo_agent_remote_session_call_list_checkpoints_json(session, child_run_id,
                                                               out_checkpoints_json);
}

CXX_C_API int turbo_agent_remote_session_load_child_history_events_bind(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    turbo_runtime_data_bind_value_t **out_events) {
  const char *child_checkpoint_id;
  const char *child_run_id;

  if (!session || !session->client || !output_item || !out_events) {
    return -1;
  }

  child_checkpoint_id = turbo_agent_state_tool_result_child_checkpoint_id(output_item);
  if (child_checkpoint_id && child_checkpoint_id[0] != '\0') {
    return turbo_agent_remote_session_call_load_history_events_bind(session, NULL,
                                                                    child_checkpoint_id,
                                                                    out_events);
  }

  child_run_id = turbo_agent_state_tool_result_child_run_id(output_item);
  if (!child_run_id || child_run_id[0] == '\0') {
    return -1;
  }
  return turbo_agent_remote_session_call_load_history_events_bind(session, child_run_id, NULL,
                                                                  out_events);
}

CXX_C_API int turbo_agent_remote_session_get_child_trace_events_bind(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    turbo_runtime_data_bind_value_t **out_events) {
  const char *child_checkpoint_id;
  const char *child_run_id;

  if (!session || !session->client || !output_item || !out_events) {
    return -1;
  }

  child_checkpoint_id = turbo_agent_state_tool_result_child_checkpoint_id(output_item);
  if (child_checkpoint_id && child_checkpoint_id[0] != '\0') {
    return turbo_agent_remote_session_call_get_checkpoint_trace_events_bind(session,
                                                                            child_checkpoint_id,
                                                                            out_events);
  }

  child_run_id = turbo_agent_state_tool_result_child_run_id(output_item);
  if (!child_run_id || child_run_id[0] == '\0') {
    return -1;
  }
  return turbo_agent_remote_session_call_get_run_trace_events_bind(session, child_run_id,
                                                                   out_events);
}

CXX_C_API int turbo_agent_remote_session_get_child_inspect(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_inspect_json) {
  json_value_t *inspect_json = NULL;
  json_value_t *run_json = NULL;
  json_value_t *checkpoints_json = NULL;
  json_value_t *latest_checkpoint_json = NULL;
  json_value_t *checkpoint_context_json = NULL;
  json_value_t *history_events_json = NULL;
  json_value_t *trace_events_json = NULL;
  json_value_t *thread_timeline_json = NULL;
  json_value_t *branch_tree_json = NULL;
  turbo_runtime_data_bind_value_t *history_events_bind = NULL;
  turbo_runtime_data_bind_value_t *trace_events_bind = NULL;
  turbo_runtime_data_bind_value_t *thread_timeline_bind = NULL;
  const char *child_checkpoint_id;

  if (!session || !session->client || !output_item || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;

  child_checkpoint_id = turbo_agent_state_tool_result_child_checkpoint_id(output_item);
  if (turbo_agent_remote_session_get_child_run(session, output_item, &run_json) != 0 || !run_json) {
    goto cleanup;
  }
  if (turbo_agent_remote_session_list_child_checkpoints(session, output_item, &checkpoints_json) !=
          0 ||
      !checkpoints_json) {
    goto cleanup;
  }
  if (child_checkpoint_id && child_checkpoint_id[0] != '\0') {
    if (turbo_agent_remote_session_get_child_checkpoint(session, output_item,
                                                        &latest_checkpoint_json) != 0 ||
        !latest_checkpoint_json) {
      goto cleanup;
    }
    if (turbo_agent_remote_session_get_child_checkpoint_context(session, output_item,
                                                                &checkpoint_context_json) != 0 ||
        !checkpoint_context_json) {
      goto cleanup;
    }
  }
  if (turbo_agent_remote_session_load_child_history_events_bind(session, output_item,
                                                                &history_events_bind) != 0 ||
      !history_events_bind) {
    goto cleanup;
  }
  history_events_json = turbo_runtime_data_bind_value_to_json(history_events_bind);
  if (!history_events_json) {
    goto cleanup;
  }
  if (turbo_agent_remote_session_get_child_trace_events_bind(session, output_item,
                                                             &trace_events_bind) != 0 ||
      !trace_events_bind) {
    goto cleanup;
  }
  trace_events_json = turbo_runtime_data_bind_value_to_json(trace_events_bind);
  if (!trace_events_json) {
    goto cleanup;
  }
  if (turbo_agent_remote_session_get_child_thread_timeline_bind(session, output_item,
                                                                &thread_timeline_bind) == 0 &&
      thread_timeline_bind) {
    thread_timeline_json = turbo_runtime_data_bind_value_to_json(thread_timeline_bind);
    if (!thread_timeline_json) {
      goto cleanup;
    }
  }
  if (turbo_agent_remote_session_get_child_branch_tree(session, output_item, &branch_tree_json) !=
      0) {
    turbo_free_json(&branch_tree_json);
  }

  inspect_json = turbo_json_create_object();
  if (!inspect_json) {
    goto cleanup;
  }
  turbo_json_object_add(inspect_json, "run", run_json);
  run_json = NULL;
  turbo_json_object_add(inspect_json, "checkpoints", checkpoints_json);
  checkpoints_json = NULL;
  if (latest_checkpoint_json) {
    turbo_json_object_add(inspect_json, "latest_checkpoint", latest_checkpoint_json);
    latest_checkpoint_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "latest_checkpoint");
  }
  if (checkpoint_context_json) {
    turbo_json_object_add(inspect_json, "checkpoint_context", checkpoint_context_json);
    checkpoint_context_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "checkpoint_context");
  }
  turbo_json_object_add(inspect_json, "history_events", history_events_json);
  history_events_json = NULL;
  turbo_json_object_add(inspect_json, "trace_events", trace_events_json);
  trace_events_json = NULL;
  if (thread_timeline_json) {
    turbo_json_object_add(inspect_json, "thread_timeline", thread_timeline_json);
    thread_timeline_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "thread_timeline");
  }
  if (branch_tree_json) {
    turbo_json_object_add(inspect_json, "branch_tree", branch_tree_json);
    branch_tree_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "branch_tree");
  }

  *out_inspect_json = inspect_json;
  inspect_json = NULL;

cleanup:
  turbo_free_json(&inspect_json);
  turbo_free_json(&branch_tree_json);
  turbo_free_json(&thread_timeline_json);
  turbo_runtime_data_bind_value_destroy(thread_timeline_bind);
  turbo_free_json(&trace_events_json);
  turbo_runtime_data_bind_value_destroy(trace_events_bind);
  turbo_free_json(&history_events_json);
  turbo_runtime_data_bind_value_destroy(history_events_bind);
  turbo_free_json(&checkpoint_context_json);
  turbo_free_json(&latest_checkpoint_json);
  turbo_free_json(&checkpoints_json);
  turbo_free_json(&run_json);
  return *out_inspect_json ? 0 : -1;
}

CXX_C_API int turbo_agent_remote_session_get_child_orchestration_inspect(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_inspect_json) {
  json_value_t *inspect_json = NULL;
  json_value_t *child_inspect_json = NULL;
  const char *parent_agent_run_id;
  const char *parent_tool_call_id;
  const char *parent_tool_name;
  const char *parent_graph_run_id;
  const char *call_frame_id;

  if (!session || !output_item || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;

  if (turbo_agent_remote_session_get_child_inspect(session, output_item, &child_inspect_json) != 0 ||
      !child_inspect_json) {
    goto cleanup;
  }

  inspect_json = turbo_json_create_object();
  if (!inspect_json) {
    goto cleanup;
  }

  parent_agent_run_id = turbo_agent_state_tool_result_parent_agent_run_id(output_item);
  parent_tool_call_id = turbo_agent_state_tool_result_parent_tool_call_id(output_item);
  parent_tool_name = turbo_agent_state_tool_result_parent_tool_name(output_item);
  parent_graph_run_id = turbo_agent_state_tool_result_parent_graph_run_id(output_item);
  call_frame_id = turbo_agent_state_tool_result_call_frame_id(output_item);

  if (parent_agent_run_id && parent_agent_run_id[0] != '\0') {
    turbo_json_object_set_string(inspect_json, "parent_agent_run_id", parent_agent_run_id);
  } else {
    turbo_json_object_set_null(inspect_json, "parent_agent_run_id");
  }
  if (parent_tool_call_id && parent_tool_call_id[0] != '\0') {
    turbo_json_object_set_string(inspect_json, "parent_tool_call_id", parent_tool_call_id);
  } else {
    turbo_json_object_set_null(inspect_json, "parent_tool_call_id");
  }
  if (parent_tool_name && parent_tool_name[0] != '\0') {
    turbo_json_object_set_string(inspect_json, "parent_tool_name", parent_tool_name);
  } else {
    turbo_json_object_set_null(inspect_json, "parent_tool_name");
  }
  if (parent_graph_run_id && parent_graph_run_id[0] != '\0') {
    turbo_json_object_set_string(inspect_json, "parent_graph_run_id", parent_graph_run_id);
  } else {
    turbo_json_object_set_null(inspect_json, "parent_graph_run_id");
  }
  if (call_frame_id && call_frame_id[0] != '\0') {
    turbo_json_object_set_string(inspect_json, "call_frame_id", call_frame_id);
  } else {
    turbo_json_object_set_null(inspect_json, "call_frame_id");
  }
  turbo_json_object_add(inspect_json, "child_inspect", child_inspect_json);
  child_inspect_json = NULL;

  *out_inspect_json = inspect_json;
  inspect_json = NULL;
  return 0;

cleanup:
  turbo_free_json(&inspect_json);
  turbo_free_json(&child_inspect_json);
  return -1;
}

CXX_C_API int turbo_agent_remote_session_get_child_multi_agent_inspect(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_inspect_json) {
  if (!session || !session->client || !session->thread_id || !session->thread_id[0] ||
      !output_item || !out_inspect_json) {
    return -1;
  }
  return turbo_agent_runtime_remote_client_get_child_multi_agent_inspect(
      session->client, session->thread_id, output_item, out_inspect_json);
}

CXX_C_API int turbo_agent_remote_session_resume_thread_command_bind(
    turbo_agent_remote_session_t *session, const char *graph_name,
    const turbo_runtime_data_bind_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  if (!session || !session->client || !graph_name || !graph_name[0] || !command ||
      !out_summary_json || !out_state) {
    return -1;
  }
  return turbo_agent_remote_session_call_thread_command(session, graph_name, command, options,
                                                        out_summary_json, out_state, 0);
}

CXX_C_API int turbo_agent_remote_session_fork_thread_command_bind(
    turbo_agent_remote_session_t *session, const char *graph_name,
    const turbo_runtime_data_bind_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  if (!session || !session->client || !graph_name || !graph_name[0] || !command ||
      !out_summary_json || !out_state) {
    return -1;
  }
  return turbo_agent_remote_session_call_thread_command(session, graph_name, command, options,
                                                        out_summary_json, out_state, 1);
}
