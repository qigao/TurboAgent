#ifndef TURBO_AGENT_REMOTE_SESSION_H
#define TURBO_AGENT_REMOTE_SESSION_H

#include <platform.h>

#include "turbo_agent_memory_store.h"
#include "turbo_agent_runtime_remote_client.h"
#include "turbo_event.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_remote_session_s turbo_agent_remote_session_t;

typedef struct turbo_agent_remote_session_config_s {
  turbo_agent_runtime_remote_client_t *client;
  turbo_agent_runtime_remote_client_config_t client_config;
  const char *thread_id;
} turbo_agent_remote_session_config_t;

/**
 * @brief Create one lightweight remote session facade over the runtime remote client.
 *
 * When `client` is non-NULL the session borrows that client. Otherwise it creates
 * and owns one client from `client_config`.
 */
CXX_C_API turbo_agent_remote_session_t *
turbo_agent_remote_session_create(const turbo_agent_remote_session_config_t *config);

/**
 * @brief Destroy one remote session facade.
 */
CXX_C_API void turbo_agent_remote_session_destroy(turbo_agent_remote_session_t *session);

/**
 * @brief Return the borrowed or owned remote client backing the session.
 */
CXX_C_API turbo_agent_runtime_remote_client_t *
turbo_agent_remote_session_client(const turbo_agent_remote_session_t *session);

/**
 * @brief Return the current remote thread id, or NULL before first start.
 */
CXX_C_API const char *turbo_agent_remote_session_thread_id(
    const turbo_agent_remote_session_t *session);

/**
 * @brief Return the last remote run id observed by the session.
 */
CXX_C_API const char *turbo_agent_remote_session_last_run_id(
    const turbo_agent_remote_session_t *session);

/**
 * @brief Return the most recent non-null checkpoint id observed by the session.
 */
CXX_C_API const char *turbo_agent_remote_session_last_checkpoint_id(
    const turbo_agent_remote_session_t *session);

/**
 * @brief Load remote startup diagnostics for one graph.
 *
 * The returned JSON object is owned by the caller.
 */
CXX_C_API int turbo_agent_remote_session_get_startup_diagnostics(
    turbo_agent_remote_session_t *session, const char *graph_name,
    json_value_t **out_diagnostics_json);

/**
 * @brief Start one remote graph run and update cached session ids.
 */
CXX_C_API int turbo_agent_remote_session_start_json_value_graph(
    turbo_agent_remote_session_t *session, const char *graph_name,
    const json_value_t *state, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state);

/**
 * @brief Start one remote graph run from one optional user text message.
 */
CXX_C_API int turbo_agent_remote_session_start_text(
    turbo_agent_remote_session_t *session, const char *graph_name, const char *user_text,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state);

/**
 * @brief Start one remote graph run from canonical prompt messages.
 */
CXX_C_API int turbo_agent_remote_session_start_messages(
    turbo_agent_remote_session_t *session, const char *graph_name,
    const json_value_t *messages, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state);

/**
 * @brief Resume one remote graph run and update cached session ids.
 *
 * When `checkpoint_id` is NULL or empty, the session falls back to its cached
 * checkpoint id.
 */
CXX_C_API int turbo_agent_remote_session_resume_json_value_graph(
    turbo_agent_remote_session_t *session, const char *graph_name, const char *checkpoint_id,
    const json_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state);

/**
 * @brief Fork one remote graph run and update cached session ids.
 *
 * When `checkpoint_id` is NULL or empty, the session falls back to its cached
 * checkpoint id.
 */
CXX_C_API int turbo_agent_remote_session_fork_json_value_graph(
    turbo_agent_remote_session_t *session, const char *graph_name, const char *checkpoint_id,
    const json_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state);

/**
 * @brief Start one remote graph run from user text and return the final answer text.
 *
 * Returned text is allocated and owned by caller.
 */
CXX_C_API int turbo_agent_remote_session_invoke_text(
    turbo_agent_remote_session_t *session, const char *graph_name, const char *user_text,
    const turbo_graph_run_options_t *options, char **out_text, json_value_t **out_summary_json);

/**
 * @brief Start one remote graph run from canonical prompt messages and return the final answer text.
 *
 * Returned text is allocated and owned by caller.
 */
CXX_C_API int turbo_agent_remote_session_invoke_messages_text(
    turbo_agent_remote_session_t *session, const char *graph_name,
    const json_value_t *messages, const turbo_graph_run_options_t *options,
    char **out_text, json_value_t **out_summary_json);

/**
 * @brief Start one remote graph run from user text and parse the final JSON output.
 */
CXX_C_API int turbo_agent_remote_session_invoke_json(
    turbo_agent_remote_session_t *session, const char *graph_name, const char *user_text,
    const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json);

/**
 * @brief Start one remote graph run from canonical prompt messages and parse the final JSON output.
 */
