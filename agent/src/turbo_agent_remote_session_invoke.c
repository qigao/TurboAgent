#include "turbo_agent_remote_session.h"
#include "turbo_agent_session.h"
#include "turbo_agent_session_result_internal.h"

CXX_C_API int turbo_agent_remote_session_start_text(turbo_agent_remote_session_t *session,
                                                    const char *graph_name, const char *user_text,
                                                    const turbo_graph_run_options_t *options,
                                                    json_value_t **out_summary_json,
                                                    json_value_t **out_state) {
  json_value_t *state;
  int rc;

  if (!session || !turbo_agent_remote_session_client(session) || !graph_name || !graph_name[0] ||
      !out_summary_json || !out_state) {
    return -1;
  }
  state = turbo_agent_session_create_input_state_json_value(user_text);
  if (!state) {
    return -1;
  }
  rc = turbo_agent_remote_session_start_json_value_graph(session, graph_name, state, options,
                                                         out_summary_json, out_state);
  turbo_runtime_json_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_start_messages(turbo_agent_remote_session_t *session,
                                                        const char *graph_name,
                                                        const json_value_t *messages,
                                                        const turbo_graph_run_options_t *options,
                                                        json_value_t **out_summary_json,
                                                        json_value_t **out_state) {
  json_value_t *state;
  int rc;

  if (!session || !turbo_agent_remote_session_client(session) || !graph_name || !graph_name[0] ||
      !out_summary_json || !out_state) {
    return -1;
  }
  state = turbo_agent_session_create_input_messages_state_json_value(messages);
  if (!state) {
    return -1;
  }
  rc = turbo_agent_remote_session_start_json_value_graph(session, graph_name, state, options,
                                                         out_summary_json, out_state);
  turbo_runtime_json_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_remote_session_invoke_text(turbo_agent_remote_session_t *session,
                                                     const char *graph_name, const char *user_text,
                                                     const turbo_graph_run_options_t *options,
                                                     char **out_text,
                                                     json_value_t **out_summary_json) {
  json_value_t *out_state = NULL;
  int rc;

  if (!out_text || !out_summary_json) {
    return -1;
  }
  *out_text = NULL;
  *out_summary_json = NULL;
  rc = turbo_agent_remote_session_start_text(session, graph_name, user_text, options,
                                             out_summary_json, &out_state);
  return turbo_agent_session_finish_text_internal(rc, out_state, out_text, out_summary_json);
}

CXX_C_API int turbo_agent_remote_session_invoke_messages_text(
    turbo_agent_remote_session_t *session, const char *graph_name, const json_value_t *messages,
    const turbo_graph_run_options_t *options, char **out_text, json_value_t **out_summary_json) {
  json_value_t *out_state = NULL;
  int rc;

  if (!out_text || !out_summary_json) {
    return -1;
  }
  *out_text = NULL;
  *out_summary_json = NULL;
  rc = turbo_agent_remote_session_start_messages(session, graph_name, messages, options,
                                                 out_summary_json, &out_state);
  return turbo_agent_session_finish_text_internal(rc, out_state, out_text, out_summary_json);
}

CXX_C_API int turbo_agent_remote_session_invoke_json(turbo_agent_remote_session_t *session,
                                                     const char *graph_name, const char *user_text,
                                                     const turbo_graph_run_options_t *options,
                                                     json_value_t **out_json,
                                                     json_value_t **out_summary_json) {
  json_value_t *out_state = NULL;
  int rc;

  if (!out_json || !out_summary_json) {
    return -1;
  }
  *out_json = NULL;
  *out_summary_json = NULL;
  rc = turbo_agent_remote_session_start_text(session, graph_name, user_text, options,
                                             out_summary_json, &out_state);
  return turbo_agent_session_finish_json_internal(rc, out_state, out_json, out_summary_json);
}

CXX_C_API int turbo_agent_remote_session_invoke_messages_json(
    turbo_agent_remote_session_t *session, const char *graph_name, const json_value_t *messages,
    const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json) {
  json_value_t *out_state = NULL;
  int rc;

  if (!out_json || !out_summary_json) {
    return -1;
  }
  *out_json = NULL;
  *out_summary_json = NULL;
  rc = turbo_agent_remote_session_start_messages(session, graph_name, messages, options,
                                                 out_summary_json, &out_state);
  return turbo_agent_session_finish_json_internal(rc, out_state, out_json, out_summary_json);
}
