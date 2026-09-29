#ifndef TURBO_AGENT_RUNTIME_H
#define TURBO_AGENT_RUNTIME_H

#include <turbo_agent_api.h>

#include "turbo_event.h"
#include "turbo_graph.h"
#include "turbo_runtime_json.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_runtime_s turbo_agent_runtime_t;

typedef int (*turbo_agent_runtime_store_put_fn)(void *user_data, const char *collection,
                                                const char *id, const char *record_json);
typedef int (*turbo_agent_runtime_store_get_fn)(void *user_data, const char *collection,
                                                const char *id, char **out_record_json);
typedef int (*turbo_agent_runtime_store_list_fn)(void *user_data, const char *collection,
                                                 const char *filter_key,
                                                 const char *filter_value,
                                                 char **out_records_json);
typedef void (*turbo_agent_runtime_store_user_data_free_fn)(void *user_data);

typedef struct turbo_agent_runtime_store_s {
  turbo_agent_runtime_store_put_fn put;
  turbo_agent_runtime_store_get_fn get;
  turbo_agent_runtime_store_list_fn list;
  void *user_data;
  turbo_agent_runtime_store_user_data_free_fn user_data_free;
} turbo_agent_runtime_store_t;

typedef struct turbo_agent_runtime_parent_link_s {
  const char *parent_agent_run_id;
  const char *parent_tool_call_id;
  const char *parent_tool_name;
  const char *parent_graph_run_id;
  const char *call_frame_id;
} turbo_agent_runtime_parent_link_t;

typedef void (*turbo_agent_observer_json_value_fn)(
    const json_value_t *event, void *user_data);
typedef void (*turbo_agent_observer_user_data_free_fn)(void *user_data);

typedef struct turbo_agent_observer_json_value_sink_s {
  turbo_agent_observer_json_value_fn callback;
  void *user_data;
  turbo_agent_observer_user_data_free_fn user_data_free;
} turbo_agent_observer_json_value_sink_t;
 
typedef enum turbo_agent_runtime_scope_e {
  TURBO_RUNTIME_SCOPE_CHECKPOINT = 0,
  TURBO_RUNTIME_SCOPE_THREAD = 1
} turbo_agent_runtime_scope_t;
 
typedef enum turbo_agent_runtime_input_kind_e {
  TURBO_RUNTIME_INPUT_OVERRIDE = 0,
  TURBO_RUNTIME_INPUT_PATCH = 1,
  TURBO_RUNTIME_INPUT_COMMAND = 2
} turbo_agent_runtime_input_kind_t;
 
typedef struct turbo_agent_runtime_exec_options_s {
  turbo_agent_runtime_scope_t scope;
  turbo_agent_runtime_input_kind_t input_kind;
  const char *checkpoint_id;
  const char *thread_id;
  turbo_event_sink_json_value_fn event_sink;
  void *event_sink_user_data;
  const turbo_agent_runtime_parent_link_t *parent_link;
} turbo_agent_runtime_exec_options_t;
 
/* ── Unified Execution ─────────────────────────────────────────────────────── */
 
CXX_C_API int turbo_agent_runtime_exec_start(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph,
    const json_value_t *state, const turbo_graph_run_options_t *options,
    const turbo_agent_runtime_exec_options_t *exec_options,
    json_value_t **out_summary_json, json_value_t **out_state);
 
CXX_C_API int turbo_agent_runtime_exec_resume(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph,
    const json_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_runtime_exec_options_t *exec_options,
    json_value_t **out_summary_json, json_value_t **out_state);
 
CXX_C_API int turbo_agent_runtime_exec_fork(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph,
    const json_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_runtime_exec_options_t *exec_options,
    json_value_t **out_summary_json, json_value_t **out_state);

