#ifndef TURBO_GRAPH_H
#define TURBO_GRAPH_H

#include <stddef.h>
#include <platform.h>
#include <turbo_parser.h>

#include "turbo_event.h"
#include "turbo_runtime_data_bind.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_graph_s turbo_graph_t;
typedef struct turbo_graph_checkpoint_s turbo_graph_checkpoint_t;

typedef enum {
  TURBO_GRAPH_EXEC_OK = 0,
  TURBO_GRAPH_EXEC_STOP = 1,
  TURBO_GRAPH_EXEC_INTERRUPTED = 2,
  TURBO_GRAPH_EXEC_ERROR = -1,
  TURBO_GRAPH_EXEC_STEP_LIMIT = -2,
  TURBO_GRAPH_EXEC_ROUTE_NOT_FOUND = -3,
  TURBO_GRAPH_EXEC_NODE_NOT_FOUND = -4,
  TURBO_GRAPH_EXEC_INVALID_ARGUMENT = -5,
  TURBO_GRAPH_EXEC_DUPLICATE_NODE = -6,
  TURBO_GRAPH_EXEC_OUT_OF_MEMORY = -7,
  TURBO_GRAPH_EXEC_CHECKPOINT_MISMATCH = -8
} turbo_graph_exec_status_t;

typedef struct turbo_graph_exec_ctx_s {
  turbo_graph_t *graph;
  json_value_t *state;
  turbo_runtime_data_bind_value_t *bind_state;
  const char *current_node;
  const char *next_node;
  size_t step;
  int stop;
  turbo_event_sink_bind_fn event_sink;
  void *event_sink_user_data;
} turbo_graph_exec_ctx_t;

typedef int (*turbo_graph_node_fn)(turbo_graph_exec_ctx_t *ctx, void *user_data);
typedef int (*turbo_graph_bind_node_fn)(turbo_graph_exec_ctx_t *ctx, void *user_data);
typedef int (*turbo_graph_edge_predicate_fn)(const turbo_graph_exec_ctx_t *ctx, void *user_data);
typedef int (*turbo_graph_bind_edge_predicate_fn)(const turbo_graph_exec_ctx_t *ctx,
                                                  void *user_data);
typedef void (*turbo_graph_checkpoint_cb)(const turbo_graph_checkpoint_t *checkpoint,
                                          void *user_data);

typedef struct turbo_graph_run_options_s {
  size_t max_steps;
  const char *start_node;
  const char *const *interrupt_before_nodes;
  size_t interrupt_before_count;
  int skip_initial_interrupt;
  turbo_graph_checkpoint_cb checkpoint_cb;
  void *checkpoint_user_data;
} turbo_graph_run_options_t;

typedef struct turbo_graph_run_result_s {
  turbo_graph_exec_status_t status;
  const char *last_node;
  const char *next_node;
  size_t steps;
} turbo_graph_run_result_t;

/**
 * @brief Create an empty graph runtime.
 * @param name Optional graph name.
 * @return Graph handle or NULL on allocation failure.
 */
CXX_C_API turbo_graph_t *turbo_graph_create(const char *name);

/**
 * @brief Destroy a graph runtime.
 * @param graph Graph handle, may be NULL.
 */
CXX_C_API void turbo_graph_destroy(turbo_graph_t *graph);

/**
 * @brief Add a node callback to the graph.
 * @param graph Graph handle.
 * @param name Unique node name.
 * @param fn Node callback.
 * @param user_data Opaque callback pointer.
 * @return Status code.
 */
CXX_C_API turbo_graph_exec_status_t turbo_graph_add_node(turbo_graph_t *graph, const char *name,
                                                         turbo_graph_node_fn fn, void *user_data);

/**
 * @brief Add a node callback with an explicit stable semantic id.
 */
CXX_C_API turbo_graph_exec_status_t turbo_graph_add_node_ex(turbo_graph_t *graph,
                                                            const char *name,
                                                            const char *semantic_id,
                                                            turbo_graph_node_fn fn,
                                                            void *user_data);

/**
 * @brief Add a bind-native node callback to the graph.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_graph_add_bind_node(turbo_graph_t *graph, const char *name, turbo_graph_bind_node_fn fn,
                          void *user_data);

/**
 * @brief Add a bind-native node callback with an explicit stable semantic id.
 */
CXX_C_API turbo_graph_exec_status_t turbo_graph_add_bind_node_ex(turbo_graph_t *graph,
                                                                 const char *name,
                                                                 const char *semantic_id,
                                                                 turbo_graph_bind_node_fn fn,
                                                                 void *user_data);

/**
 * @brief Add a directed edge between existing nodes.
 * @param graph Graph handle.
 * @param from Source node name.
 * @param to Destination node name.
 * @param predicate Optional predicate. NULL means always match.
 * @param user_data Opaque predicate pointer.
 * @return Status code.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_graph_add_edge(turbo_graph_t *graph, const char *from, const char *to,
                     turbo_graph_edge_predicate_fn predicate, void *user_data);

/**
 * @brief Add a directed edge with an explicit stable semantic id.
 */