CXX_C_API int turbo_agent_remote_session_invoke_messages_json(
    turbo_agent_remote_session_t *session, const char *graph_name,
    const json_value_t *messages, const turbo_graph_run_options_t *options,
    json_value_t **out_json, json_value_t **out_summary_json);

/**
 * @brief Load the session thread's latest persisted state through the remote runtime.
 */
CXX_C_API int turbo_agent_remote_session_get_thread_state_json_value(
    turbo_agent_remote_session_t *session, json_value_t **out_state);

/**
 * @brief Delete one canonical memory record through the remote runtime.
 */
CXX_C_API int turbo_agent_remote_session_memory_delete_record(
    const turbo_agent_remote_session_t *session, const char *memory_namespace, const char *key);

/**
 * @brief List long-term memory records by namespace prefix through the remote runtime.
 */
CXX_C_API int turbo_agent_remote_session_memory_list_records(
    const turbo_agent_remote_session_t *session, const char *namespace_prefix,
    json_value_t **out_records_json);

/**
 * @brief Load one canonical memory record through the remote runtime.
 */
CXX_C_API int turbo_agent_remote_session_memory_get_record(
    const turbo_agent_remote_session_t *session, const char *memory_namespace, const char *key,
    json_value_t **out_record_json);

/**
 * @brief Persist one canonical memory record through the remote runtime.
 */
CXX_C_API int turbo_agent_remote_session_memory_put_record(
    const turbo_agent_remote_session_t *session, const json_value_t *record_json);

/**
 * @brief Validate one canonical memory record through the remote runtime.
 */
CXX_C_API int turbo_agent_remote_session_memory_validate_record(
    const turbo_agent_remote_session_t *session, const json_value_t *record_json, int *out_valid);

/**
 * @brief Query long-term memory records through the remote runtime.
 */
CXX_C_API int turbo_agent_remote_session_memory_query_records(
    const turbo_agent_remote_session_t *session, const char *namespace_prefix, const char *kind,
    const char *key_prefix, const char *text_substring, json_value_t **out_records_json);

/**
 * @brief Query long-term memory records through the remote runtime with explicit options.
 */
CXX_C_API int turbo_agent_remote_session_memory_query_records_ex(
    const turbo_agent_remote_session_t *session,
    const turbo_agent_memory_query_options_t *options, json_value_t **out_records_json);

/**
 * @brief Load the current thread record through the remote observability bundle.
 */
CXX_C_API int turbo_agent_remote_session_get_thread(turbo_agent_remote_session_t *session,
                                                    json_value_t **out_thread_json);

/**
 * @brief Load the newest run record for the session thread through the remote
 * observability bundle.
 */
CXX_C_API int turbo_agent_remote_session_get_latest_run(turbo_agent_remote_session_t *session,
                                                        json_value_t **out_run_json);

/**
 * @brief Load the newest interrupted run record for the session thread through
 * the remote observability bundle.
 */
CXX_C_API int turbo_agent_remote_session_get_pending_run(turbo_agent_remote_session_t *session,
                                                         json_value_t **out_run_json);

/**
 * @brief Load one checkpoint context from the remote runtime.
 *
 * When `checkpoint_id` is NULL or empty, the session falls back to its cached
 * checkpoint id.
 */
CXX_C_API int turbo_agent_remote_session_get_checkpoint_context(
    turbo_agent_remote_session_t *session, const char *checkpoint_id,
    json_value_t **out_context_json);

/**
 * @brief Load one thread-scoped observability bundle from the remote runtime.
 */
CXX_C_API int turbo_agent_remote_session_get_observability_index(
    turbo_agent_remote_session_t *session, json_value_t **out_index_json);

/**
 * @brief Load one TurboParser JSON-native thread timeline snapshot from the remote runtime.
 */
CXX_C_API int turbo_agent_remote_session_get_thread_timeline_json_value(
    turbo_agent_remote_session_t *session, json_value_t **out_timeline);

/**
 * @brief Load one TurboParser JSON-native history event array for the current remote thread.
 *
 * This derives `history_events` from the existing thread timeline snapshot and
 * returns one owned clone, so callers do not borrow timeline internals.
 */
CXX_C_API int turbo_agent_remote_session_load_thread_history_events_json_value(
    turbo_agent_remote_session_t *session, json_value_t **out_events);

/**
 * @brief Replay the current remote thread's durable history events through one sink.
 */
CXX_C_API int turbo_agent_remote_session_replay_thread_history_json_value(
    turbo_agent_remote_session_t *session, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data);

/**
 * @brief Observe the current remote thread's durable history through the host-facing observer bridge.
 *
 * This reuses the existing remote observability facts and emits the same
 * observer event shape as the local runtime/session wrappers.
 */
CXX_C_API int turbo_agent_remote_session_observe_thread_history_json_value(
    turbo_agent_remote_session_t *session, const turbo_agent_observer_json_value_sink_t *sink);

/**
 * @brief Load one TurboParser JSON-native trace event array for the current remote thread.
 *
 * This derives `trace_events` from the existing observability bundle and
 * returns one owned bind tree.
 */
