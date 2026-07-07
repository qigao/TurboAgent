#ifndef TURBO_AGENT_SUBGRAPH_H
#define TURBO_AGENT_SUBGRAPH_H

#include <platform.h>

#include "turbo_agent_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_subgraph_node_config_s {
  turbo_agent_runtime_t *runtime;
  turbo_graph_t *subgraph;
  const turbo_graph_run_options_t *options;
  const char *thread_id;
  const char *parent_graph_run_id;
  const char *call_frame_id;
  const char *output_key;
} turbo_agent_subgraph_node_config_t;

/**
 * @brief Execute one child graph as a bind-native graph node.
 *
 * The node starts `config->subgraph` through `config->runtime`, using the
 * current parent runtime execution context as lineage metadata. It writes one
 * result envelope into the parent bind state under `output_key` or
 * `subgraph_result` when no output key is configured.
 *
 * The result envelope contains durable child ids, parent lineage,
 * `parent_graph_run_id`, `call_frame_id`, status booleans, pending checkpoint
 * hints for interrupted child graphs, the child runtime `summary`, and the
 * child final `state`. The child run/checkpoint records remain the single
 * durable source of truth.
 */
CXX_C_API int turbo_agent_subgraph_node(turbo_graph_exec_ctx_t *ctx, void *user_data);

/**
 * @brief Install one graph-native subgraph node.
 *
 * The caller owns `config` and must keep it valid while the parent graph can
 * execute the installed node.
 */
CXX_C_API turbo_graph_exec_status_t turbo_agent_install_subgraph_node(
    turbo_graph_t *graph, const char *node_name, const char *semantic_id,
    const turbo_agent_subgraph_node_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
