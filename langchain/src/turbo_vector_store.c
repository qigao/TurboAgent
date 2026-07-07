#include "turbo_vector_store.h"

#include "turbo_document_loader.h"
#include "turbo_embedding.h"
#include "turbo_retriever.h"
#include "turbo_action_tool.h"
#include "turbo_tool_registry.h"
#include "sqlite3.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

typedef struct turbo_vector_store_entry_s {
  char *id;
  char *uri;
  char *kind;
  char *title;
  char *text;
  json_value_t *metadata_json;
  json_value_t *embedding_json;
  size_t order;
} turbo_vector_store_entry_t;

typedef enum turbo_vector_store_backend_e {
  TURBO_VECTOR_STORE_BACKEND_MEMORY = 0,
  TURBO_VECTOR_STORE_BACKEND_SQLITE = 1
} turbo_vector_store_backend_t;

struct turbo_vector_store_s {
  turbo_vector_store_backend_t backend;
  turbo_vector_store_entry_t *entries;
  size_t count;
  size_t capacity;
  size_t dimensions;
  int has_dimensions;
  size_t next_order;
  sqlite3 *db;
};

typedef struct turbo_vector_store_match_s {
  const turbo_vector_store_entry_t *entry;
  double score;
} turbo_vector_store_match_t;

typedef struct turbo_vector_store_pending_chunk_s {
  char *id;
  char *text;
  json_value_t *embedding_json;
  int start;
  int end;
} turbo_vector_store_pending_chunk_t;

typedef struct turbo_vector_store_directory_index_state_s {
  turbo_vector_store_t *store;
  turbo_embedding_model_t *embedding_model;
  const char *kind;
  turbo_text_splitter_options_t splitter_options;
  const turbo_text_splitter_options_t *splitter_options_ptr;
  int recursive;
  size_t max_files;
  size_t max_file_bytes;
  const char *include_extensions;
  size_t visited;
  size_t indexed;
  size_t indexed_chunks;
  size_t skipped;
  size_t skipped_by_extension;
  size_t skipped_too_large;
  size_t skipped_index_error;
  size_t failed;
  int truncated;
} turbo_vector_store_directory_index_state_t;

struct turbo_vector_store_tool_binding_s {
  turbo_vector_store_t *store;
  turbo_embedding_model_t *embedding_model;
};

static char *turbo_vector_store_strdup(const char *text) {
  size_t len;
  char *copy;

  if (!text) {
    return NULL;
  }
  len = strlen(text);
  copy = (char *)malloc(len + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, text, len + 1);
  return copy;
}

static int turbo_vector_store_uri_starts_with(const char *uri,
                                              const char *prefix) {
  size_t prefix_len;

  if (!prefix || !prefix[0]) {
    return 1;
  }
  if (!uri) {
    return 0;
  }
  prefix_len = strlen(prefix);
  return strncmp(uri, prefix, prefix_len) == 0;
}

static char *turbo_vector_store_join_path(const char *dir, const char *name) {
  size_t dir_len;
  size_t name_len;
  int needs_sep;
  char *path;

  if (!dir || !name) {
    return NULL;
  }
  dir_len = strlen(dir);
  name_len = strlen(name);
  needs_sep = dir_len > 0 && dir[dir_len - 1] != '/' &&
              dir[dir_len - 1] != '\\';
  path = (char *)malloc(dir_len + (needs_sep ? 1 : 0) + name_len + 1);
  if (!path) {
    return NULL;
  }
  memcpy(path, dir, dir_len);
  if (needs_sep) {
#ifdef _WIN32
    path[dir_len++] = '\\';
#else
    path[dir_len++] = '/';
#endif
  }
  memcpy(path + dir_len, name, name_len);
  path[dir_len + name_len] = '\0';
  return path;
}

static int turbo_vector_store_path_is_directory(const char *path) {
  if (!path || !path[0]) {
    return 0;
  }
#ifdef _WIN32
  {
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
  }
#else
  {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
  }
#endif
}

static size_t turbo_vector_store_file_size(const char *path) {
  struct stat st;

  if (!path || stat(path, &st) != 0 || st.st_size < 0) {
    return 0;
  }
  return (size_t)st.st_size;
}

static int turbo_vector_store_ascii_char_equal_ignore_case(char a, char b) {
  return tolower((unsigned char)a) == tolower((unsigned char)b);
}

static int turbo_vector_store_extension_token_matches(const char *extension,
                                                      const char *token,
                                                      size_t token_len) {
  const char *expected = extension;
  size_t expected_len;
  size_t i;

  if (!extension || !token || token_len == 0) {
    return 0;
  }
  while (token_len > 0 && (*token == ' ' || *token == '\t' || *token == '.')) {
    token++;
    token_len--;
  }
  while (token_len > 0 &&
         (token[token_len - 1] == ' ' || token[token_len - 1] == '\t')) {
    token_len--;
  }
  if (token_len == 0) {
    return 0;
  }
  if (*expected == '.') {
    expected++;
  }
  expected_len = strlen(expected);
  if (expected_len != token_len) {
    return 0;
  }
  for (i = 0; i < token_len; ++i) {
    if (!turbo_vector_store_ascii_char_equal_ignore_case(expected[i],
                                                         token[i])) {
      return 0;
    }
  }
  return 1;
}

static int turbo_vector_store_extension_allowed(
    const char *path, const char *include_extensions) {
  const char *leaf;
  const char *extension;
  const char *token;
  size_t i;

  if (!include_extensions || !include_extensions[0]) {
    return 1;
  }
  leaf = strrchr(path, '/');
  if (!leaf) {
    leaf = strrchr(path, '\\');
  }
  leaf = leaf ? leaf + 1 : path;
  extension = strrchr(leaf, '.');
  if (!extension || extension == leaf) {
    return 0;
  }
  token = include_extensions;
  for (i = 0;; ++i) {
    char ch = include_extensions[i];
    if (ch == ',' || ch == ';' || ch == '|' || ch == ' ' || ch == '\t' ||
        ch == '\0') {
      if (turbo_vector_store_extension_token_matches(
              extension, token, (size_t)(include_extensions + i - token))) {
        return 1;
      }
      token = include_extensions + i + 1;
      if (ch == '\0') {
        break;
      }
    }
  }
  return 0;
}

static int turbo_vector_store_should_skip_directory(const char *name) {
  if (!name || !name[0]) {
    return 1;
  }
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    return 1;
  }
  if (name[0] == '.') {
    return 1;
  }
  return strcmp(name, "build") == 0 || strcmp(name, "node_modules") == 0 ||
         strcmp(name, "vcpkg_installed") == 0 ||
         strcmp(name, "CMakeFiles") == 0 ||
         strncmp(name, "cmake-build-", 12) == 0;
}

static int turbo_vector_store_validate_embedding(const json_value_t *embedding,
                                                 size_t *out_dimensions) {
  size_t count;
  size_t i;

  if (!embedding || turbo_json_type(embedding) != TURBO_JSON_ARRAY) {
    return -1;
  }
  count = turbo_json_array_size(embedding);
  if (count == 0) {
    return -1;
  }
  for (i = 0; i < count; ++i) {
    if (turbo_json_type(turbo_json_array_get(embedding, i)) !=
        TURBO_JSON_NUMBER) {
      return -1;
    }
  }
  if (out_dimensions) {
    *out_dimensions = count;
  }
  return 0;
}

static void turbo_vector_store_entry_clear(turbo_vector_store_entry_t *entry) {
  if (!entry) {
    return;
  }
  free(entry->id);
  free(entry->uri);
  free(entry->kind);
  free(entry->title);
  free(entry->text);
  turbo_free_json(&entry->metadata_json);
  turbo_free_json(&entry->embedding_json);
  memset(entry, 0, sizeof(*entry));
}

static int turbo_vector_store_entry_set(
    turbo_vector_store_entry_t *entry,
    const turbo_vector_store_document_t *document,
    const json_value_t *embedding_json, size_t order) {
  turbo_vector_store_entry_t next = {0};

  next.id = turbo_vector_store_strdup(document->id);
  if (!next.id) {
    goto fail;
  }
  next.uri = turbo_vector_store_strdup(document->uri);
  next.kind = turbo_vector_store_strdup(document->kind);
  next.title = turbo_vector_store_strdup(document->title);
  next.text = turbo_vector_store_strdup(document->text);
  next.embedding_json = turbo_json_clone(embedding_json);
  if (!next.embedding_json) {
    goto fail;
  }
  if (document->metadata_json) {
    next.metadata_json = turbo_json_clone(document->metadata_json);
    if (!next.metadata_json) {
      goto fail;
    }
  }
  next.order = order;

  turbo_vector_store_entry_clear(entry);
  *entry = next;
  return 0;

fail:
  turbo_vector_store_entry_clear(&next);
  return -1;
}

static int turbo_vector_store_reserve(turbo_vector_store_t *store,
                                      size_t capacity) {
  turbo_vector_store_entry_t *entries;
  size_t old_capacity;

  if (store->capacity >= capacity) {
    return 0;
  }
  old_capacity = store->capacity;
  if (capacity < old_capacity * 2) {
    capacity = old_capacity * 2;
  }
  if (capacity < 8) {
    capacity = 8;
  }
  entries = (turbo_vector_store_entry_t *)realloc(
      store->entries, capacity * sizeof(*entries));
  if (!entries) {
    return -1;
  }
  memset(entries + old_capacity, 0,
         (capacity - old_capacity) * sizeof(*entries));
  store->entries = entries;
  store->capacity = capacity;
  return 0;
}

static ptrdiff_t turbo_vector_store_find_index(const turbo_vector_store_t *store,
                                               const char *document_id) {
  size_t i;

  if (!store || !document_id) {
    return -1;
  }
  for (i = 0; i < store->count; ++i) {
    if (store->entries[i].id && strcmp(store->entries[i].id, document_id) == 0) {
      return (ptrdiff_t)i;
    }
  }
  return -1;
}

static double turbo_vector_store_similarity(const json_value_t *left,
                                            const json_value_t *right) {
  double dot = 0.0;
  double left_norm = 0.0;
  double right_norm = 0.0;
  size_t count = turbo_json_array_size(left);
  size_t i;

  for (i = 0; i < count; ++i) {
    double a = turbo_json_number(turbo_json_array_get(left, i));
    double b = turbo_json_number(turbo_json_array_get(right, i));

    dot += a * b;
    left_norm += a * a;
    right_norm += b * b;
  }
  if (left_norm == 0.0 || right_norm == 0.0) {
    return 0.0;
  }
  return dot / (sqrt(left_norm) * sqrt(right_norm));
}

static int turbo_vector_store_compare_matches(const void *left,
                                              const void *right) {
  const turbo_vector_store_match_t *a =
      (const turbo_vector_store_match_t *)left;
  const turbo_vector_store_match_t *b =
      (const turbo_vector_store_match_t *)right;

  if (a->score > b->score) {
    return -1;
  }
  if (a->score < b->score) {
    return 1;
  }
  if (a->entry->order < b->entry->order) {
    return -1;
  }
  if (a->entry->order > b->entry->order) {
    return 1;
  }
  return 0;
}

