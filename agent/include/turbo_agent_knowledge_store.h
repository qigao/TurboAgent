#ifndef TURBO_AGENT_KNOWLEDGE_STORE_H
#define TURBO_AGENT_KNOWLEDGE_STORE_H

#include <turbo_agent_api.h>
#include <stddef.h>
#include <stdint.h>

#include "turbo_action_tool.h"
#include "turbo_agent_graph.h"
#include <json_parser.h>
#include "turbo_tool_registry.h"
#include "turbo_diagram.h"

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

#define TURBO_KNOWLEDGE_DIAGRAM_MAX_DOCUMENTS 32u
#define TURBO_KNOWLEDGE_DIAGRAM_MAX_FIELD_BYTES 1024u
#define TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS 1u
#define TURBO_KNOWLEDGE_DIAGRAM_MAX_DOCUMENT_BYTES (256u * 1024u)
#define TURBO_KNOWLEDGE_DIAGRAM_MAX_SELECTION_BYTES (1024u * 1024u)

/** Project selected indexed documents and all their chunks into a knowledge view.
 * document_ids contains 1..MAX_DOCUMENTS unique nonempty UTF-8 IDs; selected text
 * fields are bounded to MAX_FIELD_BYTES. Total nodes/edges obey turbo_diagram.h.
 * Document order follows input, chunks follow ascending nonnegative ordinal.
 * IDs are document:<id>/chunk:<id>; contains edges carry relation="contains".
 * Nodes preserve collection, record_id and stored document content_hash. Chunk
 * identity is revision-relative: retain its document_id and content_hash too.
 * freshness="indexed_snapshot" does not assert that external source files are
 * current. No text/metadata payload, inferred cross-document relation or source
 * file access is included. Empty documents still produce a document node.
 *
 * Borrow store and IDs only during the synchronous call. Serialize all uses of
 * this store, including close; independent connections may write concurrently.
 * Own a read transaction across the selection; reject an already active one.
 * Clear out_view on entry; publish an independent owned JSON tree only after
 * success, release with json_free. Failure rolls back and returns no view.
 * Return OK, EINVAL for bad input (including selection count)/duplicates, ENOENT for an unknown document,
 * EPROTO for invalid stored records/schema, ENOSPC for bounds, EBUSY for locking
 * or active transaction, ENOMEM for allocation, EIO for other SQLite failures.
 * Output bounds do not cap SQLite database size or execution time.
 * Example: const char *ids[] = {"readme"};
 * turbo_agent_knowledge_store_diagram(store, ids, 1, &view); check status;
 * turbo_diagram_mermaid(view, &text); check status; tstr_free(text); json_free(view).
 */
CXX_C_API int turbo_agent_knowledge_store_diagram(turbo_agent_knowledge_store_t *store,
    const char *const *document_ids, size_t document_count, json_value_t **out_view);

/** Same ownership, snapshot and error contract as diagram; flags=0 is identical.
 * URI_MENTIONS additionally reads bounded indexed text and adds mentions_uri
 * edges for literal case-sensitive occurrences of selected document URIs.
 * Require boundaries at start/end, ASCII whitespace, quotes, backticks, angle,
 * square or curly brackets, or parentheses. No relative path/escape/fragment
 * resolution, Markdown semantics, code dependency inference or external I/O.
 * Unknown flag bits return EINVAL. Text is copied one document at a time, up to
 * MAX_DOCUMENT_BYTES per document and MAX_SELECTION_BYTES overall; bounds or
 * edge overflow return ENOSPC. Contiguous ordinals from zero and exact stored
 * byte size are required; invalid UTF-8/NUL or inconsistent records return EPROTO.
 * Each occurrence retains source/target hashes and source byte offset/length,
 * starting source_chunk_id/source_chunk_byte_offset, ending end_chunk_id and
 * end_chunk_byte_offset_exclusive. These refer to the indexed revision only.
 * Root uri_mention_scope="selected_document_uris" states the search boundary.
 * Text is never returned. Default diagram does not read text or impose these
 * extra reconstruction requirements. Example: use URI_MENTIONS here, then an
 * incoming neighborhood around a code document to inspect indexed mentions.
 */
CXX_C_API int turbo_agent_knowledge_store_diagram_ex(turbo_agent_knowledge_store_t *store,
    const char *const *document_ids, size_t document_count, unsigned flags, json_value_t **out_view);

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
