#include "turbo_agent_session.h"
#include "turbo_agent_session_result_internal.h"
#include "turbo_agent_state.h"
#include "turbo_runtime_json.h"

#include <stdlib.h>
#include <string.h>

/* Consume constructed fields even if insertion fails. */
static int session_invoke_set(json_value_t *object, const char *key, json_value_t *value) {
  if (turbo_runtime_json_object_set(object, key, value) != TURBO_RUNTIME_JSON_OK) {
    json_free(value);
    return -1;
  }
  return 0;
}

static int turbo_agent_session_batch_add_text_result(json_value_t *results, size_t index, int ok,
                                                     json_value_t **summary_json,
                                                     const char *output_text,
                                                     const char *error_text) {
  json_value_t *item;

  if (!results) {
    return -1;
  }
  item = json_create_object();
  if (!item) {
    return -1;
  }
  if (session_invoke_set(item, "index", json_create_number((double)index)) != 0 ||
      session_invoke_set(item, "ok", json_create_bool(ok != 0)) != 0)
    goto failed;
  if (summary_json && *summary_json) {
    if (turbo_runtime_json_object_set(item, "summary", *summary_json) != TURBO_RUNTIME_JSON_OK)
      goto failed;
    *summary_json = NULL;
  } else {
    if (session_invoke_set(item, "summary", json_create_null()) != 0) goto failed;
  }
  if (session_invoke_set(item, "output_text",
                         output_text ? json_create_string(output_text) : json_create_null()) != 0 ||
      session_invoke_set(item, "error",
                         error_text ? json_create_string(error_text) : json_create_null()) != 0 ||
      turbo_runtime_json_array_append(results, item) != TURBO_RUNTIME_JSON_OK)
    goto failed;
  return 0;
failed:
  json_free(item);
  return -1;
}

/* The input factory transfers one private state; the synchronous filter is
 * borrowed only while start_preset runs, including any bounded follow-ups. */
