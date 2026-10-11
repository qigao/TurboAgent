#include "turbo_agent_knowledge_store_internal.h"
#include <fmt.h>
#include <string.h>

int turbo_knowledge_diagram_database_status(int rc) {
  switch (rc & 255) {
  case SQLITE_OK: return SALTS_OK;
  case SQLITE_BUSY: case SQLITE_LOCKED: return SALTS_EBUSY;
  case SQLITE_NOMEM: return SALTS_ENOMEM;
  case SQLITE_TOOBIG: return SALTS_ENOSPC;
  case SQLITE_ERROR: case SQLITE_SCHEMA: case SQLITE_CORRUPT: case SQLITE_NOTADB: return SALTS_EPROTO;
  default: return SALTS_EIO;
  }
}

static int valid_text(const char *value, size_t length, int empty) {
  if (!value || (!empty && !length) || length > TURBO_KNOWLEDGE_DIAGRAM_MAX_FIELD_BYTES ||
      strlen(value) != length || !vstr_utf8_valid(vstr_from_cstr(value))) return 0;
  for (size_t i = 0; i < length; ++i) {
    unsigned char c = (unsigned char)value[i];
    if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127) return 0;
  }
  return 1;
}

static int column_text(sqlite3_stmt *stmt, int column, int optional, const char **out) {
  int size;
  *out = NULL;
  if (optional && sqlite3_column_type(stmt, column) == SQLITE_NULL) { *out = ""; return SALTS_OK; }
  if (sqlite3_column_type(stmt, column) != SQLITE_TEXT) return SALTS_EPROTO;
  size = sqlite3_column_bytes(stmt, column);
  if (size > TURBO_KNOWLEDGE_DIAGRAM_MAX_FIELD_BYTES) return SALTS_ENOSPC;
  *out = (const char *)sqlite3_column_text(stmt, column);
  if (!*out) return SALTS_ENOMEM;
  return valid_text(*out, (size_t)size, optional) ? SALTS_OK : SALTS_EPROTO;
}

static int document_node(json_value_t *view, sqlite3_stmt *stmt, const char *requested,
    json_value_t **out_node) {
  const char *id, *uri, *kind, *title, *hash;
  tstr node_id = NULL;
  json_value_t *nodes, *node;
  int rc;
  *out_node = NULL;
  if ((rc = column_text(stmt, 0, 0, &id)) != 0 || (rc = column_text(stmt, 1, 0, &uri)) != 0 ||
      (rc = column_text(stmt, 2, 0, &kind)) != 0 || (rc = column_text(stmt, 3, 1, &title)) != 0 ||
      (rc = column_text(stmt, 4, 0, &hash)) != 0) return rc;
  if (strcmp(id, requested) || sqlite3_column_type(stmt, 5) != SQLITE_INTEGER ||
      sqlite3_column_type(stmt, 6) != SQLITE_INTEGER || sqlite3_column_type(stmt, 7) != SQLITE_INTEGER ||
      sqlite3_column_int64(stmt, 6) < 0) return SALTS_EPROTO;
  node_id = tstr_format("document:{}", id);
  if (!node_id) return SALTS_ENOMEM;
  rc = turbo_diagram_add_node(view, node_id, title[0] ? title : id);
  tstr_free(node_id);
  if (rc != SALTS_OK) return rc;
  nodes = json_object_get(view, "nodes"); node = json_array_get(nodes, json_array_size(nodes) - 1);
  if (turbo_runtime_json_object_set_string(node, "collection", "documents") != 0 ||
      turbo_runtime_json_object_set_string(node, "record_id", id) != 0 ||
      turbo_runtime_json_object_set_string(node, "uri", uri) != 0 ||
      turbo_runtime_json_object_set_string(node, "artifact_kind", kind) != 0 ||
      turbo_runtime_json_object_set_string(node, "content_hash", hash) != 0 ||
      turbo_runtime_json_object_set_int64(node, "mtime", sqlite3_column_int64(stmt, 5)) != 0 ||
      turbo_runtime_json_object_set_int64(node, "size", sqlite3_column_int64(stmt, 6)) != 0 ||
      turbo_runtime_json_object_set_int64(node, "indexed_at", sqlite3_column_int64(stmt, 7)) != 0) return SALTS_ENOMEM;
  *out_node = node;
  return SALTS_OK;
}