/**
 * @brief Controlled variants of unified execution.
 *
 * The token is borrowed for the synchronous call. Existing exec_start/resume/
 * fork APIs are equivalent to passing NULL. Cancellation and deadlines return
 * success at the API layer with a terminal `cancelled`/`timed_out` summary and
 * a resumable checkpoint when the graph reached a safe boundary.
 */
CXX_C_API int turbo_agent_runtime_exec_start_controlled(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph,
    const json_value_t *state, const turbo_graph_run_options_t *options,
    const turbo_agent_runtime_exec_options_t *exec_options,
    const turbo_cancel_token_t *cancel_token,
    json_value_t **out_summary_json, json_value_t **out_state);

CXX_C_API int turbo_agent_runtime_exec_resume_controlled(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph,
    const json_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_runtime_exec_options_t *exec_options,
    const turbo_cancel_token_t *cancel_token,
    json_value_t **out_summary_json, json_value_t **out_state);

CXX_C_API int turbo_agent_runtime_exec_fork_controlled(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph,
    const json_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_runtime_exec_options_t *exec_options,
    const turbo_cancel_token_t *cancel_token,
    json_value_t **out_summary_json, json_value_t **out_state);

/**
 * @brief Return whether the currently executing tool call matched an approved review note.
 *
 * This is only set while workflow tool dispatch is invoking a tool. Direct
 * tool runtime calls return zero.
 */
CXX_C_API int turbo_agent_current_tool_approval_granted(void);


/**
 * @brief Create one durable runtime bound to a record store.
 *
 * The runtime copies `store` by value. The callbacks and `user_data` must stay
 * valid until `turbo_agent_runtime_destroy(...)` is called.
 *
 * @param store Runtime store callbacks.
 * @return Runtime handle or NULL on failure.
 */
CXX_C_API turbo_agent_runtime_t *
turbo_agent_runtime_create(const turbo_agent_runtime_store_t *store);

/**
 * @brief Destroy one durable runtime.
 * @param runtime Runtime handle, may be NULL.
 */
CXX_C_API void turbo_agent_runtime_destroy(turbo_agent_runtime_t *runtime);

/**
 * @brief Create one heap-backed in-memory runtime store.
 * @return Store callbacks with owned user_data, or a zeroed store on failure.
 */
CXX_C_API turbo_agent_runtime_store_t turbo_agent_runtime_store_memory_create(void);

/**
 * @brief Create one file-backed runtime store rooted at `root_dir`.
 *
 * The store writes:
 * - `root_dir/threads/<id>.json`
 * - `root_dir/runs/<id>.json`
 * - `root_dir/checkpoints/<id>.json`
 *
 * @param root_dir Root directory. Created on demand when missing.
 * @return Store callbacks with owned user_data, or a zeroed store on failure.
 */
CXX_C_API turbo_agent_runtime_store_t
turbo_agent_runtime_store_file_create(const char *root_dir);



