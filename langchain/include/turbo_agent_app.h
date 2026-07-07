#ifndef TURBO_AGENT_APP_H
#define TURBO_AGENT_APP_H

#include "turbo_agent_session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_app_s turbo_agent_app_t;

typedef struct turbo_agent_app_config_s {
  const turbo_agent_session_config_t *session_config;
} turbo_agent_app_config_t;

CXX_C_API turbo_agent_app_t *turbo_agent_app_create(const turbo_agent_app_config_t *config);
CXX_C_API void turbo_agent_app_destroy(turbo_agent_app_t *app);

CXX_C_API turbo_agent_session_t *turbo_agent_app_session(const turbo_agent_app_t *app);

CXX_C_API const char *turbo_agent_app_thread_id(const turbo_agent_app_t *app);

CXX_C_API const char *turbo_agent_app_last_run_id(const turbo_agent_app_t *app);

CXX_C_API const char *turbo_agent_app_last_checkpoint_id(const turbo_agent_app_t *app);

CXX_C_API turbo_agent_session_workflow_kind_t
turbo_agent_app_workflow_kind(const turbo_agent_app_t *app);

CXX_C_API const char *turbo_agent_app_memory_namespace(const turbo_agent_app_t *app);

CXX_C_API const turbo_agent_memory_store_t *
turbo_agent_app_memory_store(const turbo_agent_app_t *app);

/**
 * @brief Return the tool registry visible to the app's owned agent.
 *
 * The returned pointer is borrowed. It may be NULL when the app session is
 * runtime-only or the owned agent has no tools.
 */
CXX_C_API const turbo_tool_registry_t *
turbo_agent_app_tool_registry(const turbo_agent_app_t *app);

/** @brief Return the number of tools visible to the app's owned agent. */
CXX_C_API size_t turbo_agent_app_tool_count(const turbo_agent_app_t *app);

/**
 * @brief Return a host-facing snapshot of the app session's configured harness capabilities.
 *
 * The returned JSON object is owned by the caller.
 */
CXX_C_API int turbo_agent_app_get_capabilities(
    const turbo_agent_app_t *app, json_value_t **out_capabilities_json);

/**
 * @brief Return model-provider tool schema views for the app session's tool registry.
 *
 * The returned JSON object is owned by the caller.
 */
CXX_C_API int turbo_agent_app_get_tool_schemas(
    const turbo_agent_app_t *app, json_value_t **out_tool_schemas_json);

/**
 * @brief Return one startup health snapshot for host-side app harness validation.
 *
 * The returned JSON object is owned by the caller.
 */
CXX_C_API int turbo_agent_app_get_startup_diagnostics(
    const turbo_agent_app_t *app, json_value_t **out_diagnostics_json);

CXX_C_API int turbo_agent_app_add_trace_bind_sink(
    turbo_agent_app_t *app, const turbo_agent_trace_bind_sink_t *sink);

CXX_C_API int turbo_agent_app_add_observer_bind_sink(
    turbo_agent_app_t *app, const turbo_agent_observer_bind_sink_t *sink);

CXX_C_API int turbo_agent_app_set_trace_history_enabled(turbo_agent_app_t *app, int enabled);

CXX_C_API int turbo_agent_app_get_thread(turbo_agent_app_t *app, json_value_t **out_thread_json);

CXX_C_API int turbo_agent_app_get_run(turbo_agent_app_t *app, const char *run_id,
                                      json_value_t **out_run_json);

CXX_C_API int turbo_agent_app_get_latest_run(turbo_agent_app_t *app, json_value_t **out_run_json);

CXX_C_API int turbo_agent_app_get_pending_run(turbo_agent_app_t *app, json_value_t **out_run_json);

CXX_C_API int turbo_agent_app_get_checkpoint(turbo_agent_app_t *app, const char *checkpoint_id,
                                             json_value_t **out_checkpoint_json);

CXX_C_API int turbo_agent_app_get_latest_checkpoint(turbo_agent_app_t *app, const char *run_id,
                                                    json_value_t **out_checkpoint_json);

/**
 * @brief Load the app thread's latest persisted state through the owned session runtime.
 *
 * This legacy surface resolves the newest run on the thread by `updated_at`.
 * It does not prefer the thread head / pending checkpoint. New hosts that need
 * thread-head semantics should prefer `get_thread_head_state_bind(...)`.
 */
