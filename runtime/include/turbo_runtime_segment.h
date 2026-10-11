#ifndef TURBO_RUNTIME_SEGMENT_H
#define TURBO_RUNTIME_SEGMENT_H

#include "turbo_runtime_store.h"
#include "turbo_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_RUNTIME_SEGMENT_ABI_VERSION 1U

typedef struct turbo_runtime_segment_s turbo_runtime_segment_t;

/* All callback arguments except output slots are borrowed for the callback.
 * Projection callbacks return 0 and independently owned trees on success;
 * summary must return a JSON object.
 * Runtime frees any output trees also on failure. Do not return borrowed state. */
typedef int (*turbo_runtime_snapshot_fn)(
    const json_value_t *state, json_value_t **out_control,
    json_value_t **out_workflow, void *user_data);
typedef int (*turbo_runtime_summary_fn)(
    const turbo_runtime_segment_t *segment, const char *checkpoint_id,
    const turbo_graph_run_result_t *result, const json_value_t *state,
    json_value_t **out_summary, void *user_data);

/* Optional paired callbacks bracket graph execution, projection, and persistence
 * after the thread record is upserted. They cannot fail; leave runs once for every
 * enter, including graph/projection/store failures. No callbacks run on invalid
 * configuration or failed thread upsert. Nested calls need distinct scope data. */
typedef void (*turbo_runtime_segment_scope_fn)(
    const turbo_runtime_segment_t *segment, void *user_data);

struct turbo_runtime_segment_s {
  size_t struct_size;
  unsigned int abi_version;
  turbo_agent_runtime_t *runtime;
  turbo_graph_t *graph;
  const char *thread_id;
  const char *run_id;
  const char *parent_run_id;
  const char *parent_checkpoint_id;
  const char *forked_from_checkpoint_id;
  /* Opaque lineage retained in the existing persisted record format. */
  const char *parent_agent_run_id;
  const char *parent_tool_call_id;
  const char *parent_tool_name;
  const char *parent_graph_run_id;
  const char *call_frame_id;
  size_t next_checkpoint_seq; /* 1..9007199254740991: exact legacy JSON number. */
  const char *run_created_at; /* Optional prior timestamp, at most 31 bytes. */
  /* Exactly one start input is required. Both remain owned by the caller. */
  const json_value_t *start_state;
  const turbo_graph_checkpoint_t *start_checkpoint;
  const turbo_graph_run_options_t *options;
  const turbo_cancel_token_t *cancel_token;
  turbo_event_sink_json_value_fn event_sink;
  void *event_sink_user_data;
  turbo_runtime_snapshot_fn snapshot;
  turbo_runtime_summary_fn summary;
  turbo_runtime_segment_scope_fn enter;
  turbo_runtime_segment_scope_fn leave;
  void *user_data;
};

/** Initialize a request: ABI, size, sequence 1; all borrowed inputs are NULL. */
CXX_C_API void turbo_runtime_segment_init(turbo_runtime_segment_t *segment);

/**
 * Execute and persist one synchronous graph segment. All inputs/callback code
 * stay live and stable until return; this call retains no request pointers.
 * runtime, graph, thread_id and run_id are required. Initialize before filling.
 * The caller serializes conflicting runs/thread mutations and owns ID selection.
 *
 * Start from start_state, or resume/fork a caller-loaded start_checkpoint with
 * the supplied identity/lineage. Capture and forward events/checkpoints in their
 * original order. Persist interrupted/cancelled/timed-out checkpoints before the
 * run record, then construct the summary. Snapshot callback NULL means empty
 * control/workflow projections; full state is still stored in checkpoint_json.
 * Summary callback NULL returns generic identity/status/result fields, without
 * Agent commands or policy. Callbacks do not run on a background thread.
 *
 * Return 0 for completed/interrupted/cancelled/timed-out segments, -1 for graph,
 * configuration, projection or persistence failure. Clear both output slots on
 * entry. Successful outputs are owned JSON trees (release with json_free).
 * A graph ERROR may return owned diagnostic state with -1; other failures leave
 * outputs NULL. No rollback/transaction is implied: a later failure can leave a
 * thread, checkpoint or run record already written. A failed summary callback
 * occurs after run persistence. Existing graph callbacks keep their borrow rules.
 *
 * Example: turbo_runtime_segment_init(&request), set runtime/graph/thread_id/
 * run_id/start_state, then
 * turbo_runtime_segment_execute(&request, &summary, &state).
 */
CXX_C_API int turbo_runtime_segment_execute(
    const turbo_runtime_segment_t *segment, json_value_t **out_summary,
    json_value_t **out_state);

/** UTC record timestamp, 0/-1; buffer must have at least 21 bytes. */
CXX_C_API int turbo_runtime_record_timestamp(char *buffer, size_t buffer_size);
/** Create an owned malloc/free record ID, or NULL; prefix is borrowed.
 * IDs retain the legacy prefix/monotonic-tick/process-counter format. This is
 * not a globally coordinated ID allocator across processes or restarts. */
CXX_C_API char *turbo_runtime_record_id(const char *prefix);
/** Borrowed stable text corresponding to the persisted execution status. */
CXX_C_API const char *turbo_runtime_execution_status_text(turbo_graph_exec_status_t status);
/** Serialize a graph result to an owned JSON tree; 0/-1. */
CXX_C_API int turbo_runtime_execution_result_json(
    const turbo_graph_run_result_t *result, json_value_t **out_result);

#ifdef __cplusplus
}
#endif
#endif
