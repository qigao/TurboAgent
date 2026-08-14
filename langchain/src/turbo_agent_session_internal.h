#ifndef TURBO_AGENT_SESSION_INTERNAL_H
#define TURBO_AGENT_SESSION_INTERNAL_H

#include "turbo_agent_session.h"
#include "turbo_runtime_control.h"

int turbo_agent_session_prepare_start_options_internal(
    turbo_agent_session_t *session, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data, turbo_agent_runtime_parent_link_t *parent_link,
    turbo_agent_runtime_exec_options_t *out_options);

int turbo_agent_session_prepare_resume_options_internal(
    turbo_agent_session_t *session, const turbo_agent_session_exec_options_t *session_options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    turbo_agent_runtime_exec_options_t *out_options);

int turbo_agent_session_complete_execution_internal(turbo_agent_session_t *session,
                                                    turbo_graph_t *graph,
                                                    const turbo_graph_run_options_t *graph_options,
                                                    turbo_event_sink_json_value_fn event_sink,
                                                    void *event_sink_user_data,
                                                    turbo_cancel_token_t *cancel_token,
                                                    json_value_t **summary, json_value_t **state);

#endif