CXX_C_API int turbo_agent_app_get_thread_state_bind(
    turbo_agent_app_t *app, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Load the app thread head state snapshot through the owned session runtime.
 *
 * The runtime resolves the newest interrupted run first. When the thread has
 * no interrupted run, it falls back to the newest run by `updated_at`.
 */
CXX_C_API int turbo_agent_app_get_thread_head_state_bind(
    turbo_agent_app_t *app, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Compatibility accessor for the app thread's latest-run trace events.
 *
 * This legacy surface resolves the newest run on the thread by `updated_at`.
 * It does not prefer the thread head / pending checkpoint. New hosts that need
 * thread-head semantics should prefer `get_thread_head_trace_events_bind(...)`.
 */
CXX_C_API int turbo_agent_app_get_thread_trace_events_bind(
    turbo_agent_app_t *app, turbo_runtime_data_bind_value_t **out_events);

/**
 * @brief Load the app thread head trace-event snapshot through the owned session runtime.
 *
 * The runtime resolves the newest interrupted run first. When the thread has
 * no interrupted run, it falls back to the newest run by `updated_at`.
 */
CXX_C_API int turbo_agent_app_get_thread_head_trace_events_bind(
    turbo_agent_app_t *app, turbo_runtime_data_bind_value_t **out_events);

CXX_C_API int turbo_agent_app_get_run_state_bind(
    turbo_agent_app_t *app, const char *run_id,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_get_run_trace_events_bind(
    turbo_agent_app_t *app, const char *run_id,
    turbo_runtime_data_bind_value_t **out_events);

CXX_C_API int turbo_agent_app_get_checkpoint_state_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Prepare one checkpoint-scoped state override from a bind-native patch.
 *
 * The runtime loads the checkpoint state, recursively merges object fields from
 * `state_patch`, and returns the resulting full state as `out_state_override`.
 * Arrays, scalars, and null replace the target value. This helper does not
 * persist the prepared override back into the runtime.
 */
CXX_C_API int turbo_agent_app_prepare_checkpoint_state_override_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);

CXX_C_API int turbo_agent_app_apply_checkpoint_state_patch_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);

/**
 * @brief Prepare one thread-head state override from a bind-native patch.
 *
 * The runtime resolves the thread's newest interrupted run first. If no
 * interrupted run exists, it falls back to the newest run by `updated_at`,
 * then applies the patch to that run's latest checkpoint state. This helper
 * only prepares the full override value and does not persist it.
 */
CXX_C_API int turbo_agent_app_prepare_thread_state_override_bind(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);

CXX_C_API int turbo_agent_app_apply_thread_state_patch_bind(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);

CXX_C_API int turbo_agent_app_get_supervisor_inbox(
    turbo_agent_app_t *app, json_value_t **out_inbox_json);

CXX_C_API int turbo_agent_app_get_supervisor_handoff_history(
    turbo_agent_app_t *app, json_value_t **out_history_json);

CXX_C_API int turbo_agent_app_get_supervisor_inspect(
    turbo_agent_app_t *app, json_value_t **out_inspect_json);

CXX_C_API int turbo_agent_app_get_orchestration_inspect(
    turbo_agent_app_t *app, json_value_t **out_inspect_json);

/**
 * @brief Load one thread-scoped observability index for the app's current thread.
 *
 * The returned JSON bundle is a read-only aggregate over the current thread's
 * runtime inspect surfaces and derived fields such as `current_status`,
 * `current_interrupt_reason`, `current_pending_action`,
 * `current_checkpoint_summary`, `latest_run_status`, `latest_run_updated_at`,
 * `pending_run_id`, `pending_checkpoint_id`, `has_failure`, `has_model_error`,
 * `has_guardrail_rejection`, `replan_requested`, `current_failure_reason`,
 * `current_review_note`, `has_pending_review`, `has_handoff`,
 * `active_agent`, and `counts`.
 */
CXX_C_API int turbo_agent_app_get_observability_index(
    turbo_agent_app_t *app, json_value_t **out_index_json);

CXX_C_API int turbo_agent_app_append_supervisor_inbox_message_bind(
    turbo_agent_app_t *app, const char *source_agent, const char *text,
    turbo_runtime_data_bind_value_t **out_state_override);

CXX_C_API int turbo_agent_app_get_checkpoint_trace_events_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_events);

CXX_C_API int turbo_agent_app_get_child_run(turbo_agent_app_t *app,
                                            const json_value_t *output_item,
                                            json_value_t **out_run_json);

CXX_C_API int turbo_agent_app_get_child_checkpoint(turbo_agent_app_t *app,
                                                   const json_value_t *output_item,
                                                   json_value_t **out_checkpoint_json);

CXX_C_API int turbo_agent_app_get_child_checkpoint_context(turbo_agent_app_t *app,
                                                           const json_value_t *output_item,
                                                           json_value_t **out_context_json);

CXX_C_API int turbo_agent_app_get_child_thread_timeline_bind(
    turbo_agent_app_t *app, const json_value_t *output_item,
    turbo_runtime_data_bind_value_t **out_timeline);

CXX_C_API int turbo_agent_app_get_child_branch_tree(
    turbo_agent_app_t *app, const json_value_t *output_item,
    json_value_t **out_branch_tree_json);