static int chunk_node(json_value_t *view, const json_value_t *document, sqlite3_stmt *stmt,
    int64_t *previous_ordinal) {
  const char *id;
  tstr node_id = NULL, label = NULL;
  json_value_t *nodes, *node, *edges, *edge;
  int64_t ordinal;
  int rc = column_text(stmt, 0, 0, &id);
  if (rc != SALTS_OK) return rc;
  ordinal = sqlite3_column_int64(stmt, 1);
  if (sqlite3_column_type(stmt, 1) != SQLITE_INTEGER || ordinal < 0 ||
      ordinal <= *previous_ordinal) return SALTS_EPROTO;
  node_id = tstr_format("chunk:{}", id); label = tstr_format("Chunk {}", ordinal);
  if (!node_id || !label) { rc = SALTS_ENOMEM; goto done; }
  rc = turbo_diagram_add_node(view, node_id, label);
  if (rc != SALTS_OK) goto done;
  nodes = json_object_get(view, "nodes"); node = json_array_get(nodes, json_array_size(nodes) - 1);
  if (turbo_runtime_json_object_set_string(node, "collection", "chunks") != 0 ||
      turbo_runtime_json_object_set_string(node, "record_id", id) != 0 ||
      turbo_runtime_json_object_set_string(node, "document_id", json_get_string(document, "record_id")) != 0 ||
      turbo_runtime_json_object_set_string(node, "content_hash", json_get_string(document, "content_hash")) != 0 ||
      turbo_runtime_json_object_set_int64(node, "ordinal", ordinal) != 0) { rc = SALTS_ENOMEM; goto done; }
  rc = turbo_diagram_add_edge(view, json_get_string(document, "id"), node_id, "contains");
  if (rc != SALTS_OK) goto done;
  edges = json_object_get(view, "edges"); edge = json_array_get(edges, json_array_size(edges) - 1);
  if (turbo_runtime_json_object_set_string(edge, "relation", "contains") != 0 ||
      turbo_runtime_json_object_set_string(edge, "evidence", "chunks.document_id") != 0) { rc = SALTS_ENOMEM; goto done; }
  *previous_ordinal = ordinal;
done:
  tstr_free(node_id); tstr_free(label);
  return rc;
}

int turbo_agent_knowledge_store_diagram(turbo_agent_knowledge_store_t *store,
    const char *const *document_ids, size_t document_count, json_value_t **out_view) {
  return turbo_agent_knowledge_store_diagram_ex(store, document_ids, document_count, 0, out_view);
}

