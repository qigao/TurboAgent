#ifndef TURBO_STATE_GRAPH_H
#define TURBO_STATE_GRAPH_H

#include <platform.h>

#include <stddef.h>

#include "turbo_runtime_json.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_state_graph_s turbo_state_graph_t;

typedef enum {
  TURBO_STATE_GRAPH_OK = 0,
  TURBO_STATE_GRAPH_STOP = 1,
  TURBO_STATE_GRAPH_INTERRUPTED = 2,
  TURBO_STATE_GRAPH_ERROR = -1,
  TURBO_STATE_GRAPH_INVALID_ARGUMENT = -2,
  TURBO_STATE_GRAPH_OUT_OF_MEMORY = -3,
  TURBO_STATE_GRAPH_DUPLICATE_CHANNEL = -4,
  TURBO_STATE_GRAPH_DUPLICATE_NODE = -5,
  TURBO_STATE_GRAPH_CHANNEL_NOT_FOUND = -6,
  TURBO_STATE_GRAPH_NODE_NOT_FOUND = -7,
  TURBO_STATE_GRAPH_ROUTE_NOT_FOUND = -8,
  TURBO_STATE_GRAPH_STEP_LIMIT = -9,
  TURBO_STATE_GRAPH_TYPE_MISMATCH = -10,
  TURBO_STATE_GRAPH_RUN_NOT_FOUND = -11,
  TURBO_STATE_GRAPH_THREAD_NOT_FOUND = -12,
  TURBO_STATE_GRAPH_HISTORY_NOT_FOUND = -13,
  TURBO_STATE_GRAPH_RUN_COMPLETED = -14,
  TURBO_STATE_GRAPH_RUN_NOT_INTERRUPTED = -15,
  TURBO_STATE_GRAPH_INVALID_UPDATE = -16
} turbo_state_graph_status_t;

typedef enum {
  TURBO_STATE_GRAPH_REDUCER_REPLACE = 0,
  TURBO_STATE_GRAPH_REDUCER_APPEND = 1,
  TURBO_STATE_GRAPH_REDUCER_MERGE_OBJECT = 2,
  TURBO_STATE_GRAPH_REDUCER_ADD = 3,
  TURBO_STATE_GRAPH_REDUCER_LAST_NON_NULL = 4
} turbo_state_graph_reducer_kind_t;

typedef enum {
  TURBO_STATE_GRAPH_SOURCE_START = 0,
  TURBO_STATE_GRAPH_SOURCE_NODE = 1,
  TURBO_STATE_GRAPH_SOURCE_HOST = 2,
  TURBO_STATE_GRAPH_SOURCE_FORK = 3
} turbo_state_graph_source_kind_t;

typedef struct turbo_state_graph_pending_send_s turbo_state_graph_pending_send_t;

typedef struct turbo_state_graph_exec_ctx_s {
  turbo_state_graph_t *graph;
  const json_value_t *state;
  json_value_t *update;
  const char *current_node;
  const char *next_node;
  turbo_state_graph_pending_send_t *sends;
  size_t send_count;
  size_t send_capacity;
  size_t step;
  int stop;
} turbo_state_graph_exec_ctx_t;

typedef int (*turbo_state_graph_json_value_node_fn)(turbo_state_graph_exec_ctx_t *ctx,
                                              void *user_data);
typedef int (*turbo_state_graph_json_value_edge_predicate_fn)(
    const json_value_t *state, void *user_data);

typedef struct turbo_state_graph_channel_config_s {
  turbo_state_graph_reducer_kind_t reducer;
  turbo_json_type_t value_kind;
  const json_value_t *default_value;
  const char *description;
} turbo_state_graph_channel_config_t;

typedef struct turbo_state_graph_run_options_s {
  size_t max_steps;
  const char *start_node;
  const char *const *interrupt_before_nodes;
  size_t interrupt_before_count;
  int skip_initial_interrupt;
} turbo_state_graph_run_options_t;

typedef struct turbo_state_graph_history_options_s {
  const char *as_node;
  const char *reason;
} turbo_state_graph_history_options_t;

typedef struct turbo_state_graph_subgraph_node_config_s {
  turbo_state_graph_t *child_graph;
  const char *child_thread_id;
  const char *child_thread_id_channel;
  const char *output_channel;
  const char *const *input_channels;
  size_t input_channel_count;
} turbo_state_graph_subgraph_node_config_t;

typedef struct turbo_state_graph_run_result_s {
  turbo_state_graph_status_t status;
  const char *thread_id;
  const char *run_id;
  const char *history_entry_id;
  const char *checkpoint_id;
  const char *last_node;
  const char *next_node;
  size_t steps;
} turbo_state_graph_run_result_t;

CXX_C_API turbo_state_graph_t *turbo_state_graph_create(const char *name);
CXX_C_API void turbo_state_graph_destroy(turbo_state_graph_t *graph);

CXX_C_API turbo_state_graph_status_t turbo_state_graph_add_channel(
    turbo_state_graph_t *graph, const char *name,
    const turbo_state_graph_channel_config_t *config);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_add_json_value_node(turbo_state_graph_t *graph, const char *name,
                                turbo_state_graph_json_value_node_fn fn, void *user_data);