CXX_C_API int turbo_agent_remote_session_get_thread_trace_events_json_value(
    turbo_agent_remote_session_t *session, json_value_t **out_events);

/**
 * @brief Load one branch tree snapshot from the remote runtime.
 */
CXX_C_API int turbo_agent_remote_session_get_branch_tree(
    turbo_agent_remote_session_t *session, json_value_t **out_branch_tree_json);

/**
 * @brief Load one thread lineage snapshot from the remote observability bundle.
 */
CXX_C_API int turbo_agent_remote_session_list_thread_lineage(
    turbo_agent_remote_session_t *session, json_value_t **out_lineage_json);

/**
 * @brief Load the current supervisor inbox array from the remote thread state.
 */
CXX_C_API int turbo_agent_remote_session_get_supervisor_inbox(
    turbo_agent_remote_session_t *session, json_value_t **out_inbox_json);

/**
 * @brief Load the current supervisor handoff history array from the remote thread state.
 */
CXX_C_API int turbo_agent_remote_session_get_supervisor_handoff_history(
    turbo_agent_remote_session_t *session, json_value_t **out_history_json);

/**
 * @brief Build one supervisor inspect snapshot from the remote thread state.
 */
CXX_C_API int turbo_agent_remote_session_get_supervisor_inspect(
    turbo_agent_remote_session_t *session, json_value_t **out_inspect_json);

/**
 * @brief Load child runs for one parent run through the remote runtime.
 *
 * When `parent_agent_run_id` is NULL or empty, the session falls back to its
 * cached `last_run_id`.
 */
CXX_C_API int turbo_agent_remote_session_list_child_runs(
    turbo_agent_remote_session_t *session, const char *parent_agent_run_id,
    json_value_t **out_runs_json);

/**
 * @brief Build one orchestration inspect bundle for the current remote thread.
 */
CXX_C_API int turbo_agent_remote_session_get_orchestration_inspect(
    turbo_agent_remote_session_t *session, json_value_t **out_inspect_json);

/**
 * @brief Load one child run record referenced by a parent tool-result output item.
 */
CXX_C_API int turbo_agent_remote_session_get_child_run(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_run_json);

/**
 * @brief Load one child checkpoint record referenced by a parent tool-result output item.
 */
CXX_C_API int turbo_agent_remote_session_get_child_checkpoint(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_checkpoint_json);

/**
 * @brief Load one child checkpoint inspect context referenced by a parent tool-result item.
 */
CXX_C_API int turbo_agent_remote_session_get_child_checkpoint_context(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_context_json);

/**
 * @brief Load one child thread timeline referenced by a parent tool-result item.
 */
CXX_C_API int turbo_agent_remote_session_get_child_thread_timeline_json_value(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_timeline);

/**
 * @brief Load one child branch tree referenced by a parent tool-result item.
 */
CXX_C_API int turbo_agent_remote_session_get_child_branch_tree(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_branch_tree_json);

/**
 * @brief List checkpoints for one child run referenced by a parent tool-result item.
 */
CXX_C_API int turbo_agent_remote_session_list_child_checkpoints(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_checkpoints_json);

/**
 * @brief Load child durable history referenced by a parent tool-result item.
 *
 * Prefers `child_checkpoint_id` when present, else falls back to `child_run_id`.
 */
CXX_C_API int turbo_agent_remote_session_load_child_history_events_json_value(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_events);

/**
 * @brief Load child trace events referenced by a parent tool-result item.
 *
 * Prefers `child_checkpoint_id` when present, else falls back to `child_run_id`.
 */
CXX_C_API int turbo_agent_remote_session_get_child_trace_events_json_value(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_events);

/**
 * @brief Build one child inspect bundle from a parent tool-result output item.
 */
CXX_C_API int turbo_agent_remote_session_get_child_inspect(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_inspect_json);

/**
 * @brief Build one child orchestration inspect bundle from a parent tool-result item.
 */
CXX_C_API int turbo_agent_remote_session_get_child_orchestration_inspect(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_inspect_json);

/**
 * @brief Build one combined multi-agent inspect bundle with nested child inspect.
 */
CXX_C_API int turbo_agent_remote_session_get_child_multi_agent_inspect(
    turbo_agent_remote_session_t *session, const json_value_t *output_item,
    json_value_t **out_inspect_json);

/**
 * @brief Apply one command to the session thread and resume the configured remote graph.
 */
CXX_C_API int turbo_agent_remote_session_resume_thread_command_json_value(
    turbo_agent_remote_session_t *session, const char *graph_name,
    const json_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state);

/**
 * @brief Apply one command to the session thread and fork the configured remote graph.
 */
CXX_C_API int turbo_agent_remote_session_fork_thread_command_json_value(
    turbo_agent_remote_session_t *session, const char *graph_name,
    const json_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state);

#ifdef __cplusplus
}
#endif

#endif