CXX_C_API int turbo_agent_app_get_child_inspect(turbo_agent_app_t *app,
                                                const json_value_t *output_item,
                                                json_value_t **out_inspect_json);

CXX_C_API int turbo_agent_app_get_child_orchestration_inspect(
    turbo_agent_app_t *app, const json_value_t *output_item,
    json_value_t **out_inspect_json);

CXX_C_API int turbo_agent_app_get_child_multi_agent_inspect(
    turbo_agent_app_t *app, const json_value_t *output_item,
    json_value_t **out_inspect_json);

CXX_C_API int turbo_agent_app_list_runs(turbo_agent_app_t *app, json_value_t **out_runs_json);

CXX_C_API int turbo_agent_app_list_child_runs(turbo_agent_app_t *app,
                                              const char *parent_agent_run_id,
                                              json_value_t **out_runs_json);

CXX_C_API int turbo_agent_app_list_child_checkpoints(turbo_agent_app_t *app,
                                                     const json_value_t *output_item,
                                                     json_value_t **out_checkpoints_json);

CXX_C_API int turbo_agent_app_list_checkpoints(turbo_agent_app_t *app, const char *run_id,
                                               json_value_t **out_checkpoints_json);

CXX_C_API int turbo_agent_app_list_thread_lineage(turbo_agent_app_t *app,
                                                  json_value_t **out_lineage_json);

CXX_C_API int turbo_agent_app_get_branch_tree(turbo_agent_app_t *app,
                                              json_value_t **out_branch_tree_json);

CXX_C_API int turbo_agent_app_get_checkpoint_context(turbo_agent_app_t *app,
                                                     const char *checkpoint_id,
                                                     json_value_t **out_context_json);

CXX_C_API int turbo_agent_app_load_history_events_bind(
    turbo_agent_app_t *app, const char *run_id, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_events);

CXX_C_API int turbo_agent_app_load_thread_history_events_bind(
    turbo_agent_app_t *app, turbo_runtime_data_bind_value_t **out_events);

CXX_C_API int turbo_agent_app_replay_history_bind(
    turbo_agent_app_t *app, const char *run_id, const char *checkpoint_id,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data);

CXX_C_API int turbo_agent_app_replay_thread_history_bind(
    turbo_agent_app_t *app, turbo_event_sink_bind_fn event_sink, void *event_sink_user_data);

CXX_C_API int turbo_agent_app_observe_history_bind(
    turbo_agent_app_t *app, const char *run_id, const char *checkpoint_id,
    const turbo_agent_observer_bind_sink_t *sink);

CXX_C_API int turbo_agent_app_observe_thread_history_bind(
    turbo_agent_app_t *app, const turbo_agent_observer_bind_sink_t *sink);

/* ── Core execution ────────────────────────────────────────────────────────── */

typedef turbo_agent_session_exec_options_t turbo_agent_app_exec_options_t;

/**
 * @brief Start one new app-backed graph run.
 *
 * @param event_sink Optional host-facing stream sink.
 * @param event_sink_user_data Opaque cookie for the sink.
 */