static int turbo_vector_store_add_result(json_value_t *results,
                                         const turbo_vector_store_match_t *match) {
  json_value_t *item = turbo_json_create_object();

  if (!item) {
    return -1;
  }
  turbo_json_object_set_string(item, "document_id", match->entry->id);
  if (match->entry->uri) {
    turbo_json_object_set_string(item, "uri", match->entry->uri);
  }
  if (match->entry->kind) {
    turbo_json_object_set_string(item, "kind", match->entry->kind);
  }
  if (match->entry->title) {
    turbo_json_object_set_string(item, "title", match->entry->title);
  }
  if (match->entry->text) {
    turbo_json_object_set_string(item, "text", match->entry->text);
  }
  turbo_json_object_set_number(item, "score", match->score);
  turbo_json_array_add(results, item);
  return 0;
}

static void turbo_vector_store_pending_chunks_clear(
    turbo_vector_store_pending_chunk_t *chunks, size_t count) {
  size_t i;

  if (!chunks) {
    return;
  }
  for (i = 0; i < count; ++i) {
    free(chunks[i].id);
    free(chunks[i].text);
    turbo_free_json(&chunks[i].embedding_json);
  }
  free(chunks);
}

static char *turbo_vector_store_chunk_id(const char *document_id,
                                         size_t ordinal) {
  int needed;
  char *id;

  needed = snprintf(NULL, 0, "%s#chunk-%zu", document_id, ordinal);
  if (needed <= 0) {
    return NULL;
  }
  id = (char *)malloc((size_t)needed + 1);
  if (!id) {
    return NULL;
  }
  snprintf(id, (size_t)needed + 1, "%s#chunk-%zu", document_id, ordinal);
  return id;
}

static int turbo_vector_store_id_is_indexed_chunk(const char *id,
                                                  const char *document_id) {
  static const char chunk_suffix[] = "#chunk-";
  size_t document_id_len;

  if (!id || !document_id || !document_id[0]) {
    return 0;
  }
  document_id_len = strlen(document_id);
  return strncmp(id, document_id, document_id_len) == 0 &&
         strncmp(id + document_id_len, chunk_suffix,
                 sizeof(chunk_suffix) - 1) == 0 &&
         id[document_id_len + sizeof(chunk_suffix) - 1] != '\0';
}

static json_value_t *turbo_vector_store_index_summary(
    const char *document_id, const char *stage, int ok, size_t chunk_count,
    size_t indexed_count) {
  json_value_t *summary = turbo_json_create_object();

  if (!summary) {
    return NULL;
  }
  turbo_json_object_set_bool(summary, "ok", ok ? true : false);
  turbo_json_object_set_string(summary, "document_id",
                               document_id ? document_id : "");
  turbo_json_object_set_string(summary, "stage", stage ? stage : "");
  turbo_json_object_set_number(summary, "chunk_count", (double)chunk_count);
  turbo_json_object_set_number(summary, "indexed", (double)indexed_count);
  return summary;
}

static int turbo_vector_store_set_summary(json_value_t **out_summary_json,
                                          json_value_t *summary) {
  if (!out_summary_json) {
    turbo_free_json(&summary);
    return summary ? 0 : -1;
  }
  *out_summary_json = summary;
  return summary ? 0 : -1;
}

static int turbo_vector_sqlite_exec(sqlite3 *db, const char *sql) {
  char *errmsg = NULL;
  int rc;

  if (!db || !sql) {
    return -1;
  }
  rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
  if (errmsg) {
    sqlite3_free(errmsg);
  }
  return rc == SQLITE_OK ? 0 : -1;
}

static int turbo_vector_sqlite_prepare(sqlite3 *db, const char *sql,
                                       sqlite3_stmt **out_stmt) {
  if (!db || !sql || !out_stmt) {
    return -1;
  }
  *out_stmt = NULL;
  return sqlite3_prepare_v2(db, sql, -1, out_stmt, NULL) == SQLITE_OK ? 0 : -1;
}

static int turbo_vector_sqlite_init_schema(sqlite3 *db) {
  static const char *schema =
      "PRAGMA foreign_keys=ON;"
      "CREATE TABLE IF NOT EXISTS vector_documents("
      "id TEXT PRIMARY KEY,"
      "uri TEXT,"
      "kind TEXT,"
      "title TEXT,"
      "text TEXT,"
      "metadata_json TEXT,"
      "embedding_json TEXT NOT NULL,"
      "dimensions INTEGER NOT NULL,"
      "insert_order INTEGER NOT NULL,"
      "updated_at INTEGER NOT NULL DEFAULT (unixepoch()));"
      "CREATE INDEX IF NOT EXISTS idx_vector_documents_kind "
      "ON vector_documents(kind);"
      "CREATE INDEX IF NOT EXISTS idx_vector_documents_uri "
      "ON vector_documents(uri);";

  return turbo_vector_sqlite_exec(db, schema);
}

static int turbo_vector_sqlite_count_rows(sqlite3 *db, const char *sql,
                                          size_t *out_count) {
  sqlite3_stmt *stmt = NULL;
  int rc;

  if (!db || !sql || !out_count) {
    return -1;
  }
  *out_count = 0;
  if (turbo_vector_sqlite_prepare(db, sql, &stmt) != 0) {
    return -1;
  }
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    *out_count = (size_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return 0;
  }
  sqlite3_finalize(stmt);
  return -1;
}

static int turbo_vector_sqlite_current_dimensions(sqlite3 *db,
                                                 size_t *out_dimensions,
                                                 int *out_has_dimensions) {
  sqlite3_stmt *stmt = NULL;
  int rc;

  if (!db || !out_dimensions || !out_has_dimensions) {
    return -1;
  }
  *out_dimensions = 0;
  *out_has_dimensions = 0;
  if (turbo_vector_sqlite_prepare(
          db, "SELECT dimensions FROM vector_documents LIMIT 1;", &stmt) != 0) {
    return -1;
  }
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    *out_dimensions = (size_t)sqlite3_column_int64(stmt, 0);
    *out_has_dimensions = 1;
  } else if (rc != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return -1;
  }
  sqlite3_finalize(stmt);
  return 0;
}

static int turbo_vector_sqlite_existing_order(sqlite3 *db, const char *document_id,
                                              size_t *out_order,
                                              int *out_found) {
  sqlite3_stmt *stmt = NULL;
  int rc;

  if (!db || !document_id || !out_order || !out_found) {
    return -1;
  }
  *out_order = 0;
  *out_found = 0;
  if (turbo_vector_sqlite_prepare(
          db, "SELECT insert_order FROM vector_documents WHERE id=?;", &stmt) !=
      0) {
    return -1;
  }
  sqlite3_bind_text(stmt, 1, document_id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    *out_order = (size_t)sqlite3_column_int64(stmt, 0);
    *out_found = 1;
  } else if (rc != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return -1;
  }
  sqlite3_finalize(stmt);
  return 0;
}

static int turbo_vector_sqlite_next_order(sqlite3 *db, size_t *out_order) {
  sqlite3_stmt *stmt = NULL;
  int rc;

  if (!db || !out_order) {
    return -1;
  }
  *out_order = 0;
  if (turbo_vector_sqlite_prepare(
          db, "SELECT COALESCE(MAX(insert_order) + 1, 0) "
              "FROM vector_documents;",
          &stmt) != 0) {
    return -1;
  }
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    *out_order = (size_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return 0;
  }
  sqlite3_finalize(stmt);
  return -1;
}

static int turbo_vector_sqlite_delete_indexed_chunks(
    sqlite3 *db, const char *document_id) {
  sqlite3_stmt *stmt = NULL;
  char *prefix = NULL;
  int needed;
  int ok;

  if (!db || !document_id || !document_id[0]) {
    return -1;
  }
  needed = snprintf(NULL, 0, "%s#chunk-", document_id);
  if (needed <= 0) {
    return -1;
  }
  prefix = (char *)malloc((size_t)needed + 1);
  if (!prefix) {
    return -1;
  }
  snprintf(prefix, (size_t)needed + 1, "%s#chunk-", document_id);
  if (turbo_vector_sqlite_prepare(
          db, "DELETE FROM vector_documents WHERE substr(id,1,?)=?;",
          &stmt) != 0) {
    free(prefix);
    return -1;
  }
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)needed);
  sqlite3_bind_text(stmt, 2, prefix, -1, SQLITE_TRANSIENT);
  ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  free(prefix);
  return ok ? 0 : -1;
}

static char *turbo_vector_store_json_serialize(const json_value_t *json) {
  json_value_t *clone;
  char *serialized;

  if (!json) {
    return NULL;
  }
  clone = turbo_json_clone(json);
  if (!clone) {
    return NULL;
  }
  serialized = turbo_json_serialize(clone, NULL);
  turbo_free_json(&clone);
  return serialized;
}

static json_value_t *turbo_vector_store_json_parse(const char *text) {
  json_value_t *json = NULL;

  if (!text || turbo_parse_json((const uint8_t *)text, strlen(text), &json) != 0) {
    turbo_free_json(&json);
    return NULL;
  }
  return json;
}

static int turbo_vector_store_sqlite_upsert(
    turbo_vector_store_t *store, const turbo_vector_store_document_t *document,
    const json_value_t *embedding_json, size_t dimensions) {
  sqlite3_stmt *stmt = NULL;
  char *embedding_text = NULL;
  char *metadata_text = NULL;
  size_t current_dimensions = 0;
  size_t order = 0;
  int has_dimensions = 0;
  int found = 0;
  int ok;

  if (turbo_vector_sqlite_current_dimensions(store->db, &current_dimensions,
                                             &has_dimensions) != 0) {
    return -1;
  }
  if (has_dimensions && current_dimensions != dimensions) {
    return -1;
  }
  if (turbo_vector_sqlite_existing_order(store->db, document->id, &order,
                                         &found) != 0) {
    return -1;
  }
  if (!found && turbo_vector_sqlite_next_order(store->db, &order) != 0) {
    return -1;
  }
  embedding_text = turbo_vector_store_json_serialize(embedding_json);
  if (!embedding_text) {
    return -1;
  }
  if (document->metadata_json) {
    metadata_text = turbo_vector_store_json_serialize(document->metadata_json);
    if (!metadata_text) {
      turbo_json_serialize_free(embedding_text);
      return -1;
    }
  }
  if (turbo_vector_sqlite_prepare(
          store->db,
          "INSERT INTO vector_documents("
          "id,uri,kind,title,text,metadata_json,embedding_json,dimensions,"
          "insert_order,updated_at) VALUES(?,?,?,?,?,?,?,?,?,unixepoch()) "
          "ON CONFLICT(id) DO UPDATE SET "
          "uri=excluded.uri,kind=excluded.kind,title=excluded.title,"
          "text=excluded.text,metadata_json=excluded.metadata_json,"
          "embedding_json=excluded.embedding_json,"
          "dimensions=excluded.dimensions,updated_at=unixepoch();",
          &stmt) != 0) {
    turbo_json_serialize_free(metadata_text);
    turbo_json_serialize_free(embedding_text);
    return -1;
  }
  sqlite3_bind_text(stmt, 1, document->id, -1, SQLITE_TRANSIENT);
  if (document->uri) {
    sqlite3_bind_text(stmt, 2, document->uri, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 2);
  }
  if (document->kind) {
    sqlite3_bind_text(stmt, 3, document->kind, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 3);
  }
  if (document->title) {
    sqlite3_bind_text(stmt, 4, document->title, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 4);
  }
  if (document->text) {
    sqlite3_bind_text(stmt, 5, document->text, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 5);
  }
  if (metadata_text) {
    sqlite3_bind_text(stmt, 6, metadata_text, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 6);
  }
  sqlite3_bind_text(stmt, 7, embedding_text, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 8, (sqlite3_int64)dimensions);
  sqlite3_bind_int64(stmt, 9, (sqlite3_int64)order);
  ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  turbo_json_serialize_free(metadata_text);
  turbo_json_serialize_free(embedding_text);
  return ok ? 0 : -1;
}

