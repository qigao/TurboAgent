/* turbo_agent_runtime_session_internal.h
 *
 * Runtime entry-points that only turbo_agent_session.c needs.
 * Not part of the public SDK. Do not install or export.
 */
#ifndef TURBO_AGENT_RUNTIME_SESSION_INTERNAL_H
#define TURBO_AGENT_RUNTIME_SESSION_INTERNAL_H

#include "turbo_agent_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Linked-stream start: carries parent-link metadata for sub-agent registration. */
CXX_C_API int turbo_agent_runtime_start_json_value_graph_linked_stream(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph,
    const json_value_t *state, const turbo_graph_run_options_t *options,
    const char *thread_id, const turbo_agent_runtime_parent_link_t *parent_link,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state);

/* Thread-head state / trace readers (prefer over the legacy run-by-updated_at path). */
CXX_C_API int turbo_agent_runtime_get_thread_head_state_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    json_value_t **out_state);

CXX_C_API int turbo_agent_runtime_get_thread_head_trace_events_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    json_value_t **out_events);

/* State-override preparers (read + merge; no persistence). */
CXX_C_API int turbo_agent_runtime_prepare_checkpoint_state_override_json_value(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id,
    const json_value_t *state_patch,
    json_value_t **out_state_override);

CXX_C_API int turbo_agent_runtime_prepare_thread_state_override_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    const json_value_t *state_patch,
    json_value_t **out_state_override);

CXX_C_API int turbo_agent_runtime_apply_state_patch_json_value(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id,
    const json_value_t *state_patch,
    json_value_t **out_state_override);

/* Command-override preparers (read + interpret; no persistence). */
CXX_C_API int turbo_agent_runtime_prepare_checkpoint_command_override_json_value(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id,
    const json_value_t *command,
    json_value_t **out_state_override);

CXX_C_API int turbo_agent_runtime_prepare_thread_command_override_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    const json_value_t *command,
    json_value_t **out_state_override);

/* Graph resume with state-patch input (implicit checkpoint fallback). */
CXX_C_API int turbo_agent_runtime_resume_state_patch_json_value_graph(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state);

/* Graph resume with state-patch input (thread scope). */
CXX_C_API int turbo_agent_runtime_resume_thread_state_patch_json_value_graph(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *thread_id,
    const json_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state);

/* Graph fork with state-patch input (implicit checkpoint fallback). */
CXX_C_API int turbo_agent_runtime_fork_state_patch_json_value_graph(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state);

/* Graph fork with state-patch input (thread scope). */
CXX_C_API int turbo_agent_runtime_fork_thread_state_patch_json_value_graph(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *thread_id,
    const json_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_AGENT_RUNTIME_SESSION_INTERNAL_H */