CXX_C_API turbo_graph_exec_status_t turbo_graph_add_edge_ex(turbo_graph_t *graph,
                                                            const char *from, const char *to,
                                                            const char *semantic_id,
                                                            turbo_graph_edge_predicate_fn predicate,
                                                            void *user_data);

/**
 * @brief Add a directed edge with a bind-native predicate.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_graph_add_bind_edge(turbo_graph_t *graph, const char *from, const char *to,
                          turbo_graph_bind_edge_predicate_fn predicate, void *user_data);

/**
 * @brief Add a directed bind-native edge with an explicit stable semantic id.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_graph_add_bind_edge_ex(turbo_graph_t *graph, const char *from, const char *to,
                             const char *semantic_id,
                             turbo_graph_bind_edge_predicate_fn predicate, void *user_data);

/**
 * @brief Set the default entry node.
 * @param graph Graph handle.
 * @param name Existing node name.
 * @return Status code.
 */
CXX_C_API turbo_graph_exec_status_t turbo_graph_set_entry(turbo_graph_t *graph,
                                                          const char *name);

/**
 * @brief Get the configured entry node.
 * @param graph Graph handle.
 * @return Entry node name or NULL.
 */
CXX_C_API const char *turbo_graph_get_entry(const turbo_graph_t *graph);

/**
 * @brief Get the number of registered nodes.
 * @param graph Graph handle.
 * @return Node count.
 */
CXX_C_API size_t turbo_graph_node_count(const turbo_graph_t *graph);

/**
 * @brief Get the number of registered edges.
 * @param graph Graph handle.
 * @return Edge count.
 */
CXX_C_API size_t turbo_graph_edge_count(const turbo_graph_t *graph);

/**
 * @brief Return the canonical stable topology id for a graph.
 * @param graph Graph handle.
 * @return Borrowed topology id string or NULL on failure.
 */
CXX_C_API const char *turbo_graph_topology_id(const turbo_graph_t *graph);

/**
 * @brief Execute the graph against a mutable JSON state value.
 * @param graph Graph handle.
 * @param state Mutable JSON state tree.
 * @param options Optional run options.
 * @param out_result Optional result summary.
 * @return Status code.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_graph_run(turbo_graph_t *graph, json_value_t *state,
                const turbo_graph_run_options_t *options,
                turbo_graph_run_result_t *out_result);

/**
 * @brief Execute the graph against a runtime data-bind state boundary.
 * @param graph Graph handle.
 * @param state Optional input state tree. NULL creates a null state.
 * @param options Optional run options.
 * @param out_result Optional result summary.
 * @param out_state Output runtime state tree owned by caller.
 * @return Status code.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_graph_run_bind(turbo_graph_t *graph, const turbo_runtime_data_bind_value_t *state,
                     const turbo_graph_run_options_t *options,
                     turbo_graph_run_result_t *out_result,
                     turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Execute the graph against a runtime data-bind state boundary and emit canonical events.
 */
