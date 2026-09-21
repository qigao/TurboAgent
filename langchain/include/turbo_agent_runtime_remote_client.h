#ifndef TURBO_AGENT_RUNTIME_REMOTE_CLIENT_H
#define TURBO_AGENT_RUNTIME_REMOTE_CLIENT_H

#include <platform.h>

#include <http_client/http.h>
#include "turbo_agent_memory_store.h"
#include "turbo_agent_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_runtime_remote_client_s turbo_agent_runtime_remote_client_t;

typedef struct turbo_agent_runtime_remote_client_config_s {
  const char *url;
  chttp_client *http_client;
} turbo_agent_runtime_remote_client_config_t;

/**
 * @brief Create one runtime remote client over an RPC JSON-RPC endpoint.
 *
 * When `http_client` is provided, this wrapper borrows it and never destroys it.
 * `url` always names the runtime JSON-RPC endpoint. Otherwise the wrapper
 * creates one owned bounded CHTTP client.
 */
CXX_C_API turbo_agent_runtime_remote_client_t *turbo_agent_runtime_remote_client_create(
    const turbo_agent_runtime_remote_client_config_t *config);

/**
 * @brief Destroy one runtime remote client wrapper.
 */
CXX_C_API void turbo_agent_runtime_remote_client_destroy(
    turbo_agent_runtime_remote_client_t *client);

/**
 * @brief Call one runtime remote JSON-RPC method with JSON params.
 *
 * On a JSON-RPC success response, this returns `0` and stores the parsed
 * `result` payload in `out_result_json` when requested.
 *
 * On a JSON-RPC error response, this also returns `0`, leaves
 * `out_result_json == NULL`, and stores one normalized error object in
 * `out_error_json` when requested. That error object contains:
 * - `code`
 * - `message`
 * - `http_status`
 * - `transport_error`
 *
 * On local transport or parse failure, this returns `-1`. A best-effort
 * diagnostic error object may still be returned through `out_error_json`.
 *
 * Caller owns returned JSON values and must free them with `turbo_free_json(...)`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_call_json(
    turbo_agent_runtime_remote_client_t *client, const char *method,
    const json_value_t *params_json, json_value_t **out_result_json,
    json_value_t **out_error_json);

/**
 * @brief Load remote startup diagnostics for one graph through `runtime.getStartupDiagnostics`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_get_startup_diagnostics(
    turbo_agent_runtime_remote_client_t *client, const char *graph_name,
    json_value_t **out_diagnostics_json, json_value_t **out_error_json);

/**
 * @brief Start one remote graph run through `runtime.start`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_start_json_value_graph(
    turbo_agent_runtime_remote_client_t *client, const char *graph_name,
    const json_value_t *state, const turbo_graph_run_options_t *options,
    const char *thread_id, json_value_t **out_summary_json,
    json_value_t **out_state, json_value_t **out_error_json);

/**
 * @brief Resume one remote checkpoint through `runtime.resume`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_resume_json_value_graph(
    turbo_agent_runtime_remote_client_t *client, const char *graph_name,
    const char *checkpoint_id, const json_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state, json_value_t **out_error_json);

/**
 * @brief Fork one new remote run from one checkpoint through `runtime.fork`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_fork_json_value_graph(
    turbo_agent_runtime_remote_client_t *client, const char *graph_name,
    const char *checkpoint_id, const json_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state, json_value_t **out_error_json);

/**
 * @brief Load one remote thread head state through `runtime.getThreadState`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_get_thread_state_json_value(
    turbo_agent_runtime_remote_client_t *client, const char *thread_id,
    json_value_t **out_state, json_value_t **out_error_json);

/**
 * @brief Load one remote checkpoint inspect context through `runtime.getCheckpointContext`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_get_checkpoint_context(
    turbo_agent_runtime_remote_client_t *client, const char *checkpoint_id,
    json_value_t **out_context_json, json_value_t **out_error_json);

/**
 * @brief Load one remote run record through `runtime.getRun`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_get_run(
    turbo_agent_runtime_remote_client_t *client, const char *run_id,
    json_value_t **out_run_json, json_value_t **out_error_json);

/**
 * @brief Load one remote checkpoint record through `runtime.getCheckpoint`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_get_checkpoint(
    turbo_agent_runtime_remote_client_t *client, const char *checkpoint_id,
    json_value_t **out_checkpoint_json, json_value_t **out_error_json);

/**
 * @brief List remote checkpoints for one run through `runtime.listCheckpoints`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_list_checkpoints(
    turbo_agent_runtime_remote_client_t *client, const char *run_id,
    json_value_t **out_checkpoints_json, json_value_t **out_error_json);

/**
 * @brief Load remote durable history events through `runtime.loadHistoryEvents`.
 *
 * At least one of `run_id` or `checkpoint_id` must be non-empty.
 */