CXX_C_API int turbo_agent_app_exec_start(
    turbo_agent_app_t *app, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state, const turbo_graph_run_options_t *options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Resume one app-backed checkpointed graph run.
 *
 * `exec_options` controls scope (checkpoint / thread), input semantics
 * (override / patch / command), and optional explicit checkpoint id.
 */
CXX_C_API int turbo_agent_app_exec_resume(
    turbo_agent_app_t *app, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_app_exec_options_t *exec_options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Fork one app-backed checkpointed graph run.
 */
CXX_C_API int turbo_agent_app_exec_fork(
    turbo_agent_app_t *app, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_app_exec_options_t *exec_options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/* ── Preset execution ──────────────────────────────────────────────────────── */

/** @brief Start one canned workflow graph by kind. */
CXX_C_API int turbo_agent_app_start_preset(
    turbo_agent_app_t *app, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *state, const turbo_graph_run_options_t *options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/** @brief Resume one canned workflow graph from a checkpoint. */
CXX_C_API int turbo_agent_app_resume_preset(
    turbo_agent_app_t *app, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_app_exec_options_t *exec_options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/** @brief Fork one canned workflow graph from a checkpoint. */
CXX_C_API int turbo_agent_app_fork_preset(
    turbo_agent_app_t *app, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_app_exec_options_t *exec_options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/* ── High-level convenience entry points ───────────────────────────────────── */

/** @brief Start the app's default workflow from one user message. */
CXX_C_API int turbo_agent_app_start_text(turbo_agent_app_t *app, const char *user_text,
                                         const turbo_graph_run_options_t *options,
                                         json_value_t **out_summary_json,
                                         turbo_runtime_data_bind_value_t **out_state);

/** @brief Start the app's default workflow from one user message and emit one stream mode. */
CXX_C_API int turbo_agent_app_start_text_stream(
    turbo_agent_app_t *app, const char *user_text,
    const turbo_graph_run_options_t *options, turbo_event_stream_mode_t stream_mode,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/** @brief Start the app's default workflow from canonical prompt messages. */
CXX_C_API int turbo_agent_app_start_messages(turbo_agent_app_t *app,
                                             const turbo_runtime_data_bind_value_t *messages,
                                             const turbo_graph_run_options_t *options,
                                             json_value_t **out_summary_json,
                                             turbo_runtime_data_bind_value_t **out_state);

/** @brief Start the app's default workflow from canonical messages and emit one stream mode. */
CXX_C_API int turbo_agent_app_start_messages_stream(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *messages,
    const turbo_graph_run_options_t *options, turbo_event_stream_mode_t stream_mode,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Extract one user-facing final answer text from a bind-native result state.
 *
 * Returned text is allocated and owned by caller.
 */
CXX_C_API char *turbo_agent_app_result_text(const turbo_runtime_data_bind_value_t *state);

/** @brief Invoke the app's default workflow and return the final answer text.
 *
 * `out_summary_json` may be NULL when the caller does not need the runtime summary.
 */
CXX_C_API int turbo_agent_app_invoke_text(turbo_agent_app_t *app, const char *user_text,
                                          const turbo_graph_run_options_t *options,
                                          char **out_text, json_value_t **out_summary_json);

/** @brief Invoke the app's default workflow and return the final answer text.
 *
 * `out_summary_json` may be NULL when the caller does not need the runtime summary.
 */
CXX_C_API int turbo_agent_app_invoke_messages_text(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *messages,
    const turbo_graph_run_options_t *options, char **out_text, json_value_t **out_summary_json);

/** @brief Invoke the app's default workflow and parse the final answer as JSON.
 *
 * `out_summary_json` may be NULL when the caller does not need the runtime summary.
 */
CXX_C_API int turbo_agent_app_invoke_json(turbo_agent_app_t *app, const char *user_text,
                                          const turbo_graph_run_options_t *options,
                                          json_value_t **out_json,
                                          json_value_t **out_summary_json);

/** @brief Invoke the app's default workflow and parse the final answer as JSON.
 *
 * `out_summary_json` may be NULL when the caller does not need the runtime summary.
 */
CXX_C_API int turbo_agent_app_invoke_messages_json(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *messages,
    const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json);

/** @brief Invoke the app's default workflow for a batch of user messages. */
CXX_C_API int turbo_agent_app_batch_text(
    turbo_agent_app_t *app, const char *const *user_texts, size_t count,
    const turbo_graph_run_options_t *options, json_value_t **out_results_json);

CXX_C_API int turbo_agent_app_memory_get(const turbo_agent_app_t *app, const char *memory_namespace,
                                         const char *key, char **out_value_json);

CXX_C_API int turbo_agent_app_memory_put(const turbo_agent_app_t *app, const char *memory_namespace,
                                         const char *key, const char *value_json);

CXX_C_API int turbo_agent_app_memory_put_context(const turbo_agent_app_t *app,
                                                 const char *memory_namespace, const char *key,
                                                 const char *scope, const char *path,
                                                 const char *text);

CXX_C_API int turbo_agent_app_memory_delete(const turbo_agent_app_t *app,
                                            const char *memory_namespace, const char *key);

CXX_C_API int turbo_agent_app_memory_list(const turbo_agent_app_t *app,
                                          const char *namespace_prefix,
                                          json_value_t **out_records_json);

CXX_C_API int turbo_agent_app_memory_list_records(const turbo_agent_app_t *app,
                                                  const char *namespace_prefix,
                                                  json_value_t **out_records_json);

CXX_C_API int turbo_agent_app_memory_get_record(const turbo_agent_app_t *app,
                                                const char *memory_namespace, const char *key,
                                                json_value_t **out_record_json);

CXX_C_API int turbo_agent_app_memory_put_record(const turbo_agent_app_t *app,
                                                const json_value_t *record_json);

CXX_C_API int turbo_agent_app_memory_validate_record(const json_value_t *record_json);

CXX_C_API int turbo_agent_app_memory_query_records(const turbo_agent_app_t *app,
                                                   const char *namespace_prefix,
                                                   const char *kind,
                                                   const char *key_prefix,
                                                   const char *text_substring,
                                                   json_value_t **out_records_json);

CXX_C_API int turbo_agent_app_memory_query_records_ex(
    const turbo_agent_app_t *app, const turbo_agent_memory_query_options_t *options,
    json_value_t **out_records_json);

#ifdef __cplusplus
}
#endif

#endif