/**
 * @brief Fetch one persisted thread record as JSON.
 * @param runtime Runtime handle.
 * @param thread_id Thread id.
 * @param out_thread_json Output JSON object owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_get_thread(turbo_agent_runtime_t *runtime,
                                             const char *thread_id,
                                             json_value_t **out_thread_json);

/**
 * @brief Fetch one persisted run record as JSON.
 * @param runtime Runtime handle.
 * @param run_id Run id.
 * @param out_run_json Output JSON object owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_get_run(turbo_agent_runtime_t *runtime, const char *run_id,
                                          json_value_t **out_run_json);

/**
 * @brief Fetch the newest persisted run record for one thread as JSON.
 * @param runtime Runtime handle.
 * @param thread_id Thread id.
 * @param out_run_json Output JSON object owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_get_latest_run(turbo_agent_runtime_t *runtime,
                                                 const char *thread_id,
                                                 json_value_t **out_run_json);

/**
 * @brief Fetch the newest interrupted run record for one thread as JSON.
 *
 * This is the narrow host-facing "pending run" query: only interrupted runs
 * are considered resumable/pending.
 *
 * @param runtime Runtime handle.
 * @param thread_id Thread id.
 * @param out_run_json Output JSON object owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_get_pending_run(turbo_agent_runtime_t *runtime,
                                                  const char *thread_id,
                                                  json_value_t **out_run_json);

/**
 * @brief Fetch one persisted checkpoint record as JSON.
 * @param runtime Runtime handle.
 * @param checkpoint_id Checkpoint id.
 * @param out_checkpoint_json Output JSON object owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_get_checkpoint(turbo_agent_runtime_t *runtime,
                                                 const char *checkpoint_id,
                                                 json_value_t **out_checkpoint_json);

/**
 * @brief Fetch the latest persisted checkpoint record for one run as JSON.
 *
 * This resolves the run record's `latest_checkpoint_id`. Completed runs may not
 * have one.
 *
 * @param runtime Runtime handle.
 * @param run_id Run id.
 * @param out_checkpoint_json Output JSON object owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_get_latest_checkpoint(turbo_agent_runtime_t *runtime,
                                                        const char *run_id,
                                                        json_value_t **out_checkpoint_json);

/**
 * @brief Load one checkpoint's serialized state as a TurboParser JSON-native object.
 * @param runtime Runtime handle.
 * @param checkpoint_id Checkpoint id.
 * @param out_state Output TurboParser JSON-native state owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_get_checkpoint_state_json_value(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id,
    json_value_t **out_state);

CXX_C_API int turbo_agent_runtime_get_checkpoint_trace_events_json_value(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id,
    json_value_t **out_events);

/**
 * @brief Load one run's latest persisted state as a TurboParser JSON-native object.
 *
 * Newer runtime records persist `state_snapshot` directly on the run record.
 * Older interrupted runs fall back to `latest_checkpoint_id`.
 *
 * @param runtime Runtime handle.
 * @param run_id Run id.
 * @param out_state Output TurboParser JSON-native state owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_get_run_state_json_value(
    turbo_agent_runtime_t *runtime, const char *run_id,
    json_value_t **out_state);

CXX_C_API int turbo_agent_runtime_get_run_trace_events_json_value(
    turbo_agent_runtime_t *runtime, const char *run_id,
    json_value_t **out_events);

/**
 * @brief Compatibility accessor for one thread's latest-run state snapshot.
 *
 * This legacy surface resolves the newest run on the thread by `updated_at`.
 * It does not prefer the thread head / pending checkpoint. New hosts that need
 * thread-head semantics should prefer `get_thread_head_state_json_value(...)`.
 *
 * @param runtime Runtime handle.
 * @param thread_id Thread id.
 * @param out_state Output TurboParser JSON-native state owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_get_thread_state_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    json_value_t **out_state);

/**
 * @brief Load one thread head state snapshot as a TurboParser JSON-native object.
 *
 * The runtime resolves the newest interrupted run first. When the thread has
 * no interrupted run, it falls back to the newest run by `updated_at`.
 *
 * @param runtime Runtime handle.
 * @param thread_id Thread id.
 * @param out_state Output TurboParser JSON-native state owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_get_thread_head_state_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    json_value_t **out_state);

/**
 * @brief Prepare one checkpoint-scoped state override from a TurboParser JSON-native patch.
 *
 * The runtime loads the checkpoint state, recursively merges object fields from
 * `state_patch`, and returns the resulting full state as `out_state_override`.
 * Arrays, scalars, and null replace the target value. This helper does not
 * persist the prepared override back into the runtime.
 */
CXX_C_API int turbo_agent_runtime_prepare_checkpoint_state_override_json_value(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id,
    const json_value_t *state_patch,
    json_value_t **out_state_override);



/**
 * @brief Prepare one thread-head state override from a TurboParser JSON-native patch.
 *
 * The runtime resolves the thread's newest interrupted run first. If no
 * interrupted run exists, it falls back to the newest run by `updated_at`,
 * then applies the patch to that run's latest checkpoint state. This helper
 * only prepares the full override value and does not persist it.
 */