int turbo_agent_knowledge_store_diagram_ex(turbo_agent_knowledge_store_t *store,
    const char *const *document_ids, size_t document_count, unsigned flags, json_value_t **out_view) {
  sqlite3_stmt *documents = NULL, *chunks = NULL;
  json_value_t *view = NULL;
  int rc, transaction = 0;
  if (out_view) *out_view = NULL;
  if (!store || !store->db || !document_ids || !out_view || !document_count ||
      document_count > TURBO_KNOWLEDGE_DIAGRAM_MAX_DOCUMENTS ||
      (flags & ~TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS)) return SALTS_EINVAL;
  for (size_t i = 0; i < document_count; ++i) {
    if (!document_ids[i] || !valid_text(document_ids[i], strlen(document_ids[i]), 0)) return SALTS_EINVAL;
    for (size_t j = 0; j < i; ++j) if (!strcmp(document_ids[i], document_ids[j])) return SALTS_EINVAL;
  }
  if (!sqlite3_get_autocommit(store->db)) return SALTS_EBUSY;
  rc = turbo_knowledge_diagram_database_status(sqlite3_exec(store->db, "BEGIN", NULL, NULL, NULL));
  if (rc != SALTS_OK) return rc;
  transaction = 1;
  rc = turbo_knowledge_diagram_database_status(sqlite3_prepare_v2(store->db,
      "SELECT id,uri,kind,title,content_hash,mtime,size,updated_at FROM documents WHERE id=?1",
      -1, &documents, NULL));
  if (rc != SALTS_OK) goto done;
  rc = turbo_knowledge_diagram_database_status(sqlite3_prepare_v2(store->db,
      "SELECT id,ordinal FROM chunks WHERE document_id=?1 ORDER BY ordinal,id LIMIT ?2",
      -1, &chunks, NULL));
  if (rc != SALTS_OK) goto done;
  view = turbo_diagram_create("knowledge");
  if (!view || turbo_runtime_json_object_set_string(view, "scope", "selected_documents") != 0 ||
      turbo_runtime_json_object_set_string(view, "freshness", "indexed_snapshot") != 0 ||
      turbo_runtime_json_object_set_string(view, "extractor", "knowledge-store/contains-v1") != 0) {
    rc = SALTS_ENOMEM; goto done;
  }
  for (size_t i = 0; i < document_count; ++i) {
    json_value_t *document;
    int status;
    int64_t previous_ordinal = -1;
    size_t remaining;
    rc = turbo_knowledge_diagram_database_status(sqlite3_bind_text(documents, 1, document_ids[i], -1, SQLITE_STATIC));
    if (rc != SALTS_OK) goto done;
    status = sqlite3_step(documents);
    if (status != SQLITE_ROW) { rc = status == SQLITE_DONE ? SALTS_ENOENT : turbo_knowledge_diagram_database_status(status); goto done; }
    rc = document_node(view, documents, document_ids[i], &document);
    if (rc != SALTS_OK) goto done;
    status = sqlite3_step(documents);
    if (status != SQLITE_DONE) { rc = status == SQLITE_ROW ? SALTS_EPROTO : turbo_knowledge_diagram_database_status(status); goto done; }
    rc = turbo_knowledge_diagram_database_status(sqlite3_reset(documents));
    if (rc != SALTS_OK) goto done;
    remaining = TURBO_DIAGRAM_MAX_NODES - json_array_size(json_object_get(view, "nodes"));
    rc = turbo_knowledge_diagram_database_status(sqlite3_bind_text(chunks, 1, document_ids[i], -1, SQLITE_STATIC));
    if (rc != SALTS_OK) goto done;
    rc = turbo_knowledge_diagram_database_status(sqlite3_bind_int64(chunks, 2, (sqlite3_int64)(remaining + 1)));
    if (rc != SALTS_OK) goto done;
    while ((status = sqlite3_step(chunks)) == SQLITE_ROW) {
      if (!remaining) { rc = SALTS_ENOSPC; goto done; }
      rc = chunk_node(view, document, chunks, &previous_ordinal);
      if (rc != SALTS_OK) goto done;
      --remaining;
    }
    if (status != SQLITE_DONE) { rc = turbo_knowledge_diagram_database_status(status); goto done; }
    rc = turbo_knowledge_diagram_database_status(sqlite3_reset(chunks));
    if (rc != SALTS_OK) goto done;
  }
  rc = flags & TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS
      ? turbo_knowledge_diagram_add_uri_mentions(store->db, view) : SALTS_OK;
done:
  {
    int result = sqlite3_finalize(documents);
    if (rc == SALTS_OK) rc = turbo_knowledge_diagram_database_status(result);
    result = sqlite3_finalize(chunks);
    if (rc == SALTS_OK) rc = turbo_knowledge_diagram_database_status(result);
  }
  if (rc == SALTS_OK) {
    rc = turbo_knowledge_diagram_database_status(sqlite3_exec(store->db, "COMMIT", NULL, NULL, NULL));
    if (rc == SALTS_OK) transaction = 0;
  }
  if (transaction && sqlite3_exec(store->db, "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK) rc = SALTS_EIO;
  if (rc == SALTS_OK) { *out_view = view; view = NULL; }
  json_free(view);
  return rc;
}

const char turbo_knowledge_diagram_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"document_ids\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"minItems\":1,\"maxItems\":32,\"uniqueItems\":true},"
    "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"mermaid\"]},"
    "\"include_uri_mentions\":{\"type\":\"boolean\"},"
    "\"focus_id\":{\"type\":\"string\"},"
    "\"direction\":{\"type\":\"string\",\"enum\":[\"incoming\",\"outgoing\",\"both\"]},"
    "\"max_hops\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":127}},"
    "\"required\":[\"document_ids\"],\"additionalProperties\":false}";

