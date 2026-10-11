#include "turbo_agent_child_inspect_internal.h"
#include "turbo_agent_remote_session.h"
#include "turbo_agent_session.h"
#include "turbo_agent_state.h"
#include "turbo_agent_util_internal.h"
#include <stdlib.h>
#include <string.h>

static void turbo_agent_remote_session_json_free(json_value_t **value) {
  if (!value || !*value) {
    return;
  }
  json_free(*value);
  *value = NULL;
}

static char *turbo_agent_remote_session_strdup_or_null(const char *value) {
  return (char *)turbo_agent_util_strdup(value);
}

struct turbo_agent_remote_session_s {
  turbo_agent_runtime_remote_client_t *client;
  int owns_client;
  tstr thread_id;
  tstr last_run_id;
  tstr last_checkpoint_id;
};



static int turbo_agent_remote_session_capture_summary(turbo_agent_remote_session_t *session,
                                                      const json_value_t *summary) {
  if (!session) return -1;
  return turbo_agent_util_capture_summary_ids(summary, &session->thread_id,
                                              &session->last_run_id,
                                              &session->last_checkpoint_id);
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
    const json_value_t *state, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state) {
  char *thread_id = NULL;
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !graph_name || !graph_name[0] || !out_summary_json ||
      !out_state) {
    return -1;
  }
  thread_id = turbo_agent_remote_session_strdup_or_null(
      turbo_agent_remote_session_resolve_thread_id(session));
  if (!thread_id) return -1;
  rc = turbo_agent_runtime_remote_client_start_json_value_graph(
      session->client, graph_name, state, options, thread_id, out_summary_json, out_state,
      &error_json);
  tstr_free(thread_id);
  turbo_agent_remote_session_json_free(&error_json);
  if (rc != 0) {
    return rc;
  }
  return turbo_agent_remote_session_capture_summary(session, *out_summary_json);
}

static int turbo_agent_remote_session_call_resume(
    turbo_agent_remote_session_t *session, const char *graph_name, const char *checkpoint_id,
    const json_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state, int fork_mode) {
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
    rc = turbo_agent_runtime_remote_client_fork_json_value_graph(
        session->client, graph_name, owned_checkpoint_id, state_override, options,
        out_summary_json, out_state, &error_json);
  } else {
    rc = turbo_agent_runtime_remote_client_resume_json_value_graph(
        session->client, graph_name, owned_checkpoint_id, state_override, options,
        out_summary_json, out_state, &error_json);
  }
  tstr_free(owned_checkpoint_id);
  turbo_agent_remote_session_json_free(&error_json);
  if (rc != 0) {
    return rc;
  }
  return turbo_agent_remote_session_capture_summary(session, *out_summary_json);
}

static int turbo_agent_remote_session_call_thread_command(
    turbo_agent_remote_session_t *session, const char *graph_name,
    const json_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state, int fork_mode) {
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
    rc = turbo_agent_runtime_remote_client_fork_thread_command_json_value(
        session->client, graph_name, owned_thread_id, command, options, out_summary_json, out_state,
        &error_json);
  } else {
    rc = turbo_agent_runtime_remote_client_resume_thread_command_json_value(
        session->client, graph_name, owned_thread_id, command, options, out_summary_json, out_state,
        &error_json);
  }
  tstr_free(owned_thread_id);
  turbo_agent_remote_session_json_free(&error_json);
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
    turbo_agent_remote_session_json_free(&index_json);
    return rc;
  }

  field_json = json_object_get(index_json, field_name);
  if (!field_json || json_type(field_json) != JSON_OBJECT) {
    turbo_agent_remote_session_json_free(&index_json);
    return -1;
  }

  *out_object_json = json_clone(field_json);
  turbo_agent_remote_session_json_free(&index_json);
  return *out_object_json ? 0 : -1;
}