static int turbo_vector_store_sqlite_delete_document(
    turbo_vector_store_t *store, const char *document_id) {
  sqlite3_stmt *stmt = NULL;
  int ok;

  if (turbo_vector_sqlite_prepare(
          store->db, "DELETE FROM vector_documents WHERE id=?;", &stmt) != 0) {
    return -1;
  }
  sqlite3_bind_text(stmt, 1, document_id, -1, SQLITE_TRANSIENT);
  ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok ? 0 : -1;
}

static int turbo_vector_store_sqlite_load_entry(sqlite3_stmt *stmt,
                                                turbo_vector_store_entry_t *entry) {
  const char *embedding_text;
  const char *metadata_text;

  memset(entry, 0, sizeof(*entry));
  entry->id = turbo_vector_store_strdup((const char *)sqlite3_column_text(stmt, 0));
  entry->uri = turbo_vector_store_strdup((const char *)sqlite3_column_text(stmt, 1));
  entry->kind = turbo_vector_store_strdup((const char *)sqlite3_column_text(stmt, 2));
  entry->title = turbo_vector_store_strdup((const char *)sqlite3_column_text(stmt, 3));
  entry->text = turbo_vector_store_strdup((const char *)sqlite3_column_text(stmt, 4));
  metadata_text = (const char *)sqlite3_column_text(stmt, 5);
  embedding_text = (const char *)sqlite3_column_text(stmt, 6);
  entry->order = (size_t)sqlite3_column_int64(stmt, 7);
  if (!entry->id || !embedding_text) {
    turbo_vector_store_entry_clear(entry);
    return -1;
  }
  if (metadata_text) {
    entry->metadata_json = turbo_vector_store_json_parse(metadata_text);
    if (!entry->metadata_json) {
      turbo_vector_store_entry_clear(entry);
      return -1;
    }
  }
  entry->embedding_json = turbo_vector_store_json_parse(embedding_text);
  if (!entry->embedding_json) {
    turbo_vector_store_entry_clear(entry);
    return -1;
  }
  return 0;
}

static int turbo_vector_store_sqlite_query(
    turbo_vector_store_t *store, const json_value_t *query_embedding_json,
    const turbo_vector_store_query_options_t *options,
    json_value_t **out_results_json, size_t dimensions) {
  turbo_vector_store_query_options_t effective_options = {0};
  turbo_vector_store_match_t *matches = NULL;
  turbo_vector_store_entry_t *entries = NULL;
  json_value_t *results = NULL;
  sqlite3_stmt *stmt = NULL;
  size_t current_dimensions = 0;
  size_t candidate_count = 0;
  size_t match_count = 0;
  size_t limit;
  int has_dimensions = 0;
  int rc;
  size_t i;

  if (options) {
    effective_options = *options;
  }
  if (turbo_vector_sqlite_current_dimensions(store->db, &current_dimensions,
                                             &has_dimensions) != 0) {
    return -1;
  }
  if (has_dimensions && current_dimensions != dimensions) {
    return -1;
  }
  limit = effective_options.limit == 0 ? 8 : effective_options.limit;
  if (turbo_vector_sqlite_count_rows(
          store->db, "SELECT COUNT(*) FROM vector_documents;",
          &candidate_count) != 0) {
    return -1;
  }
  entries = (turbo_vector_store_entry_t *)calloc(candidate_count,
                                                sizeof(*entries));
  matches = (turbo_vector_store_match_t *)calloc(candidate_count,
                                                sizeof(*matches));
  results = turbo_json_create_array();
  if ((!entries && candidate_count > 0) || (!matches && candidate_count > 0) ||
      !results) {
    free(matches);
    free(entries);
    turbo_free_json(&results);
    return -1;
  }

  if (turbo_vector_sqlite_prepare(
          store->db,
          "SELECT id,uri,kind,title,text,metadata_json,embedding_json,"
          "insert_order FROM vector_documents "
          "WHERE (?1 IS NULL OR kind=?1) "
          "ORDER BY insert_order ASC;",
          &stmt) != 0) {
    free(matches);
    free(entries);
    turbo_free_json(&results);
    return -1;
  }
  if (effective_options.kind && effective_options.kind[0]) {
    sqlite3_bind_text(stmt, 1, effective_options.kind, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 1);
  }
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (match_count >= candidate_count ||
        turbo_vector_store_sqlite_load_entry(stmt, &entries[match_count]) != 0) {
      sqlite3_finalize(stmt);
      for (i = 0; i < candidate_count; ++i) {
        turbo_vector_store_entry_clear(&entries[i]);
      }
      free(matches);
      free(entries);
      turbo_free_json(&results);
      return -1;
    }
    if (!turbo_vector_store_uri_starts_with(entries[match_count].uri,
                                            effective_options.uri_prefix)) {
      turbo_vector_store_entry_clear(&entries[match_count]);
      continue;
    }
    matches[match_count].entry = &entries[match_count];
    matches[match_count].score = turbo_vector_store_similarity(
        query_embedding_json, entries[match_count].embedding_json);
    match_count++;
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    for (i = 0; i < candidate_count; ++i) {
      turbo_vector_store_entry_clear(&entries[i]);
    }
    free(matches);
    free(entries);
    turbo_free_json(&results);
    return -1;
  }

  qsort(matches, match_count, sizeof(*matches),
        turbo_vector_store_compare_matches);
  if (limit > match_count) {
    limit = match_count;
  }
  for (i = 0; i < limit; ++i) {
    if (turbo_vector_store_add_result(results, &matches[i]) != 0) {
      for (i = 0; i < candidate_count; ++i) {
        turbo_vector_store_entry_clear(&entries[i]);
      }
      free(matches);
      free(entries);
      turbo_free_json(&results);
      return -1;
    }
  }
  for (i = 0; i < candidate_count; ++i) {
    turbo_vector_store_entry_clear(&entries[i]);
  }
  free(matches);
  free(entries);
  *out_results_json = results;
  return 0;
}

turbo_vector_store_t *turbo_vector_store_create_memory(void) {
  return (turbo_vector_store_t *)calloc(1, sizeof(turbo_vector_store_t));
}

turbo_vector_store_t *turbo_vector_store_sqlite_open(const char *db_path) {
  turbo_vector_store_t *store;
  sqlite3 *db = NULL;

  if (!db_path || !db_path[0]) {
    return NULL;
  }
  if (sqlite3_open(db_path, &db) != SQLITE_OK) {
    if (db) {
      sqlite3_close(db);
    }
    return NULL;
  }
  if (turbo_vector_sqlite_init_schema(db) != 0) {
    sqlite3_close(db);
    return NULL;
  }
  store = (turbo_vector_store_t *)calloc(1, sizeof(*store));
  if (!store) {
    sqlite3_close(db);
    return NULL;
  }
  store->backend = TURBO_VECTOR_STORE_BACKEND_SQLITE;
  store->db = db;
  return store;
}

void turbo_vector_store_destroy(turbo_vector_store_t *store) {
  size_t i;

  if (!store) {
    return;
  }
  if (store->db) {
    sqlite3_close(store->db);
  }
  for (i = 0; i < store->count; ++i) {
    turbo_vector_store_entry_clear(&store->entries[i]);
  }
  free(store->entries);
  free(store);
}

int turbo_vector_store_upsert(turbo_vector_store_t *store,
                              const turbo_vector_store_document_t *document,
                              const json_value_t *embedding_json) {
  size_t dimensions = 0;
  ptrdiff_t existing_index;
  size_t order;

  if (!store || !document || !document->id || !document->id[0]) {
    return -1;
  }
  if (turbo_vector_store_validate_embedding(embedding_json, &dimensions) != 0) {
    return -1;
  }
  if (store->backend == TURBO_VECTOR_STORE_BACKEND_SQLITE) {
    return turbo_vector_store_sqlite_upsert(store, document, embedding_json,
                                            dimensions);
  }
  if (store->has_dimensions && store->dimensions != dimensions) {
    return -1;
  }
  existing_index = turbo_vector_store_find_index(store, document->id);
  if (existing_index >= 0) {
    order = store->entries[existing_index].order;
    return turbo_vector_store_entry_set(&store->entries[existing_index],
                                        document, embedding_json, order);
  }
  if (turbo_vector_store_reserve(store, store->count + 1) != 0) {
    return -1;
  }
  order = store->next_order++;
  if (turbo_vector_store_entry_set(&store->entries[store->count], document,
                                   embedding_json, order) != 0) {
    return -1;
  }
  if (!store->has_dimensions) {
    store->dimensions = dimensions;
    store->has_dimensions = 1;
  }
  store->count++;
  return 0;
}

int turbo_vector_store_delete_document(turbo_vector_store_t *store,
                                       const char *document_id) {
  ptrdiff_t index;

  if (!store || !document_id || !document_id[0]) {
    return -1;
  }
  if (store->backend == TURBO_VECTOR_STORE_BACKEND_SQLITE) {
    return turbo_vector_store_sqlite_delete_document(store, document_id);
  }
  index = turbo_vector_store_find_index(store, document_id);
  if (index < 0) {
    return 0;
  }
  turbo_vector_store_entry_clear(&store->entries[index]);
  if ((size_t)index + 1 < store->count) {
    store->entries[index] = store->entries[store->count - 1];
    memset(&store->entries[store->count - 1], 0,
           sizeof(store->entries[store->count - 1]));
  }
  store->count--;
  if (store->count == 0) {
    store->dimensions = 0;
    store->has_dimensions = 0;
  }
  return 0;
}