CXX_C_API int turbo_agent_runtime_prepare_thread_state_override_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    const json_value_t *state_patch,
    json_value_t **out_state_override);



/**
 * @brief Compatibility accessor for one thread's latest-run trace events.
 *
 * This legacy surface resolves the newest run on the thread by `updated_at`.
 * It does not prefer the thread head / pending checkpoint. New hosts that need
 * thread-head semantics should prefer `get_thread_head_trace_events_json_value(...)`.
 */
CXX_C_API int turbo_agent_runtime_get_thread_trace_events_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    json_value_t **out_events);

/**
 * @brief Load one thread head trace-event snapshot as a TurboParser JSON-native array.
 *
 * The runtime resolves the newest interrupted run first. When the thread has
 * no interrupted run, it falls back to the newest run by `updated_at`.
 */
CXX_C_API int turbo_agent_runtime_get_thread_head_trace_events_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    json_value_t **out_events);

/**
 * @brief List all run records for one thread.
 * @param runtime Runtime handle.
 * @param thread_id Thread id.
 * @param out_runs_json Output JSON array owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_list_runs(turbo_agent_runtime_t *runtime,
                                            const char *thread_id,
                                            json_value_t **out_runs_json);

/**
 * @brief List child runs by `parent_agent_run_id`.
 * @param runtime Runtime handle.
 * @param parent_agent_run_id Parent agent run id.
 * @param out_runs_json Output JSON array owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_list_child_runs(
    turbo_agent_runtime_t *runtime, const char *parent_agent_run_id,
    json_value_t **out_runs_json);

/**
 * @brief List all checkpoint records for one run.
 * @param runtime Runtime handle.
 * @param run_id Run id.
 * @param out_checkpoints_json Output JSON array owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_list_checkpoints(turbo_agent_runtime_t *runtime,
                                                   const char *run_id,
                                                   json_value_t **out_checkpoints_json);

/**
 * @brief List one thread-scoped lineage summary for host/UI inspection.
 *
 * The returned object is read-only lineage metadata. It does not alter runtime
 * execution semantics. The shape contains at least:
 * - `thread_id`
 * - `latest_run_id`
 * - `pending_run_id`
 * - `root_checkpoint_id`
 * - `branches`
 *
 * Each `branches[]` entry describes one run-level branch with fields such as:
 * - `run_id`
 * - `checkpoint_id`
 * - `branch_root_checkpoint_id`
 * - `parent_checkpoint_id`
 * - `parent_run_id`
 * - `forked_from_checkpoint_id`
 * - `status`
 * - `updated_at`
 *
 * @param runtime Runtime handle.
 * @param thread_id Thread id.
 * @param out_lineage_json Output JSON object owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_list_thread_lineage(turbo_agent_runtime_t *runtime,
                                                      const char *thread_id,
                                                      json_value_t **out_lineage_json);

/**
 * @brief Build one thread-scoped branch tree for host/UI inspection.
 *
 * This is a read-only inspect surface built from persisted thread/run/checkpoint
 * records. It does not change resume/fork semantics.
 *
 * The returned JSON object contains at least:
 * - `thread_id`
 * - `current_run_id`
 * - `latest_run_id`
 * - `pending_run_id`
 * - `current_checkpoint_id`
 * - `current_checkpoint_summary`
 * - `current_branch`
 * - `branches`
 * - `edges`
 *
 * `current_branch` is the branch node for `current_run_id` when present.
 * `current_checkpoint_summary` is a lightweight summary for
 * `current_checkpoint_id` when one exists; it is not the full checkpoint
 * record.
 * Branch nodes may expose lightweight checkpoint summaries via
 * `checkpoint_summary` and `source_checkpoint_summary`, plus
 * `branch_root_checkpoint_id` as the stable entry checkpoint anchor for that
 * branch.
 * `branches[]` contains run-level branch nodes. `edges[]` contains fork edges
 * from `source_checkpoint_id` to `target_run_id`, and may also expose a
 * lightweight `source_checkpoint_summary`.
 *
 * @param runtime Runtime handle.
 * @param thread_id Thread id.
 * @param out_branch_tree_json Output JSON object owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_get_branch_tree(turbo_agent_runtime_t *runtime,
                                                  const char *thread_id,
                                                  json_value_t **out_branch_tree_json);

/**
 * @brief Build one checkpoint-scoped inspect context for host/UI rendering.
 *
 * This is a read-only aggregate view built from existing checkpoint/run/thread
 * records. It does not change replay semantics and does not persist new state.
 *
 * The returned JSON object contains at least:
 * - `checkpoint_summary`
 * - `state`
 * - `run`
 * - `thread`
 * - `history_events`
 *
 * `checkpoint_summary` reuses the same lightweight checkpoint summary shape
 * used by thread timeline and branch-tree inspect surfaces. `history_events`
 * is the durable canonical event chain ending at the requested checkpoint,
 * converted to JSON.
 *
 * @param runtime Runtime handle.
 * @param checkpoint_id Explicit checkpoint id.
 * @param out_context_json Output JSON object owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_get_checkpoint_context(turbo_agent_runtime_t *runtime,
                                                         const char *checkpoint_id,
                                                         json_value_t **out_context_json);

/**
 * @brief Load durable canonical event history as one TurboParser JSON-native array.
 *
 * Pass exactly one selector:
 * - `run_id` to load all checkpointed segments for that run
 * - `checkpoint_id` to load the ancestor chain ending at that checkpoint
 *
 * @param runtime Runtime handle.
 * @param run_id Optional run id.
 * @param checkpoint_id Optional checkpoint id.
 * @param out_events Output TurboParser JSON-native array owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_load_history_events_json_value(
    turbo_agent_runtime_t *runtime, const char *run_id, const char *checkpoint_id,
    json_value_t **out_events);

/**
 * @brief Load durable canonical event history for the current thread lineage.
 *
 * The runtime resolves the thread's newest interrupted run first. If no
 * interrupted run exists, it falls back to the newest run by `updated_at`.
 *
 * @param runtime Runtime handle.
 * @param thread_id Thread id.
 * @param out_events Output TurboParser JSON-native array owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_load_thread_history_events_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    json_value_t **out_events);

/**
 * @brief Replay durable history events into one TurboParser JSON-native sink callback.
 *
 * Pass exactly one selector:
 * - `run_id` to replay all checkpointed segments for that run
 * - `checkpoint_id` to replay the ancestor chain ending at that checkpoint
 *
 * The sink receives the same TurboParser JSON-native history event objects returned by
 * `turbo_agent_runtime_load_history_events_json_value(...)`, in replay order.
 */