static int turbo_agent_remote_session_get_supervisor_array_json_local(
    turbo_agent_remote_session_t *session,
    const json_value_t *(*selector)(const json_value_t *state),
    json_value_t **out_array_json) {
  json_value_t *state_json = NULL;
  json_value_t *array_json = NULL;
  const json_value_t *selected = NULL;
  int rc = -1;

  if (!session || !selector || !out_array_json) {
    return -1;
  }
  *out_array_json = NULL;
  if (turbo_agent_remote_session_get_thread_state_json_value(session, &state_json) != 0 || !state_json) {
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
  turbo_agent_remote_session_json_free(&array_json);
  turbo_agent_remote_session_json_free(&state_json);
  return rc;
}

static int turbo_agent_remote_session_clone_json_value_field(
    const json_value_t *object, const char *field_name,
    json_value_t **out_value) {
  const json_value_t *field_value;

  if (!object || !field_name || !field_name[0] || !out_value ||
      json_type(object) != JSON_OBJECT) {
    return -1;
  }
  field_value = json_object_get(object, field_name);
  if (!field_value) {
    return -1;
  }
  *out_value = json_clone(field_value);
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
  turbo_agent_remote_session_json_free(&error_json);
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
  turbo_agent_remote_session_json_free(&error_json);
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
  turbo_agent_remote_session_json_free(&error_json);
  return rc;
}

static int turbo_agent_remote_session_call_load_history_events_json_value(
    turbo_agent_remote_session_t *session, const char *run_id, const char *checkpoint_id,
    json_value_t **out_events) {
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client ||
      ((!run_id || !run_id[0]) && (!checkpoint_id || !checkpoint_id[0])) || !out_events) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_load_history_events_json_value(session->client, run_id,
                                                                  checkpoint_id, out_events,
                                                                  &error_json);
  turbo_agent_remote_session_json_free(&error_json);
  return rc;
}

static int turbo_agent_remote_session_call_get_run_trace_events_json_value(
    turbo_agent_remote_session_t *session, const char *run_id,
    json_value_t **out_events) {
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !run_id || !run_id[0] || !out_events) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_get_run_trace_events_json_value(session->client, run_id,
                                                                   out_events, &error_json);
  turbo_agent_remote_session_json_free(&error_json);
  return rc;
}

static int turbo_agent_remote_session_call_get_checkpoint_trace_events_json_value(
    turbo_agent_remote_session_t *session, const char *checkpoint_id,
    json_value_t **out_events) {
  json_value_t *error_json = NULL;
  int rc;

  if (!session || !session->client || !checkpoint_id || !checkpoint_id[0] || !out_events) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_get_checkpoint_trace_events_json_value(session->client,
                                                                          checkpoint_id,
                                                                          out_events, &error_json);
  turbo_agent_remote_session_json_free(&error_json);
  return rc;
}