static int session_start_owned_state(turbo_agent_session_t *session, json_value_t *state,
                                     const turbo_graph_run_options_t *options,
                                     turbo_event_stream_mode_t stream_mode,
                                     turbo_event_sink_json_value_fn event_sink,
                                     void *event_sink_user_data, json_value_t **out_summary,
                                     json_value_t **out_state) {
  turbo_event_stream_filter_t filter = {0};
  int rc;
  if (!state) return -1;
  filter.mode = stream_mode;
  filter.sink = event_sink;
  filter.sink_user_data = event_sink_user_data;
  rc = turbo_agent_session_start_preset(
      session, turbo_agent_session_workflow_kind(session), state, options,
      event_sink ? turbo_event_stream_filter_sink_json_value : NULL, event_sink ? &filter : NULL,
      out_summary, out_state);
  turbo_runtime_json_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_session_start_messages(turbo_agent_session_t *session,
                                                 const json_value_t *messages,
                                                 const turbo_graph_run_options_t *options,
                                                 json_value_t **out_summary_json,
                                                 json_value_t **out_state) {
  return turbo_agent_session_start_messages_stream(
      session, messages, options, TURBO_EVENT_STREAM_ALL, NULL, NULL, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_session_start_messages_stream(
    turbo_agent_session_t *session, const json_value_t *messages,
    const turbo_graph_run_options_t *options, turbo_event_stream_mode_t stream_mode,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  json_value_t *state;
  const char *memory_namespace = turbo_agent_session_memory_namespace(session);

  if (!session) {
    return -1;
  }
  if (memory_namespace && memory_namespace[0] != '\0') {
    state = turbo_agent_session_create_input_messages_state_with_memory_json_value(
        session, messages, memory_namespace);
  } else {
    state = turbo_agent_session_create_input_messages_state_json_value(messages);
  }
  return session_start_owned_state(session, state, options, stream_mode, event_sink,
                                   event_sink_user_data, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_session_start_text(turbo_agent_session_t *session, const char *user_text,
                                             const turbo_graph_run_options_t *options,
                                             json_value_t **out_summary_json,
                                             json_value_t **out_state) {
  return turbo_agent_session_start_text_stream(session, user_text, options, TURBO_EVENT_STREAM_ALL,
                                               NULL, NULL, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_session_start_text_stream(
    turbo_agent_session_t *session, const char *user_text, const turbo_graph_run_options_t *options,
    turbo_event_stream_mode_t stream_mode, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data, json_value_t **out_summary_json, json_value_t **out_state) {
  json_value_t *state;
  const char *memory_namespace = turbo_agent_session_memory_namespace(session);

  if (!session) {
    return -1;
  }
  if (memory_namespace && memory_namespace[0] != '\0') {
    state = turbo_agent_session_create_input_state_with_memory_json_value(session, user_text,
                                                                          memory_namespace);
  } else {
    state = turbo_agent_session_create_input_state_json_value(user_text);
  }
  return session_start_owned_state(session, state, options, stream_mode, event_sink,
                                   event_sink_user_data, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_session_invoke_text(turbo_agent_session_t *session, const char *user_text,
                                              const turbo_graph_run_options_t *options,
                                              char **out_text, json_value_t **out_summary_json) {
  json_value_t *out_state = NULL;
  int rc;

  if (!session || !out_text) {
    return -1;
  }
  *out_text = NULL;
  if (out_summary_json) {
    *out_summary_json = NULL;
  }
  rc = turbo_agent_session_start_text(session, user_text, options, out_summary_json, &out_state);
  return turbo_agent_session_finish_text_internal(rc, out_state, out_text, out_summary_json);
}

CXX_C_API int turbo_agent_session_invoke_json(turbo_agent_session_t *session, const char *user_text,
                                              const turbo_graph_run_options_t *options,
                                              json_value_t **out_json,
                                              json_value_t **out_summary_json) {
  json_value_t *out_state = NULL;
  int rc;

  if (!session || !out_json) {
    return -1;
  }
  *out_json = NULL;
  if (out_summary_json) {
    *out_summary_json = NULL;
  }
  rc = turbo_agent_session_start_text(session, user_text, options, out_summary_json, &out_state);
  return turbo_agent_session_finish_json_internal(rc, out_state, out_json, out_summary_json);
}

CXX_C_API int turbo_agent_session_invoke_messages_text(turbo_agent_session_t *session,
                                                       const json_value_t *messages,
                                                       const turbo_graph_run_options_t *options,
                                                       char **out_text,
                                                       json_value_t **out_summary_json) {
  json_value_t *out_state = NULL;
  int rc;

  if (!session || !out_text) {
    return -1;
  }
  *out_text = NULL;
  if (out_summary_json) {
    *out_summary_json = NULL;
  }
  rc = turbo_agent_session_start_messages(session, messages, options, out_summary_json, &out_state);
  return turbo_agent_session_finish_text_internal(rc, out_state, out_text, out_summary_json);
}

CXX_C_API int turbo_agent_session_invoke_messages_json(turbo_agent_session_t *session,
                                                       const json_value_t *messages,
                                                       const turbo_graph_run_options_t *options,
                                                       json_value_t **out_json,
                                                       json_value_t **out_summary_json) {
  json_value_t *out_state = NULL;
  int rc;

  if (!session || !out_json) {
    return -1;
  }
  *out_json = NULL;
  if (out_summary_json) {
    *out_summary_json = NULL;
  }
  rc = turbo_agent_session_start_messages(session, messages, options, out_summary_json, &out_state);
  return turbo_agent_session_finish_json_internal(rc, out_state, out_json, out_summary_json);
}

CXX_C_API int turbo_agent_session_batch_text(turbo_agent_session_t *session,
                                             const char *const *user_texts, size_t count,
                                             const turbo_graph_run_options_t *options,
                                             json_value_t **out_results_json) {
  json_value_t *results;
  size_t i;

  if (!session || (!user_texts && count > 0) || !out_results_json) {
    return -1;
  }
  *out_results_json = NULL;
  results = json_create_array();
  if (!results) {
    return -1;
  }
  for (i = 0; i < count; ++i) {
    json_value_t *summary = NULL;
    char *text = NULL;
    int rc;

    if (!user_texts[i]) {
      if (turbo_agent_session_batch_add_text_result(results, i, 0, NULL, NULL, "invalid input") !=
          0) {
        json_free(results);
        results = NULL;
        return -1;
      }
      continue;
    }

    rc = turbo_agent_session_invoke_text(session, user_texts[i], options, &text, &summary);
    if (turbo_agent_session_batch_add_text_result(results, i, rc == 0, &summary,
                                                  rc == 0 ? text : NULL,
                                                  rc == 0 ? NULL : "invoke failed") != 0) {
      free(text);
      json_free(summary);
      summary = NULL;
      json_free(results);
      results = NULL;
      return -1;
    }
    free(text);
    json_free(summary);
    summary = NULL;
  }
  *out_results_json = results;
  return 0;
}
