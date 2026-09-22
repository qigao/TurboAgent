#ifndef TURBO_VECTOR_STORE_H
#define TURBO_VECTOR_STORE_H

#include <turbo_agent_api.h>
#include <stddef.h>

#include <json_parser.h>
#include "turbo_text_splitter.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_embedding_model_s turbo_embedding_model_t;
typedef struct turbo_action_tool_registry_s turbo_action_tool_registry_t;
typedef struct turbo_tool_registry_s turbo_tool_registry_t;
typedef struct turbo_vector_store_tool_binding_s turbo_vector_store_tool_binding_t;
typedef struct turbo_vector_store_s turbo_vector_store_t;

typedef struct turbo_vector_store_document_s {
  const char *id;
  const char *uri;
  const char *kind;
  const char *title;
  const char *text;
  const json_value_t *metadata_json;
} turbo_vector_store_document_t;

typedef struct turbo_vector_store_query_options_s {
  const char *kind;
  const char *uri_prefix;
  size_t limit;
} turbo_vector_store_query_options_t;

typedef struct turbo_vector_store_directory_options_s {
  const char *kind;
  turbo_text_splitter_options_t splitter_options;
  int recursive;
  size_t max_files;
  size_t max_file_bytes;
  const char *include_extensions;
} turbo_vector_store_directory_options_t;

CXX_C_API turbo_vector_store_t *turbo_vector_store_create_memory(void);

CXX_C_API turbo_vector_store_t *turbo_vector_store_sqlite_open(
    const char *db_path);

CXX_C_API void turbo_vector_store_destroy(turbo_vector_store_t *store);

CXX_C_API int turbo_vector_store_upsert(
    turbo_vector_store_t *store, const turbo_vector_store_document_t *document,
    const json_value_t *embedding_json);

CXX_C_API int turbo_vector_store_delete_document(turbo_vector_store_t *store,
                                                 const char *document_id);

CXX_C_API int turbo_vector_store_query(
    turbo_vector_store_t *store, const json_value_t *query_embedding_json,
    const turbo_vector_store_query_options_t *options,
    json_value_t **out_results_json);

CXX_C_API int turbo_vector_store_index_text(
    turbo_vector_store_t *store, turbo_embedding_model_t *embedding_model,
    const turbo_vector_store_document_t *document,
    const turbo_text_splitter_options_t *splitter_options,
    json_value_t **out_summary_json);

CXX_C_API int turbo_vector_store_index_text_file(
    turbo_vector_store_t *store, turbo_embedding_model_t *embedding_model,
    const char *path, const char *kind,
    const turbo_text_splitter_options_t *splitter_options,
    json_value_t **out_summary_json);

CXX_C_API int turbo_vector_store_index_directory(
    turbo_vector_store_t *store, turbo_embedding_model_t *embedding_model,
    const char *root_dir, const char *kind,
    const turbo_text_splitter_options_t *splitter_options, int recursive,
    size_t max_files, json_value_t **out_summary_json);

CXX_C_API int turbo_vector_store_index_directory_ex(
    turbo_vector_store_t *store, turbo_embedding_model_t *embedding_model,
    const char *root_dir,
    const turbo_vector_store_directory_options_t *options,
    json_value_t **out_summary_json);

CXX_C_API size_t turbo_vector_store_count(const turbo_vector_store_t *store);

CXX_C_API turbo_vector_store_tool_binding_t *
turbo_vector_store_tool_binding_create(turbo_vector_store_t *store,
                                       turbo_embedding_model_t *embedding_model);

CXX_C_API void turbo_vector_store_tool_binding_destroy(
    turbo_vector_store_tool_binding_t *binding);

CXX_C_API int turbo_vector_store_tool_graph(json_value_t **out_graph_json);

CXX_C_API int turbo_vector_store_add_tools(
    turbo_tool_registry_t *registry, turbo_vector_store_tool_binding_t *binding);

CXX_C_API int turbo_vector_store_add_action_tools(
    turbo_action_tool_registry_t *registry,
    turbo_vector_store_tool_binding_t *binding);

CXX_C_API int turbo_vector_store_add_indexing_action_tools(
    turbo_action_tool_registry_t *registry,
    turbo_vector_store_tool_binding_t *binding);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_VECTOR_STORE_H */