int turbo_vector_store_query(
    turbo_vector_store_t *store, const json_value_t *query_embedding_json,
    const turbo_vector_store_query_options_t *options,
    json_value_t **out_results_json) {
  turbo_vector_store_query_options_t effective_options = {0};
  turbo_vector_store_match_t *matches = NULL;
  json_value_t *results = NULL;
  size_t dimensions = 0;
  size_t match_count = 0;
  size_t i;
  size_t limit;

  if (!store || !out_results_json) {
    return -1;
  }
  *out_results_json = NULL;
  if (turbo_vector_store_validate_embedding(query_embedding_json, &dimensions) !=
      0) {
    return -1;
  }
  if (store->backend == TURBO_VECTOR_STORE_BACKEND_SQLITE) {
    return turbo_vector_store_sqlite_query(store, query_embedding_json, options,
                                           out_results_json, dimensions);
  }
  if (store->has_dimensions && store->dimensions != dimensions) {
    return -1;
  }
  if (options) {
    effective_options = *options;
  }
  limit = effective_options.limit == 0 ? 8 : effective_options.limit;

  matches = (turbo_vector_store_match_t *)calloc(store->count, sizeof(*matches));
  results = turbo_json_create_array();
  if ((!matches && store->count > 0) || !results) {
    free(matches);
    turbo_free_json(&results);
    return -1;
  }

  for (i = 0; i < store->count; ++i) {
    const turbo_vector_store_entry_t *entry = &store->entries[i];

    if (effective_options.kind && effective_options.kind[0] &&
        (!entry->kind || strcmp(entry->kind, effective_options.kind) != 0)) {
      continue;
    }
    if (!turbo_vector_store_uri_starts_with(entry->uri,
                                            effective_options.uri_prefix)) {
      continue;
    }
    matches[match_count].entry = entry;
    matches[match_count].score = turbo_vector_store_similarity(
        query_embedding_json, entry->embedding_json);
    match_count++;
  }

  qsort(matches, match_count, sizeof(*matches),
        turbo_vector_store_compare_matches);
  if (limit > match_count) {
    limit = match_count;
  }
  for (i = 0; i < limit; ++i) {
    if (turbo_vector_store_add_result(results, &matches[i]) != 0) {
      free(matches);
      turbo_free_json(&results);
      return -1;
    }
  }

  free(matches);
  *out_results_json = results;
  return 0;
}

static int turbo_vector_store_memory_replace_indexed_chunks(
    turbo_vector_store_t *store, const turbo_vector_store_document_t *document,
    const turbo_vector_store_pending_chunk_t *chunks, size_t chunk_count,
    size_t *out_indexed_count) {
  turbo_vector_store_entry_t *next_entries = NULL;
  size_t remaining_count = 0;
  size_t next_count;
  size_t next_capacity = 0;
  size_t next_order;
  size_t next_dimensions = 0;
  int next_has_dimensions = 0;
  size_t i;

  if (!store || !document || !document->id ||
      (!chunks && chunk_count > 0) || !out_indexed_count) {
    return -1;
  }
  *out_indexed_count = 0;
  for (i = 0; i < store->count; ++i) {
    if (!turbo_vector_store_id_is_indexed_chunk(store->entries[i].id,
                                                document->id)) {
      remaining_count++;
    }
  }
  next_count = remaining_count + chunk_count;
  if (next_count > 0) {
    next_capacity = next_count < 8 ? 8 : next_count;
    next_entries = (turbo_vector_store_entry_t *)calloc(
        next_capacity, sizeof(*next_entries));
    if (!next_entries) {
      return -1;
    }
  }

  next_count = 0;
  for (i = 0; i < store->count; ++i) {
    turbo_vector_store_entry_t *entry = &store->entries[i];
    turbo_vector_store_document_t entry_document = {0};
    size_t entry_dimensions = 0;

    if (turbo_vector_store_id_is_indexed_chunk(entry->id, document->id)) {
      continue;
    }
    if (turbo_vector_store_validate_embedding(entry->embedding_json,
                                              &entry_dimensions) != 0 ||
        (next_has_dimensions && next_dimensions != entry_dimensions)) {
      goto fail;
    }
    next_dimensions = entry_dimensions;
    next_has_dimensions = 1;
    entry_document.id = entry->id;
    entry_document.uri = entry->uri;
    entry_document.kind = entry->kind;
    entry_document.title = entry->title;
    entry_document.text = entry->text;
    entry_document.metadata_json = entry->metadata_json;
    if (turbo_vector_store_entry_set(&next_entries[next_count],
                                     &entry_document, entry->embedding_json,
                                     entry->order) != 0) {
      goto fail;
    }
    next_count++;
  }

  next_order = store->next_order;
  for (i = 0; i < chunk_count; ++i) {
    turbo_vector_store_document_t chunk_document = {0};
    size_t chunk_dimensions = 0;

    if (!chunks[i].id || !chunks[i].text || !chunks[i].embedding_json ||
        turbo_vector_store_validate_embedding(chunks[i].embedding_json,
                                              &chunk_dimensions) != 0 ||
        (next_has_dimensions && next_dimensions != chunk_dimensions)) {
      goto fail;
    }
    next_dimensions = chunk_dimensions;
    next_has_dimensions = 1;
    chunk_document.id = chunks[i].id;
    chunk_document.uri = document->uri;
    chunk_document.kind = document->kind;
    chunk_document.title = document->title;
    chunk_document.text = chunks[i].text;
    chunk_document.metadata_json = document->metadata_json;
    if (turbo_vector_store_entry_set(&next_entries[next_count],
                                     &chunk_document, chunks[i].embedding_json,
                                     next_order++) != 0) {
      goto fail;
    }
    next_count++;
    (*out_indexed_count)++;
  }

  for (i = 0; i < store->count; ++i) {
    turbo_vector_store_entry_clear(&store->entries[i]);
  }
  free(store->entries);
  store->entries = next_entries;
  store->count = next_count;
  store->capacity = next_capacity;
  store->dimensions = next_has_dimensions ? next_dimensions : 0;
  store->has_dimensions = next_has_dimensions;
  store->next_order = next_order;
  return 0;

fail:
  if (next_entries) {
    for (i = 0; i < next_capacity; ++i) {
      turbo_vector_store_entry_clear(&next_entries[i]);
    }
  }
  free(next_entries);
  *out_indexed_count = 0;
  return -1;
}

int turbo_vector_store_index_text(
    turbo_vector_store_t *store, turbo_embedding_model_t *embedding_model,
    const turbo_vector_store_document_t *document,
    const turbo_text_splitter_options_t *splitter_options,
    json_value_t **out_summary_json) {
  json_value_t *chunks_json = NULL;
  turbo_vector_store_pending_chunk_t *pending_chunks = NULL;
  size_t chunk_count;
  size_t indexed_count = 0;
  size_t i;

  if (out_summary_json) {
    *out_summary_json = NULL;
  }
  if (!store || !embedding_model || !document || !document->id ||
      !document->id[0] || !document->text) {
    return -1;
  }
  if (turbo_text_splitter_split_text(document->text, splitter_options,
                                     &chunks_json) != 0) {
    return -1;
  }
  chunk_count = turbo_json_array_size(chunks_json);
  pending_chunks = (turbo_vector_store_pending_chunk_t *)calloc(
      chunk_count, sizeof(*pending_chunks));
  if (!pending_chunks && chunk_count > 0) {
    turbo_free_json(&chunks_json);
    return -1;
  }

  for (i = 0; i < chunk_count; ++i) {
    const json_value_t *chunk = turbo_json_array_get(chunks_json, i);
    const char *chunk_text = turbo_json_get_string(chunk, "text");

    if (!chunk_text) {
      turbo_vector_store_pending_chunks_clear(pending_chunks, chunk_count);
      turbo_free_json(&chunks_json);
      turbo_vector_store_set_summary(
          out_summary_json,
          turbo_vector_store_index_summary(document->id, "split", 0,
                                           chunk_count, 0));
      return -1;
    }
    pending_chunks[i].id = turbo_vector_store_chunk_id(document->id, i);
    pending_chunks[i].text = turbo_vector_store_strdup(chunk_text);
    pending_chunks[i].start = turbo_json_get_int(chunk, "start", 0);
    pending_chunks[i].end = turbo_json_get_int(chunk, "end", 0);
    if (!pending_chunks[i].id || !pending_chunks[i].text ||
        turbo_embedding_model_embed_text(embedding_model, chunk_text,
                                         &pending_chunks[i].embedding_json) !=
            0) {
      turbo_vector_store_pending_chunks_clear(pending_chunks, chunk_count);
      turbo_free_json(&chunks_json);
      turbo_vector_store_set_summary(
          out_summary_json,
          turbo_vector_store_index_summary(document->id, "embed", 0,
                                           chunk_count, 0));
      return -1;
    }
  }

  if (store->backend == TURBO_VECTOR_STORE_BACKEND_SQLITE) {
    int sqlite_in_tx = 0;

    if (turbo_vector_sqlite_exec(store->db, "BEGIN IMMEDIATE;") != 0) {
      turbo_vector_store_pending_chunks_clear(pending_chunks, chunk_count);
      turbo_free_json(&chunks_json);
      turbo_vector_store_set_summary(
          out_summary_json,
          turbo_vector_store_index_summary(document->id, "upsert", 0,
                                           chunk_count, indexed_count));
      return -1;
    }
    sqlite_in_tx = 1;
    if (turbo_vector_sqlite_delete_indexed_chunks(store->db, document->id) !=
        0) {
      goto sqlite_fail;
    }
    for (i = 0; i < chunk_count; ++i) {
      turbo_vector_store_document_t chunk_document = {0};
      size_t chunk_dimensions = 0;

      if (turbo_vector_store_validate_embedding(
              pending_chunks[i].embedding_json, &chunk_dimensions) != 0) {
        goto sqlite_fail;
      }
      chunk_document.id = pending_chunks[i].id;
      chunk_document.uri = document->uri;
      chunk_document.kind = document->kind;
      chunk_document.title = document->title;
      chunk_document.text = pending_chunks[i].text;
      chunk_document.metadata_json = document->metadata_json;
      if (turbo_vector_store_sqlite_upsert(store, &chunk_document,
                                           pending_chunks[i].embedding_json,
                                           chunk_dimensions) != 0) {
        goto sqlite_fail;
      }
      indexed_count++;
    }
    if (turbo_vector_sqlite_exec(store->db, "COMMIT;") != 0) {
      goto sqlite_fail;
    }
    sqlite_in_tx = 0;
    goto complete;

sqlite_fail:
    if (sqlite_in_tx) {
      turbo_vector_sqlite_exec(store->db, "ROLLBACK;");
    }
    turbo_vector_store_pending_chunks_clear(pending_chunks, chunk_count);
    turbo_free_json(&chunks_json);
    turbo_vector_store_set_summary(
        out_summary_json,
        turbo_vector_store_index_summary(document->id, "upsert", 0,
                                         chunk_count, indexed_count));
    return -1;
  }

  if (turbo_vector_store_memory_replace_indexed_chunks(
          store, document, pending_chunks, chunk_count, &indexed_count) != 0) {
    turbo_vector_store_pending_chunks_clear(pending_chunks, chunk_count);
    turbo_free_json(&chunks_json);
    turbo_vector_store_set_summary(
        out_summary_json,
        turbo_vector_store_index_summary(document->id, "upsert", 0,
                                         chunk_count, indexed_count));
    return -1;
  }

complete:
  turbo_vector_store_pending_chunks_clear(pending_chunks, chunk_count);
  turbo_free_json(&chunks_json);
  return turbo_vector_store_set_summary(
      out_summary_json,
      turbo_vector_store_index_summary(document->id, "complete", 1,
                                       chunk_count, indexed_count));
}