CXX_C_API turbo_graph_exec_status_t turbo_graph_run_bind_stream(
    turbo_graph_t *graph, const turbo_runtime_data_bind_value_t *state,
    const turbo_graph_run_options_t *options, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data, turbo_graph_run_result_t *out_result,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Resume graph execution from a checkpoint snapshot.
 * @param graph Graph handle.
 * @param checkpoint Checkpoint created earlier by the same graph topology.
 * @param options Optional run options. `start_node` is ignored.
 * @param out_result Optional result summary.
 * @return Status code.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_graph_run_checkpoint(turbo_graph_t *graph, turbo_graph_checkpoint_t *checkpoint,
                           const turbo_graph_run_options_t *options,
                           turbo_graph_run_result_t *out_result);

/**
 * @brief Resume graph execution from a checkpoint into a bind-native state boundary.
 * @param graph Graph handle.
 * @param checkpoint Checkpoint created earlier by the same graph topology.
 * @param options Optional run options. `start_node` is ignored.
 * @param out_result Optional result summary.
 * @param out_state Output runtime state tree owned by caller.
 * @return Status code.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_graph_run_checkpoint_bind(turbo_graph_t *graph, const turbo_graph_checkpoint_t *checkpoint,
                                const turbo_graph_run_options_t *options,
                                turbo_graph_run_result_t *out_result,
                                turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Resume graph execution from a checkpoint, emit canonical events, and return bind state.
 * @param graph Graph handle.
 * @param checkpoint Checkpoint created earlier by the same graph topology.
 * @param options Optional run options. `start_node` is ignored.
 * @param event_sink Event callback receiving canonical bind-native events.
 * @param event_sink_user_data Opaque pointer passed to event_sink.
 * @param out_result Optional result summary.
 * @param out_state Output runtime state tree owned by caller.
 * @return Status code.
 */
CXX_C_API turbo_graph_exec_status_t turbo_graph_run_checkpoint_bind_stream(
    turbo_graph_t *graph, const turbo_graph_checkpoint_t *checkpoint,
    const turbo_graph_run_options_t *options, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data, turbo_graph_run_result_t *out_result,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Override the next node from inside a node callback.
 * @param ctx Execution context.
 * @param next_node Existing node name.
 * @return Status code.
 */
CXX_C_API turbo_graph_exec_status_t turbo_graph_ctx_set_next(turbo_graph_exec_ctx_t *ctx,
                                                             const char *next_node);

/**
 * @brief Stop execution successfully from inside a node callback.
 * @param ctx Execution context.
 */
CXX_C_API void turbo_graph_ctx_stop(turbo_graph_exec_ctx_t *ctx);

/**
 * @brief Create a checkpoint that can later resume graph execution.
 * @param next_node Next node to execute on resume.
 * @param steps Number of nodes already executed.
 * @param state State snapshot to clone.
 * @param out_checkpoint Output checkpoint pointer.
 * @return Status code.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_graph_checkpoint_create(const char *next_node, size_t steps, const json_value_t *state,
                              turbo_graph_checkpoint_t **out_checkpoint);

/**
 * @brief Create a checkpoint from a runtime data-bind state snapshot.
 * @param next_node Next node to execute on resume.
 * @param steps Number of nodes already executed.
 * @param state State snapshot to clone. NULL stores a null state.
 * @param out_checkpoint Output checkpoint pointer.
 * @return Status code.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_graph_checkpoint_create_bind(const char *next_node, size_t steps,
                                   const turbo_runtime_data_bind_value_t *state,
                                   turbo_graph_checkpoint_t **out_checkpoint);

/**
 * @brief Clone a checkpoint, including its topology id and state snapshot.
 * @param checkpoint Source checkpoint.
 * @param out_checkpoint Output checkpoint pointer.
 * @return Status code.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_graph_checkpoint_clone(const turbo_graph_checkpoint_t *checkpoint,
                             turbo_graph_checkpoint_t **out_checkpoint);

/**
 * @brief Return the current serialized checkpoint schema version.
 * @return Checkpoint schema version number.
 */
CXX_C_API size_t turbo_graph_checkpoint_schema_version(void);

/**
 * @brief Get the canonical stable topology id stored in a checkpoint.
 * @param checkpoint Checkpoint handle.
 * @return Borrowed topology id string or NULL.
 */
CXX_C_API const char *turbo_graph_checkpoint_topology_id(
    const turbo_graph_checkpoint_t *checkpoint);

/**
 * @brief Destroy a checkpoint.
 * @param checkpoint Checkpoint handle, may be NULL.
 */
CXX_C_API void turbo_graph_checkpoint_destroy(turbo_graph_checkpoint_t *checkpoint);

/**
 * @brief Serialize a checkpoint to JSON.
 * @param checkpoint Checkpoint handle.
 * @param out_len Optional serialized byte count.
 * @return Allocated JSON string or NULL on failure.
 */
CXX_C_API char *turbo_graph_checkpoint_serialize(const turbo_graph_checkpoint_t *checkpoint,
                                                 size_t *out_len);

/**
 * @brief Parse a checkpoint from JSON.
 * @param json Serialized checkpoint data.
 * @param len Byte length of json.
 * @param out_checkpoint Output checkpoint pointer.
 * @return Status code.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_graph_checkpoint_deserialize(const char *json, size_t len,
                                   turbo_graph_checkpoint_t **out_checkpoint);

/**
 * @brief Get the next node stored in a checkpoint.
 * @param checkpoint Checkpoint handle.
 * @return Node name or NULL.
 */
CXX_C_API const char *turbo_graph_checkpoint_next_node(
    const turbo_graph_checkpoint_t *checkpoint);

/**
 * @brief Get the executed step count stored in a checkpoint.
 * @param checkpoint Checkpoint handle.
 * @return Executed steps.
 */
CXX_C_API size_t turbo_graph_checkpoint_steps(const turbo_graph_checkpoint_t *checkpoint);

/**
 * @brief Get the mutable state snapshot owned by a checkpoint.
 * @param checkpoint Checkpoint handle.
 * @return State snapshot or NULL.
 */
CXX_C_API json_value_t *turbo_graph_checkpoint_state(turbo_graph_checkpoint_t *checkpoint);

/**
 * @brief Return a runtime data-bind clone of the checkpoint state.
 * @param checkpoint Checkpoint handle.
 * @return Owned runtime state tree or NULL on failure.
 */
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_graph_checkpoint_state_bind(const turbo_graph_checkpoint_t *checkpoint);

#ifdef __cplusplus
}
#endif

#endif
