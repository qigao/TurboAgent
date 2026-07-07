#ifndef TURBO_AGENT_KNOWLEDGE_STORE_H
#define TURBO_AGENT_KNOWLEDGE_STORE_H

#include <platform.h>
#include <stddef.h>
#include <stdint.h>

#include "turbo_action_tool.h"
#include "turbo_agent_graph.h"
#include "turbo_parser.h"
#include "turbo_tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_knowledge_store_s turbo_agent_knowledge_store_t;

typedef struct turbo_agent_knowledge_document_s {
  const char *id;
  const char *uri;
  const char *kind;
  const char *title;
  int64_t mtime;
  int64_t size;
  const char *content_hash;
  const char *metadata_json;
} turbo_agent_knowledge_document_t;

typedef struct turbo_agent_knowledge_context_config_s {
  turbo_agent_knowledge_store_t *store;
  const char *query;
  const char *kind;
  const char *uri_prefix;
  size_t limit;
} turbo_agent_knowledge_context_config_t;

typedef struct turbo_agent_knowledge_directory_options_s {
  const char *kind;
  size_t chunk_target_bytes;
  int recursive;
  size_t max_files;
  size_t max_file_bytes;
  const char *include_extensions;
} turbo_agent_knowledge_directory_options_t;

CXX_C_API turbo_agent_knowledge_store_t *
turbo_agent_knowledge_store_sqlite_open(const char *db_path);

CXX_C_API void
turbo_agent_knowledge_store_close(turbo_agent_knowledge_store_t *store);

CXX_C_API int turbo_agent_knowledge_store_upsert_text(
    turbo_agent_knowledge_store_t *store,
    const turbo_agent_knowledge_document_t *document, const char *text,
    size_t chunk_target_bytes);

CXX_C_API int turbo_agent_knowledge_store_index_file(
    turbo_agent_knowledge_store_t *store, const char *path, const char *kind,
    size_t chunk_target_bytes);

CXX_C_API int turbo_agent_knowledge_store_index_directory(
    turbo_agent_knowledge_store_t *store, const char *root_dir, const char *kind,
    size_t chunk_target_bytes, int recursive, size_t max_files,
    json_value_t **out_summary_json);

CXX_C_API int turbo_agent_knowledge_store_index_directory_ex(
    turbo_agent_knowledge_store_t *store, const char *root_dir,
    const turbo_agent_knowledge_directory_options_t *options,
    json_value_t **out_summary_json);

CXX_C_API int turbo_agent_knowledge_store_delete_document(
    turbo_agent_knowledge_store_t *store, const char *document_id);

CXX_C_API int turbo_agent_knowledge_store_query(
    turbo_agent_knowledge_store_t *store, const char *query, size_t limit,
    json_value_t **out_results_json);

CXX_C_API int turbo_agent_knowledge_store_query_ex(
    turbo_agent_knowledge_store_t *store, const char *query, const char *kind,
    const char *uri_prefix, size_t limit, json_value_t **out_results_json);

CXX_C_API int turbo_agent_knowledge_store_list_documents(
    turbo_agent_knowledge_store_t *store, const char *kind,
    const char *uri_prefix, size_t limit, json_value_t **out_documents_json);

CXX_C_API int turbo_agent_knowledge_store_get_document(
    turbo_agent_knowledge_store_t *store, const char *document_id,
    json_value_t **out_document_json);

CXX_C_API int turbo_agent_knowledge_store_load_context(
    turbo_agent_knowledge_store_t *store, json_value_t *state, const char *query,
    size_t limit);

CXX_C_API int turbo_agent_knowledge_store_load_context_ex(
    turbo_agent_knowledge_store_t *store, json_value_t *state, const char *query,
    const char *kind, const char *uri_prefix, size_t limit);

CXX_C_API int turbo_agent_knowledge_store_build_context(
    turbo_agent_knowledge_store_t *store, const char *query, const char *kind,
    const char *uri_prefix, size_t limit, json_value_t **out_context_json);

CXX_C_API int turbo_agent_knowledge_store_stats(
    turbo_agent_knowledge_store_t *store, json_value_t **out_stats_json);

CXX_C_API int turbo_agent_knowledge_store_tool_graph(
    json_value_t **out_graph_json);

CXX_C_API int turbo_agent_knowledge_store_add_tools(
    turbo_tool_registry_t *registry, turbo_agent_knowledge_store_t *store);

CXX_C_API int turbo_agent_knowledge_store_add_action_tools(
    turbo_action_tool_registry_t *registry, turbo_agent_knowledge_store_t *store);

CXX_C_API int turbo_agent_knowledge_context_node(turbo_graph_exec_ctx_t *ctx,
                                                 void *user_data);

CXX_C_API turbo_graph_exec_status_t
turbo_agent_install_knowledge_engineering_loop(
    turbo_graph_t *graph, const turbo_agent_knowledge_context_config_t *knowledge_config,
    turbo_agent_t *planner_agent, turbo_agent_t *executor_agent,
    const char *knowledge_node_name, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *review_node_name, const char *executor_node_name,
    const char *tool_node_name, const char *detect_failure_node_name,
    const char *replan_route_node_name, const char *replan_prepare_node_name,
    const char *plan_advance_node_name, const char *end_node_name, int set_entry);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_AGENT_KNOWLEDGE_STORE_H */