CXX_C_API turbo_agent_remote_session_t *
turbo_agent_remote_session_create(const turbo_agent_remote_session_config_t *config) {
  turbo_agent_remote_session_t *session;

  if (!config || (!config->client && (!config->client_config.http_client &&
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
  if (!session->thread_id) {
    turbo_agent_remote_session_destroy(session);
    return NULL;
  }
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
  turbo_agent_remote_session_json_free(&error_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_start_json_value_graph(
    turbo_agent_remote_session_t *session, const char *graph_name,
    const json_value_t *state, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state) {
  if (!session || !session->client || !graph_name || !graph_name[0] || !out_summary_json ||
      !out_state) {
    return -1;
  }
  return turbo_agent_remote_session_call_start(session, graph_name, state, options,
                                               out_summary_json, out_state);
}

CXX_C_API int turbo_agent_remote_session_resume_json_value_graph(
    turbo_agent_remote_session_t *session, const char *graph_name, const char *checkpoint_id,
    const json_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state) {
  if (!session || !session->client || !graph_name || !graph_name[0] || !out_summary_json ||
      !out_state) {
    return -1;
  }
  return turbo_agent_remote_session_call_resume(session, graph_name, checkpoint_id, state_override,
                                                options, out_summary_json, out_state, 0);
}

CXX_C_API int turbo_agent_remote_session_fork_json_value_graph(
    turbo_agent_remote_session_t *session, const char *graph_name, const char *checkpoint_id,
    const json_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state) {
  if (!session || !session->client || !graph_name || !graph_name[0] || !out_summary_json ||
      !out_state) {
    return -1;
  }
  return turbo_agent_remote_session_call_resume(session, graph_name, checkpoint_id, state_override,
                                                options, out_summary_json, out_state, 1);
}

CXX_C_API int turbo_agent_remote_session_get_thread_state_json_value(
    turbo_agent_remote_session_t *session, json_value_t **out_state) {
  const char *thread_id;
  json_value_t *error_json = NULL;
  int rc;

  thread_id = turbo_agent_remote_session_resolve_thread_id(session);
  if (!session || !session->client || !thread_id || !thread_id[0] || !out_state) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_get_thread_state_json_value(session->client, thread_id,
                                                               out_state, &error_json);
  turbo_agent_remote_session_json_free(&error_json);
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
  turbo_agent_remote_session_json_free(&error_json);
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
  turbo_agent_remote_session_json_free(&error_json);
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
  turbo_agent_remote_session_json_free(&error_json);
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
  turbo_agent_remote_session_json_free(&stored_record_json);
  turbo_agent_remote_session_json_free(&error_json);
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
  turbo_agent_remote_session_json_free(&error_json);
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
  turbo_agent_remote_session_json_free(&error_json);
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
  turbo_agent_remote_session_json_free(&error_json);
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
  turbo_agent_remote_session_json_free(&error_json);
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
  turbo_agent_remote_session_json_free(&error_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_get_thread_timeline_json_value(
    turbo_agent_remote_session_t *session, json_value_t **out_timeline) {
  const char *thread_id;
  json_value_t *error_json = NULL;
  int rc;

  thread_id = turbo_agent_remote_session_resolve_thread_id(session);
  if (!session || !session->client || !thread_id || !thread_id[0] || !out_timeline) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_get_thread_timeline_json_value(session->client, thread_id,
                                                                  out_timeline, &error_json);
  turbo_agent_remote_session_json_free(&error_json);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_load_thread_history_events_json_value(
    turbo_agent_remote_session_t *session, json_value_t **out_events) {
  json_value_t *timeline = NULL;
  int rc;

  if (!session || !session->client || !out_events) {
    return -1;
  }
  rc = turbo_agent_remote_session_get_thread_timeline_json_value(session, &timeline);
  if (rc != 0 || !timeline) {
    turbo_runtime_json_destroy(timeline);
    return -1;
  }
  rc = turbo_agent_remote_session_clone_json_value_field(timeline, "history_events", out_events);
  turbo_runtime_json_destroy(timeline);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_get_thread_trace_events_json_value(
    turbo_agent_remote_session_t *session, json_value_t **out_events) {
  json_value_t *index_json = NULL;
  const json_value_t *trace_events_json;
  int rc;

  if (!session || !session->client || !out_events) {
    return -1;
  }
  rc = turbo_agent_remote_session_get_observability_index(session, &index_json);
  if (rc != 0 || !index_json) {
    turbo_agent_remote_session_json_free(&index_json);
    return -1;
  }
  trace_events_json = json_object_get(index_json, "trace_events");
  if (!trace_events_json || json_type(trace_events_json) != JSON_ARRAY) {
    turbo_agent_remote_session_json_free(&index_json);
    return -1;
  }
  *out_events = json_clone(trace_events_json);
  turbo_agent_remote_session_json_free(&index_json);
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
  turbo_agent_remote_session_json_free(&error_json);
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
  turbo_agent_remote_session_json_free(&error_json);
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
  turbo_agent_remote_session_json_free(&error_json);
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
  turbo_agent_remote_session_json_free(&error_json);
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

CXX_C_API int turbo_agent_remote_session_get_child_thread_timeline_json_value(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_timeline) {
  const char *child_thread_id = turbo_agent_state_tool_result_child_thread_id(output_item);

  if (!session || !session->client || !child_thread_id || child_thread_id[0] == '\0' ||
      !out_timeline) {
    return -1;
  }
  return turbo_agent_runtime_remote_client_get_thread_timeline_json_value(session->client, child_thread_id,
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

CXX_C_API int turbo_agent_remote_session_load_child_history_events_json_value(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_events) {
  const char *child_checkpoint_id;
  const char *child_run_id;

  if (!session || !session->client || !output_item || !out_events) {
    return -1;
  }

  child_checkpoint_id = turbo_agent_state_tool_result_child_checkpoint_id(output_item);
  if (child_checkpoint_id && child_checkpoint_id[0] != '\0') {
    return turbo_agent_remote_session_call_load_history_events_json_value(session, NULL,
                                                                    child_checkpoint_id,
                                                                    out_events);
  }

  child_run_id = turbo_agent_state_tool_result_child_run_id(output_item);
  if (!child_run_id || child_run_id[0] == '\0') {
    return -1;
  }
  return turbo_agent_remote_session_call_load_history_events_json_value(session, child_run_id, NULL,
                                                                  out_events);
}

CXX_C_API int turbo_agent_remote_session_get_child_trace_events_json_value(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_events) {
  const char *child_checkpoint_id;
  const char *child_run_id;

  if (!session || !session->client || !output_item || !out_events) {
    return -1;
  }

  child_checkpoint_id = turbo_agent_state_tool_result_child_checkpoint_id(output_item);
  if (child_checkpoint_id && child_checkpoint_id[0] != '\0') {
    return turbo_agent_remote_session_call_get_checkpoint_trace_events_json_value(session,
                                                                            child_checkpoint_id,
                                                                            out_events);
  }

  child_run_id = turbo_agent_state_tool_result_child_run_id(output_item);
  if (!child_run_id || child_run_id[0] == '\0') {
    return -1;
  }
  return turbo_agent_remote_session_call_get_run_trace_events_json_value(session, child_run_id,
                                                                   out_events);
}

CXX_C_API int turbo_agent_remote_session_get_child_inspect(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_inspect_json) {
  turbo_agent_child_inspect_parts_t parts = {0};
  const char *child_checkpoint_id;

  if (!session || !session->client || !output_item || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;

  child_checkpoint_id = turbo_agent_state_tool_result_child_checkpoint_id(output_item);
  if (turbo_agent_remote_session_get_child_run(session, output_item, &parts.run) != 0 || !parts.run) {
    goto cleanup;
  }
  if (turbo_agent_remote_session_list_child_checkpoints(session, output_item, &parts.checkpoints) !=
          0 ||
      !parts.checkpoints) {
    goto cleanup;
  }
  if (child_checkpoint_id && child_checkpoint_id[0] != '\0') {
    if (turbo_agent_remote_session_get_child_checkpoint(session, output_item,
                                                        &parts.latest_checkpoint) != 0 ||
        !parts.latest_checkpoint) {
      goto cleanup;
    }
    if (turbo_agent_remote_session_get_child_checkpoint_context(session, output_item,
                                                                &parts.checkpoint_context) != 0 ||
        !parts.checkpoint_context) {
      goto cleanup;
    }
  }
  if (turbo_agent_remote_session_load_child_history_events_json_value(session, output_item,
                                                                &parts.history_events) != 0 ||
      !parts.history_events) {
    goto cleanup;
  }
  if (turbo_agent_remote_session_get_child_trace_events_json_value(session, output_item,
                                                             &parts.trace_events) != 0 ||
      !parts.trace_events) {
    goto cleanup;
  }
  /* Preserve the two independent optional queries, including their failure handling. */
  if (turbo_agent_remote_session_get_child_thread_timeline_json_value(
          session, output_item, &parts.thread_timeline) != 0) {
    turbo_agent_remote_session_json_free(&parts.thread_timeline);
  }
  if (turbo_agent_remote_session_get_child_branch_tree(session, output_item, &parts.branch_tree) != 0) {
    turbo_agent_remote_session_json_free(&parts.branch_tree);
  }

  return turbo_agent_child_inspect_build_internal(&parts, out_inspect_json);

cleanup:
  turbo_agent_child_inspect_parts_clear_internal(&parts);
  return -1;
}

CXX_C_API int turbo_agent_remote_session_get_child_orchestration_inspect(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_inspect_json) {
  int rc = -1;
  json_value_t *child_inspect_json = NULL;

  if (!session || !output_item || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;

  if (turbo_agent_remote_session_get_child_inspect(session, output_item, &child_inspect_json) != 0 ||
      !child_inspect_json) {
    goto cleanup;
  }

  rc = turbo_agent_child_orchestration_build_internal(output_item, &child_inspect_json,
                                                       out_inspect_json);

cleanup:
  turbo_agent_remote_session_json_free(&child_inspect_json);
  return rc;
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

CXX_C_API int turbo_agent_remote_session_resume_thread_command_json_value(
    turbo_agent_remote_session_t *session, const char *graph_name,
    const json_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state) {
  if (!session || !session->client || !graph_name || !graph_name[0] || !command ||
      !out_summary_json || !out_state) {
    return -1;
  }
  return turbo_agent_remote_session_call_thread_command(session, graph_name, command, options,
                                                        out_summary_json, out_state, 0);
}

CXX_C_API int turbo_agent_remote_session_fork_thread_command_json_value(
    turbo_agent_remote_session_t *session, const char *graph_name,
    const json_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state) {
  if (!session || !session->client || !graph_name || !graph_name[0] || !command ||
      !out_summary_json || !out_state) {
    return -1;
  }
  return turbo_agent_remote_session_call_thread_command(session, graph_name, command, options,
                                                        out_summary_json, out_state, 1);
}