CXX_C_API int turbo_agent_runtime_replay_history_json_value(
    turbo_agent_runtime_t *runtime, const char *run_id, const char *checkpoint_id,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data);

/**
 * @brief Replay the current thread lineage's durable history into one sink.
 *
 * The runtime resolves the thread's newest interrupted run first. If no
 * interrupted run exists, it falls back to the newest run by `updated_at`.
 */
CXX_C_API int turbo_agent_runtime_replay_thread_history_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data);

/**
 * @brief Observe one durable history selector through the host-facing observer bridge.
 *
 * Pass exactly one selector:
 * - `run_id` to observe all checkpointed segments for that run
 * - `checkpoint_id` to observe the ancestor chain ending at that checkpoint
 *
 * The observer sink receives one TurboParser JSON-native object per mapped event with:
 * - `kind = "observer"`
 * - `type` in `model_delta`, `tool_call_started`, `tool_result`,
 *   `state_updated`, `interrupted`, `completed`
 * - `event` holding the original canonical history or trace event clone
 *
 * This is a host-facing bridge over the existing durable history facts; it
 * does not define or persist a second event log.
 */
CXX_C_API int turbo_agent_runtime_observe_history_json_value(
    turbo_agent_runtime_t *runtime, const char *run_id, const char *checkpoint_id,
    const turbo_agent_observer_json_value_sink_t *sink);

