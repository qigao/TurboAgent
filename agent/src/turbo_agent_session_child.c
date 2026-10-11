#include "turbo_agent_child_inspect_internal.h"
#include "turbo_agent_session.h"
#include "turbo_agent_state.h"
#include "turbo_runtime_json.h"

/* Runtime query results are owned. Publish missing optional children as JSON null;
 * retain each child locally until insertion succeeds. */
static int session_child_take(json_value_t *object, const char *key, json_value_t **value) {
  if (!*value) *value = json_create_null();
  if (turbo_runtime_json_object_set(object, key, *value) != TURBO_RUNTIME_JSON_OK) return -1;
  *value = NULL;
  return 0;
}

CXX_C_API int turbo_agent_session_get_child_run(turbo_agent_session_t *session,
                                                const json_value_t *output_item,
                                                json_value_t **out_run_json) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *child_run_id = turbo_agent_state_tool_result_child_run_id(output_item);

  if (!session || !runtime || !child_run_id || child_run_id[0] == '\0' || !out_run_json) {
    return -1;
  }
  return turbo_agent_runtime_get_run(runtime, child_run_id, out_run_json);
}

CXX_C_API int turbo_agent_session_get_child_checkpoint(turbo_agent_session_t *session,
                                                       const json_value_t *output_item,
                                                       json_value_t **out_checkpoint_json) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *child_checkpoint_id = turbo_agent_state_tool_result_child_checkpoint_id(output_item);

  if (!session || !runtime || !child_checkpoint_id || child_checkpoint_id[0] == '\0' ||
      !out_checkpoint_json) {
    return -1;
  }
  return turbo_agent_runtime_get_checkpoint(runtime, child_checkpoint_id, out_checkpoint_json);
}

CXX_C_API int turbo_agent_session_get_child_checkpoint_context(turbo_agent_session_t *session,
                                                               const json_value_t *output_item,
                                                               json_value_t **out_context_json) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *child_checkpoint_id = turbo_agent_state_tool_result_child_checkpoint_id(output_item);

  if (!session || !runtime || !child_checkpoint_id || child_checkpoint_id[0] == '\0' ||
      !out_context_json) {
    return -1;
  }
  return turbo_agent_runtime_get_checkpoint_context(runtime, child_checkpoint_id, out_context_json);
}

CXX_C_API int turbo_agent_session_get_child_thread_timeline_json_value(
    turbo_agent_session_t *session, const json_value_t *output_item, json_value_t **out_timeline) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *child_thread_id = turbo_agent_state_tool_result_child_thread_id(output_item);

  if (!session || !runtime || !child_thread_id || child_thread_id[0] == '\0' || !out_timeline) {
    return -1;
  }
  return turbo_agent_runtime_get_thread_timeline_json_value(runtime, child_thread_id, out_timeline);
}

CXX_C_API int turbo_agent_session_get_child_branch_tree(turbo_agent_session_t *session,
                                                        const json_value_t *output_item,
                                                        json_value_t **out_branch_tree_json) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *child_thread_id = turbo_agent_state_tool_result_child_thread_id(output_item);

  if (!session || !runtime || !child_thread_id || child_thread_id[0] == '\0' ||
      !out_branch_tree_json) {
    return -1;
  }
  return turbo_agent_runtime_get_branch_tree(runtime, child_thread_id, out_branch_tree_json);
}

CXX_C_API int turbo_agent_session_get_child_inspect(turbo_agent_session_t *session,
                                                    const json_value_t *output_item,
                                                    json_value_t **out_inspect_json) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  turbo_agent_child_inspect_parts_t parts = {0};
  const char *child_checkpoint_id;

  if (!session || !runtime || !output_item || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;

  child_checkpoint_id = turbo_agent_state_tool_result_child_checkpoint_id(output_item);
  if (turbo_agent_session_get_child_run(session, output_item, &parts.run) != 0 || !parts.run) {
    goto cleanup;
  }
  if (turbo_agent_session_list_child_checkpoints(session, output_item, &parts.checkpoints) != 0 ||
      !parts.checkpoints) {
    goto cleanup;
  }
  if (child_checkpoint_id && child_checkpoint_id[0] != '\0') {
    if (turbo_agent_session_get_child_checkpoint(session, output_item, &parts.latest_checkpoint) !=
            0 ||
        !parts.latest_checkpoint) {
      goto cleanup;
    }
    if (turbo_agent_session_get_child_checkpoint_context(session, output_item,
                                                         &parts.checkpoint_context) != 0 ||
        !parts.checkpoint_context) {
      goto cleanup;
    }
  }
  if (turbo_agent_session_load_child_history_events_json_value(session, output_item,
                                                               &parts.history_events) != 0 ||
      !parts.history_events) {
    goto cleanup;
  }
  if (turbo_agent_session_get_child_trace_events_json_value(session, output_item,
                                                            &parts.trace_events) != 0 ||
      !parts.trace_events) {
    goto cleanup;
  }
  /* Timeline and branch metadata remain optional; required queries above fail the report. */
  if (turbo_agent_session_get_child_thread_timeline_json_value(
          session, output_item, &parts.thread_timeline) != 0) {
    json_free(parts.thread_timeline);
    parts.thread_timeline = NULL;
  }
  if (turbo_agent_session_get_child_branch_tree(session, output_item, &parts.branch_tree) != 0) {
    json_free(parts.branch_tree);
    parts.branch_tree = NULL;
  }

  return turbo_agent_child_inspect_build_internal(&parts, out_inspect_json);

cleanup:
  turbo_agent_child_inspect_parts_clear_internal(&parts);
  return -1;
}