int turbo_vector_store_index_text_file(
    turbo_vector_store_t *store, turbo_embedding_model_t *embedding_model,
    const char *path, const char *kind,
    const turbo_text_splitter_options_t *splitter_options,
    json_value_t **out_summary_json) {
  json_value_t *document_json = NULL;
  turbo_vector_store_document_t document = {0};
  int rc;

  if (out_summary_json) {
    *out_summary_json = NULL;
  }
  if (!store || !embedding_model || !path || !path[0]) {
    return -1;
  }
  if (turbo_document_loader_load_text_file(path, kind, &document_json) != 0) {
    turbo_vector_store_set_summary(
        out_summary_json,
        turbo_vector_store_index_summary(path, "load", 0, 0, 0));
    return -1;
  }

  document.id = turbo_json_get_string(document_json, "id");
  document.uri = turbo_json_get_string(document_json, "uri");
  document.kind = turbo_json_get_string(document_json, "kind");
  document.title = turbo_json_get_string(document_json, "title");
  document.text = turbo_json_get_string(document_json, "text");
  rc = turbo_vector_store_index_text(store, embedding_model, &document,
                                     splitter_options, out_summary_json);
  turbo_free_json(&document_json);
  return rc;
}

static int turbo_vector_store_make_directory_summary(
    const char *root_dir, const char *kind,
    const turbo_vector_store_directory_index_state_t *state,
    json_value_t **out_summary_json) {
  json_value_t *summary;

  if (!out_summary_json) {
    return 0;
  }
  *out_summary_json = NULL;
  summary = turbo_json_create_object();
  if (!summary) {
    return -1;
  }
  turbo_json_object_set_string(summary, "root_dir", root_dir ? root_dir : "");
  turbo_json_object_set_string(summary, "kind", kind ? kind : "file");
  turbo_json_object_set_string(
      summary, "include_extensions",
      state->include_extensions ? state->include_extensions : "");
  turbo_json_object_set_number(summary, "chunk_size",
                               (double)state->splitter_options.chunk_size);
  turbo_json_object_set_number(summary, "chunk_overlap",
                               (double)state->splitter_options.chunk_overlap);
  turbo_json_object_set_bool(summary, "recursive", state->recursive ? 1 : 0);
  turbo_json_object_set_number(summary, "max_files", (double)state->max_files);
  turbo_json_object_set_number(summary, "max_file_bytes",
                               (double)state->max_file_bytes);
  turbo_json_object_set_number(summary, "visited", (double)state->visited);
  turbo_json_object_set_number(summary, "indexed", (double)state->indexed);
  turbo_json_object_set_number(summary, "indexed_chunks",
                               (double)state->indexed_chunks);
  turbo_json_object_set_number(summary, "skipped", (double)state->skipped);
  turbo_json_object_set_number(summary, "skipped_by_extension",
                               (double)state->skipped_by_extension);
  turbo_json_object_set_number(summary, "skipped_too_large",
                               (double)state->skipped_too_large);
  turbo_json_object_set_number(summary, "skipped_index_error",
                               (double)state->skipped_index_error);
  turbo_json_object_set_number(summary, "failed", (double)state->failed);
  turbo_json_object_set_bool(summary, "truncated", state->truncated ? 1 : 0);
  *out_summary_json = summary;
  return 0;
}

static int turbo_vector_store_index_directory_file(
    turbo_vector_store_directory_index_state_t *state, const char *path) {
  json_value_t *summary = NULL;

  if (!state || !path) {
    return -1;
  }
  if (state->max_files > 0 && state->visited >= state->max_files) {
    state->truncated = 1;
    return 1;
  }
  state->visited++;
  if (!turbo_vector_store_extension_allowed(path, state->include_extensions)) {
    state->skipped++;
    state->skipped_by_extension++;
    return 0;
  }
  if (state->max_file_bytes > 0 &&
      turbo_vector_store_file_size(path) > state->max_file_bytes) {
    state->skipped++;
    state->skipped_too_large++;
    return 0;
  }
  if (turbo_vector_store_index_text_file(
          state->store, state->embedding_model, path,
          state->kind ? state->kind : "file", state->splitter_options_ptr,
          &summary) == 0) {
    state->indexed++;
    state->indexed_chunks += (size_t)turbo_json_get_int(summary, "indexed", 0);
    turbo_free_json(&summary);
    return 0;
  }

  turbo_free_json(&summary);
  state->skipped++;
  state->skipped_index_error++;
  return 0;
}

static int turbo_vector_store_index_directory_walk(
    turbo_vector_store_directory_index_state_t *state, const char *dir) {
#ifdef _WIN32
  WIN32_FIND_DATAA find_data;
  HANDLE handle;
  char *pattern;

  pattern = turbo_vector_store_join_path(dir, "*");
  if (!pattern) {
    return -1;
  }
  handle = FindFirstFileA(pattern, &find_data);
  free(pattern);
  if (handle == INVALID_HANDLE_VALUE) {
    state->failed++;
    return -1;
  }
  do {
    const char *name = find_data.cFileName;
    char *path;
    int rc;

    if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (!state->recursive ||
          turbo_vector_store_should_skip_directory(name)) {
        continue;
      }
      path = turbo_vector_store_join_path(dir, name);
      if (!path) {
        FindClose(handle);
        return -1;
      }
      rc = turbo_vector_store_index_directory_walk(state, path);
      free(path);
      if (rc != 0) {
        FindClose(handle);
        return rc;
      }
      continue;
    }

    path = turbo_vector_store_join_path(dir, name);
    if (!path) {
      FindClose(handle);
      return -1;
    }
    rc = turbo_vector_store_index_directory_file(state, path);
    free(path);
    if (rc != 0) {
      FindClose(handle);
      return rc;
    }
  } while (FindNextFileA(handle, &find_data));
  FindClose(handle);
  return 0;
#else
  DIR *handle;
  struct dirent *entry;

  handle = opendir(dir);
  if (!handle) {
    state->failed++;
    return -1;
  }
  while ((entry = readdir(handle)) != NULL) {
    const char *name = entry->d_name;
    char *path;
    int is_dir;
    int rc;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }
    path = turbo_vector_store_join_path(dir, name);
    if (!path) {
      closedir(handle);
      return -1;
    }
    is_dir = turbo_vector_store_path_is_directory(path);
    if (is_dir) {
      if (state->recursive &&
          !turbo_vector_store_should_skip_directory(name)) {
        rc = turbo_vector_store_index_directory_walk(state, path);
        free(path);
        if (rc != 0) {
          closedir(handle);
          return rc;
        }
      } else {
        free(path);
      }
      continue;
    }
    rc = turbo_vector_store_index_directory_file(state, path);
    free(path);
    if (rc != 0) {
      closedir(handle);
      return rc;
    }
  }
  closedir(handle);
  return 0;
#endif
}

int turbo_vector_store_index_directory_ex(
    turbo_vector_store_t *store, turbo_embedding_model_t *embedding_model,
    const char *root_dir,
    const turbo_vector_store_directory_options_t *options,
    json_value_t **out_summary_json) {
  turbo_vector_store_directory_index_state_t state;
  int rc;

  if (out_summary_json) {
    *out_summary_json = NULL;
  }
  if (!store || !embedding_model || !root_dir || !root_dir[0] ||
      !turbo_vector_store_path_is_directory(root_dir)) {
    return -1;
  }
  memset(&state, 0, sizeof(state));
  state.store = store;
  state.embedding_model = embedding_model;
  state.kind = options && options->kind ? options->kind : "file";
  if (options) {
    state.splitter_options = options->splitter_options;
  }
  state.splitter_options_ptr =
      options ? &state.splitter_options : NULL;
  state.recursive = options && options->recursive ? 1 : 0;
  state.max_files = options ? options->max_files : 0;
  state.max_file_bytes = options ? options->max_file_bytes : 0;
  state.include_extensions = options ? options->include_extensions : NULL;

  rc = turbo_vector_store_index_directory_walk(&state, root_dir);
  if (rc < 0) {
    if (out_summary_json) {
      *out_summary_json = NULL;
    }
    return -1;
  }
  if (turbo_vector_store_make_directory_summary(root_dir, state.kind, &state,
                                                out_summary_json) != 0) {
    return -1;
  }
  return 0;
}

int turbo_vector_store_index_directory(
    turbo_vector_store_t *store, turbo_embedding_model_t *embedding_model,
    const char *root_dir, const char *kind,
    const turbo_text_splitter_options_t *splitter_options, int recursive,
    size_t max_files, json_value_t **out_summary_json) {
  turbo_vector_store_directory_options_t options;

  memset(&options, 0, sizeof(options));
  options.kind = kind;
  if (splitter_options) {
    options.splitter_options = *splitter_options;
  }
  options.recursive = recursive;
  options.max_files = max_files;
  return turbo_vector_store_index_directory_ex(store, embedding_model, root_dir,
                                               &options, out_summary_json);
}

size_t turbo_vector_store_count(const turbo_vector_store_t *store) {
  size_t count = 0;

  if (!store) {
    return 0;
  }
  if (store->backend == TURBO_VECTOR_STORE_BACKEND_SQLITE) {
    if (turbo_vector_sqlite_count_rows(
            store->db, "SELECT COUNT(*) FROM vector_documents;", &count) != 0) {
      return 0;
    }
    return count;
  }
  return store->count;
}

turbo_vector_store_tool_binding_t *
turbo_vector_store_tool_binding_create(turbo_vector_store_t *store,
                                       turbo_embedding_model_t *embedding_model) {
  turbo_vector_store_tool_binding_t *binding;

  if (!store || !embedding_model) {
    return NULL;
  }
  binding = (turbo_vector_store_tool_binding_t *)calloc(1, sizeof(*binding));
  if (!binding) {
    return NULL;
  }
  binding->store = store;
  binding->embedding_model = embedding_model;
  return binding;
}

void turbo_vector_store_tool_binding_destroy(
    turbo_vector_store_tool_binding_t *binding) {
  free(binding);
}

static int turbo_vector_tool_graph_add_node(json_value_t *nodes,
                                            const char *name,
                                            const char *kind,
                                            const char *description) {
  json_value_t *node = turbo_json_create_object();

  if (!node) {
    return -1;
  }
  turbo_json_object_set_string(node, "name", name);
  turbo_json_object_set_string(node, "kind", kind);
  turbo_json_object_set_string(node, "description", description);
  turbo_json_array_add(nodes, node);
  return 0;
}

static int turbo_vector_tool_graph_add_edge(json_value_t *edges,
                                            const char *from, const char *to,
                                            const char *reason) {
  json_value_t *edge = turbo_json_create_object();

  if (!edge) {
    return -1;
  }
  turbo_json_object_set_string(edge, "from", from);
  turbo_json_object_set_string(edge, "to", to);
  turbo_json_object_set_string(edge, "reason", reason);
  turbo_json_array_add(edges, edge);
  return 0;
}