static int argument_string(const json_value_t *arguments, const char *key,
    const char *default_value, const char **out) {
  const json_value_t *value = json_object_get(arguments, key);
  *out = default_value;
  if (!value) return 0;
  *out = json_string(value);
  return json_type(value) == JSON_STRING && *out && (*out)[0] &&
      json_string_len(value) <= TURBO_DIAGRAM_MAX_TEXT_BYTES &&
      strlen(*out) == json_string_len(value) ? 0 : -1;
}

int turbo_knowledge_diagram_tool_value(const json_value_t *arguments,
    json_value_t **out_result, void *user_data) {
  const char *ids[TURBO_KNOWLEDGE_DIAGRAM_MAX_DOCUMENTS], *format, *focus, *direction_text;
  const json_value_t *id_values, *hops_value, *mentions_value;
  turbo_diagram_direction_t direction = TURBO_DIAGRAM_BOTH;
  json_value_t *view = NULL, *selected = NULL, *result = NULL;
  tstr mermaid = NULL;
  int64_t hops = 1;
  unsigned flags = 0;
  int rc = -1;
  if (out_result) *out_result = NULL;
  if (!user_data || !out_result || json_type(arguments) != JSON_OBJECT) return -1;
  /* Action handlers can be called without the normal registry's schema checks. */
  for (size_t i = 0; i < json_object_size(arguments); ++i) {
    const char *key = json_object_key(arguments, i);
    if (!key || strlen(key) != json_object_key_len(arguments, i) ||
        (strcmp(key, "document_ids") && strcmp(key, "format") && strcmp(key, "focus_id") &&
         strcmp(key, "direction") && strcmp(key, "max_hops") && strcmp(key, "include_uri_mentions"))) return -1;
  }
  id_values = json_object_get(arguments, "document_ids");
  if (json_type(id_values) != JSON_ARRAY || !json_array_size(id_values) ||
      json_array_size(id_values) > TURBO_KNOWLEDGE_DIAGRAM_MAX_DOCUMENTS) return -1;
  for (size_t i = 0; i < json_array_size(id_values); ++i) {
    const json_value_t *value = json_array_get(id_values, i);
    ids[i] = json_string(value);
    if (json_type(value) != JSON_STRING || !valid_text(ids[i], json_string_len(value), 0)) return -1;
  }
  if (argument_string(arguments, "format", "json", &format) != 0 ||
      argument_string(arguments, "focus_id", NULL, &focus) != 0 ||
      argument_string(arguments, "direction", "both", &direction_text) != 0) return -1;
  if (strcmp(format, "json") && strcmp(format, "mermaid")) return -1;
  if (!strcmp(direction_text, "incoming")) direction = TURBO_DIAGRAM_INCOMING;
  else if (!strcmp(direction_text, "outgoing")) direction = TURBO_DIAGRAM_OUTGOING;
  else if (strcmp(direction_text, "both")) return -1;
  hops_value = json_object_get(arguments, "max_hops");
  if (hops_value) hops = turbo_runtime_json_value_as_int64(hops_value, -1);
  if (hops < 0 || hops >= TURBO_DIAGRAM_MAX_NODES ||
      (!focus && (hops_value || json_object_get(arguments, "direction")))) return -1;
  mentions_value = json_object_get(arguments, "include_uri_mentions");
  if (mentions_value) {
    if (json_type(mentions_value) != JSON_BOOL) return -1;
    if (json_bool(mentions_value)) flags = TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS;
  }
  if (turbo_agent_knowledge_store_diagram_ex(user_data, ids, json_array_size(id_values), flags, &view) != SALTS_OK) goto done;
  if (focus) {
    if (turbo_diagram_neighborhood(view, focus, direction, (size_t)hops, &selected) != SALTS_OK) goto done;
    json_free(view); view = selected; selected = NULL;
  }
  result = turbo_action_result_create(1, "indexed knowledge diagram; external source freshness unverified");
  if (!result) goto done;
  if (!strcmp(format, "mermaid")) {
    if (turbo_diagram_mermaid(view, &mermaid) != SALTS_OK ||
        turbo_runtime_json_object_set_string(result, "mermaid", mermaid) != 0) goto done;
  } else {
    if (turbo_runtime_json_object_set(result, "diagram", view) != 0) goto done;
    view = NULL;
  }
  *out_result = result; result = NULL; rc = 0;
done:
  json_free(view); json_free(selected); json_free(result); tstr_free(mermaid);
  return rc;
}