CXX_C_API int turbo_agent_session_get_child_orchestration_inspect(turbo_agent_session_t *session,
                                                                  const json_value_t *output_item,
                                                                  json_value_t **out_inspect_json) {
  int rc = -1;
  json_value_t *child_inspect_json = NULL;

  if (!session || !output_item || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;

  if (turbo_agent_session_get_child_inspect(session, output_item, &child_inspect_json) != 0 ||
      !child_inspect_json) {
    goto cleanup;
  }

  rc = turbo_agent_child_orchestration_build_internal(output_item, &child_inspect_json,
                                                       out_inspect_json);

cleanup:
  json_free(child_inspect_json);
  child_inspect_json = NULL;
  return rc;
}

CXX_C_API int turbo_agent_session_get_child_multi_agent_inspect(turbo_agent_session_t *session,
                                                                const json_value_t *output_item,
                                                                json_value_t **out_inspect_json) {
  json_value_t *inspect_json = NULL;
  json_value_t *supervisor_inspect_json = NULL;
  json_value_t *orchestration_inspect_json = NULL;
  json_value_t *child_orchestration_inspect_json = NULL;

  if (!session || !output_item || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;

  if (turbo_agent_session_get_supervisor_inspect(session, &supervisor_inspect_json) != 0 ||
      !supervisor_inspect_json) {
    goto cleanup;
  }
  if (turbo_agent_session_get_orchestration_inspect(session, &orchestration_inspect_json) != 0 ||
      !orchestration_inspect_json) {
    goto cleanup;
  }
  if (turbo_agent_session_get_child_orchestration_inspect(session, output_item,
                                                          &child_orchestration_inspect_json) != 0 ||
      !child_orchestration_inspect_json) {
    goto cleanup;
  }

  inspect_json = json_create_object();
  if (!inspect_json) {
    goto cleanup;
  }
  if (session_child_take(inspect_json, "supervisor_inspect", &supervisor_inspect_json) != 0)
    goto cleanup;
  if (session_child_take(inspect_json, "orchestration_inspect", &orchestration_inspect_json) != 0)
    goto cleanup;
  if (session_child_take(inspect_json, "child_orchestration_inspect",
                         &child_orchestration_inspect_json) != 0)
    goto cleanup;

  *out_inspect_json = inspect_json;
  inspect_json = NULL;
  return 0;

cleanup:
  json_free(inspect_json);
  inspect_json = NULL;
  json_free(child_orchestration_inspect_json);
  child_orchestration_inspect_json = NULL;
  json_free(orchestration_inspect_json);
  orchestration_inspect_json = NULL;
  json_free(supervisor_inspect_json);
  supervisor_inspect_json = NULL;
  return -1;
}

CXX_C_API int turbo_agent_session_list_child_checkpoints(turbo_agent_session_t *session,
                                                         const json_value_t *output_item,
                                                         json_value_t **out_checkpoints_json) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *child_run_id = turbo_agent_state_tool_result_child_run_id(output_item);

  if (!session || !runtime || !child_run_id || child_run_id[0] == '\0' || !out_checkpoints_json) {
    return -1;
  }
  return turbo_agent_runtime_list_checkpoints(runtime, child_run_id, out_checkpoints_json);
}

CXX_C_API int turbo_agent_session_load_child_history_events_json_value(
    turbo_agent_session_t *session, const json_value_t *output_item, json_value_t **out_events) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *child_checkpoint_id;
  const char *child_run_id;

  if (!session || !runtime || !output_item || !out_events) {
    return -1;
  }

  child_checkpoint_id = turbo_agent_state_tool_result_child_checkpoint_id(output_item);
  if (child_checkpoint_id && child_checkpoint_id[0] != '\0') {
    return turbo_agent_runtime_load_history_events_json_value(runtime, NULL, child_checkpoint_id,
                                                              out_events);
  }

  child_run_id = turbo_agent_state_tool_result_child_run_id(output_item);
  if (!child_run_id || child_run_id[0] == '\0') {
    return -1;
  }
  return turbo_agent_runtime_load_history_events_json_value(runtime, child_run_id, NULL,
                                                            out_events);
}

CXX_C_API int turbo_agent_session_get_child_trace_events_json_value(turbo_agent_session_t *session,
                                                                    const json_value_t *output_item,
                                                                    json_value_t **out_events) {
  turbo_agent_runtime_t *runtime = turbo_agent_session_runtime(session);
  const char *child_checkpoint_id;
  const char *child_run_id;

  if (!session || !runtime || !output_item || !out_events) {
    return -1;
  }

  child_checkpoint_id = turbo_agent_state_tool_result_child_checkpoint_id(output_item);
  if (child_checkpoint_id && child_checkpoint_id[0] != '\0') {
    return turbo_agent_runtime_get_checkpoint_trace_events_json_value(runtime, child_checkpoint_id,
                                                                      out_events);
  }

  child_run_id = turbo_agent_state_tool_result_child_run_id(output_item);
  if (!child_run_id || child_run_id[0] == '\0') {
    return -1;
  }
  return turbo_agent_runtime_get_run_trace_events_json_value(runtime, child_run_id, out_events);
}