/**
 * @brief Observe the current thread lineage through the host-facing observer bridge.
 *
 * The runtime resolves the thread's newest interrupted run first. If no
 * interrupted run exists, it falls back to the newest run by `updated_at`.
 *
 * The sink receives the same observer events that
 * `turbo_agent_runtime_observe_history_json_value(...)` would emit for the resolved
 * run lineage.
 */
CXX_C_API int turbo_agent_runtime_observe_thread_history_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    const turbo_agent_observer_json_value_sink_t *sink);

/**
 * @brief Load one host-facing thread timeline snapshot as a TurboParser JSON-native object.
 *
 * The returned object aggregates the thread record, the newest run, the newest
 * interrupted run, the resolved current run, current-run checkpoints, and the
 * resolved current run's durable history in one call.
 *
 * Current-run resolution is explicit and stable:
 * - prefer the newest interrupted run on the thread
 * - otherwise fall back to the newest run by `updated_at`
 *
 * The resulting object contains at least:
 * - `thread`
 * - `resolved_current_run_id`
 * - `resolved_current_checkpoint_id`
 * - `resolved_current_run_source`
 * - `resolved_current_run`
 * - `resolved_current_checkpoint`
 * - `latest_run`
 * - `pending_run`
 * - `runs`
 * - `current_run_checkpoints`
 * - `history_events`
 *
 * `resolved_current_checkpoint` is a lightweight checkpoint summary for the
 * current thread head. Fetch the full persisted checkpoint record separately
 * through `turbo_agent_runtime_get_checkpoint(...)` when needed.
 *
 * @param runtime Runtime handle.
 * @param thread_id Thread id.
 * @param out_timeline Output TurboParser JSON-native object owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_get_thread_timeline_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    json_value_t **out_timeline);

/**
 * @brief Build one thread-scoped observability index bundle for host/UI inspection.
 *
 * The returned JSON object is a read-only aggregate over existing inspect
 * surfaces. It contains:
 * - `thread`
 * - `latest_run`
 * - `pending_run`
 * - `thread_timeline`
 * - `thread_lineage`
 * - `branch_tree`
 * - `current_status`
 * - `current_interrupt_reason`
 * - `current_pending_action`
 * - `current_checkpoint_summary`
 * - `latest_run_status`
 * - `latest_run_updated_at`
 * - `pending_run_id`
 * - `pending_checkpoint_id`
 * - `has_failure`
 * - `has_model_error`
 * - `has_guardrail_rejection`
 * - `replan_requested`
 * - `current_failure_reason`
 * - `current_review_note`
 * - `has_pending_review`
 * - `has_handoff`
 * - `active_agent`
 * - `history_events`
 * - `trace_events`
 * - `counts`
 *
 * `counts` currently summarizes:
 * - `runs`
 * - `interrupted_runs`
 * - `completed_runs`
 * - `current_run_checkpoints`
 * - `branches`
 * - `edges`
 * - `history_events`
 * - `trace_events`
 */
CXX_C_API int turbo_agent_runtime_get_thread_observability_index(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    json_value_t **out_index_json);

