#ifndef TURBO_RETRIEVER_H
#define TURBO_RETRIEVER_H

#include <platform.h>
#include <stddef.h>

#include "turbo_agent_workflow.h"
#include "turbo_graph.h"
#include <json_parser.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_knowledge_store_s turbo_agent_knowledge_store_t;
typedef struct turbo_embedding_model_s turbo_embedding_model_t;
typedef struct turbo_retriever_s turbo_retriever_t;
typedef struct turbo_vector_store_s turbo_vector_store_t;

typedef struct turbo_retriever_query_options_s {
  const char *kind;
  const char *uri_prefix;
  size_t limit;
} turbo_retriever_query_options_t;

typedef struct turbo_retriever_context_config_s {
  turbo_retriever_t *retriever;
  const char *query;
  const char *kind;
  const char *uri_prefix;
  size_t limit;
  const char *scope;
} turbo_retriever_context_config_t;

typedef int (*turbo_retriever_query_fn)(
    void *user_data, const char *query,
    const turbo_retriever_query_options_t *options, json_value_t **out_results_json);

typedef void (*turbo_retriever_user_data_free_fn)(void *user_data);

typedef struct turbo_retriever_config_s {
  turbo_retriever_query_fn query;
  void *user_data;
  turbo_retriever_user_data_free_fn user_data_free;
} turbo_retriever_config_t;

CXX_C_API turbo_retriever_t *
turbo_retriever_create(const turbo_retriever_config_t *config);

CXX_C_API void turbo_retriever_destroy(turbo_retriever_t *retriever);

CXX_C_API int turbo_retriever_query(
    turbo_retriever_t *retriever, const char *query,
    const turbo_retriever_query_options_t *options, json_value_t **out_results_json);

CXX_C_API int turbo_retriever_build_context(
    turbo_retriever_t *retriever, const char *query,
    const turbo_retriever_query_options_t *options, json_value_t **out_context_json);

CXX_C_API int turbo_retriever_load_context(
    turbo_retriever_t *retriever, json_value_t *state, const char *query,
    const turbo_retriever_query_options_t *options, const char *scope);

CXX_C_API int turbo_retriever_context_node(turbo_graph_exec_ctx_t *ctx,
                                           void *user_data);

CXX_C_API turbo_graph_exec_status_t
turbo_agent_install_retriever_engineering_loop(
    turbo_graph_t *graph, const turbo_retriever_context_config_t *retriever_config,
    turbo_agent_t *planner_agent, turbo_agent_t *executor_agent,
    const char *retriever_node_name, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *review_node_name, const char *executor_node_name,
    const char *tool_node_name, const char *detect_failure_node_name,
    const char *replan_route_node_name, const char *replan_prepare_node_name,
    const char *plan_advance_node_name, const char *end_node_name, int set_entry);

/*
 * Create a retriever adapter over an existing knowledge store.
 * The returned retriever borrows `store`; callers must keep the store alive
 * until after `turbo_retriever_destroy(...)`.
 */
CXX_C_API turbo_retriever_t *
turbo_retriever_from_knowledge_store(turbo_agent_knowledge_store_t *store);

/*
 * Create a retriever adapter over an existing vector store and embedding model.
 * The returned retriever borrows both values; callers must keep them alive until
 * after `turbo_retriever_destroy(...)`.
 */
CXX_C_API turbo_retriever_t *turbo_retriever_from_vector_store(
    turbo_vector_store_t *store, turbo_embedding_model_t *embedding_model);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_RETRIEVER_H */