CXX_C_API turbo_state_graph_status_t turbo_state_graph_add_subgraph_node(
    turbo_state_graph_t *graph, const char *name,
    const turbo_state_graph_subgraph_node_config_t *config);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_add_json_value_edge(turbo_state_graph_t *graph, const char *from, const char *to,
                                turbo_state_graph_json_value_edge_predicate_fn predicate,
                                void *user_data);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_set_entry(turbo_state_graph_t *graph, const char *name);

CXX_C_API const char *turbo_state_graph_get_entry(const turbo_state_graph_t *graph);
CXX_C_API size_t turbo_state_graph_channel_count(const turbo_state_graph_t *graph);
CXX_C_API size_t turbo_state_graph_node_count(const turbo_state_graph_t *graph);
CXX_C_API size_t turbo_state_graph_edge_count(const turbo_state_graph_t *graph);
CXX_C_API size_t turbo_state_graph_snapshot_schema_version(void);

CXX_C_API char *turbo_state_graph_serialize_snapshot(const turbo_state_graph_t *graph,
                                                     size_t *out_len);

CXX_C_API turbo_state_graph_status_t turbo_state_graph_load_snapshot(
    turbo_state_graph_t *graph, const char *json, size_t len);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_ctx_set_next(turbo_state_graph_exec_ctx_t *ctx, const char *next_node);
CXX_C_API turbo_state_graph_status_t
turbo_state_graph_ctx_goto(turbo_state_graph_exec_ctx_t *ctx, const char *next_node);
CXX_C_API turbo_state_graph_status_t turbo_state_graph_ctx_send(
    turbo_state_graph_exec_ctx_t *ctx, const char *target_node,
    const json_value_t *update);
CXX_C_API void turbo_state_graph_ctx_stop(turbo_state_graph_exec_ctx_t *ctx);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_start(turbo_state_graph_t *graph, const char *thread_id,
                        const json_value_t *input_state,
                        const turbo_state_graph_run_options_t *options,
                        turbo_state_graph_run_result_t *out_result,
                        json_value_t **out_state);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_resume(turbo_state_graph_t *graph, const char *run_id,
                         const turbo_state_graph_run_options_t *options,
                         turbo_state_graph_run_result_t *out_result,
                         json_value_t **out_state);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_update_state(turbo_state_graph_t *graph, const char *run_id,
                               const json_value_t *update,
                               const turbo_state_graph_history_options_t *options,
                               turbo_state_graph_run_result_t *out_result,
                               json_value_t **out_state);

CXX_C_API turbo_state_graph_status_t turbo_state_graph_resume_from_history(
    turbo_state_graph_t *graph, const char *history_entry_id,
    const json_value_t *update,
    const turbo_state_graph_history_options_t *history_options,
    const turbo_state_graph_run_options_t *run_options,
    turbo_state_graph_run_result_t *out_result,
    json_value_t **out_state);

CXX_C_API turbo_state_graph_status_t turbo_state_graph_fork_from_history(
    turbo_state_graph_t *graph, const char *history_entry_id,
    const json_value_t *update,
    const turbo_state_graph_history_options_t *history_options,
    const turbo_state_graph_run_options_t *run_options,
    turbo_state_graph_run_result_t *out_result,
    json_value_t **out_state);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_get_state(turbo_state_graph_t *graph, const char *run_id,
                            json_value_t **out_state);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_get_run(turbo_state_graph_t *graph, const char *run_id,
                          json_value_t **out_run);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_get_thread(turbo_state_graph_t *graph, const char *thread_id,
                             json_value_t **out_thread);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_get_latest_run(turbo_state_graph_t *graph, const char *thread_id,
                                 json_value_t **out_run);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_get_pending_run(turbo_state_graph_t *graph, const char *thread_id,
                                  json_value_t **out_run);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_get_thread_state(turbo_state_graph_t *graph, const char *thread_id,
                                   json_value_t **out_state);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_get_history_entry(turbo_state_graph_t *graph,
                                    const char *history_entry_id,
                                    json_value_t **out_entry);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_get_history_entry_state(turbo_state_graph_t *graph,
                                          const char *history_entry_id,
                                          json_value_t **out_state);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_get_checkpoint(turbo_state_graph_t *graph, const char *checkpoint_id,
                                 json_value_t **out_checkpoint);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_list_state_history(turbo_state_graph_t *graph, const char *run_id,
                                     json_value_t **out_entries);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_list_checkpoints(turbo_state_graph_t *graph, const char *run_id,
                                   json_value_t **out_checkpoints);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_list_runs(turbo_state_graph_t *graph, const char *thread_id,
                            json_value_t **out_runs);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_get_checkpoint_context(turbo_state_graph_t *graph,
                                         const char *checkpoint_id,
                                         json_value_t **out_context);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_get_branch_tree(turbo_state_graph_t *graph, const char *thread_id,
                                  json_value_t **out_tree);

#ifdef __cplusplus
}
#endif

#endif