CXX_C_API int turbo_agent_runtime_remote_client_load_history_events_json_value(
    turbo_agent_runtime_remote_client_t *client, const char *run_id, const char *checkpoint_id,
    json_value_t **out_events, json_value_t **out_error_json);

/**
 * @brief Load remote run trace events through `runtime.getRunTraceEvents`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_get_run_trace_events_json_value(
    turbo_agent_runtime_remote_client_t *client, const char *run_id,
    json_value_t **out_events, json_value_t **out_error_json);

/**
 * @brief Load remote checkpoint trace events through `runtime.getCheckpointTraceEvents`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_get_checkpoint_trace_events_json_value(
    turbo_agent_runtime_remote_client_t *client, const char *checkpoint_id,
    json_value_t **out_events, json_value_t **out_error_json);

/**
 * @brief Delete one remote canonical memory record through `memory.deleteRecord`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_delete_memory_record(
    turbo_agent_runtime_remote_client_t *client, const char *memory_namespace,
    const char *key, json_value_t **out_error_json);

/**
 * @brief Load one remote canonical memory record through `memory.getRecord`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_get_memory_record(
    turbo_agent_runtime_remote_client_t *client, const char *memory_namespace,
    const char *key, json_value_t **out_record_json, json_value_t **out_error_json);

/**
 * @brief Persist one remote canonical memory record through `memory.putRecord`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_put_memory_record(
    turbo_agent_runtime_remote_client_t *client, const json_value_t *record_json,
    json_value_t **out_record_json, json_value_t **out_error_json);

/**
 * @brief Validate one remote canonical memory record through `memory.validateRecord`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_validate_memory_record(
    turbo_agent_runtime_remote_client_t *client, const json_value_t *record_json, int *out_valid,
    json_value_t **out_error_json);

/**
 * @brief Query remote canonical memory records through `memory.queryRecordsEx`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_query_memory_records_ex(
    turbo_agent_runtime_remote_client_t *client, const turbo_agent_memory_query_options_t *options,
    json_value_t **out_records_json, json_value_t **out_error_json);

/**
 * @brief Query remote canonical memory records through the convenience wrapper.
 */
CXX_C_API int turbo_agent_runtime_remote_client_query_memory_records(
    turbo_agent_runtime_remote_client_t *client, const char *namespace_prefix,
    const char *kind, const char *key_prefix, const char *text_substring,
    json_value_t **out_records_json, json_value_t **out_error_json);

/**
 * @brief List remote canonical memory records through `memory.listRecords`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_list_memory_records(
    turbo_agent_runtime_remote_client_t *client, const char *namespace_prefix,
    json_value_t **out_records_json, json_value_t **out_error_json);

/**
 * @brief Load one remote thread timeline snapshot through the observability bundle.
 */
CXX_C_API int turbo_agent_runtime_remote_client_get_thread_timeline_json_value(
    turbo_agent_runtime_remote_client_t *client, const char *thread_id,
    json_value_t **out_timeline, json_value_t **out_error_json);

/**
 * @brief Load one remote branch tree snapshot through the observability bundle.
 */