/**
 * @brief List one lightweight observability summary per persisted thread.
 *
 * This is a runtime-only host/UI surface for cross-thread inspection. Each
 * array item is a narrowed summary derived from
 * `turbo_agent_runtime_get_thread_observability_index(...)`, not a second
 * persisted index. The returned summary contains at least:
 * - `thread`
 * - `latest_run`
 * - `pending_run`
 * - `current_status`
 * - `current_interrupt_reason`
 * - `current_pending_action`
 * - `current_checkpoint_summary`
 * - `latest_run_status`
 * - `latest_run_updated_at`
 * - `pending_run_id`
 * - `pending_checkpoint_id`
 * - `has_failure`
 * - `has_model_error`
 * - `has_guardrail_rejection`
 * - `replan_requested`
 * - `current_failure_reason`
 * - `current_review_note`
 * - `has_pending_review`
 * - `has_handoff`
 * - `active_agent`
 * - `counts`
 *
 * The returned array is sorted by thread `updated_at` descending.
 *
 * @param runtime Runtime handle.
 * @param out_indexes_json Output JSON array owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_list_observability_indexes(
    turbo_agent_runtime_t *runtime, json_value_t **out_indexes_json);

/**
 * @brief List lightweight observability summaries across persisted threads with filters.
 *
 * This runtime-only host/UI surface uses the same derived observability
 * summary fields as `turbo_agent_runtime_list_observability_indexes(...)`,
 * then applies optional JSON-object filters. A NULL filter is equivalent to
 * the unfiltered list API.
 *
 * The currently supported filter keys are:
 * - `status` string, matched against summary `current_status`
 * - `has_pending_review` bool
 * - `has_failure` bool
 * - `has_handoff` bool
 * - `active_agent` string
 * - `has_model_error` bool
 * - `has_guardrail_rejection` bool
 * - `replan_requested` bool
 * - `current_interrupt_reason` string
 * - `latest_run_status` string
 * - `latest_run_updated_after` string, matched strictly against
 *   `latest_run_updated_at`
 * - `latest_run_updated_before` string, matched strictly against
 *   `latest_run_updated_at`
 * - `thread_id_prefix` string, matched against `thread.id`
 * - `sort_by` string in `latest_run_updated_at` or `thread_id`
 * - `sort_order` string in `asc` or `desc`
 * - `limit` non-negative number
 *
 * Default sorting is:
 * - `latest_run_updated_at` + `desc` when no sort keys are provided
 * - `thread_id` + `asc` when `sort_by=thread_id` and `sort_order` is omitted
 * - `latest_run_updated_at` + `desc` when only `sort_order` is provided
 *
 * Unknown keys are ignored so the filter object stays forward-compatible.
 *
 * @param runtime Runtime handle.
 * @param filters_json Optional JSON object filter.
 * @param out_indexes_json Output JSON array owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_list_observability_indexes_filtered(
    turbo_agent_runtime_t *runtime, const json_value_t *filters_json,
    json_value_t **out_indexes_json);

/**
 * @brief Apply one host-facing runtime command to a checkpoint state.
 *
 * The command must be a TurboParser JSON-native object with `kind`. The initial supported
 * commands are:
 *
 * - `approve_review` with optional `approved` bool
 * - `reject_review` with optional `reason`
 * - `request_replan` with optional `reason`
 * - `append_feedback` with `text`
 * - `append_user_message` with `text`
 * - `override_final_output` with `text` or `output_json`
 *
 * The returned `out_state_override` is suitable for
 * `resume_json_value_graph(...)` or `fork_json_value_graph(...)`. This helper does not
 * persist the prepared override back into the runtime.
 *
 * @param runtime Runtime handle.
 * @param checkpoint_id Checkpoint id to read and modify.
 * @param command Command object.
 * @param out_state_override Output TurboParser JSON-native state owned by caller.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runtime_prepare_checkpoint_command_override_json_value(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id,
    const json_value_t *command,
    json_value_t **out_state_override);

/**
 * @brief Prepare one thread-head command-derived state override.
 *
 * The runtime resolves the thread's newest interrupted run first, then falls
 * back to the newest run by `updated_at`, and finally applies the command to
 * that run's latest checkpoint. This helper only prepares the resulting
 * override and does not persist it.
 */
CXX_C_API int turbo_agent_runtime_prepare_thread_command_override_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    const json_value_t *command,
    json_value_t **out_state_override);

#ifdef __cplusplus
}
#endif

#endif