int turbo_vector_store_tool_graph(json_value_t **out_graph_json) {
  json_value_t *graph;
  json_value_t *nodes;
  json_value_t *edges;

  if (!out_graph_json) {
    return -1;
  }
  *out_graph_json = NULL;
  graph = turbo_json_create_object();
  nodes = turbo_json_create_array();
  edges = turbo_json_create_array();
  if (!graph || !nodes || !edges) {
    turbo_free_json(&graph);
    turbo_free_json(&nodes);
    turbo_free_json(&edges);
    return -1;
  }
  if (turbo_vector_tool_graph_add_node(
          nodes, "agent.vector.stats", "observe",
          "Inspect local vector store statistics.") != 0 ||
      turbo_vector_tool_graph_add_node(
          nodes, "agent.vector.search", "observe",
          "Search the local vector store using the configured embedding model.") != 0 ||
      turbo_vector_tool_graph_add_node(
          nodes, "agent.vector.build_context", "observe",
          "Build model context plus evidence from vector search results.") != 0 ||
      turbo_vector_tool_graph_add_node(
          nodes, "agent.vector.tool_graph", "observe",
          "Describe recommended dependencies between vector tools.") != 0 ||
      turbo_vector_tool_graph_add_edge(
          edges, "agent.vector.stats", "agent.vector.search",
          "Inspect available local vector context before searching.") != 0 ||
      turbo_vector_tool_graph_add_edge(
          edges, "agent.vector.search", "agent.vector.build_context",
          "Build context from search results before planner/executor use.") != 0) {
    turbo_free_json(&graph);
    turbo_free_json(&nodes);
    turbo_free_json(&edges);
    return -1;
  }
  turbo_json_object_add(graph, "nodes", nodes);
  turbo_json_object_add(graph, "edges", edges);
  *out_graph_json = graph;
  return 0;
}

static int turbo_vector_bind_set_string(turbo_runtime_data_bind_value_t *object,
                                        const char *key, const char *value) {
  turbo_runtime_data_bind_value_t *field =
      turbo_runtime_data_bind_value_create_string(value ? value : "");

  if (!field) {
    return -1;
  }
  if (turbo_runtime_data_bind_object_set(object, key, field) !=
      TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(field);
    return -1;
  }
  return 0;
}

static int turbo_vector_bind_set_bool(turbo_runtime_data_bind_value_t *object,
                                      const char *key, int value) {
  turbo_runtime_data_bind_value_t *field =
      turbo_runtime_data_bind_value_create_bool(value ? 1 : 0);

  if (!field) {
    return -1;
  }
  if (turbo_runtime_data_bind_object_set(object, key, field) !=
      TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(field);
    return -1;
  }
  return 0;
}

static turbo_retriever_t *turbo_vector_tool_create_retriever(
    turbo_vector_store_tool_binding_t *binding) {
  if (!binding || !binding->store || !binding->embedding_model) {
    return NULL;
  }
  return turbo_retriever_from_vector_store(binding->store,
                                           binding->embedding_model);
}

static int turbo_vector_search_json(turbo_vector_store_tool_binding_t *binding,
                                    const char *query, const char *kind,
                                    const char *uri_prefix, size_t limit,
                                    json_value_t **out_results_json) {
  turbo_retriever_t *retriever;
  turbo_retriever_query_options_t options = {0};
  int rc;

  if (!binding || !query || !query[0] || !out_results_json) {
    return -1;
  }
  retriever = turbo_vector_tool_create_retriever(binding);
  if (!retriever) {
    return -1;
  }
  options.kind = kind;
  options.uri_prefix = uri_prefix;
  options.limit = limit;
  rc = turbo_retriever_query(retriever, query, &options, out_results_json);
  turbo_retriever_destroy(retriever);
  return rc;
}

static int turbo_vector_build_context_json(
    turbo_vector_store_tool_binding_t *binding, const char *query,
    const char *kind, const char *uri_prefix, size_t limit,
    json_value_t **out_context_json) {
  turbo_retriever_t *retriever;
  turbo_retriever_query_options_t options = {0};
  int rc;

  if (!binding || !query || !query[0] || !out_context_json) {
    return -1;
  }
  retriever = turbo_vector_tool_create_retriever(binding);
  if (!retriever) {
    return -1;
  }
  options.kind = kind;
  options.uri_prefix = uri_prefix;
  options.limit = limit;
  rc = turbo_retriever_build_context(retriever, query, &options,
                                     out_context_json);
  turbo_retriever_destroy(retriever);
  return rc;
}

static int turbo_vector_stats_json(turbo_vector_store_tool_binding_t *binding,
                                   json_value_t **out_stats_json) {
  json_value_t *stats;

  if (!binding || !binding->store || !out_stats_json) {
    return -1;
  }
  *out_stats_json = NULL;
  stats = turbo_json_create_object();
  if (!stats) {
    return -1;
  }
  turbo_json_object_set_number(stats, "document_count",
                               (double)turbo_vector_store_count(binding->store));
  *out_stats_json = stats;
  return 0;
}

static int turbo_vector_search_tool_bind(
    const turbo_runtime_data_bind_value_t *arguments,
    turbo_runtime_data_bind_value_t **out_result, void *user_data) {
  turbo_vector_store_tool_binding_t *binding =
      (turbo_vector_store_tool_binding_t *)user_data;
  const char *query;
  const char *kind;
  const char *uri_prefix;
  int64_t limit;
  json_value_t *results_json = NULL;
  turbo_runtime_data_bind_value_t *results_bind = NULL;
  turbo_runtime_data_bind_value_t *result = NULL;

  if (!binding || !arguments || !out_result) {
    return -1;
  }
  *out_result = NULL;
  query = turbo_runtime_data_bind_value_as_string(
      turbo_runtime_data_bind_object_get(arguments, "query"));
  kind = turbo_runtime_data_bind_value_as_string(
      turbo_runtime_data_bind_object_get(arguments, "kind"));
  uri_prefix = turbo_runtime_data_bind_value_as_string(
      turbo_runtime_data_bind_object_get(arguments, "uri_prefix"));
  limit = turbo_runtime_data_bind_value_as_int64(
      turbo_runtime_data_bind_object_get(arguments, "limit"), 8);
  if (!query || !query[0] || limit < 0) {
    return -1;
  }
  if (turbo_vector_search_json(binding, query, kind, uri_prefix, (size_t)limit,
                               &results_json) != 0) {
    return -1;
  }
  results_bind = turbo_runtime_data_bind_value_from_json(results_json);
  turbo_free_json(&results_json);
  result = turbo_runtime_data_bind_value_create_object();
  if (!result || !results_bind ||
      turbo_vector_bind_set_bool(result, "ok", 1) != 0 ||
      turbo_vector_bind_set_string(result, "summary",
                                   "vector search completed") != 0 ||
      turbo_runtime_data_bind_object_set(result, "results", results_bind) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(results_bind);
    turbo_runtime_data_bind_value_destroy(result);
    return -1;
  }
  *out_result = result;
  return 0;
}

static int turbo_vector_build_context_tool_bind(
    const turbo_runtime_data_bind_value_t *arguments,
    turbo_runtime_data_bind_value_t **out_result, void *user_data) {
  turbo_vector_store_tool_binding_t *binding =
      (turbo_vector_store_tool_binding_t *)user_data;
  const char *query;
  const char *kind;
  const char *uri_prefix;
  int64_t limit;
  json_value_t *context_json = NULL;
  turbo_runtime_data_bind_value_t *context_bind = NULL;
  turbo_runtime_data_bind_value_t *result = NULL;

  if (!binding || !arguments || !out_result) {
    return -1;
  }
  *out_result = NULL;
  query = turbo_runtime_data_bind_value_as_string(
      turbo_runtime_data_bind_object_get(arguments, "query"));
  kind = turbo_runtime_data_bind_value_as_string(
      turbo_runtime_data_bind_object_get(arguments, "kind"));
  uri_prefix = turbo_runtime_data_bind_value_as_string(
      turbo_runtime_data_bind_object_get(arguments, "uri_prefix"));
  limit = turbo_runtime_data_bind_value_as_int64(
      turbo_runtime_data_bind_object_get(arguments, "limit"), 8);
  if (!query || !query[0] || limit < 0) {
    return -1;
  }
  if (turbo_vector_build_context_json(binding, query, kind, uri_prefix,
                                      (size_t)limit, &context_json) != 0) {
    return -1;
  }
  context_bind = turbo_runtime_data_bind_value_from_json(context_json);
  turbo_free_json(&context_json);
  result = turbo_runtime_data_bind_value_create_object();
  if (!result || !context_bind ||
      turbo_vector_bind_set_bool(result, "ok", 1) != 0 ||
      turbo_vector_bind_set_string(result, "summary",
                                   "vector context built") != 0 ||
      turbo_runtime_data_bind_object_set(result, "context", context_bind) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(context_bind);
    turbo_runtime_data_bind_value_destroy(result);
    return -1;
  }
  *out_result = result;
  return 0;
}

static int turbo_vector_stats_tool_bind(
    const turbo_runtime_data_bind_value_t *arguments,
    turbo_runtime_data_bind_value_t **out_result, void *user_data) {
  turbo_vector_store_tool_binding_t *binding =
      (turbo_vector_store_tool_binding_t *)user_data;
  json_value_t *stats_json = NULL;
  turbo_runtime_data_bind_value_t *stats_bind = NULL;
  turbo_runtime_data_bind_value_t *result = NULL;

  (void)arguments;
  if (!binding || !out_result) {
    return -1;
  }
  *out_result = NULL;
  if (turbo_vector_stats_json(binding, &stats_json) != 0) {
    return -1;
  }
  stats_bind = turbo_runtime_data_bind_value_from_json(stats_json);
  turbo_free_json(&stats_json);
  result = turbo_runtime_data_bind_value_create_object();
  if (!result || !stats_bind ||
      turbo_vector_bind_set_bool(result, "ok", 1) != 0 ||
      turbo_vector_bind_set_string(result, "summary",
                                   "vector stats loaded") != 0 ||
      turbo_runtime_data_bind_object_set(result, "stats", stats_bind) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(stats_bind);
    turbo_runtime_data_bind_value_destroy(result);
    return -1;
  }
  *out_result = result;
  return 0;
}

static int turbo_vector_tool_graph_tool_bind(
    const turbo_runtime_data_bind_value_t *arguments,
    turbo_runtime_data_bind_value_t **out_result, void *user_data) {
  json_value_t *graph_json = NULL;
  turbo_runtime_data_bind_value_t *graph_bind = NULL;
  turbo_runtime_data_bind_value_t *result = NULL;

  (void)arguments;
  (void)user_data;
  if (!out_result) {
    return -1;
  }
  *out_result = NULL;
  if (turbo_vector_store_tool_graph(&graph_json) != 0) {
    return -1;
  }
  graph_bind = turbo_runtime_data_bind_value_from_json(graph_json);
  turbo_free_json(&graph_json);
  result = turbo_runtime_data_bind_value_create_object();
  if (!result || !graph_bind ||
      turbo_vector_bind_set_bool(result, "ok", 1) != 0 ||
      turbo_vector_bind_set_string(result, "summary",
                                   "vector tool graph loaded") != 0 ||
      turbo_runtime_data_bind_object_set(result, "graph", graph_bind) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(graph_bind);
    turbo_runtime_data_bind_value_destroy(result);
    return -1;
  }
  *out_result = result;
  return 0;
}

