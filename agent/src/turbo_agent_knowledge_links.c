#include "turbo_agent_knowledge_store_internal.h"
#include <string.h>

typedef struct chunk_span_s {
  const json_value_t *node;
  size_t begin, end;
} chunk_span_t;

static int uri_boundary(unsigned char c) {
  return c == ' ' || (c >= '\t' && c <= '\r') ||
      c == '\'' || c == '"' || c == '`' || c == '<' || c == '>' ||
      c == '[' || c == ']' || c == '{' || c == '}' || c == '(' || c == ')';
}

static const json_value_t *find_chunk(const json_value_t *view, const char *id) {
  const json_value_t *nodes = json_object_get(view, "nodes");
  for (size_t i = 0; i < json_array_size(nodes); ++i) {
    const json_value_t *node = json_array_get(nodes, i);
    if (!strcmp(json_get_string(node, "collection"), "chunks") &&
        !strcmp(json_get_string(node, "record_id"), id)) return node;
  }
  return NULL;
}

/* Reassemble before UTF-8 validation: existing byte chunks may split a codepoint.
 * Only one bounded body is owned at a time; spans borrow immutable view nodes. */
static int read_body(sqlite3_stmt *stmt, const json_value_t *view,
    const json_value_t *document, size_t *total, tstr *body,
    chunk_span_t *spans, size_t *count) {
  int status, rc;
  *count = 0;
  rc = turbo_knowledge_diagram_database_status(sqlite3_bind_text(stmt, 1,
      json_get_string(document, "record_id"), -1, SQLITE_STATIC));
  if (rc != SALTS_OK) return rc;
  while ((status = sqlite3_step(stmt)) == SQLITE_ROW) {
    const json_value_t *chunk;
    const char *id, *type;
    const void *bytes;
    sqlite3_int64 length;
    size_t size, begin = tstr_len(*body);
    tstr next;
    if (*count == TURBO_DIAGRAM_MAX_NODES) return SALTS_ENOSPC;
    if (sqlite3_column_type(stmt, 0) != SQLITE_TEXT ||
        sqlite3_column_type(stmt, 1) != SQLITE_INTEGER ||
        sqlite3_column_int64(stmt, 1) != (sqlite3_int64)*count ||
        sqlite3_column_type(stmt, 3) != SQLITE_INTEGER) return SALTS_EPROTO;
    id = (const char *)sqlite3_column_text(stmt, 0);
    type = (const char *)sqlite3_column_text(stmt, 2);
    if (!id || !type) return SALTS_ENOMEM;
    if (strcmp(type, "text")) return SALTS_EPROTO;
    length = sqlite3_column_int64(stmt, 3);
    if (length < 0) return SALTS_EPROTO;
    if (length > (sqlite3_int64)(TURBO_KNOWLEDGE_DIAGRAM_MAX_DOCUMENT_BYTES - begin) ||
        length > (sqlite3_int64)(TURBO_KNOWLEDGE_DIAGRAM_MAX_SELECTION_BYTES - *total)) return SALTS_ENOSPC;
    size = (size_t)length;
    bytes = sqlite3_column_blob(stmt, 4);
    if (size && !bytes) return SALTS_ENOMEM;
    if ((size_t)sqlite3_column_bytes(stmt, 4) != size ||
        (size && memchr(bytes, 0, size))) return SALTS_EPROTO;
    chunk = find_chunk(view, id);
    if (!chunk || strcmp(json_get_string(chunk, "document_id"), json_get_string(document, "record_id")) ||
        turbo_runtime_json_value_as_int64(json_object_get(chunk, "ordinal"), -1) != (int64_t)*count)
      return SALTS_EPROTO;
    if (size) {
      next = tstr_cat_len(*body, bytes, size);
      if (!next) return SALTS_ENOMEM;
      *body = next;
    }
    spans[(*count)++] = (chunk_span_t){chunk, begin, begin + size};
    *total += size;
  }
  if (status != SQLITE_DONE) return turbo_knowledge_diagram_database_status(status);
  if (turbo_runtime_json_value_as_int64(json_object_get(document, "size"), -1) != (int64_t)tstr_len(*body) ||
      !vstr_utf8_valid(vstr_from_buf(*body, tstr_len(*body)))) return SALTS_EPROTO;
  return turbo_knowledge_diagram_database_status(sqlite3_reset(stmt));
}

