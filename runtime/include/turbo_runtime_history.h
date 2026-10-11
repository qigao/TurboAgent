#ifndef TURBO_RUNTIME_HISTORY_H
#define TURBO_RUNTIME_HISTORY_H

#include "turbo_runtime_store.h"
#include "turbo_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Generic durable record navigation. Historical Agent-prefixed names are kept
 * for source compatibility; these operations have no Agent state dependency.
 * All JSON outputs are independent owned trees, released with json_free().
 * Output slots are cleared on entry. Return 0 on success, -1 on invalid input,
 * missing data, malformed records or storage failure. No record is modified.
 * The caller keeps runtime alive and serializes conflicting mutations during
 * multi-record queries; per-operation store locking is not a read transaction.
 * Run selection keeps the first matching record on tied updated_at values.
 * List ordering is the backend's order unless the operation documents a sort.
 */

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

/** ID selection helpers return malloc-owned text (free), or NULL on -1.
 * latest uses updated_at; pending restricts status to interrupted. current/head
 * prefer pending, falling back to latest as the legacy API requires. */
CXX_C_API int turbo_agent_runtime_latest_run_id_for_thread(
    turbo_agent_runtime_t *runtime, const char *thread_id, char **out_run_id);
CXX_C_API int turbo_agent_runtime_pending_run_id_for_thread(
    turbo_agent_runtime_t *runtime, const char *thread_id, char **out_run_id);
CXX_C_API int turbo_agent_runtime_current_run_id_for_thread(
    turbo_agent_runtime_t *runtime, const char *thread_id, char **out_run_id);
CXX_C_API int turbo_agent_runtime_current_checkpoint_id_for_thread(
    turbo_agent_runtime_t *runtime, const char *thread_id, char **out_checkpoint_id);
CXX_C_API int turbo_agent_runtime_thread_head_run_id_for_thread(
    turbo_agent_runtime_t *runtime, const char *thread_id, char **out_run_id);

/**
 * Read a checkpoint record without taking ownership. Output pointers borrow
 * record storage and remain valid only while that tree stays alive/unmodified.
 * Required IDs must be strings, checkpoint_json an object, and seq an exact
 * positive integer representable by size_t and the legacy JSON number format.
 * Return 0/-1; clear every supplied output on failure. parent ID may be NULL.
 */
CXX_C_API int turbo_runtime_checkpoint_record_read(
    const json_value_t *record, const char **out_thread_id, const char **out_run_id,
    const char **out_parent_checkpoint_id, size_t *out_seq,
    const json_value_t **out_checkpoint_json);

/**
 * Load an independent graph checkpoint from borrowed checkpoint_json. Optional
 * state_override is copied; neither input is mutated or consumed. Return 0/-1
 * and clear output on failure. Release success with turbo_graph_checkpoint_destroy.
 * Example: decode record fields, then pass checkpoint_json to this function.
 */
CXX_C_API int turbo_runtime_checkpoint_from_json(
    const json_value_t *checkpoint_json, const json_value_t *state_override,
    turbo_graph_checkpoint_t **out_checkpoint);

/** Return an owned array sorted by positive exact seq, or NULL on failure.
 * Borrows records, clones each record, and leaves the input unchanged. Equal
 * sequence ordering is unspecified. Release the result with json_free(). */
CXX_C_API json_value_t *turbo_runtime_checkpoint_records_sorted(const json_value_t *records);

/** Follow parent IDs to the root; return malloc-owned ID, or NULL on -1.
 * Missing parents, non-string links and cycles fail without partial output.
 * Cycle detection requires O(depth) temporary ID storage. */
CXX_C_API int turbo_runtime_root_checkpoint_id(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id, char **out_root_id);

#ifdef __cplusplus
}
#endif
#endif