CXX_C_API int turbo_agent_runtime_remote_client_get_branch_tree(
    turbo_agent_runtime_remote_client_t *client, const char *thread_id,
    json_value_t **out_branch_tree_json, json_value_t **out_error_json);

/**
 * @brief Load one remote observability bundle through `runtime.getThreadObservabilityIndex`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_get_thread_observability_index(
    turbo_agent_runtime_remote_client_t *client, const char *thread_id,
    json_value_t **out_index_json, json_value_t **out_error_json);

/**
 * @brief List lightweight remote observability summaries across persisted threads.
 */
CXX_C_API int turbo_agent_runtime_remote_client_list_observability_indexes(
    turbo_agent_runtime_remote_client_t *client, json_value_t **out_indexes_json,
    json_value_t **out_error_json);

/**
 * @brief List lightweight remote observability summaries with optional filters.
 */
CXX_C_API int turbo_agent_runtime_remote_client_list_observability_indexes_filtered(
    turbo_agent_runtime_remote_client_t *client, const json_value_t *filters_json,
    json_value_t **out_indexes_json, json_value_t **out_error_json);

/**
 * @brief List child runs for one remote parent agent run through `runtime.listChildRuns`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_list_child_runs(
    turbo_agent_runtime_remote_client_t *client, const char *parent_agent_run_id,
    json_value_t **out_runs_json, json_value_t **out_error_json);

/**
 * @brief Build one supervisor inspect snapshot from one remote thread state.
 */
CXX_C_API int turbo_agent_runtime_remote_client_get_supervisor_inspect(
    turbo_agent_runtime_remote_client_t *client, const char *thread_id,
    json_value_t **out_inspect_json);

/**
 * @brief Build one orchestration inspect bundle from one remote thread.
 */
CXX_C_API int turbo_agent_runtime_remote_client_get_orchestration_inspect(
    turbo_agent_runtime_remote_client_t *client, const char *thread_id,
    json_value_t **out_inspect_json);

/**
 * @brief Load one child execution inspect bundle from a parent tool-result output item.
 *
 * The returned JSON bundle mirrors the existing child inspect contract and
 * contains the child `run`, `checkpoints`, optional `latest_checkpoint`,
 * optional `checkpoint_context`, optional `thread_timeline`, optional
 * `branch_tree`, and child `history_events` / `trace_events`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_get_child_inspect(
    turbo_agent_runtime_remote_client_t *client, const json_value_t *output_item,
    json_value_t **out_inspect_json);

/**
 * @brief Load one child orchestration inspect bundle from a parent tool-result output item.
 *
 * The returned JSON bundle contains the parent lineage fields
 * `parent_agent_run_id` / `parent_tool_call_id` / `parent_tool_name` plus
 * nested call-frame metadata `parent_graph_run_id` / `call_frame_id`,
 * together with nested `child_inspect`.
 */
CXX_C_API int turbo_agent_runtime_remote_client_get_child_orchestration_inspect(
    turbo_agent_runtime_remote_client_t *client, const json_value_t *output_item,
    json_value_t **out_inspect_json);

/**
 * @brief Build one combined multi-agent inspect bundle for one parent thread.
 */
CXX_C_API int turbo_agent_runtime_remote_client_get_child_multi_agent_inspect(
    turbo_agent_runtime_remote_client_t *client, const char *thread_id,
    const json_value_t *output_item, json_value_t **out_inspect_json);

/**
 * @brief Apply one command to one remote thread head and resume it.
 */
CXX_C_API int turbo_agent_runtime_remote_client_resume_thread_command_json_value(
    turbo_agent_runtime_remote_client_t *client, const char *graph_name,
    const char *thread_id, const json_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state, json_value_t **out_error_json);

/**
 * @brief Apply one command to one remote thread head and fork a new run from it.
 */
CXX_C_API int turbo_agent_runtime_remote_client_fork_thread_command_json_value(
    turbo_agent_runtime_remote_client_t *client, const char *graph_name,
    const char *thread_id, const json_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state, json_value_t **out_error_json);

#ifdef __cplusplus
}
#endif

#endif
