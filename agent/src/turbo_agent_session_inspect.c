#include "turbo_agent_session.h"
#include "turbo_agent_supervisor_inspect_internal.h"
#include "turbo_agent_state.h"
#include "turbo_runtime_json.h"

/* Reports own inserted children; failed insertion leaves ownership here. */
static int session_inspect_take(json_value_t *object, const char *key, json_value_t **value) {
  if (turbo_runtime_json_object_set(object, key, *value) != TURBO_RUNTIME_JSON_OK) return -1;
  *value = NULL;
  return 0;
}

static int turbo_agent_session_get_supervisor_array_json_local(
    turbo_agent_session_t *session, const json_value_t *(*selector)(const json_value_t *state),
    json_value_t **out_array_json) {
  json_value_t *state_json = NULL;
  json_value_t *array_json = NULL;
  const json_value_t *selected = NULL;
  int rc = -1;

  if (!session || !selector || !out_array_json) {
    return -1;
  }
  *out_array_json = NULL;

  if (turbo_agent_session_get_thread_state_json_value(session, &state_json) != 0 || !state_json) {
    goto cleanup;
  }

  selected = selector(state_json);
  if (selected && json_type(selected) == JSON_ARRAY) {
    array_json = json_clone(selected);
  } else {
    array_json = json_create_array();
  }
  if (!array_json) {
    goto cleanup;
  }

  *out_array_json = array_json;
  array_json = NULL;
  rc = 0;

cleanup:
  json_free(array_json);
  array_json = NULL;
  json_free(state_json);
  state_json = NULL;
  return rc;
}

static int turbo_agent_session_build_supervisor_inbox_override_local(
    turbo_agent_session_t *session, const char *source_agent, const char *text,
    json_value_t **out_state_override) {
  json_value_t *state_json = NULL;
  int rc = -1;

  if (!session || !text || !out_state_override) {
    return -1;
  }
  *out_state_override = NULL;

  if (turbo_agent_session_get_thread_state_json_value(session, &state_json) != 0 || !state_json) {
    goto cleanup;
  }
  if (turbo_agent_state_append_supervisor_inbox_message(state_json, source_agent, text) != 0) {
    goto cleanup;
  }
  *out_state_override = state_json;
  state_json = NULL;
  rc = 0;

cleanup:
  json_free(state_json);
  state_json = NULL;
  return rc;
}

static int turbo_agent_session_build_supervisor_inspect_local(
    turbo_agent_session_t *session, json_value_t **out_inspect_json) {
  json_value_t *state = NULL;
  int rc = -1;
  if (!session || !out_inspect_json) return -1;
  *out_inspect_json = NULL;
  if (turbo_agent_session_get_thread_state_json_value(session, &state) == 0 && state)
    rc = turbo_agent_supervisor_inspect_build_internal(state, out_inspect_json);
  json_free(state);
  return rc;
}

static int turbo_agent_session_build_orchestration_inspect_local(turbo_agent_session_t *session,
                                                                 json_value_t **out_inspect_json) {
  json_value_t *inspect_json = NULL;
  json_value_t *supervisor_inspect_json = NULL;
  json_value_t *thread_lineage_json = NULL;
  json_value_t *branch_tree_json = NULL;
  json_value_t *child_runs_json = NULL;
  json_value_t *thread_timeline_json = NULL;
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *last_run_id = turbo_agent_session_last_run_id(session);
  int rc = -1;

  if (!session || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;

  if (turbo_agent_session_get_supervisor_inspect(session, &supervisor_inspect_json) != 0 ||
      !supervisor_inspect_json) {
    goto cleanup;
  }
  if (turbo_agent_session_get_thread_timeline_json_value(session, &thread_timeline_json) != 0) {
    json_free(thread_timeline_json);
    thread_timeline_json = NULL;
  }
  if (turbo_agent_session_list_thread_lineage(session, &thread_lineage_json) != 0 ||
      !thread_lineage_json) {
    goto cleanup;
  }
  if (turbo_agent_session_get_branch_tree(session, &branch_tree_json) != 0 || !branch_tree_json) {
    goto cleanup;
  }
  if (runtime && last_run_id && last_run_id[0] != '\0') {
    if (turbo_agent_runtime_list_child_runs(runtime, last_run_id, &child_runs_json) != 0) {
      goto cleanup;
    }
  } else {
    child_runs_json = json_create_array();
  }
  if (!child_runs_json) {
    goto cleanup;
  }

  inspect_json = json_create_object();
  if (!inspect_json) {
    goto cleanup;
  }
  if (session_inspect_take(inspect_json, "supervisor_inspect", &supervisor_inspect_json) != 0)
    goto cleanup;
  if (!thread_timeline_json) thread_timeline_json = json_create_null();
  if (session_inspect_take(inspect_json, "thread_timeline", &thread_timeline_json) != 0)
    goto cleanup;
  if (session_inspect_take(inspect_json, "thread_lineage", &thread_lineage_json) != 0) goto cleanup;
  if (session_inspect_take(inspect_json, "branch_tree", &branch_tree_json) != 0) goto cleanup;
  if (session_inspect_take(inspect_json, "child_runs", &child_runs_json) != 0) goto cleanup;

  *out_inspect_json = inspect_json;
  inspect_json = NULL;
  rc = 0;

cleanup:
  json_free(inspect_json);
  inspect_json = NULL;
  json_free(child_runs_json);
  child_runs_json = NULL;
  json_free(branch_tree_json);
  branch_tree_json = NULL;
  json_free(thread_lineage_json);
  thread_lineage_json = NULL;
  json_free(thread_timeline_json);
  thread_timeline_json = NULL;
  json_free(supervisor_inspect_json);
  supervisor_inspect_json = NULL;
  return rc;
}

CXX_C_API int turbo_agent_session_get_supervisor_inbox(turbo_agent_session_t *session,
                                                       json_value_t **out_inbox_json) {
  return turbo_agent_session_get_supervisor_array_json_local(
      session, turbo_agent_state_supervisor_inbox, out_inbox_json);
}

CXX_C_API int turbo_agent_session_get_supervisor_handoff_history(turbo_agent_session_t *session,
                                                                 json_value_t **out_history_json) {
  return turbo_agent_session_get_supervisor_array_json_local(
      session, turbo_agent_state_supervisor_handoff_history, out_history_json);
}

CXX_C_API int turbo_agent_session_get_supervisor_inspect(turbo_agent_session_t *session,
                                                         json_value_t **out_inspect_json) {
  return turbo_agent_session_build_supervisor_inspect_local(session, out_inspect_json);
}

CXX_C_API int turbo_agent_session_get_orchestration_inspect(turbo_agent_session_t *session,
                                                            json_value_t **out_inspect_json) {
  return turbo_agent_session_build_orchestration_inspect_local(session, out_inspect_json);
}

CXX_C_API int turbo_agent_session_append_supervisor_inbox_message_json_value(
    turbo_agent_session_t *session, const char *source_agent, const char *text,
    json_value_t **out_state_override) {
  return turbo_agent_session_build_supervisor_inbox_override_local(session, source_agent, text,
                                                                   out_state_override);
}