static int turbo_vector_tool_json_handler(
    const char *arguments_json, char **out_output,
    int (*bind_handler)(const turbo_runtime_data_bind_value_t *,
                        turbo_runtime_data_bind_value_t **, void *),
    void *user_data) {
  json_value_t *args_json = NULL;
  json_value_t *result_json = NULL;
  turbo_runtime_data_bind_value_t *args = NULL;
  turbo_runtime_data_bind_value_t *result = NULL;
  char *serialized = NULL;
  int rc;

  if (!out_output || !bind_handler) {
    return -1;
  }
  *out_output = NULL;
  if (turbo_parse_json((const uint8_t *)(arguments_json ? arguments_json : "{}"),
                       strlen(arguments_json ? arguments_json : "{}"),
                       &args_json) != 0 ||
      !args_json) {
    return -1;
  }
  args = turbo_runtime_data_bind_value_from_json(args_json);
  turbo_free_json(&args_json);
  if (!args) {
    return -1;
  }
  rc = bind_handler(args, &result, user_data);
  turbo_runtime_data_bind_value_destroy(args);
  if (rc != 0 || !result) {
    turbo_runtime_data_bind_value_destroy(result);
    return -1;
  }
  result_json = turbo_runtime_data_bind_value_to_json(result);
  turbo_runtime_data_bind_value_destroy(result);
  if (!result_json) {
    return -1;
  }
  serialized = turbo_json_serialize(result_json, NULL);
  turbo_free_json(&result_json);
  if (!serialized) {
    return -1;
  }
  *out_output = serialized;
  return 0;
}

static int turbo_vector_search_tool_json(const char *arguments_json,
                                         char **out_output, void *user_data) {
  return turbo_vector_tool_json_handler(arguments_json, out_output,
                                        turbo_vector_search_tool_bind,
                                        user_data);
}

static int turbo_vector_build_context_tool_json(const char *arguments_json,
                                                char **out_output,
                                                void *user_data) {
  return turbo_vector_tool_json_handler(arguments_json, out_output,
                                        turbo_vector_build_context_tool_bind,
                                        user_data);
}

static int turbo_vector_stats_tool_json(const char *arguments_json,
                                        char **out_output, void *user_data) {
  return turbo_vector_tool_json_handler(arguments_json, out_output,
                                        turbo_vector_stats_tool_bind,
                                        user_data);
}

static int turbo_vector_tool_graph_tool_json(const char *arguments_json,
                                             char **out_output,
                                             void *user_data) {
  return turbo_vector_tool_json_handler(arguments_json, out_output,
                                        turbo_vector_tool_graph_tool_bind,
                                        user_data);
}

static int turbo_vector_search_action_handler(const json_value_t *args,
                                              json_value_t **out_result,
                                              void *user_data) {
  turbo_vector_store_tool_binding_t *binding =
      (turbo_vector_store_tool_binding_t *)user_data;
  const char *query;
  const char *kind;
  const char *uri_prefix;
  int limit;
  json_value_t *results = NULL;
  json_value_t *result = NULL;

  if (!binding || !args || !out_result ||
      turbo_json_type(args) != TURBO_JSON_OBJECT) {
    return -1;
  }
  *out_result = NULL;
  query = turbo_json_get_string(args, "query");
  kind = turbo_json_get_string(args, "kind");
  uri_prefix = turbo_json_get_string(args, "uri_prefix");
  limit = turbo_json_get_int(args, "limit", 8);
  if (!query || !query[0] || limit < 0) {
    return -1;
  }
  if (turbo_vector_search_json(binding, query, kind, uri_prefix, (size_t)limit,
                               &results) != 0) {
    return -1;
  }
  result = turbo_action_result_create(1, "vector search completed");
  if (!result) {
    turbo_free_json(&results);
    return -1;
  }
  turbo_json_object_add(result, "results", results);
  *out_result = result;
  return 0;
}

static int turbo_vector_build_context_action_handler(const json_value_t *args,
                                                     json_value_t **out_result,
                                                     void *user_data) {
  turbo_vector_store_tool_binding_t *binding =
      (turbo_vector_store_tool_binding_t *)user_data;
  const char *query;
  const char *kind;
  const char *uri_prefix;
  int limit;
  json_value_t *context = NULL;
  json_value_t *result = NULL;

  if (!binding || !args || !out_result ||
      turbo_json_type(args) != TURBO_JSON_OBJECT) {
    return -1;
  }
  *out_result = NULL;
  query = turbo_json_get_string(args, "query");
  kind = turbo_json_get_string(args, "kind");
  uri_prefix = turbo_json_get_string(args, "uri_prefix");
  limit = turbo_json_get_int(args, "limit", 8);
  if (!query || !query[0] || limit < 0) {
    return -1;
  }
  if (turbo_vector_build_context_json(binding, query, kind, uri_prefix,
                                      (size_t)limit, &context) != 0) {
    return -1;
  }
  result = turbo_action_result_create(1, "vector context built");
  if (!result) {
    turbo_free_json(&context);
    return -1;
  }
  turbo_json_object_add(result, "context", context);
  *out_result = result;
  return 0;
}

static int turbo_vector_stats_action_handler(const json_value_t *args,
                                             json_value_t **out_result,
                                             void *user_data) {
  turbo_vector_store_tool_binding_t *binding =
      (turbo_vector_store_tool_binding_t *)user_data;
  json_value_t *stats = NULL;
  json_value_t *result = NULL;

  (void)args;
  if (!binding || !out_result) {
    return -1;
  }
  *out_result = NULL;
  if (turbo_vector_stats_json(binding, &stats) != 0) {
    return -1;
  }
  result = turbo_action_result_create(1, "vector stats loaded");
  if (!result) {
    turbo_free_json(&stats);
    return -1;
  }
  turbo_json_object_add(result, "stats", stats);
  *out_result = result;
  return 0;
}

static int turbo_vector_tool_graph_action_handler(const json_value_t *args,
                                                  json_value_t **out_result,
                                                  void *user_data) {
  json_value_t *graph = NULL;
  json_value_t *result = NULL;

  (void)args;
  (void)user_data;
  if (!out_result) {
    return -1;
  }
  *out_result = NULL;
  if (turbo_vector_store_tool_graph(&graph) != 0) {
    return -1;
  }
  result = turbo_action_result_create(1, "vector tool graph loaded");
  if (!result) {
    turbo_free_json(&graph);
    return -1;
  }
  turbo_json_object_add(result, "graph", graph);
  *out_result = result;
  return 0;
}

static int turbo_vector_upsert_text_action_handler(const json_value_t *args,
                                                   json_value_t **out_result,
                                                   void *user_data) {
  turbo_vector_store_tool_binding_t *binding =
      (turbo_vector_store_tool_binding_t *)user_data;
  turbo_vector_store_document_t document = {0};
  turbo_text_splitter_options_t splitter_options = {0};
  json_value_t *summary = NULL;
  json_value_t *result = NULL;
  const char *id;
  const char *uri;
  const char *kind;
  const char *title;
  const char *text;
  int chunk_size;
  int chunk_overlap;

  if (!binding || !binding->store || !binding->embedding_model || !args ||
      !out_result || turbo_json_type(args) != TURBO_JSON_OBJECT) {
    return -1;
  }
  *out_result = NULL;
  id = turbo_json_get_string(args, "id");
  uri = turbo_json_get_string(args, "uri");
  kind = turbo_json_get_string(args, "kind");
  title = turbo_json_get_string(args, "title");
  text = turbo_json_get_string(args, "text");
  chunk_size = turbo_json_get_int(args, "chunk_size", 0);
  chunk_overlap = turbo_json_get_int(args, "chunk_overlap", 0);
  if (!id || !id[0] || !text || chunk_size < 0 || chunk_overlap < 0) {
    return -1;
  }
  document.id = id;
  document.uri = uri && uri[0] ? uri : id;
  document.kind = kind && kind[0] ? kind : "note";
  document.title = title;
  document.text = text;
  splitter_options.chunk_size = (size_t)chunk_size;
  splitter_options.chunk_overlap = (size_t)chunk_overlap;
  if (turbo_vector_store_index_text(binding->store, binding->embedding_model,
                                    &document, &splitter_options,
                                    &summary) != 0) {
    turbo_free_json(&summary);
    return -1;
  }
  result = turbo_action_result_create(1, "vector text indexed");
  if (!result) {
    turbo_free_json(&summary);
    return -1;
  }
  turbo_json_object_set_string(result, "document_id", document.id);
  turbo_json_object_set_string(result, "uri", document.uri);
  turbo_json_object_set_string(result, "kind", document.kind);
  turbo_json_object_add(result, "index", summary);
  *out_result = result;
  return 0;
}

static int turbo_vector_index_file_action_handler(const json_value_t *args,
                                                  json_value_t **out_result,
                                                  void *user_data) {
  turbo_vector_store_tool_binding_t *binding =
      (turbo_vector_store_tool_binding_t *)user_data;
  turbo_text_splitter_options_t splitter_options = {0};
  const char *path;
  const char *kind;
  int chunk_size;
  int chunk_overlap;
  json_value_t *summary = NULL;
  json_value_t *result = NULL;

  if (!binding || !binding->store || !binding->embedding_model || !args ||
      !out_result || turbo_json_type(args) != TURBO_JSON_OBJECT) {
    return -1;
  }
  *out_result = NULL;
  path = turbo_json_get_string(args, "path");
  kind = turbo_json_get_string(args, "kind");
  chunk_size = turbo_json_get_int(args, "chunk_size", 0);
  chunk_overlap = turbo_json_get_int(args, "chunk_overlap", 0);
  if (!path || !path[0] || chunk_size < 0 || chunk_overlap < 0) {
    return -1;
  }
  splitter_options.chunk_size = (size_t)chunk_size;
  splitter_options.chunk_overlap = (size_t)chunk_overlap;
  if (turbo_vector_store_index_text_file(binding->store,
                                         binding->embedding_model, path,
                                         kind ? kind : "file",
                                         &splitter_options, &summary) != 0) {
    turbo_free_json(&summary);
    return -1;
  }
  result = turbo_action_result_create(1, "vector file indexed");
  if (!result) {
    turbo_free_json(&summary);
    return -1;
  }
  turbo_json_object_set_string(result, "uri", path);
  turbo_json_object_add(result, "index", summary);
  *out_result = result;
  return 0;
}