static int add_mention(json_value_t *view, const json_value_t *source,
    const json_value_t *target, const chunk_span_t *spans, size_t count,
    size_t offset, size_t length) {
  const chunk_span_t *first = NULL, *last = NULL;
  json_value_t *edges, *edge;
  int rc;
  for (size_t i = 0; i < count; ++i) {
    if (spans[i].begin <= offset && offset < spans[i].end) first = &spans[i];
    if (spans[i].begin <= offset + length - 1 && offset + length - 1 < spans[i].end) last = &spans[i];
  }
  if (!first || !last) return SALTS_EPROTO;
  rc = turbo_diagram_add_edge(view, json_get_string(source, "id"), json_get_string(target, "id"), "mentions URI");
  if (rc != SALTS_OK) return rc;
  edges = json_object_get(view, "edges"); edge = json_array_get(edges, json_array_size(edges) - 1);
  if (turbo_runtime_json_object_set_string(edge, "relation", "mentions_uri") != 0 ||
      turbo_runtime_json_object_set_string(edge, "extractor", "knowledge-store/literal-uri-v1") != 0 ||
      turbo_runtime_json_object_set_string(edge, "evidence", "chunks.text") != 0 ||
      turbo_runtime_json_object_set_string(edge, "source_content_hash", json_get_string(source, "content_hash")) != 0 ||
      turbo_runtime_json_object_set_string(edge, "target_content_hash", json_get_string(target, "content_hash")) != 0 ||
      turbo_runtime_json_object_set_int64(edge, "source_byte_offset", (int64_t)offset) != 0 ||
      turbo_runtime_json_object_set_int64(edge, "source_byte_length", (int64_t)length) != 0 ||
      turbo_runtime_json_object_set_string(edge, "source_chunk_id", json_get_string(first->node, "record_id")) != 0 ||
      turbo_runtime_json_object_set_int64(edge, "source_chunk_byte_offset", (int64_t)(offset - first->begin)) != 0 ||
      turbo_runtime_json_object_set_string(edge, "end_chunk_id", json_get_string(last->node, "record_id")) != 0 ||
      turbo_runtime_json_object_set_int64(edge, "end_chunk_byte_offset_exclusive", (int64_t)(offset + length - last->begin)) != 0)
    return SALTS_ENOMEM;
  return SALTS_OK;
}

int turbo_knowledge_diagram_add_uri_mentions(sqlite3 *db, json_value_t *view) {
  const json_value_t *documents[TURBO_KNOWLEDGE_DIAGRAM_MAX_DOCUMENTS];
  const json_value_t *nodes = json_object_get(view, "nodes");
  chunk_span_t spans[TURBO_DIAGRAM_MAX_NODES];
  size_t document_count = 0, total = 0;
  sqlite3_stmt *stmt = NULL;
  tstr body = NULL;
  int rc;
  for (size_t i = 0; i < json_array_size(nodes); ++i) {
    const json_value_t *node = json_array_get(nodes, i);
    if (!strcmp(json_get_string(node, "collection"), "documents")) {
      if (document_count == TURBO_KNOWLEDGE_DIAGRAM_MAX_DOCUMENTS) return SALTS_ENOSPC;
      documents[document_count++] = node;
    }
  }
  rc = turbo_knowledge_diagram_database_status(sqlite3_prepare_v2(db,
      "SELECT id,ordinal,typeof(text),length(CAST(text AS BLOB)),"
      "CASE WHEN length(CAST(text AS BLOB))<=?2 THEN CAST(text AS BLOB) END "
      "FROM chunks WHERE document_id=?1 ORDER BY ordinal,id LIMIT ?3", -1, &stmt, NULL));
  if (rc != SALTS_OK) goto done;
  rc = turbo_knowledge_diagram_database_status(sqlite3_bind_int64(stmt, 2, TURBO_KNOWLEDGE_DIAGRAM_MAX_DOCUMENT_BYTES));
  if (rc != SALTS_OK) goto done;
  rc = turbo_knowledge_diagram_database_status(sqlite3_bind_int64(stmt, 3, TURBO_DIAGRAM_MAX_NODES + 1));
  if (rc != SALTS_OK) goto done;
  for (size_t i = 0; i < document_count; ++i) {
    size_t count;
    vstr source;
    body = tstr_new();
    if (!body) { rc = SALTS_ENOMEM; goto done; }
    rc = read_body(stmt, view, documents[i], &total, &body, spans, &count);
    if (rc != SALTS_OK) goto done;
    source = vstr_from_buf(body, tstr_len(body));
    /* Bounded selected targets, then occurrence order. Never keep a body view
     * across an append or free; no normalization or semantic link inference. */
    for (size_t j = 0; j < document_count; ++j) {
      vstr needle = vstr_from_cstr(json_get_string(documents[j], "uri"));
      size_t position = 0;
      while (position < source.len) {
        size_t found = vstr_find(vstr_sub(source, position, source.len - position), needle);
        size_t end;
        if (found == VSTR_NPOS) break;
        found += position; end = found + needle.len;
        if ((!found || uri_boundary((unsigned char)body[found - 1])) &&
            (end == source.len || uri_boundary((unsigned char)body[end]))) {
          rc = add_mention(view, documents[i], documents[j], spans, count, found, needle.len);
          if (rc != SALTS_OK) goto done;
        }
        position = found + 1;
      }
    }
    tstr_free(body); body = NULL;
  }
  if (turbo_runtime_json_object_set_string(view, "uri_mention_scope", "selected_document_uris") != 0 ||
      turbo_runtime_json_object_set_string(view, "uri_mention_extractor", "knowledge-store/literal-uri-v1") != 0)
    rc = SALTS_ENOMEM;
done:
  tstr_free(body);
  {
    int status = sqlite3_finalize(stmt);
    if (rc == SALTS_OK) rc = turbo_knowledge_diagram_database_status(status);
  }
  return rc;
}