static int turbo_vector_index_directory_action_handler(
    const json_value_t *args, json_value_t **out_result, void *user_data) {
  turbo_vector_store_tool_binding_t *binding =
      (turbo_vector_store_tool_binding_t *)user_data;
  turbo_vector_store_directory_options_t options = {0};
  const char *root_dir;
  int chunk_size;
  int chunk_overlap;
  int max_files;
  int max_file_bytes;
  json_value_t *summary = NULL;
  json_value_t *result = NULL;

  if (!binding || !binding->store || !binding->embedding_model || !args ||
      !out_result || turbo_json_type(args) != TURBO_JSON_OBJECT) {
    return -1;
  }
  *out_result = NULL;
  root_dir = turbo_json_get_string(args, "root_dir");
  chunk_size = turbo_json_get_int(args, "chunk_size", 0);
  chunk_overlap = turbo_json_get_int(args, "chunk_overlap", 0);
  max_files = turbo_json_get_int(args, "max_files", 0);
  max_file_bytes = turbo_json_get_int(args, "max_file_bytes", 0);
  if (!root_dir || !root_dir[0] || chunk_size < 0 || chunk_overlap < 0 ||
      max_files < 0 || max_file_bytes < 0) {
    return -1;
  }
  options.kind = turbo_json_get_string(args, "kind");
  options.splitter_options.chunk_size = (size_t)chunk_size;
  options.splitter_options.chunk_overlap = (size_t)chunk_overlap;
  options.recursive = turbo_json_get_bool(args, "recursive", 1) ? 1 : 0;
  options.max_files = (size_t)max_files;
  options.max_file_bytes = (size_t)max_file_bytes;
  options.include_extensions = turbo_json_get_string(args, "include_extensions");
  if (turbo_vector_store_index_directory_ex(binding->store,
                                            binding->embedding_model,
                                            root_dir, &options,
                                            &summary) != 0) {
    turbo_free_json(&summary);
    return -1;
  }
  result = turbo_action_result_create(1, "vector directory indexed");
  if (!result) {
    turbo_free_json(&summary);
    return -1;
  }
  turbo_json_object_set_string(result, "root_dir", root_dir);
  turbo_json_object_add(result, "index", summary);
  *out_result = result;
  return 0;
}

static int turbo_vector_delete_document_action_handler(const json_value_t *args,
                                                       json_value_t **out_result,
                                                       void *user_data) {
  turbo_vector_store_tool_binding_t *binding =
      (turbo_vector_store_tool_binding_t *)user_data;
  const char *document_id;
  json_value_t *result;

  if (!binding || !binding->store || !args || !out_result ||
      turbo_json_type(args) != TURBO_JSON_OBJECT) {
    return -1;
  }
  *out_result = NULL;
  document_id = turbo_json_get_string(args, "document_id");
  if (!document_id || !document_id[0]) {
    return -1;
  }
  if (turbo_vector_store_delete_document(binding->store, document_id) != 0) {
    return -1;
  }
  result = turbo_action_result_create(1, "vector document deleted");
  if (!result) {
    return -1;
  }
  turbo_json_object_set_string(result, "document_id", document_id);
  *out_result = result;
  return 0;
}

int turbo_vector_store_add_tools(turbo_tool_registry_t *registry,
                                 turbo_vector_store_tool_binding_t *binding) {
  static const char *query_schema =
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},"
      "\"kind\":{\"type\":\"string\"},"
      "\"uri_prefix\":{\"type\":\"string\"},"
      "\"limit\":{\"type\":\"integer\",\"minimum\":0}},"
      "\"required\":[\"query\"],\"additionalProperties\":false}";
  static const char *empty_schema =
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}";
  turbo_tool_definition_t definition;

  if (!registry || !binding) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.vector.search";
  definition.description = "Search the local vector store.";
  definition.parameters_json = query_schema;
  definition.strict = 1;
  definition.handler = turbo_vector_search_tool_json;
  definition.bind_handler = turbo_vector_search_tool_bind;
  definition.user_data = binding;
  if (turbo_tool_registry_add(registry, &definition) != TURBO_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.vector.build_context";
  definition.description =
      "Build local vector context text and evidence from search results.";
  definition.parameters_json = query_schema;
  definition.strict = 1;
  definition.handler = turbo_vector_build_context_tool_json;
  definition.bind_handler = turbo_vector_build_context_tool_bind;
  definition.user_data = binding;
  if (turbo_tool_registry_add(registry, &definition) != TURBO_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.vector.stats";
  definition.description = "Inspect local vector store statistics.";
  definition.parameters_json = empty_schema;
  definition.strict = 1;
  definition.handler = turbo_vector_stats_tool_json;
  definition.bind_handler = turbo_vector_stats_tool_bind;
  definition.user_data = binding;
  if (turbo_tool_registry_add(registry, &definition) != TURBO_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.vector.tool_graph";
  definition.description =
      "Describe recommended dependencies between local vector tools.";
  definition.parameters_json = empty_schema;
  definition.strict = 1;
  definition.handler = turbo_vector_tool_graph_tool_json;
  definition.bind_handler = turbo_vector_tool_graph_tool_bind;
  definition.user_data = binding;
  if (turbo_tool_registry_add(registry, &definition) != TURBO_TOOL_OK) {
    return -1;
  }

  return 0;
}

int turbo_vector_store_add_action_tools(
    turbo_action_tool_registry_t *registry,
    turbo_vector_store_tool_binding_t *binding) {
  static const char *query_schema =
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},"
      "\"kind\":{\"type\":\"string\"},"
      "\"uri_prefix\":{\"type\":\"string\"},"
      "\"limit\":{\"type\":\"integer\",\"minimum\":0}},"
      "\"required\":[\"query\"],\"additionalProperties\":false}";
  static const char *empty_schema =
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}";
  turbo_action_tool_definition_t definition;

  if (!registry || !binding) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.vector.search";
  definition.description = "Search the local vector store.";
  definition.parameters_json = query_schema;
  definition.kind = TURBO_ACTION_OBSERVE;
  definition.idempotent = 1;
  definition.handler = turbo_vector_search_action_handler;
  definition.user_data = binding;
  if (turbo_action_tool_registry_add(registry, &definition) !=
      TURBO_ACTION_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.vector.build_context";
  definition.description =
      "Build local vector context text and evidence from search results.";
  definition.parameters_json = query_schema;
  definition.kind = TURBO_ACTION_OBSERVE;
  definition.idempotent = 1;
  definition.handler = turbo_vector_build_context_action_handler;
  definition.user_data = binding;
  if (turbo_action_tool_registry_add(registry, &definition) !=
      TURBO_ACTION_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.vector.stats";
  definition.description = "Inspect local vector store statistics.";
  definition.parameters_json = empty_schema;
  definition.kind = TURBO_ACTION_OBSERVE;
  definition.idempotent = 1;
  definition.handler = turbo_vector_stats_action_handler;
  definition.user_data = binding;
  if (turbo_action_tool_registry_add(registry, &definition) !=
      TURBO_ACTION_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.vector.tool_graph";
  definition.description =
      "Describe recommended dependencies between local vector tools.";
  definition.parameters_json = empty_schema;
  definition.kind = TURBO_ACTION_OBSERVE;
  definition.idempotent = 1;
  definition.handler = turbo_vector_tool_graph_action_handler;
  definition.user_data = binding;
  if (turbo_action_tool_registry_add(registry, &definition) !=
      TURBO_ACTION_TOOL_OK) {
    return -1;
  }

  return 0;
}

int turbo_vector_store_add_indexing_action_tools(
    turbo_action_tool_registry_t *registry,
    turbo_vector_store_tool_binding_t *binding) {
  static const char *upsert_text_schema =
      "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},"
      "\"uri\":{\"type\":\"string\"},\"kind\":{\"type\":\"string\"},"
      "\"title\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"},"
      "\"chunk_size\":{\"type\":\"integer\",\"minimum\":0},"
      "\"chunk_overlap\":{\"type\":\"integer\",\"minimum\":0}},"
      "\"required\":[\"id\",\"text\"],\"additionalProperties\":false}";
  static const char *index_file_schema =
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
      "\"kind\":{\"type\":\"string\"},"
      "\"chunk_size\":{\"type\":\"integer\",\"minimum\":0},"
      "\"chunk_overlap\":{\"type\":\"integer\",\"minimum\":0}},"
      "\"required\":[\"path\"],\"additionalProperties\":false}";
  static const char *index_directory_schema =
      "{\"type\":\"object\",\"properties\":{\"root_dir\":{\"type\":\"string\"},"
      "\"kind\":{\"type\":\"string\"},\"recursive\":{\"type\":\"boolean\"},"
      "\"max_files\":{\"type\":\"integer\",\"minimum\":0},"
      "\"max_file_bytes\":{\"type\":\"integer\",\"minimum\":0},"
      "\"include_extensions\":{\"type\":\"string\"},"
      "\"chunk_size\":{\"type\":\"integer\",\"minimum\":0},"
      "\"chunk_overlap\":{\"type\":\"integer\",\"minimum\":0}},"
      "\"required\":[\"root_dir\"],\"additionalProperties\":false}";
  static const char *delete_document_schema =
      "{\"type\":\"object\",\"properties\":{\"document_id\":{\"type\":\"string\"}},"
      "\"required\":[\"document_id\"],\"additionalProperties\":false}";
  turbo_action_tool_definition_t definition;

  if (!registry || !binding) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.vector.upsert_text";
  definition.description =
      "Embed and index text chunks into the local vector store.";
  definition.parameters_json = upsert_text_schema;
  definition.kind = TURBO_ACTION_MUTATE;
  definition.idempotent = 1;
  definition.handler = turbo_vector_upsert_text_action_handler;
  definition.user_data = binding;
  if (turbo_action_tool_registry_add(registry, &definition) !=
      TURBO_ACTION_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.vector.index_file";
  definition.description =
      "Embed and index one local text file into the local vector store.";
  definition.parameters_json = index_file_schema;
  definition.kind = TURBO_ACTION_MUTATE;
  definition.idempotent = 1;
  definition.handler = turbo_vector_index_file_action_handler;
  definition.user_data = binding;
  if (turbo_action_tool_registry_add(registry, &definition) !=
      TURBO_ACTION_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.vector.index_directory";
  definition.description =
      "Embed and index local text files under a directory into the local vector store.";
  definition.parameters_json = index_directory_schema;
  definition.kind = TURBO_ACTION_MUTATE;
  definition.idempotent = 1;
  definition.handler = turbo_vector_index_directory_action_handler;
  definition.user_data = binding;
  if (turbo_action_tool_registry_add(registry, &definition) !=
      TURBO_ACTION_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.vector.delete_document";
  definition.description = "Delete one vector document by exact document id.";
  definition.parameters_json = delete_document_schema;
  definition.kind = TURBO_ACTION_MUTATE;
  definition.idempotent = 1;
  definition.handler = turbo_vector_delete_document_action_handler;
  definition.user_data = binding;
  if (turbo_action_tool_registry_add(registry, &definition) !=
      TURBO_ACTION_TOOL_OK) {
    return -1;
  }

  return 0;
}
