#include "turbo_agent_knowledge_store.h"

#include "turbo_agent_state.h"
#include "turbo_agent_workflow.h"
#include "sqlite3.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

struct turbo_agent_knowledge_store_s {
  sqlite3 *db;
  int fts5_enabled;
};

#define TURBO_KNOWLEDGE_QUERY_MAX_TOKENS 16

typedef struct turbo_knowledge_query_token_s {
  const char *text;
  size_t len;
} turbo_knowledge_query_token_t;

typedef struct turbo_knowledge_directory_index_state_s {
  turbo_agent_knowledge_store_t *store;
  const char *kind;
  size_t chunk_target_bytes;
  int recursive;
  size_t max_files;
  size_t max_file_bytes;
  const char *include_extensions;
  size_t visited;
  size_t indexed;
  size_t skipped;
  size_t skipped_by_extension;
  size_t skipped_too_large;
  size_t skipped_index_error;
  size_t failed;
  int truncated;
} turbo_knowledge_directory_index_state_t;

static char *turbo_knowledge_strdup(const char *text) {
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

static int turbo_knowledge_exec(sqlite3 *db, const char *sql) {
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

static int turbo_knowledge_prepare(sqlite3 *db, const char *sql, sqlite3_stmt **out_stmt) {
  if (!db || !sql || !out_stmt) {
    return -1;
  }
  *out_stmt = NULL;
  return sqlite3_prepare_v2(db, sql, -1, out_stmt, NULL) == SQLITE_OK ? 0 : -1;
}

static uint64_t turbo_knowledge_hash_bytes(const unsigned char *data, size_t size) {
  uint64_t hash = 1469598103934665603ull;
  size_t i;

  for (i = 0; i < size; ++i) {
    hash ^= (uint64_t)data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

static char *turbo_knowledge_hash_text(const char *text, size_t size) {
  char buffer[32];
  uint64_t hash;

  if (!text) {
    return NULL;
  }
  hash = turbo_knowledge_hash_bytes((const unsigned char *)text, size);
  snprintf(buffer, sizeof(buffer), "%016llx", (unsigned long long)hash);
  return turbo_knowledge_strdup(buffer);
}

static char *turbo_knowledge_read_file(const char *path, size_t *out_size, int *out_mtime) {
  FILE *file;
  long size_long;
  size_t size;
  char *buffer;
  struct stat st;

  if (!path) {
    return NULL;
  }
  file = fopen(path, "rb");
  if (!file) {
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  size_long = ftell(file);
  if (size_long < 0) {
    fclose(file);
    return NULL;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  size = (size_t)size_long;
  buffer = (char *)malloc(size + 1);
  if (!buffer) {
    fclose(file);
    return NULL;
  }
  if (size > 0 && fread(buffer, 1, size, file) != size) {
    free(buffer);
    fclose(file);
    return NULL;
  }
  fclose(file);
  buffer[size] = '\0';
  if (out_size) {
    *out_size = size;
  }
  if (out_mtime) {
    *out_mtime = stat(path, &st) == 0 ? (int)st.st_mtime : 0;
  }
  return buffer;
}

static int turbo_knowledge_text_is_binary(const char *text, size_t size) {
  return size > 0 && memchr(text, '\0', size) != NULL;
}

static char *turbo_knowledge_join_path(const char *dir, const char *name) {
  size_t dir_len;
  size_t name_len;
  int needs_sep;
  char *path;

  if (!dir || !name) {
    return NULL;
  }
  dir_len = strlen(dir);
  name_len = strlen(name);
  needs_sep = dir_len > 0 && dir[dir_len - 1] != '/' && dir[dir_len - 1] != '\\';
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

static int turbo_knowledge_path_is_directory(const char *path) {
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

static size_t turbo_knowledge_file_size(const char *path) {
  struct stat st;

  if (!path || stat(path, &st) != 0 || st.st_size < 0) {
    return 0;
  }
  return (size_t)st.st_size;
}

static int turbo_knowledge_ascii_char_equal_ignore_case(char a, char b) {
  return tolower((unsigned char)a) == tolower((unsigned char)b);
}

static int turbo_knowledge_extension_token_matches(const char *extension,
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
  while (token_len > 0 && (token[token_len - 1] == ' ' ||
                           token[token_len - 1] == '\t')) {
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
    if (!turbo_knowledge_ascii_char_equal_ignore_case(expected[i], token[i])) {
      return 0;
    }
  }
  return 1;
}

static int turbo_knowledge_extension_allowed(const char *path,
                                             const char *include_extensions) {
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
      if (turbo_knowledge_extension_token_matches(extension, token,
                                                  (size_t)(include_extensions + i -
                                                           token))) {
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

static int turbo_knowledge_should_skip_directory(const char *name) {
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
         strcmp(name, "vcpkg_installed") == 0 || strcmp(name, "CMakeFiles") == 0 ||
         strncmp(name, "cmake-build-", 12) == 0;
}

static int turbo_knowledge_query_token_char(char ch) {
  unsigned char c = (unsigned char)ch;

  return isalnum(c) || ch == '_';
}

static size_t turbo_knowledge_collect_query_tokens(
    const char *query, turbo_knowledge_query_token_t *tokens, size_t max_tokens) {
  size_t len;
  size_t i = 0;
  size_t count = 0;

  if (!query || !tokens || max_tokens == 0) {
    return 0;
  }
  len = strlen(query);
  while (i < len && count < max_tokens) {
    size_t start;

    while (i < len && !turbo_knowledge_query_token_char(query[i])) {
      i++;
    }
    start = i;
    while (i < len && turbo_knowledge_query_token_char(query[i])) {
      i++;
    }
    if (i > start) {
      tokens[count].text = query + start;
      tokens[count].len = i - start;
      count++;
    }
  }
  return count;
}

static char *turbo_knowledge_build_phrase_query(const char *query) {
  size_t len;
  char *match;
  size_t out = 0;
  size_t i;

  if (!query || !query[0]) {
    return NULL;
  }
  len = strlen(query);
  match = (char *)malloc((len * 2) + 3);
  if (!match) {
    return NULL;
  }
  match[out++] = '"';
  for (i = 0; i < len; ++i) {
    if (query[i] == '"') {
      match[out++] = '"';
      match[out++] = '"';
    } else {
      match[out++] = query[i];
    }
  }
  match[out++] = '"';
  match[out] = '\0';
  return match;
}

static char *turbo_knowledge_build_match_query(const char *query) {
  turbo_knowledge_query_token_t tokens[TURBO_KNOWLEDGE_QUERY_MAX_TOKENS];
  size_t token_count;
  size_t match_len = 0;
  char *match;
  size_t out = 0;
  size_t i;

  if (!query || !query[0]) {
    return NULL;
  }
  token_count = turbo_knowledge_collect_query_tokens(
      query, tokens, TURBO_KNOWLEDGE_QUERY_MAX_TOKENS);
  if (token_count == 0) {
    return turbo_knowledge_build_phrase_query(query);
  }
  for (i = 0; i < token_count; ++i) {
    match_len += tokens[i].len + 2;
  }
  match_len += (token_count - 1) * 4;
  match = (char *)malloc(match_len + 1);
  if (!match) {
    return NULL;
  }
  for (i = 0; i < token_count; ++i) {
    if (i > 0) {
      memcpy(match + out, " OR ", 4);
      out += 4;
    }
    match[out++] = '"';
    memcpy(match + out, tokens[i].text, tokens[i].len);
    out += tokens[i].len;
    match[out++] = '"';
  }
  match[out] = '\0';
  return match;
}

static char *turbo_knowledge_build_like_pattern(const char *text, size_t len) {
  char *pattern;
  size_t out = 0;
  size_t i;

  if (!text) {
    return NULL;
  }
  pattern = (char *)malloc((len * 2) + 3);
  if (!pattern) {
    return NULL;
  }
  pattern[out++] = '%';
  for (i = 0; i < len; ++i) {
    if (text[i] == '\\' || text[i] == '%' || text[i] == '_') {
      pattern[out++] = '\\';
    }
    pattern[out++] = text[i];
  }
  pattern[out++] = '%';
  pattern[out] = '\0';
  return pattern;
}

static char *turbo_knowledge_build_like_query_sql(size_t token_count) {
  static const char *prefix =
      "SELECT c.id, c.document_id, d.uri, c.ordinal, c.text, "
      "d.kind, d.title, 0.0 AS score "
      "FROM chunks c "
      "JOIN documents d ON d.id=c.document_id "
      "WHERE (";
  static const char *term = "c.text LIKE ? ESCAPE '\\'";
  static const char *sep = " OR ";
  static const char *suffix =
      ") "
      "AND (? IS NULL OR d.kind=?) "
      "AND (? IS NULL OR d.uri LIKE ?) "
      "ORDER BY d.updated_at DESC, c.ordinal ASC LIMIT ?;";
  size_t effective_count = token_count == 0 ? 1 : token_count;
  size_t len;
  char *sql;
  size_t out = 0;
  size_t i;

  len = strlen(prefix) + strlen(suffix) +
        (strlen(term) * effective_count) +
        (strlen(sep) * (effective_count - 1));
  sql = (char *)malloc(len + 1);
  if (!sql) {
    return NULL;
  }
  memcpy(sql + out, prefix, strlen(prefix));
  out += strlen(prefix);
  for (i = 0; i < effective_count; ++i) {
    if (i > 0) {
      memcpy(sql + out, sep, strlen(sep));
      out += strlen(sep);
    }
    memcpy(sql + out, term, strlen(term));
    out += strlen(term);
  }
  memcpy(sql + out, suffix, strlen(suffix));
  out += strlen(suffix);
  sql[out] = '\0';
  return sql;
}

static char *turbo_knowledge_chunk_id(const char *document_id, size_t ordinal) {
  int needed;
  char *id;

  if (!document_id) {
    return NULL;
  }
  needed = snprintf(NULL, 0, "%s#%zu", document_id, ordinal);
  if (needed < 0) {
    return NULL;
  }
  id = (char *)malloc((size_t)needed + 1);
  if (!id) {
    return NULL;
  }
  snprintf(id, (size_t)needed + 1, "%s#%zu", document_id, ordinal);
  return id;
}

static int turbo_knowledge_init_schema(sqlite3 *db, int *out_fts5_enabled) {
  static const char *schema =
      "PRAGMA foreign_keys=ON;"
      "CREATE TABLE IF NOT EXISTS documents("
      "id TEXT PRIMARY KEY,"
      "uri TEXT NOT NULL UNIQUE,"
      "kind TEXT NOT NULL,"
      "title TEXT,"
      "mtime INTEGER NOT NULL DEFAULT 0,"
      "size INTEGER NOT NULL DEFAULT 0,"
      "content_hash TEXT NOT NULL,"
      "metadata_json TEXT,"
      "updated_at INTEGER NOT NULL DEFAULT (unixepoch()));"
      "CREATE TABLE IF NOT EXISTS chunks("
      "id TEXT PRIMARY KEY,"
      "document_id TEXT NOT NULL,"
      "ordinal INTEGER NOT NULL,"
      "text TEXT NOT NULL,"
      "metadata_json TEXT,"
      "FOREIGN KEY(document_id) REFERENCES documents(id) ON DELETE CASCADE);"
      "CREATE INDEX IF NOT EXISTS idx_chunks_document_id ON chunks(document_id);"
      "CREATE TABLE IF NOT EXISTS ingest_runs("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "uri TEXT NOT NULL,"
      "document_id TEXT,"
      "status TEXT NOT NULL,"
      "message TEXT,"
      "created_at INTEGER NOT NULL DEFAULT (unixepoch()));";
  static const char *fts5_schema =
      "CREATE VIRTUAL TABLE IF NOT EXISTS chunks_fts USING fts5("
      "chunk_id UNINDEXED, document_id UNINDEXED, uri UNINDEXED, text);";

  if (out_fts5_enabled) {
    *out_fts5_enabled = 0;
  }
  if (turbo_knowledge_exec(db, schema) != 0) {
    return -1;
  }
  if (turbo_knowledge_exec(db, fts5_schema) == 0 && out_fts5_enabled) {
    *out_fts5_enabled = 1;
  }
  return 0;
}

turbo_agent_knowledge_store_t *
turbo_agent_knowledge_store_sqlite_open(const char *db_path) {
  turbo_agent_knowledge_store_t *store;
  sqlite3 *db = NULL;
  int fts5_enabled = 0;

  if (!db_path || !db_path[0]) {
    return NULL;
  }
  if (sqlite3_open(db_path, &db) != SQLITE_OK) {
    if (db) {
      sqlite3_close(db);
    }
    return NULL;
  }
  if (turbo_knowledge_init_schema(db, &fts5_enabled) != 0) {
    sqlite3_close(db);
    return NULL;
  }
  store = (turbo_agent_knowledge_store_t *)calloc(1, sizeof(*store));
  if (!store) {
    sqlite3_close(db);
    return NULL;
  }
  store->db = db;
  store->fts5_enabled = fts5_enabled;
  return store;
}

void turbo_agent_knowledge_store_close(turbo_agent_knowledge_store_t *store) {
  if (!store) {
    return;
  }
  if (store->db) {
    sqlite3_close(store->db);
  }
  free(store);
}

static int turbo_knowledge_delete_document_tx(sqlite3 *db, const char *document_id) {
  sqlite3_stmt *stmt = NULL;
  int ok = 0;

  if (turbo_knowledge_prepare(db, "DELETE FROM chunks_fts WHERE document_id=?;", &stmt) == 0) {
    sqlite3_bind_text(stmt, 1, document_id, -1, SQLITE_TRANSIENT);
    ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok) {
      return -1;
    }
  }
  if (turbo_knowledge_prepare(db, "DELETE FROM chunks WHERE document_id=?;", &stmt) != 0) {
    return -1;
  }
  sqlite3_bind_text(stmt, 1, document_id, -1, SQLITE_TRANSIENT);
  ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  if (!ok) {
    return -1;
  }
  if (turbo_knowledge_prepare(db, "DELETE FROM documents WHERE id=?;", &stmt) != 0) {
    return -1;
  }
  sqlite3_bind_text(stmt, 1, document_id, -1, SQLITE_TRANSIENT);
  ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok ? 0 : -1;
}

static int turbo_knowledge_delete_document_or_uri_tx(sqlite3 *db, const char *document_id,
                                                     const char *uri) {
  sqlite3_stmt *stmt = NULL;
  int ok = 0;

  if (!document_id || !uri) {
    return -1;
  }
  if (turbo_knowledge_prepare(
          db,
          "DELETE FROM chunks_fts WHERE document_id IN "
          "(SELECT id FROM documents WHERE id=? OR uri=?);",
          &stmt) == 0) {
    sqlite3_bind_text(stmt, 1, document_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, uri, -1, SQLITE_TRANSIENT);
    ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok) {
      return -1;
    }
  }
  if (turbo_knowledge_prepare(
          db, "DELETE FROM chunks WHERE document_id IN "
              "(SELECT id FROM documents WHERE id=? OR uri=?);",
          &stmt) != 0) {
    return -1;
  }
  sqlite3_bind_text(stmt, 1, document_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, uri, -1, SQLITE_TRANSIENT);
  ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  if (!ok) {
    return -1;
  }
  if (turbo_knowledge_prepare(db, "DELETE FROM documents WHERE id=? OR uri=?;",
                              &stmt) != 0) {
    return -1;
  }
  sqlite3_bind_text(stmt, 1, document_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, uri, -1, SQLITE_TRANSIENT);
  ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok ? 0 : -1;
}

int turbo_agent_knowledge_store_delete_document(turbo_agent_knowledge_store_t *store,
                                                const char *document_id) {
  int rc;

  if (!store || !store->db || !document_id || !document_id[0]) {
    return -1;
  }
  if (turbo_knowledge_exec(store->db, "BEGIN IMMEDIATE;") != 0) {
    return -1;
  }
  rc = turbo_knowledge_delete_document_tx(store->db, document_id);
  if (rc == 0) {
    rc = turbo_knowledge_exec(store->db, "COMMIT;");
  } else {
    turbo_knowledge_exec(store->db, "ROLLBACK;");
  }
  return rc;
}

static int turbo_knowledge_insert_document(sqlite3 *db,
                                           const turbo_agent_knowledge_document_t *document) {
  sqlite3_stmt *stmt = NULL;
  int ok;

  if (turbo_knowledge_prepare(
          db,
          "INSERT INTO documents(id,uri,kind,title,mtime,size,content_hash,metadata_json,"
          "updated_at) VALUES(?,?,?,?,?,?,?,?,unixepoch());",
          &stmt) != 0) {
    return -1;
  }
  sqlite3_bind_text(stmt, 1, document->id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, document->uri, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, document->kind ? document->kind : "text", -1, SQLITE_TRANSIENT);
  if (document->title) {
    sqlite3_bind_text(stmt, 4, document->title, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 4);
  }
  sqlite3_bind_int64(stmt, 5, (sqlite3_int64)document->mtime);
  sqlite3_bind_int64(stmt, 6, (sqlite3_int64)document->size);
  sqlite3_bind_text(stmt, 7, document->content_hash, -1, SQLITE_TRANSIENT);
  if (document->metadata_json) {
    sqlite3_bind_text(stmt, 8, document->metadata_json, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 8);
  }
  ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok ? 0 : -1;
}

static int turbo_knowledge_insert_chunk(sqlite3 *db, const char *document_id,
                                        const char *uri, size_t ordinal,
                                        const char *text, size_t text_len,
                                        const char *metadata_json, int fts5_enabled) {
  sqlite3_stmt *stmt = NULL;
  char *chunk_id = NULL;
  int ok;

  chunk_id = turbo_knowledge_chunk_id(document_id, ordinal);
  if (!chunk_id) {
    return -1;
  }
  if (turbo_knowledge_prepare(
          db,
          "INSERT INTO chunks(id,document_id,ordinal,text,metadata_json) VALUES(?,?,?,?,?);",
          &stmt) != 0) {
    free(chunk_id);
    return -1;
  }
  sqlite3_bind_text(stmt, 1, chunk_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, document_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, (sqlite3_int64)ordinal);
  sqlite3_bind_text(stmt, 4, text, (int)text_len, SQLITE_TRANSIENT);
  if (metadata_json) {
    sqlite3_bind_text(stmt, 5, metadata_json, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 5);
  }
  ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  if (!ok) {
    free(chunk_id);
    return -1;
  }

  if (fts5_enabled) {
    if (turbo_knowledge_prepare(
            db,
            "INSERT INTO chunks_fts(chunk_id,document_id,uri,text) VALUES(?,?,?,?);",
            &stmt) != 0) {
      free(chunk_id);
      return -1;
    }
    sqlite3_bind_text(stmt, 1, chunk_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, document_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, uri, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, text, (int)text_len, SQLITE_TRANSIENT);
    ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
  } else {
    ok = 1;
  }
  free(chunk_id);
  return ok ? 0 : -1;
}

static size_t turbo_knowledge_next_chunk_len(const char *text, size_t remaining,
                                             size_t target) {
  size_t limit;
  size_t i;

  if (remaining <= target) {
    return remaining;
  }
  limit = target;
  for (i = limit; i > target / 2; --i) {
    if (text[i] == '\n') {
      return i + 1;
    }
  }
  return limit;
}

int turbo_agent_knowledge_store_upsert_text(
    turbo_agent_knowledge_store_t *store,
    const turbo_agent_knowledge_document_t *document, const char *text,
    size_t chunk_target_bytes) {
  size_t text_len;
  size_t offset = 0;
  size_t ordinal = 0;
  char *owned_hash = NULL;
  turbo_agent_knowledge_document_t effective_document;
  int rc = -1;

  if (!store || !store->db || !document || !document->id || !document->uri || !text) {
    return -1;
  }
  text_len = strlen(text);
  if (turbo_knowledge_text_is_binary(text, text_len)) {
    return -1;
  }
  if (chunk_target_bytes == 0) {
    chunk_target_bytes = 2048;
  }
  if (chunk_target_bytes < 128) {
    chunk_target_bytes = 128;
  }

  effective_document = *document;
  if (!effective_document.content_hash) {
    owned_hash = turbo_knowledge_hash_text(text, text_len);
    if (!owned_hash) {
      return -1;
    }
    effective_document.content_hash = owned_hash;
  }
  if (effective_document.size == 0) {
    effective_document.size = (int64_t)text_len;
  }

  if (turbo_knowledge_exec(store->db, "BEGIN IMMEDIATE;") != 0) {
    free(owned_hash);
    return -1;
  }
  if (turbo_knowledge_delete_document_or_uri_tx(store->db, effective_document.id,
                                                effective_document.uri) != 0 ||
      turbo_knowledge_insert_document(store->db, &effective_document) != 0) {
    goto cleanup;
  }

  while (offset < text_len || (text_len == 0 && ordinal == 0)) {
    size_t remaining = text_len - offset;
    size_t chunk_len = text_len == 0
                           ? 0
                           : turbo_knowledge_next_chunk_len(text + offset, remaining,
                                                            chunk_target_bytes);
    if (turbo_knowledge_insert_chunk(store->db, effective_document.id,
                                     effective_document.uri, ordinal, text + offset,
                                     chunk_len, effective_document.metadata_json,
                                     store->fts5_enabled) != 0) {
      goto cleanup;
    }
    ordinal++;
    if (text_len == 0) {
      break;
    }
    offset += chunk_len;
  }

  rc = turbo_knowledge_exec(store->db, "COMMIT;");

cleanup:
  if (rc != 0) {
    turbo_knowledge_exec(store->db, "ROLLBACK;");
  }
  free(owned_hash);
  return rc;
}

int turbo_agent_knowledge_store_index_file(turbo_agent_knowledge_store_t *store,
                                           const char *path, const char *kind,
                                           size_t chunk_target_bytes) {
  char *text;
  char *hash;
  size_t size = 0;
  int mtime = 0;
  turbo_agent_knowledge_document_t document;
  int rc;

  if (!store || !path || !path[0]) {
    return -1;
  }
  text = turbo_knowledge_read_file(path, &size, &mtime);
  if (!text) {
    return -1;
  }
  if (turbo_knowledge_text_is_binary(text, size)) {
    free(text);
    return -1;
  }
  hash = turbo_knowledge_hash_text(text, size);
  if (!hash) {
    free(text);
    return -1;
  }
  memset(&document, 0, sizeof(document));
  document.id = path;
  document.uri = path;
  document.kind = kind ? kind : "file";
  document.title = path;
  document.mtime = (int64_t)mtime;
  document.size = (int64_t)size;
  document.content_hash = hash;
  rc = turbo_agent_knowledge_store_upsert_text(store, &document, text, chunk_target_bytes);
  free(hash);
  free(text);
  return rc;
}

static int turbo_knowledge_make_directory_summary(
    const char *root_dir, const char *kind,
    const turbo_knowledge_directory_index_state_t *state,
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
  turbo_json_object_set_string(summary, "include_extensions",
                               state->include_extensions ? state->include_extensions : "");
  turbo_json_object_set_number(summary, "chunk_target_bytes",
                               (double)state->chunk_target_bytes);
  turbo_json_object_set_bool(summary, "recursive", state->recursive ? 1 : 0);
  turbo_json_object_set_number(summary, "max_files", (double)state->max_files);
  turbo_json_object_set_number(summary, "max_file_bytes",
                               (double)state->max_file_bytes);
  turbo_json_object_set_number(summary, "visited", (double)state->visited);
  turbo_json_object_set_number(summary, "indexed", (double)state->indexed);
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

static int turbo_knowledge_index_directory_file(
    turbo_knowledge_directory_index_state_t *state, const char *path) {
  if (!state || !path) {
    return -1;
  }
  if (state->max_files > 0 && state->visited >= state->max_files) {
    state->truncated = 1;
    return 1;
  }
  state->visited++;
  if (!turbo_knowledge_extension_allowed(path, state->include_extensions)) {
    state->skipped++;
    state->skipped_by_extension++;
    return 0;
  }
  if (state->max_file_bytes > 0 &&
      turbo_knowledge_file_size(path) > state->max_file_bytes) {
    state->skipped++;
    state->skipped_too_large++;
    return 0;
  }
  if (turbo_agent_knowledge_store_index_file(state->store, path,
                                             state->kind ? state->kind : "file",
                                             state->chunk_target_bytes) == 0) {
    state->indexed++;
  } else {
    state->skipped++;
    state->skipped_index_error++;
  }
  return 0;
}

static int turbo_knowledge_index_directory_walk(
    turbo_knowledge_directory_index_state_t *state, const char *dir) {
#ifdef _WIN32
  WIN32_FIND_DATAA find_data;
  HANDLE handle;
  char *pattern;

  pattern = turbo_knowledge_join_path(dir, "*");
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
      if (!state->recursive || turbo_knowledge_should_skip_directory(name)) {
        continue;
      }
      path = turbo_knowledge_join_path(dir, name);
      if (!path) {
        FindClose(handle);
        return -1;
      }
      rc = turbo_knowledge_index_directory_walk(state, path);
      free(path);
      if (rc != 0) {
        FindClose(handle);
        return rc;
      }
      continue;
    }

    path = turbo_knowledge_join_path(dir, name);
    if (!path) {
      FindClose(handle);
      return -1;
    }
    rc = turbo_knowledge_index_directory_file(state, path);
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
    path = turbo_knowledge_join_path(dir, name);
    if (!path) {
      closedir(handle);
      return -1;
    }
    is_dir = turbo_knowledge_path_is_directory(path);
    if (is_dir) {
      if (state->recursive && !turbo_knowledge_should_skip_directory(name)) {
        rc = turbo_knowledge_index_directory_walk(state, path);
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
    rc = turbo_knowledge_index_directory_file(state, path);
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

int turbo_agent_knowledge_store_index_directory_ex(
    turbo_agent_knowledge_store_t *store, const char *root_dir,
    const turbo_agent_knowledge_directory_options_t *options,
    json_value_t **out_summary_json) {
  turbo_knowledge_directory_index_state_t state;
  int rc;

  if (out_summary_json) {
    *out_summary_json = NULL;
  }
  if (!store || !root_dir || !root_dir[0] ||
      !turbo_knowledge_path_is_directory(root_dir)) {
    return -1;
  }
  memset(&state, 0, sizeof(state));
  state.store = store;
  state.kind = options && options->kind ? options->kind : "file";
  state.chunk_target_bytes =
      options && options->chunk_target_bytes != 0 ? options->chunk_target_bytes : 2048;
  state.recursive = options && options->recursive ? 1 : 0;
  state.max_files = options ? options->max_files : 0;
  state.max_file_bytes = options ? options->max_file_bytes : 0;
  state.include_extensions = options ? options->include_extensions : NULL;

  rc = turbo_knowledge_index_directory_walk(&state, root_dir);
  if (rc < 0) {
    turbo_free_json(out_summary_json);
    return -1;
  }
  if (turbo_knowledge_make_directory_summary(root_dir, state.kind, &state,
                                             out_summary_json) != 0) {
    return -1;
  }
  return 0;
}

int turbo_agent_knowledge_store_index_directory(
    turbo_agent_knowledge_store_t *store, const char *root_dir, const char *kind,
    size_t chunk_target_bytes, int recursive, size_t max_files,
    json_value_t **out_summary_json) {
  turbo_agent_knowledge_directory_options_t options;

  memset(&options, 0, sizeof(options));
  options.kind = kind;
  options.chunk_target_bytes = chunk_target_bytes;
  options.recursive = recursive;
  options.max_files = max_files;
  return turbo_agent_knowledge_store_index_directory_ex(store, root_dir, &options,
                                                       out_summary_json);
}

static int turbo_knowledge_json_add_string(json_value_t *object, const char *key,
                                           const char *value) {
  turbo_json_object_set_string(object, key, value ? value : "");
  return 0;
}

int turbo_agent_knowledge_store_query_ex(
    turbo_agent_knowledge_store_t *store, const char *query, const char *kind,
    const char *uri_prefix, size_t limit, json_value_t **out_results_json) {
  json_value_t *results = NULL;
  sqlite3_stmt *stmt = NULL;
  char *match_query = NULL;
  char *like_sql = NULL;
  char *uri_like = NULL;
  turbo_knowledge_query_token_t tokens[TURBO_KNOWLEDGE_QUERY_MAX_TOKENS];
  size_t token_count = 0;
  int filter_bind_index = 2;
  int rc = -1;

  if (!store || !store->db || !query || !query[0] || !out_results_json) {
    return -1;
  }
  *out_results_json = NULL;
  if (limit == 0) {
    limit = 8;
  }
  if (uri_prefix && uri_prefix[0]) {
    size_t prefix_len = strlen(uri_prefix);
    uri_like = (char *)malloc(prefix_len + 2);
    if (!uri_like) {
      return -1;
    }
    memcpy(uri_like, uri_prefix, prefix_len);
    uri_like[prefix_len] = '%';
    uri_like[prefix_len + 1] = '\0';
  }
  if (store->fts5_enabled) {
    match_query = turbo_knowledge_build_match_query(query);
    if (!match_query) {
      free(uri_like);
      return -1;
    }
    if (turbo_knowledge_prepare(
            store->db,
            "SELECT f.chunk_id, f.document_id, f.uri, c.ordinal, c.text, "
            "d.kind, d.title, bm25(chunks_fts) AS score "
            "FROM chunks_fts f "
            "JOIN chunks c ON c.id=f.chunk_id "
            "JOIN documents d ON d.id=f.document_id "
            "WHERE chunks_fts MATCH ? "
            "AND (? IS NULL OR d.kind=?) "
            "AND (? IS NULL OR d.uri LIKE ?) "
            "ORDER BY score LIMIT ?;",
            &stmt) != 0) {
      free(match_query);
      free(uri_like);
      return -1;
    }
    sqlite3_bind_text(stmt, 1, match_query, -1, SQLITE_TRANSIENT);
    free(match_query);
  } else {
    size_t i;

    token_count = turbo_knowledge_collect_query_tokens(
        query, tokens, TURBO_KNOWLEDGE_QUERY_MAX_TOKENS);
    like_sql = turbo_knowledge_build_like_query_sql(token_count);
    if (!like_sql) {
      free(uri_like);
      return -1;
    }
    if (turbo_knowledge_prepare(store->db, like_sql, &stmt) != 0) {
      free(like_sql);
      free(uri_like);
      return -1;
    }
    free(like_sql);
    if (token_count == 0) {
      char *like_query = turbo_knowledge_build_like_pattern(query, strlen(query));
      if (!like_query) {
        sqlite3_finalize(stmt);
        free(uri_like);
        return -1;
      }
      sqlite3_bind_text(stmt, 1, like_query, -1, SQLITE_TRANSIENT);
      free(like_query);
      filter_bind_index = 2;
    } else {
      for (i = 0; i < token_count; ++i) {
        char *like_query =
            turbo_knowledge_build_like_pattern(tokens[i].text, tokens[i].len);
        if (!like_query) {
          sqlite3_finalize(stmt);
          free(uri_like);
          return -1;
        }
        sqlite3_bind_text(stmt, (int)i + 1, like_query, -1, SQLITE_TRANSIENT);
        free(like_query);
      }
      filter_bind_index = (int)token_count + 1;
    }
  }
  if (kind && kind[0]) {
    sqlite3_bind_text(stmt, filter_bind_index, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, filter_bind_index + 1, kind, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, filter_bind_index);
    sqlite3_bind_null(stmt, filter_bind_index + 1);
  }
  if (uri_like) {
    sqlite3_bind_text(stmt, filter_bind_index + 2, uri_like, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, filter_bind_index + 3, uri_like, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, filter_bind_index + 2);
    sqlite3_bind_null(stmt, filter_bind_index + 3);
  }
  sqlite3_bind_int64(stmt, filter_bind_index + 4, (sqlite3_int64)limit);
  free(uri_like);

  results = turbo_json_create_array();
  if (!results) {
    sqlite3_finalize(stmt);
    return -1;
  }
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    json_value_t *item = turbo_json_create_object();
    if (!item) {
      rc = -1;
      goto cleanup;
    }
    turbo_knowledge_json_add_string(item, "chunk_id",
                                    (const char *)sqlite3_column_text(stmt, 0));
    turbo_knowledge_json_add_string(item, "document_id",
                                    (const char *)sqlite3_column_text(stmt, 1));
    turbo_knowledge_json_add_string(item, "uri",
                                    (const char *)sqlite3_column_text(stmt, 2));
    turbo_json_object_set_number(item, "ordinal", (double)sqlite3_column_int64(stmt, 3));
    turbo_knowledge_json_add_string(item, "text",
                                    (const char *)sqlite3_column_text(stmt, 4));
    turbo_knowledge_json_add_string(item, "kind",
                                    (const char *)sqlite3_column_text(stmt, 5));
    turbo_knowledge_json_add_string(item, "title",
                                    (const char *)sqlite3_column_text(stmt, 6));
    turbo_json_object_set_number(item, "score", sqlite3_column_double(stmt, 7));
    turbo_json_array_add(results, item);
  }
  rc = rc == SQLITE_DONE ? 0 : -1;

cleanup:
  sqlite3_finalize(stmt);
  if (rc == 0) {
    *out_results_json = results;
  } else {
    turbo_free_json(&results);
  }
  return rc;
}

int turbo_agent_knowledge_store_query(turbo_agent_knowledge_store_t *store,
                                      const char *query, size_t limit,
                                      json_value_t **out_results_json) {
  return turbo_agent_knowledge_store_query_ex(store, query, NULL, NULL, limit,
                                              out_results_json);
}

int turbo_agent_knowledge_store_list_documents(
    turbo_agent_knowledge_store_t *store, const char *kind,
    const char *uri_prefix, size_t limit, json_value_t **out_documents_json) {
  json_value_t *documents = NULL;
  sqlite3_stmt *stmt = NULL;
  char *uri_like = NULL;
  int rc = -1;

  if (!store || !store->db || !out_documents_json) {
    return -1;
  }
  *out_documents_json = NULL;
  if (limit == 0) {
    limit = 100;
  }
  if (uri_prefix && uri_prefix[0]) {
    size_t prefix_len = strlen(uri_prefix);
    uri_like = (char *)malloc(prefix_len + 2);
    if (!uri_like) {
      return -1;
    }
    memcpy(uri_like, uri_prefix, prefix_len);
    uri_like[prefix_len] = '%';
    uri_like[prefix_len + 1] = '\0';
  }

  if (turbo_knowledge_prepare(
          store->db,
          "SELECT d.id,d.uri,d.kind,d.title,d.mtime,d.size,d.content_hash,"
          "d.metadata_json,d.updated_at,COUNT(c.id) AS chunk_count "
          "FROM documents d LEFT JOIN chunks c ON c.document_id=d.id "
          "WHERE (?1 IS NULL OR d.kind=?1) AND (?2 IS NULL OR d.uri LIKE ?2) "
          "GROUP BY d.id,d.uri,d.kind,d.title,d.mtime,d.size,d.content_hash,"
          "d.metadata_json,d.updated_at "
          "ORDER BY d.updated_at DESC,d.uri ASC LIMIT ?3;",
          &stmt) != 0) {
    free(uri_like);
    return -1;
  }
  if (kind && kind[0]) {
    sqlite3_bind_text(stmt, 1, kind, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 1);
  }
  if (uri_like) {
    sqlite3_bind_text(stmt, 2, uri_like, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 2);
  }
  sqlite3_bind_int64(stmt, 3, (sqlite3_int64)limit);
  free(uri_like);

  documents = turbo_json_create_array();
  if (!documents) {
    sqlite3_finalize(stmt);
    return -1;
  }
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    json_value_t *item = turbo_json_create_object();
    if (!item) {
      rc = -1;
      goto cleanup;
    }
    turbo_knowledge_json_add_string(item, "id",
                                    (const char *)sqlite3_column_text(stmt, 0));
    turbo_knowledge_json_add_string(item, "uri",
                                    (const char *)sqlite3_column_text(stmt, 1));
    turbo_knowledge_json_add_string(item, "kind",
                                    (const char *)sqlite3_column_text(stmt, 2));
    turbo_knowledge_json_add_string(item, "title",
                                    (const char *)sqlite3_column_text(stmt, 3));
    turbo_json_object_set_number(item, "mtime",
                                 (double)sqlite3_column_int64(stmt, 4));
    turbo_json_object_set_number(item, "size",
                                 (double)sqlite3_column_int64(stmt, 5));
    turbo_knowledge_json_add_string(item, "content_hash",
                                    (const char *)sqlite3_column_text(stmt, 6));
    turbo_knowledge_json_add_string(item, "metadata_json",
                                    (const char *)sqlite3_column_text(stmt, 7));
    turbo_json_object_set_number(item, "updated_at",
                                 (double)sqlite3_column_int64(stmt, 8));
    turbo_json_object_set_number(item, "chunk_count",
                                 (double)sqlite3_column_int64(stmt, 9));
    turbo_json_array_add(documents, item);
  }
  rc = rc == SQLITE_DONE ? 0 : -1;

cleanup:
  sqlite3_finalize(stmt);
  if (rc == 0) {
    *out_documents_json = documents;
  } else {
    turbo_free_json(&documents);
  }
  return rc;
}

int turbo_agent_knowledge_store_get_document(
    turbo_agent_knowledge_store_t *store, const char *document_id,
    json_value_t **out_document_json) {
  sqlite3_stmt *stmt = NULL;
  json_value_t *document = NULL;
  json_value_t *chunks = NULL;
  int rc = -1;

  if (!store || !store->db || !document_id || !document_id[0] || !out_document_json) {
    return -1;
  }
  *out_document_json = NULL;
  if (turbo_knowledge_prepare(
          store->db,
          "SELECT id,uri,kind,title,mtime,size,content_hash,metadata_json,updated_at "
          "FROM documents WHERE id=?;",
          &stmt) != 0) {
    return -1;
  }
  sqlite3_bind_text(stmt, 1, document_id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return -1;
  }
  document = turbo_json_create_object();
  if (!document) {
    sqlite3_finalize(stmt);
    return -1;
  }
  turbo_knowledge_json_add_string(document, "id",
                                  (const char *)sqlite3_column_text(stmt, 0));
  turbo_knowledge_json_add_string(document, "uri",
                                  (const char *)sqlite3_column_text(stmt, 1));
  turbo_knowledge_json_add_string(document, "kind",
                                  (const char *)sqlite3_column_text(stmt, 2));
  turbo_knowledge_json_add_string(document, "title",
                                  (const char *)sqlite3_column_text(stmt, 3));
  turbo_json_object_set_number(document, "mtime",
                               (double)sqlite3_column_int64(stmt, 4));
  turbo_json_object_set_number(document, "size",
                               (double)sqlite3_column_int64(stmt, 5));
  turbo_knowledge_json_add_string(document, "content_hash",
                                  (const char *)sqlite3_column_text(stmt, 6));
  turbo_knowledge_json_add_string(document, "metadata_json",
                                  (const char *)sqlite3_column_text(stmt, 7));
  turbo_json_object_set_number(document, "updated_at",
                               (double)sqlite3_column_int64(stmt, 8));
  sqlite3_finalize(stmt);
  stmt = NULL;

  if (turbo_knowledge_prepare(
          store->db,
          "SELECT id,ordinal,text,metadata_json FROM chunks "
          "WHERE document_id=? ORDER BY ordinal ASC;",
          &stmt) != 0) {
    turbo_free_json(&document);
    return -1;
  }
  sqlite3_bind_text(stmt, 1, document_id, -1, SQLITE_TRANSIENT);
  chunks = turbo_json_create_array();
  if (!chunks) {
    sqlite3_finalize(stmt);
    turbo_free_json(&document);
    return -1;
  }
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    json_value_t *chunk = turbo_json_create_object();
    if (!chunk) {
      rc = -1;
      goto cleanup;
    }
    turbo_knowledge_json_add_string(chunk, "id",
                                    (const char *)sqlite3_column_text(stmt, 0));
    turbo_json_object_set_number(chunk, "ordinal",
                                 (double)sqlite3_column_int64(stmt, 1));
    turbo_knowledge_json_add_string(chunk, "text",
                                    (const char *)sqlite3_column_text(stmt, 2));
    turbo_knowledge_json_add_string(chunk, "metadata_json",
                                    (const char *)sqlite3_column_text(stmt, 3));
    turbo_json_array_add(chunks, chunk);
  }
  rc = rc == SQLITE_DONE ? 0 : -1;

cleanup:
  sqlite3_finalize(stmt);
  if (rc == 0) {
    turbo_json_object_add(document, "chunks", chunks);
    *out_document_json = document;
  } else {
    turbo_free_json(&chunks);
    turbo_free_json(&document);
  }
  return rc;
}

static const char *turbo_knowledge_latest_user_query(const json_value_t *state) {
  const json_value_t *input;
  size_t i;

  if (!state || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return NULL;
  }
  input = turbo_json_object_get(state, "input");
  if (!input || turbo_json_type(input) != TURBO_JSON_ARRAY) {
    return NULL;
  }
  for (i = turbo_json_array_size(input); i > 0; --i) {
    const json_value_t *message = turbo_json_array_get(input, i - 1);
    const json_value_t *content;
    const char *role;
    const char *text;

    if (!message || turbo_json_type(message) != TURBO_JSON_OBJECT) {
      continue;
    }
    role = turbo_json_get_string(message, "role");
    if (!role || strcmp(role, "user") != 0) {
      continue;
    }
    content = turbo_json_object_get(message, "content");
    if (!content || turbo_json_type(content) != TURBO_JSON_STRING) {
      continue;
    }
    text = turbo_json_get_string(message, "content");
    if (text && text[0] != '\0') {
      return text;
    }
  }
  return NULL;
}

int turbo_agent_knowledge_store_load_context_ex(
    turbo_agent_knowledge_store_t *store, json_value_t *state, const char *query,
    const char *kind, const char *uri_prefix, size_t limit) {
  json_value_t *results = NULL;
  size_t i;
  int rc = 0;

  if (!store || !state || !query || !query[0] || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return -1;
  }
  if (turbo_agent_knowledge_store_query_ex(store, query, kind, uri_prefix, limit,
                                           &results) != 0) {
    return -1;
  }
  if (!results || turbo_json_type(results) != TURBO_JSON_ARRAY) {
    turbo_free_json(&results);
    return -1;
  }

  for (i = 0; i < turbo_json_array_size(results); ++i) {
    const json_value_t *item = turbo_json_array_get(results, i);
    const char *uri;
    const char *text;

    if (!item || turbo_json_type(item) != TURBO_JSON_OBJECT) {
      continue;
    }
    uri = turbo_json_get_string(item, "uri");
    text = turbo_json_get_string(item, "text");
    if (text && text[0] != '\0' &&
        turbo_agent_state_add_memory_context_layer(state, "knowledge", uri, text) != 0) {
      rc = -1;
      break;
    }
  }

  turbo_free_json(&results);
  return rc;
}

int turbo_agent_knowledge_store_load_context(
    turbo_agent_knowledge_store_t *store, json_value_t *state, const char *query,
    size_t limit) {
  return turbo_agent_knowledge_store_load_context_ex(store, state, query, NULL,
                                                    NULL, limit);
}

int turbo_agent_knowledge_store_build_context(
    turbo_agent_knowledge_store_t *store, const char *query, const char *kind,
    const char *uri_prefix, size_t limit, json_value_t **out_context_json) {
  json_value_t *state = NULL;
  json_value_t *context = NULL;
  json_value_t *layers = NULL;
  json_value_t *evidence = NULL;
  const json_value_t *state_layers;
  char *context_text = NULL;

  if (!store || !query || !query[0] || !out_context_json) {
    return -1;
  }
  *out_context_json = NULL;

  state = turbo_agent_state_create();
  context = turbo_json_create_object();
  if (!state || !context) {
    turbo_free_json(&state);
    turbo_free_json(&context);
    return -1;
  }

  if (turbo_agent_knowledge_store_load_context_ex(store, state, query, kind,
                                                  uri_prefix, limit) != 0) {
    turbo_free_json(&state);
    turbo_free_json(&context);
    return -1;
  }
  if (turbo_agent_knowledge_store_query_ex(store, query, kind, uri_prefix, limit,
                                           &evidence) != 0) {
    turbo_free_json(&state);
    turbo_free_json(&context);
    return -1;
  }

  state_layers = turbo_agent_state_memory_layers(state);
  layers = state_layers ? turbo_json_clone(state_layers) : turbo_json_create_array();
  if (!layers) {
    turbo_free_json(&evidence);
    turbo_free_json(&state);
    turbo_free_json(&context);
    return -1;
  }
  context_text = turbo_agent_state_memory_context_text(state);

  turbo_json_object_set_string(context, "query", query);
  turbo_json_object_set_string(context, "kind", kind ? kind : "");
  turbo_json_object_set_string(context, "uri_prefix", uri_prefix ? uri_prefix : "");
  turbo_json_object_set_number(context, "layer_count",
                               (double)turbo_json_array_size(layers));
  turbo_json_object_set_string(context, "context_text",
                               context_text ? context_text : "");
  turbo_json_object_add(context, "layers", layers);
  turbo_json_object_add(context, "evidence", evidence);
  free(context_text);
  turbo_free_json(&state);
  *out_context_json = context;
  return 0;
}

static int turbo_knowledge_count_rows(sqlite3 *db, const char *sql,
                                      int64_t *out_count) {
  sqlite3_stmt *stmt = NULL;
  int rc;

  if (!db || !sql || !out_count) {
    return -1;
  }
  *out_count = 0;
  if (turbo_knowledge_prepare(db, sql, &stmt) != 0) {
    return -1;
  }
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    *out_count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return 0;
  }
  sqlite3_finalize(stmt);
  return -1;
}

static int turbo_knowledge_build_kind_stats(sqlite3 *db,
                                            json_value_t **out_kinds_json) {
  sqlite3_stmt *stmt = NULL;
  json_value_t *kinds = NULL;
  int rc = -1;

  if (!db || !out_kinds_json) {
    return -1;
  }
  *out_kinds_json = NULL;
  if (turbo_knowledge_prepare(
          db,
          "SELECT d.kind, COUNT(DISTINCT d.id) AS document_count, "
          "COUNT(c.id) AS chunk_count "
          "FROM documents d "
          "LEFT JOIN chunks c ON c.document_id=d.id "
          "GROUP BY d.kind ORDER BY d.kind ASC;",
          &stmt) != 0) {
    return -1;
  }
  kinds = turbo_json_create_array();
  if (!kinds) {
    sqlite3_finalize(stmt);
    return -1;
  }
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    json_value_t *item = turbo_json_create_object();
    if (!item) {
      rc = -1;
      goto cleanup;
    }
    turbo_knowledge_json_add_string(item, "kind",
                                    (const char *)sqlite3_column_text(stmt, 0));
    turbo_json_object_set_number(item, "document_count",
                                 (double)sqlite3_column_int64(stmt, 1));
    turbo_json_object_set_number(item, "chunk_count",
                                 (double)sqlite3_column_int64(stmt, 2));
    turbo_json_array_add(kinds, item);
  }
  rc = rc == SQLITE_DONE ? 0 : -1;

cleanup:
  sqlite3_finalize(stmt);
  if (rc == 0) {
    *out_kinds_json = kinds;
  } else {
    turbo_free_json(&kinds);
  }
  return rc;
}

int turbo_agent_knowledge_store_stats(turbo_agent_knowledge_store_t *store,
                                      json_value_t **out_stats_json) {
  json_value_t *stats = NULL;
  json_value_t *kinds = NULL;
  int64_t document_count = 0;
  int64_t chunk_count = 0;

  if (!store || !store->db || !out_stats_json) {
    return -1;
  }
  *out_stats_json = NULL;
  if (turbo_knowledge_count_rows(store->db, "SELECT COUNT(*) FROM documents;",
                                 &document_count) != 0 ||
      turbo_knowledge_count_rows(store->db, "SELECT COUNT(*) FROM chunks;",
                                 &chunk_count) != 0) {
    return -1;
  }
  if (turbo_knowledge_build_kind_stats(store->db, &kinds) != 0) {
    return -1;
  }
  stats = turbo_json_create_object();
  if (!stats) {
    turbo_free_json(&kinds);
    return -1;
  }
  turbo_json_object_set_bool(stats, "fts5_enabled",
                             store->fts5_enabled ? true : false);
  turbo_json_object_set_number(stats, "document_count", (double)document_count);
  turbo_json_object_set_number(stats, "chunk_count", (double)chunk_count);
  turbo_json_object_add(stats, "kinds", kinds);
  *out_stats_json = stats;
  return 0;
}

static int turbo_knowledge_tool_graph_add_node(json_value_t *nodes,
                                               const char *name,
                                               const char *kind,
                                               const char *purpose) {
  json_value_t *node;

  if (!nodes || !name || !kind || !purpose) {
    return -1;
  }
  node = turbo_json_create_object();
  if (!node) {
    return -1;
  }
  turbo_json_object_set_string(node, "name", name);
  turbo_json_object_set_string(node, "kind", kind);
  turbo_json_object_set_string(node, "purpose", purpose);
  turbo_json_array_add(nodes, node);
  return 0;
}

static int turbo_knowledge_tool_graph_add_edge(json_value_t *edges,
                                               const char *from,
                                               const char *to,
                                               const char *reason) {
  json_value_t *edge;

  if (!edges || !from || !to || !reason) {
    return -1;
  }
  edge = turbo_json_create_object();
  if (!edge) {
    return -1;
  }
  turbo_json_object_set_string(edge, "from", from);
  turbo_json_object_set_string(edge, "to", to);
  turbo_json_object_set_string(edge, "reason", reason);
  turbo_json_array_add(edges, edge);
  return 0;
}

int turbo_agent_knowledge_store_tool_graph(json_value_t **out_graph_json) {
  json_value_t *graph = NULL;
  json_value_t *nodes = NULL;
  json_value_t *edges = NULL;

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

  if (turbo_knowledge_tool_graph_add_node(
          nodes, "agent.knowledge.stats", "observe",
          "Inspect whether local knowledge is indexed and which document kinds exist.") != 0 ||
      turbo_knowledge_tool_graph_add_node(
          nodes, "agent.knowledge.list_documents", "observe",
          "List indexed documents before loading a specific source.") != 0 ||
      turbo_knowledge_tool_graph_add_node(
          nodes, "agent.knowledge.search", "observe",
          "Find relevant chunks with optional kind and uri_prefix filters.") != 0 ||
      turbo_knowledge_tool_graph_add_node(
          nodes, "agent.knowledge.build_context", "observe",
          "Build merged model context plus evidence from filtered search results.") != 0 ||
      turbo_knowledge_tool_graph_add_node(
          nodes, "agent.knowledge.get_document", "observe",
          "Load one indexed document and all of its ordered chunks.") != 0 ||
      turbo_knowledge_tool_graph_add_node(
          nodes, "agent.knowledge.upsert_text", "mutate",
          "Persist notes, task summaries, or discovered facts into local knowledge.") != 0 ||
      turbo_knowledge_tool_graph_add_node(
          nodes, "agent.knowledge.index_file", "mutate",
          "Index one local text file into local knowledge.") != 0 ||
      turbo_knowledge_tool_graph_add_node(
          nodes, "agent.knowledge.index_directory", "mutate",
          "Index selected files under a local directory into local knowledge.") != 0 ||
      turbo_knowledge_tool_graph_add_node(
          nodes, "agent.knowledge.delete_document", "mutate",
          "Remove stale or incorrect knowledge by document id.") != 0 ||
      turbo_knowledge_tool_graph_add_edge(
          edges, "agent.knowledge.stats", "agent.knowledge.search",
          "Choose filters after inspecting available kinds and counts.") != 0 ||
      turbo_knowledge_tool_graph_add_edge(
          edges, "agent.knowledge.stats", "agent.knowledge.index_directory",
          "Index local files when the store is empty or missing project docs.") != 0 ||
      turbo_knowledge_tool_graph_add_edge(
          edges, "agent.knowledge.search", "agent.knowledge.build_context",
          "Build prompt-ready context from the same query and filters.") != 0 ||
      turbo_knowledge_tool_graph_add_edge(
          edges, "agent.knowledge.search", "agent.knowledge.get_document",
          "Load a full source document when a matching chunk needs more context.") != 0 ||
      turbo_knowledge_tool_graph_add_edge(
          edges, "agent.knowledge.list_documents", "agent.knowledge.get_document",
          "Open one listed document by id.") != 0 ||
      turbo_knowledge_tool_graph_add_edge(
          edges, "agent.knowledge.upsert_text", "agent.knowledge.stats",
          "Refresh counts after writing memory or facts.") != 0 ||
      turbo_knowledge_tool_graph_add_edge(
          edges, "agent.knowledge.index_file", "agent.knowledge.stats",
          "Refresh counts after indexing a file.") != 0 ||
      turbo_knowledge_tool_graph_add_edge(
          edges, "agent.knowledge.index_directory", "agent.knowledge.stats",
          "Refresh counts after indexing a directory.") != 0) {
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

int turbo_agent_knowledge_context_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const turbo_agent_knowledge_context_config_t *config =
      (const turbo_agent_knowledge_context_config_t *)user_data;
  const char *query;

  if (!ctx || !ctx->state || !config || !config->store) {
    return -1;
  }
  query = config->query && config->query[0] != '\0'
              ? config->query
              : turbo_knowledge_latest_user_query(ctx->state);
  if (!query || query[0] == '\0') {
    return 0;
  }
  return turbo_agent_knowledge_store_load_context_ex(
      config->store, ctx->state, query, config->kind, config->uri_prefix,
      config->limit);
}

turbo_graph_exec_status_t
turbo_agent_install_knowledge_engineering_loop(
    turbo_graph_t *graph, const turbo_agent_knowledge_context_config_t *knowledge_config,
    turbo_agent_t *planner_agent, turbo_agent_t *executor_agent,
    const char *knowledge_node_name, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *review_node_name, const char *executor_node_name,
    const char *tool_node_name, const char *detect_failure_node_name,
    const char *replan_route_node_name, const char *replan_prepare_node_name,
    const char *plan_advance_node_name, const char *end_node_name, int set_entry) {
  turbo_graph_exec_status_t status;

  if (!graph || !knowledge_config || !knowledge_config->store ||
      !planner_agent || !executor_agent || !knowledge_node_name ||
      !planner_node_name || !plan_commit_node_name || !plan_step_node_name ||
      !review_node_name || !executor_node_name || !tool_node_name ||
      !detect_failure_node_name || !replan_route_node_name ||
      !replan_prepare_node_name || !plan_advance_node_name || !end_node_name) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  status = turbo_graph_add_node(graph, knowledge_node_name,
                                turbo_agent_knowledge_context_node,
                                (void *)knowledge_config);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_install_engineering_loop(
      graph, planner_agent, executor_agent, planner_node_name, plan_commit_node_name,
      plan_step_node_name, review_node_name, executor_node_name, tool_node_name,
      detect_failure_node_name, replan_route_node_name, replan_prepare_node_name,
      plan_advance_node_name, end_node_name, 0);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, knowledge_node_name, planner_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  return set_entry ? turbo_graph_set_entry(graph, knowledge_node_name)
                   : TURBO_GRAPH_EXEC_OK;
}

static int turbo_knowledge_json_value_set_string(json_value_t *object,
                                           const char *key, const char *value) {
  json_value_t *field;

  field = turbo_json_create_string(value ? value : "");
  if (!field) {
    return -1;
  }
  if (turbo_runtime_json_object_set(object, key, field) != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(field);
    return -1;
  }
  return 0;
}

static int turbo_knowledge_json_value_set_bool(json_value_t *object,
                                         const char *key, int value) {
  json_value_t *field;

  field = turbo_json_create_bool(value ? 1 : 0);
  if (!field) {
    return -1;
  }
  if (turbo_runtime_json_object_set(object, key, field) != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(field);
    return -1;
  }
  return 0;
}

static int turbo_knowledge_json_value_set_int64(json_value_t *object,
                                          const char *key, int64_t value) {
  json_value_t *field;

  field = turbo_json_create_int64(value);
  if (!field) {
    return -1;
  }
  if (turbo_runtime_json_object_set(object, key, field) != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(field);
    return -1;
  }
  return 0;
}

static int turbo_knowledge_list_documents_tool_json_value(
    const json_value_t *arguments,
    json_value_t **out_result, void *user_data) {
  turbo_agent_knowledge_store_t *store = (turbo_agent_knowledge_store_t *)user_data;
  const char *kind;
  const char *uri_prefix;
  int64_t limit;
  json_value_t *documents_json = NULL;
  json_value_t *documents_json_value = NULL;
  json_value_t *result = NULL;

  if (!store || !arguments || !out_result) {
    return -1;
  }
  *out_result = NULL;
  kind = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "kind"));
  uri_prefix = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "uri_prefix"));
  limit = turbo_runtime_json_value_as_int64(
      turbo_json_object_get(arguments, "limit"), 100);
  if (limit < 0) {
    return -1;
  }
  if (turbo_agent_knowledge_store_list_documents(
          store, kind, uri_prefix, (size_t)limit, &documents_json) != 0) {
    return -1;
  }
  documents_json_value = turbo_json_clone(documents_json);
  turbo_free_json(&documents_json);
  result = turbo_json_create_object();
  if (!result || !documents_json_value ||
      turbo_knowledge_json_value_set_bool(result, "ok", 1) != 0 ||
      turbo_knowledge_json_value_set_string(result, "summary", "documents listed") != 0 ||
      turbo_runtime_json_object_set(result, "documents", documents_json_value) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(documents_json_value);
    turbo_runtime_json_destroy(result);
    return -1;
  }
  *out_result = result;
  return 0;
}

static int turbo_knowledge_stats_tool_json_value(
    const json_value_t *arguments,
    json_value_t **out_result, void *user_data) {
  turbo_agent_knowledge_store_t *store = (turbo_agent_knowledge_store_t *)user_data;
  json_value_t *stats_json = NULL;
  json_value_t *stats_json_value = NULL;
  json_value_t *result = NULL;

  (void)arguments;
  if (!store || !out_result) {
    return -1;
  }
  *out_result = NULL;
  if (turbo_agent_knowledge_store_stats(store, &stats_json) != 0) {
    return -1;
  }
  stats_json_value = turbo_json_clone(stats_json);
  turbo_free_json(&stats_json);
  result = turbo_json_create_object();
  if (!result || !stats_json_value ||
      turbo_knowledge_json_value_set_bool(result, "ok", 1) != 0 ||
      turbo_knowledge_json_value_set_string(result, "summary", "knowledge stats loaded") != 0 ||
      turbo_runtime_json_object_set(result, "stats", stats_json_value) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(stats_json_value);
    turbo_runtime_json_destroy(result);
    return -1;
  }
  *out_result = result;
  return 0;
}

static int turbo_knowledge_tool_graph_tool_json_value(
    const json_value_t *arguments,
    json_value_t **out_result, void *user_data) {
  json_value_t *graph_json = NULL;
  json_value_t *graph_json_value = NULL;
  json_value_t *result = NULL;

  (void)arguments;
  (void)user_data;
  if (!out_result) {
    return -1;
  }
  *out_result = NULL;
  if (turbo_agent_knowledge_store_tool_graph(&graph_json) != 0) {
    return -1;
  }
  graph_json_value = turbo_json_clone(graph_json);
  turbo_free_json(&graph_json);
  result = turbo_json_create_object();
  if (!result || !graph_json_value ||
      turbo_knowledge_json_value_set_bool(result, "ok", 1) != 0 ||
      turbo_knowledge_json_value_set_string(result, "summary", "knowledge tool graph loaded") != 0 ||
      turbo_runtime_json_object_set(result, "graph", graph_json_value) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(graph_json_value);
    turbo_runtime_json_destroy(result);
    return -1;
  }
  *out_result = result;
  return 0;
}

static int turbo_knowledge_get_document_tool_json_value(
    const json_value_t *arguments,
    json_value_t **out_result, void *user_data) {
  turbo_agent_knowledge_store_t *store = (turbo_agent_knowledge_store_t *)user_data;
  const char *document_id;
  json_value_t *document_json = NULL;
  json_value_t *document_json_value = NULL;
  json_value_t *result = NULL;

  if (!store || !arguments || !out_result) {
    return -1;
  }
  *out_result = NULL;
  document_id = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "document_id"));
  if (!document_id || !document_id[0]) {
    return -1;
  }
  if (turbo_agent_knowledge_store_get_document(store, document_id, &document_json) != 0) {
    return -1;
  }
  document_json_value = turbo_json_clone(document_json);
  turbo_free_json(&document_json);
  result = turbo_json_create_object();
  if (!result || !document_json_value ||
      turbo_knowledge_json_value_set_bool(result, "ok", 1) != 0 ||
      turbo_knowledge_json_value_set_string(result, "summary", "document loaded") != 0 ||
      turbo_runtime_json_object_set(result, "document", document_json_value) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(document_json_value);
    turbo_runtime_json_destroy(result);
    return -1;
  }
  *out_result = result;
  return 0;
}

static int turbo_knowledge_search_tool_json_value(
    const json_value_t *arguments,
    json_value_t **out_result, void *user_data) {
  turbo_agent_knowledge_store_t *store = (turbo_agent_knowledge_store_t *)user_data;
  const char *query;
  const char *kind;
  const char *uri_prefix;
  int64_t limit;
  json_value_t *results_json = NULL;
  json_value_t *results_json_value = NULL;
  json_value_t *result = NULL;

  if (!store || !arguments || !out_result) {
    return -1;
  }
  *out_result = NULL;
  query = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "query"));
  kind = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "kind"));
  uri_prefix = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "uri_prefix"));
  limit = turbo_runtime_json_value_as_int64(
      turbo_json_object_get(arguments, "limit"), 8);
  if (!query || !query[0] || limit < 0) {
    return -1;
  }
  if (turbo_agent_knowledge_store_query_ex(
          store, query, kind, uri_prefix, (size_t)limit, &results_json) != 0) {
    return -1;
  }
  results_json_value = turbo_json_clone(results_json);
  turbo_free_json(&results_json);
  result = turbo_json_create_object();
  if (!result || !results_json_value ||
      turbo_knowledge_json_value_set_bool(result, "ok", 1) != 0 ||
      turbo_knowledge_json_value_set_string(result, "summary", "knowledge search completed") != 0 ||
      turbo_runtime_json_object_set(result, "results", results_json_value) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(results_json_value);
    turbo_runtime_json_destroy(result);
    return -1;
  }
  *out_result = result;
  return 0;
}

static int turbo_knowledge_build_context_tool_json_value(
    const json_value_t *arguments,
    json_value_t **out_result, void *user_data) {
  turbo_agent_knowledge_store_t *store = (turbo_agent_knowledge_store_t *)user_data;
  const char *query;
  const char *kind;
  const char *uri_prefix;
  int64_t limit;
  json_value_t *context_json = NULL;
  json_value_t *context_json_value = NULL;
  json_value_t *result = NULL;

  if (!store || !arguments || !out_result) {
    return -1;
  }
  *out_result = NULL;
  query = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "query"));
  kind = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "kind"));
  uri_prefix = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "uri_prefix"));
  limit = turbo_runtime_json_value_as_int64(
      turbo_json_object_get(arguments, "limit"), 8);
  if (!query || !query[0] || limit < 0) {
    return -1;
  }
  if (turbo_agent_knowledge_store_build_context(
          store, query, kind, uri_prefix, (size_t)limit, &context_json) != 0) {
    return -1;
  }
  context_json_value = turbo_json_clone(context_json);
  turbo_free_json(&context_json);
  result = turbo_json_create_object();
  if (!result || !context_json_value ||
      turbo_knowledge_json_value_set_bool(result, "ok", 1) != 0 ||
      turbo_knowledge_json_value_set_string(result, "summary", "knowledge context built") != 0 ||
      turbo_runtime_json_object_set(result, "context", context_json_value) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(context_json_value);
    turbo_runtime_json_destroy(result);
    return -1;
  }
  *out_result = result;
  return 0;
}

static int turbo_knowledge_upsert_text_tool_json_value(
    const json_value_t *arguments,
    json_value_t **out_result, void *user_data) {
  turbo_agent_knowledge_store_t *store = (turbo_agent_knowledge_store_t *)user_data;
  const char *id;
  const char *uri;
  const char *kind;
  const char *title;
  const char *metadata_json;
  const char *text;
  int64_t chunk_target_bytes;
  turbo_agent_knowledge_document_t document;
  json_value_t *result = NULL;

  if (!store || !arguments || !out_result) {
    return -1;
  }
  *out_result = NULL;
  id = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "id"));
  uri = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "uri"));
  kind = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "kind"));
  title = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "title"));
  metadata_json = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "metadata_json"));
  text = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "text"));
  chunk_target_bytes = turbo_runtime_json_value_as_int64(
      turbo_json_object_get(arguments, "chunk_target_bytes"), 2048);
  if (!id || !id[0] || !text || chunk_target_bytes < 0) {
    return -1;
  }
  memset(&document, 0, sizeof(document));
  document.id = id;
  document.uri = uri && uri[0] ? uri : id;
  document.kind = kind && kind[0] ? kind : "note";
  document.title = title;
  document.metadata_json = metadata_json;
  if (turbo_agent_knowledge_store_upsert_text(store, &document, text,
                                              (size_t)chunk_target_bytes) != 0) {
    return -1;
  }
  result = turbo_json_create_object();
  if (!result || turbo_knowledge_json_value_set_bool(result, "ok", 1) != 0 ||
      turbo_knowledge_json_value_set_string(result, "summary", "text indexed") != 0 ||
      turbo_knowledge_json_value_set_string(result, "document_id", document.id) != 0 ||
      turbo_knowledge_json_value_set_string(result, "uri", document.uri) != 0 ||
      turbo_knowledge_json_value_set_string(result, "kind", document.kind) != 0 ||
      turbo_knowledge_json_value_set_int64(result, "chunk_target_bytes",
                                     chunk_target_bytes) != 0) {
    turbo_runtime_json_destroy(result);
    return -1;
  }
  *out_result = result;
  return 0;
}

static int turbo_knowledge_index_file_tool_json_value(
    const json_value_t *arguments,
    json_value_t **out_result, void *user_data) {
  turbo_agent_knowledge_store_t *store = (turbo_agent_knowledge_store_t *)user_data;
  const char *path;
  const char *kind;
  int64_t chunk_target_bytes;
  json_value_t *result = NULL;

  if (!store || !arguments || !out_result) {
    return -1;
  }
  *out_result = NULL;
  path = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "path"));
  kind = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "kind"));
  chunk_target_bytes = turbo_runtime_json_value_as_int64(
      turbo_json_object_get(arguments, "chunk_target_bytes"), 2048);
  if (!path || !path[0] || chunk_target_bytes < 0) {
    return -1;
  }
  if (turbo_agent_knowledge_store_index_file(store, path, kind ? kind : "file",
                                             (size_t)chunk_target_bytes) != 0) {
    return -1;
  }
  result = turbo_json_create_object();
  if (!result || turbo_knowledge_json_value_set_bool(result, "ok", 1) != 0 ||
      turbo_knowledge_json_value_set_string(result, "summary", "file indexed") != 0 ||
      turbo_knowledge_json_value_set_string(result, "uri", path) != 0 ||
      turbo_knowledge_json_value_set_int64(result, "chunk_target_bytes",
                                     chunk_target_bytes) != 0) {
    turbo_runtime_json_destroy(result);
    return -1;
  }
  *out_result = result;
  return 0;
}

static int turbo_knowledge_index_directory_tool_json_value(
    const json_value_t *arguments,
    json_value_t **out_result, void *user_data) {
  turbo_agent_knowledge_store_t *store = (turbo_agent_knowledge_store_t *)user_data;
  const char *root_dir;
  const char *kind;
  const char *include_extensions;
  int64_t chunk_target_bytes;
  int recursive;
  int64_t max_files;
  int64_t max_file_bytes;
  turbo_agent_knowledge_directory_options_t options;
  json_value_t *summary_json = NULL;
  json_value_t *summary_json_value = NULL;
  json_value_t *result = NULL;

  if (!store || !arguments || !out_result) {
    return -1;
  }
  *out_result = NULL;
  root_dir = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "root_dir"));
  kind = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "kind"));
  include_extensions = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "include_extensions"));
  chunk_target_bytes = turbo_runtime_json_value_as_int64(
      turbo_json_object_get(arguments, "chunk_target_bytes"), 2048);
  recursive = turbo_runtime_json_value_as_bool(
      turbo_json_object_get(arguments, "recursive"), 1);
  max_files = turbo_runtime_json_value_as_int64(
      turbo_json_object_get(arguments, "max_files"), 0);
  max_file_bytes = turbo_runtime_json_value_as_int64(
      turbo_json_object_get(arguments, "max_file_bytes"), 0);
  if (!root_dir || !root_dir[0] || chunk_target_bytes < 0 || max_files < 0 ||
      max_file_bytes < 0) {
    return -1;
  }
  memset(&options, 0, sizeof(options));
  options.kind = kind ? kind : "file";
  options.chunk_target_bytes = (size_t)chunk_target_bytes;
  options.recursive = recursive;
  options.max_files = (size_t)max_files;
  options.max_file_bytes = (size_t)max_file_bytes;
  options.include_extensions = include_extensions;
  if (turbo_agent_knowledge_store_index_directory_ex(store, root_dir, &options,
                                                     &summary_json) != 0) {
    return -1;
  }
  summary_json_value = turbo_json_clone(summary_json);
  turbo_free_json(&summary_json);
  result = turbo_json_create_object();
  if (!result || !summary_json_value ||
      turbo_knowledge_json_value_set_bool(result, "ok", 1) != 0 ||
      turbo_knowledge_json_value_set_string(result, "summary", "directory indexed") != 0 ||
      turbo_knowledge_json_value_set_string(result, "root_dir", root_dir) != 0 ||
      turbo_runtime_json_object_set(result, "stats", summary_json_value) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(summary_json_value);
    turbo_runtime_json_destroy(result);
    return -1;
  }
  *out_result = result;
  return 0;
}

static int turbo_knowledge_delete_document_tool_json_value(
    const json_value_t *arguments,
    json_value_t **out_result, void *user_data) {
  turbo_agent_knowledge_store_t *store = (turbo_agent_knowledge_store_t *)user_data;
  const char *document_id;
  json_value_t *result = NULL;

  if (!store || !arguments || !out_result) {
    return -1;
  }
  *out_result = NULL;
  document_id = turbo_runtime_json_value_as_string(
      turbo_json_object_get(arguments, "document_id"));
  if (!document_id || !document_id[0]) {
    return -1;
  }
  if (turbo_agent_knowledge_store_delete_document(store, document_id) != 0) {
    return -1;
  }
  result = turbo_json_create_object();
  if (!result || turbo_knowledge_json_value_set_bool(result, "ok", 1) != 0 ||
      turbo_knowledge_json_value_set_string(result, "summary", "document deleted") != 0 ||
      turbo_knowledge_json_value_set_string(result, "document_id", document_id) != 0) {
    turbo_runtime_json_destroy(result);
    return -1;
  }
  *out_result = result;
  return 0;
}

static int turbo_knowledge_tool_json_handler(
    const char *arguments_json, char **out_output,
    int (*json_value_handler)(const json_value_t *,
                        json_value_t **, void *),
    void *user_data) {
  json_value_t *args_json = NULL;
  json_value_t *result_json = NULL;
  json_value_t *args = NULL;
  json_value_t *result = NULL;
  char *serialized = NULL;
  int rc;

  if (!out_output || !json_value_handler) {
    return -1;
  }
  *out_output = NULL;
  if (turbo_parse_json((const uint8_t *)(arguments_json ? arguments_json : "{}"),
                       strlen(arguments_json ? arguments_json : "{}"), &args_json) != 0 ||
      !args_json) {
    return -1;
  }
  args = turbo_json_clone(args_json);
  turbo_free_json(&args_json);
  if (!args) {
    return -1;
  }
  rc = json_value_handler(args, &result, user_data);
  turbo_runtime_json_destroy(args);
  if (rc != 0 || !result) {
    turbo_runtime_json_destroy(result);
    return -1;
  }
  result_json = turbo_json_clone(result);
  turbo_runtime_json_destroy(result);
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

static int turbo_knowledge_search_tool_json(const char *arguments_json,
                                            char **out_output, void *user_data) {
  return turbo_knowledge_tool_json_handler(arguments_json, out_output,
                                           turbo_knowledge_search_tool_json_value,
                                           user_data);
}

static int turbo_knowledge_build_context_tool_json(const char *arguments_json,
                                                   char **out_output,
                                                   void *user_data) {
  return turbo_knowledge_tool_json_handler(arguments_json, out_output,
                                           turbo_knowledge_build_context_tool_json_value,
                                           user_data);
}

static int turbo_knowledge_list_documents_tool_json(const char *arguments_json,
                                                    char **out_output,
                                                    void *user_data) {
  return turbo_knowledge_tool_json_handler(arguments_json, out_output,
                                           turbo_knowledge_list_documents_tool_json_value,
                                           user_data);
}

static int turbo_knowledge_stats_tool_json(const char *arguments_json,
                                           char **out_output, void *user_data) {
  return turbo_knowledge_tool_json_handler(arguments_json, out_output,
                                           turbo_knowledge_stats_tool_json_value,
                                           user_data);
}

static int turbo_knowledge_tool_graph_tool_json(const char *arguments_json,
                                                char **out_output,
                                                void *user_data) {
  return turbo_knowledge_tool_json_handler(arguments_json, out_output,
                                           turbo_knowledge_tool_graph_tool_json_value,
                                           user_data);
}

static int turbo_knowledge_get_document_tool_json(const char *arguments_json,
                                                  char **out_output,
                                                  void *user_data) {
  return turbo_knowledge_tool_json_handler(arguments_json, out_output,
                                           turbo_knowledge_get_document_tool_json_value,
                                           user_data);
}

static int turbo_knowledge_upsert_text_tool_json(const char *arguments_json,
                                                 char **out_output,
                                                 void *user_data) {
  return turbo_knowledge_tool_json_handler(arguments_json, out_output,
                                           turbo_knowledge_upsert_text_tool_json_value,
                                           user_data);
}

static int turbo_knowledge_index_file_tool_json(const char *arguments_json,
                                                char **out_output,
                                                void *user_data) {
  return turbo_knowledge_tool_json_handler(arguments_json, out_output,
                                           turbo_knowledge_index_file_tool_json_value,
                                           user_data);
}

static int turbo_knowledge_index_directory_tool_json(const char *arguments_json,
                                                     char **out_output,
                                                     void *user_data) {
  return turbo_knowledge_tool_json_handler(
      arguments_json, out_output, turbo_knowledge_index_directory_tool_json_value,
      user_data);
}

static int turbo_knowledge_delete_document_tool_json(const char *arguments_json,
                                                     char **out_output,
                                                     void *user_data) {
  return turbo_knowledge_tool_json_handler(arguments_json, out_output,
                                           turbo_knowledge_delete_document_tool_json_value,
                                           user_data);
}

static int turbo_knowledge_search_action_handler(const json_value_t *args,
                                                 json_value_t **out_result,
                                                 void *user_data) {
  turbo_agent_knowledge_store_t *store = (turbo_agent_knowledge_store_t *)user_data;
  const char *query;
  const char *kind;
  const char *uri_prefix;
  int limit;
  json_value_t *results = NULL;
  json_value_t *result = NULL;

  if (!store || !args || !out_result || turbo_json_type(args) != TURBO_JSON_OBJECT) {
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
  if (turbo_agent_knowledge_store_query_ex(
          store, query, kind, uri_prefix, (size_t)limit, &results) != 0) {
    return -1;
  }
  result = turbo_action_result_create(1, "knowledge search completed");
  if (!result) {
    turbo_free_json(&results);
    return -1;
  }
  turbo_json_object_add(result, "results", results);
  *out_result = result;
  return 0;
}

static int turbo_knowledge_build_context_action_handler(
    const json_value_t *args, json_value_t **out_result, void *user_data) {
  turbo_agent_knowledge_store_t *store = (turbo_agent_knowledge_store_t *)user_data;
  const char *query;
  const char *kind;
  const char *uri_prefix;
  int limit;
  json_value_t *context = NULL;
  json_value_t *result = NULL;

  if (!store || !args || !out_result || turbo_json_type(args) != TURBO_JSON_OBJECT) {
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
  if (turbo_agent_knowledge_store_build_context(
          store, query, kind, uri_prefix, (size_t)limit, &context) != 0) {
    return -1;
  }
  result = turbo_action_result_create(1, "knowledge context built");
  if (!result) {
    turbo_free_json(&context);
    return -1;
  }
  turbo_json_object_add(result, "context", context);
  *out_result = result;
  return 0;
}

static int turbo_knowledge_list_documents_action_handler(
    const json_value_t *args, json_value_t **out_result, void *user_data) {
  turbo_agent_knowledge_store_t *store = (turbo_agent_knowledge_store_t *)user_data;
  const char *kind;
  const char *uri_prefix;
  int limit;
  json_value_t *documents = NULL;
  json_value_t *result = NULL;

  if (!store || !args || !out_result || turbo_json_type(args) != TURBO_JSON_OBJECT) {
    return -1;
  }
  *out_result = NULL;
  kind = turbo_json_get_string(args, "kind");
  uri_prefix = turbo_json_get_string(args, "uri_prefix");
  limit = turbo_json_get_int(args, "limit", 100);
  if (limit < 0) {
    return -1;
  }
  if (turbo_agent_knowledge_store_list_documents(
          store, kind, uri_prefix, (size_t)limit, &documents) != 0) {
    return -1;
  }
  result = turbo_action_result_create(1, "documents listed");
  if (!result) {
    turbo_free_json(&documents);
    return -1;
  }
  turbo_json_object_add(result, "documents", documents);
  *out_result = result;
  return 0;
}

static int turbo_knowledge_stats_action_handler(const json_value_t *args,
                                                json_value_t **out_result,
                                                void *user_data) {
  turbo_agent_knowledge_store_t *store = (turbo_agent_knowledge_store_t *)user_data;
  json_value_t *stats = NULL;
  json_value_t *result = NULL;

  (void)args;
  if (!store || !out_result) {
    return -1;
  }
  *out_result = NULL;
  if (turbo_agent_knowledge_store_stats(store, &stats) != 0) {
    return -1;
  }
  result = turbo_action_result_create(1, "knowledge stats loaded");
  if (!result) {
    turbo_free_json(&stats);
    return -1;
  }
  turbo_json_object_add(result, "stats", stats);
  *out_result = result;
  return 0;
}

static int turbo_knowledge_tool_graph_action_handler(
    const json_value_t *args, json_value_t **out_result, void *user_data) {
  json_value_t *graph = NULL;
  json_value_t *result = NULL;

  (void)args;
  (void)user_data;
  if (!out_result) {
    return -1;
  }
  *out_result = NULL;
  if (turbo_agent_knowledge_store_tool_graph(&graph) != 0) {
    return -1;
  }
  result = turbo_action_result_create(1, "knowledge tool graph loaded");
  if (!result) {
    turbo_free_json(&graph);
    return -1;
  }
  turbo_json_object_add(result, "graph", graph);
  *out_result = result;
  return 0;
}

static int turbo_knowledge_get_document_action_handler(
    const json_value_t *args, json_value_t **out_result, void *user_data) {
  turbo_agent_knowledge_store_t *store = (turbo_agent_knowledge_store_t *)user_data;
  const char *document_id;
  json_value_t *document = NULL;
  json_value_t *result = NULL;

  if (!store || !args || !out_result || turbo_json_type(args) != TURBO_JSON_OBJECT) {
    return -1;
  }
  *out_result = NULL;
  document_id = turbo_json_get_string(args, "document_id");
  if (!document_id || !document_id[0]) {
    return -1;
  }
  if (turbo_agent_knowledge_store_get_document(store, document_id, &document) != 0) {
    return -1;
  }
  result = turbo_action_result_create(1, "document loaded");
  if (!result) {
    turbo_free_json(&document);
    return -1;
  }
  turbo_json_object_add(result, "document", document);
  *out_result = result;
  return 0;
}

static int turbo_knowledge_upsert_text_action_handler(
    const json_value_t *args, json_value_t **out_result, void *user_data) {
  turbo_agent_knowledge_store_t *store = (turbo_agent_knowledge_store_t *)user_data;
  const char *id;
  const char *uri;
  const char *kind;
  const char *title;
  const char *metadata_json;
  const char *text;
  int chunk_target_bytes;
  turbo_agent_knowledge_document_t document;
  json_value_t *result = NULL;

  if (!store || !args || !out_result || turbo_json_type(args) != TURBO_JSON_OBJECT) {
    return -1;
  }
  *out_result = NULL;
  id = turbo_json_get_string(args, "id");
  uri = turbo_json_get_string(args, "uri");
  kind = turbo_json_get_string(args, "kind");
  title = turbo_json_get_string(args, "title");
  metadata_json = turbo_json_get_string(args, "metadata_json");
  text = turbo_json_get_string(args, "text");
  chunk_target_bytes = turbo_json_get_int(args, "chunk_target_bytes", 2048);
  if (!id || !id[0] || !text || chunk_target_bytes < 0) {
    return -1;
  }
  memset(&document, 0, sizeof(document));
  document.id = id;
  document.uri = uri && uri[0] ? uri : id;
  document.kind = kind && kind[0] ? kind : "note";
  document.title = title;
  document.metadata_json = metadata_json;
  if (turbo_agent_knowledge_store_upsert_text(store, &document, text,
                                              (size_t)chunk_target_bytes) != 0) {
    return -1;
  }
  result = turbo_action_result_create(1, "text indexed");
  if (!result) {
    return -1;
  }
  turbo_json_object_set_string(result, "document_id", document.id);
  turbo_json_object_set_string(result, "uri", document.uri);
  turbo_json_object_set_string(result, "kind", document.kind);
  turbo_json_object_set_number(result, "chunk_target_bytes", chunk_target_bytes);
  *out_result = result;
  return 0;
}

static int turbo_knowledge_index_file_action_handler(const json_value_t *args,
                                                     json_value_t **out_result,
                                                     void *user_data) {
  turbo_agent_knowledge_store_t *store = (turbo_agent_knowledge_store_t *)user_data;
  const char *path;
  const char *kind;
  int chunk_target_bytes;
  json_value_t *result;

  if (!store || !args || !out_result || turbo_json_type(args) != TURBO_JSON_OBJECT) {
    return -1;
  }
  *out_result = NULL;
  path = turbo_json_get_string(args, "path");
  kind = turbo_json_get_string(args, "kind");
  chunk_target_bytes = turbo_json_get_int(args, "chunk_target_bytes", 2048);
  if (!path || !path[0] || chunk_target_bytes < 0) {
    return -1;
  }
  if (turbo_agent_knowledge_store_index_file(store, path, kind ? kind : "file",
                                             (size_t)chunk_target_bytes) != 0) {
    return -1;
  }
  result = turbo_action_result_create(1, "file indexed");
  if (!result) {
    return -1;
  }
  turbo_json_object_set_string(result, "uri", path);
  turbo_json_object_set_number(result, "chunk_target_bytes",
                               (double)chunk_target_bytes);
  *out_result = result;
  return 0;
}

static int turbo_knowledge_index_directory_action_handler(
    const json_value_t *args, json_value_t **out_result, void *user_data) {
  turbo_agent_knowledge_store_t *store = (turbo_agent_knowledge_store_t *)user_data;
  const char *root_dir;
  turbo_agent_knowledge_directory_options_t options;
  json_value_t *stats = NULL;
  json_value_t *result = NULL;
  int chunk_target_bytes;
  int max_files;
  int max_file_bytes;

  if (!store || !args || !out_result || turbo_json_type(args) != TURBO_JSON_OBJECT) {
    return -1;
  }
  *out_result = NULL;
  root_dir = turbo_json_get_string(args, "root_dir");
  chunk_target_bytes = turbo_json_get_int(args, "chunk_target_bytes", 2048);
  max_files = turbo_json_get_int(args, "max_files", 0);
  max_file_bytes = turbo_json_get_int(args, "max_file_bytes", 0);
  if (!root_dir || !root_dir[0] || chunk_target_bytes < 0 || max_files < 0 ||
      max_file_bytes < 0) {
    return -1;
  }
  memset(&options, 0, sizeof(options));
  options.kind = turbo_json_get_string(args, "kind");
  options.chunk_target_bytes = (size_t)chunk_target_bytes;
  options.recursive = turbo_json_get_bool(args, "recursive", 1) ? 1 : 0;
  options.max_files = (size_t)max_files;
  options.max_file_bytes = (size_t)max_file_bytes;
  options.include_extensions = turbo_json_get_string(args, "include_extensions");
  if (turbo_agent_knowledge_store_index_directory_ex(store, root_dir, &options,
                                                     &stats) != 0) {
    return -1;
  }
  result = turbo_action_result_create(1, "directory indexed");
  if (!result) {
    turbo_free_json(&stats);
    return -1;
  }
  turbo_json_object_set_string(result, "root_dir", root_dir);
  turbo_json_object_add(result, "stats", stats);
  *out_result = result;
  return 0;
}

static int turbo_knowledge_delete_document_action_handler(
    const json_value_t *args, json_value_t **out_result, void *user_data) {
  turbo_agent_knowledge_store_t *store = (turbo_agent_knowledge_store_t *)user_data;
  const char *document_id;
  json_value_t *result;

  if (!store || !args || !out_result || turbo_json_type(args) != TURBO_JSON_OBJECT) {
    return -1;
  }
  *out_result = NULL;
  document_id = turbo_json_get_string(args, "document_id");
  if (!document_id || !document_id[0]) {
    return -1;
  }
  if (turbo_agent_knowledge_store_delete_document(store, document_id) != 0) {
    return -1;
  }
  result = turbo_action_result_create(1, "document deleted");
  if (!result) {
    return -1;
  }
  turbo_json_object_set_string(result, "document_id", document_id);
  *out_result = result;
  return 0;
}

int turbo_agent_knowledge_store_add_tools(turbo_tool_registry_t *registry,
                                          turbo_agent_knowledge_store_t *store) {
  static const char *search_schema =
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},"
      "\"kind\":{\"type\":\"string\"},"
      "\"uri_prefix\":{\"type\":\"string\"},"
      "\"limit\":{\"type\":\"integer\",\"minimum\":0}},"
      "\"required\":[\"query\"],\"additionalProperties\":false}";
  static const char *build_context_schema =
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},"
      "\"kind\":{\"type\":\"string\"},"
      "\"uri_prefix\":{\"type\":\"string\"},"
      "\"limit\":{\"type\":\"integer\",\"minimum\":0}},"
      "\"required\":[\"query\"],\"additionalProperties\":false}";
  static const char *stats_schema =
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}";
  static const char *tool_graph_schema =
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}";
  static const char *list_documents_schema =
      "{\"type\":\"object\",\"properties\":{\"kind\":{\"type\":\"string\"},"
      "\"uri_prefix\":{\"type\":\"string\"},"
      "\"limit\":{\"type\":\"integer\",\"minimum\":0}},"
      "\"additionalProperties\":false}";
  static const char *get_document_schema =
      "{\"type\":\"object\",\"properties\":{\"document_id\":{\"type\":\"string\"}},"
      "\"required\":[\"document_id\"],\"additionalProperties\":false}";
  static const char *upsert_text_schema =
      "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},"
      "\"uri\":{\"type\":\"string\"},"
      "\"kind\":{\"type\":\"string\"},"
      "\"title\":{\"type\":\"string\"},"
      "\"metadata_json\":{\"type\":\"string\"},"
      "\"text\":{\"type\":\"string\"},"
      "\"chunk_target_bytes\":{\"type\":\"integer\",\"minimum\":0}},"
      "\"required\":[\"id\",\"text\"],\"additionalProperties\":false}";
  static const char *index_schema =
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
      "\"kind\":{\"type\":\"string\"},"
      "\"chunk_target_bytes\":{\"type\":\"integer\",\"minimum\":0}},"
      "\"required\":[\"path\"],\"additionalProperties\":false}";
  static const char *index_directory_schema =
      "{\"type\":\"object\",\"properties\":{\"root_dir\":{\"type\":\"string\"},"
      "\"kind\":{\"type\":\"string\"},"
      "\"include_extensions\":{\"type\":\"string\"},"
      "\"chunk_target_bytes\":{\"type\":\"integer\",\"minimum\":0},"
      "\"recursive\":{\"type\":\"boolean\"},"
      "\"max_files\":{\"type\":\"integer\",\"minimum\":0},"
      "\"max_file_bytes\":{\"type\":\"integer\",\"minimum\":0}},"
      "\"required\":[\"root_dir\"],\"additionalProperties\":false}";
  static const char *delete_document_schema =
      "{\"type\":\"object\",\"properties\":{\"document_id\":{\"type\":\"string\"}},"
      "\"required\":[\"document_id\"],\"additionalProperties\":false}";
  turbo_tool_definition_t definition;

  if (!registry || !store) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.search";
  definition.description = "Search the local SQLite FTS5 knowledge index.";
  definition.parameters_json = search_schema;
  definition.strict = 1;
  definition.handler = turbo_knowledge_search_tool_json;
  definition.json_value_handler = turbo_knowledge_search_tool_json_value;
  definition.user_data = store;
  if (turbo_tool_registry_add(registry, &definition) != TURBO_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.build_context";
  definition.description =
      "Build merged local knowledge context text from the SQLite FTS5 index.";
  definition.parameters_json = build_context_schema;
  definition.strict = 1;
  definition.handler = turbo_knowledge_build_context_tool_json;
  definition.json_value_handler = turbo_knowledge_build_context_tool_json_value;
  definition.user_data = store;
  if (turbo_tool_registry_add(registry, &definition) != TURBO_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.stats";
  definition.description = "Inspect local SQLite FTS5 knowledge index statistics.";
  definition.parameters_json = stats_schema;
  definition.strict = 1;
  definition.handler = turbo_knowledge_stats_tool_json;
  definition.json_value_handler = turbo_knowledge_stats_tool_json_value;
  definition.user_data = store;
  if (turbo_tool_registry_add(registry, &definition) != TURBO_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.tool_graph";
  definition.description = "Describe recommended dependencies between local knowledge tools.";
  definition.parameters_json = tool_graph_schema;
  definition.strict = 1;
  definition.handler = turbo_knowledge_tool_graph_tool_json;
  definition.json_value_handler = turbo_knowledge_tool_graph_tool_json_value;
  definition.user_data = store;
  if (turbo_tool_registry_add(registry, &definition) != TURBO_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.list_documents";
  definition.description = "List documents currently indexed in the knowledge store.";
  definition.parameters_json = list_documents_schema;
  definition.strict = 1;
  definition.handler = turbo_knowledge_list_documents_tool_json;
  definition.json_value_handler = turbo_knowledge_list_documents_tool_json_value;
  definition.user_data = store;
  if (turbo_tool_registry_add(registry, &definition) != TURBO_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.get_document";
  definition.description = "Load one indexed document with its chunks.";
  definition.parameters_json = get_document_schema;
  definition.strict = 1;
  definition.handler = turbo_knowledge_get_document_tool_json;
  definition.json_value_handler = turbo_knowledge_get_document_tool_json_value;
  definition.user_data = store;
  if (turbo_tool_registry_add(registry, &definition) != TURBO_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.upsert_text";
  definition.description = "Index or replace one text document in the SQLite FTS5 knowledge index.";
  definition.parameters_json = upsert_text_schema;
  definition.strict = 1;
  definition.handler = turbo_knowledge_upsert_text_tool_json;
  definition.json_value_handler = turbo_knowledge_upsert_text_tool_json_value;
  definition.user_data = store;
  if (turbo_tool_registry_add(registry, &definition) != TURBO_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.index_file";
  definition.description = "Index one local text file into the SQLite FTS5 knowledge index.";
  definition.parameters_json = index_schema;
  definition.strict = 1;
  definition.handler = turbo_knowledge_index_file_tool_json;
  definition.json_value_handler = turbo_knowledge_index_file_tool_json_value;
  definition.user_data = store;
  if (turbo_tool_registry_add(registry, &definition) != TURBO_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.index_directory";
  definition.description =
      "Index local text files under a directory into the SQLite FTS5 knowledge index.";
  definition.parameters_json = index_directory_schema;
  definition.strict = 1;
  definition.handler = turbo_knowledge_index_directory_tool_json;
  definition.json_value_handler = turbo_knowledge_index_directory_tool_json_value;
  definition.user_data = store;
  if (turbo_tool_registry_add(registry, &definition) != TURBO_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.delete_document";
  definition.description = "Delete one document and its chunks from the knowledge index.";
  definition.parameters_json = delete_document_schema;
  definition.strict = 1;
  definition.handler = turbo_knowledge_delete_document_tool_json;
  definition.json_value_handler = turbo_knowledge_delete_document_tool_json_value;
  definition.user_data = store;
  if (turbo_tool_registry_add(registry, &definition) != TURBO_TOOL_OK) {
    return -1;
  }

  return 0;
}

int turbo_agent_knowledge_store_add_action_tools(
    turbo_action_tool_registry_t *registry, turbo_agent_knowledge_store_t *store) {
  static const char *search_schema =
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},"
      "\"kind\":{\"type\":\"string\"},"
      "\"uri_prefix\":{\"type\":\"string\"},"
      "\"limit\":{\"type\":\"integer\",\"minimum\":0}},"
      "\"required\":[\"query\"],\"additionalProperties\":false}";
  static const char *build_context_schema =
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},"
      "\"kind\":{\"type\":\"string\"},"
      "\"uri_prefix\":{\"type\":\"string\"},"
      "\"limit\":{\"type\":\"integer\",\"minimum\":0}},"
      "\"required\":[\"query\"],\"additionalProperties\":false}";
  static const char *stats_schema =
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}";
  static const char *tool_graph_schema =
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}";
  static const char *list_documents_schema =
      "{\"type\":\"object\",\"properties\":{\"kind\":{\"type\":\"string\"},"
      "\"uri_prefix\":{\"type\":\"string\"},"
      "\"limit\":{\"type\":\"integer\",\"minimum\":0}},"
      "\"additionalProperties\":false}";
  static const char *get_document_schema =
      "{\"type\":\"object\",\"properties\":{\"document_id\":{\"type\":\"string\"}},"
      "\"required\":[\"document_id\"],\"additionalProperties\":false}";
  static const char *upsert_text_schema =
      "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},"
      "\"uri\":{\"type\":\"string\"},"
      "\"kind\":{\"type\":\"string\"},"
      "\"title\":{\"type\":\"string\"},"
      "\"metadata_json\":{\"type\":\"string\"},"
      "\"text\":{\"type\":\"string\"},"
      "\"chunk_target_bytes\":{\"type\":\"integer\",\"minimum\":0}},"
      "\"required\":[\"id\",\"text\"],\"additionalProperties\":false}";
  static const char *index_schema =
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
      "\"kind\":{\"type\":\"string\"},"
      "\"chunk_target_bytes\":{\"type\":\"integer\",\"minimum\":0}},"
      "\"required\":[\"path\"],\"additionalProperties\":false}";
  static const char *index_directory_schema =
      "{\"type\":\"object\",\"properties\":{\"root_dir\":{\"type\":\"string\"},"
      "\"kind\":{\"type\":\"string\"},"
      "\"include_extensions\":{\"type\":\"string\"},"
      "\"chunk_target_bytes\":{\"type\":\"integer\",\"minimum\":0},"
      "\"recursive\":{\"type\":\"boolean\"},"
      "\"max_files\":{\"type\":\"integer\",\"minimum\":0},"
      "\"max_file_bytes\":{\"type\":\"integer\",\"minimum\":0}},"
      "\"required\":[\"root_dir\"],\"additionalProperties\":false}";
  static const char *delete_document_schema =
      "{\"type\":\"object\",\"properties\":{\"document_id\":{\"type\":\"string\"}},"
      "\"required\":[\"document_id\"],\"additionalProperties\":false}";
  turbo_action_tool_definition_t definition;

  if (!registry || !store) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.search";
  definition.description = "Search the local SQLite FTS5 knowledge index.";
  definition.parameters_json = search_schema;
  definition.kind = TURBO_ACTION_OBSERVE;
  definition.idempotent = 1;
  definition.handler = turbo_knowledge_search_action_handler;
  definition.user_data = store;
  if (turbo_action_tool_registry_add(registry, &definition) != TURBO_ACTION_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.build_context";
  definition.description =
      "Build merged local knowledge context text from the SQLite FTS5 index.";
  definition.parameters_json = build_context_schema;
  definition.kind = TURBO_ACTION_OBSERVE;
  definition.idempotent = 1;
  definition.handler = turbo_knowledge_build_context_action_handler;
  definition.user_data = store;
  if (turbo_action_tool_registry_add(registry, &definition) != TURBO_ACTION_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.stats";
  definition.description = "Inspect local SQLite FTS5 knowledge index statistics.";
  definition.parameters_json = stats_schema;
  definition.kind = TURBO_ACTION_OBSERVE;
  definition.idempotent = 1;
  definition.handler = turbo_knowledge_stats_action_handler;
  definition.user_data = store;
  if (turbo_action_tool_registry_add(registry, &definition) != TURBO_ACTION_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.tool_graph";
  definition.description = "Describe recommended dependencies between local knowledge tools.";
  definition.parameters_json = tool_graph_schema;
  definition.kind = TURBO_ACTION_OBSERVE;
  definition.idempotent = 1;
  definition.handler = turbo_knowledge_tool_graph_action_handler;
  definition.user_data = store;
  if (turbo_action_tool_registry_add(registry, &definition) != TURBO_ACTION_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.list_documents";
  definition.description = "List documents currently indexed in the knowledge store.";
  definition.parameters_json = list_documents_schema;
  definition.kind = TURBO_ACTION_OBSERVE;
  definition.idempotent = 1;
  definition.handler = turbo_knowledge_list_documents_action_handler;
  definition.user_data = store;
  if (turbo_action_tool_registry_add(registry, &definition) != TURBO_ACTION_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.get_document";
  definition.description = "Load one indexed document with its chunks.";
  definition.parameters_json = get_document_schema;
  definition.kind = TURBO_ACTION_OBSERVE;
  definition.idempotent = 1;
  definition.handler = turbo_knowledge_get_document_action_handler;
  definition.user_data = store;
  if (turbo_action_tool_registry_add(registry, &definition) != TURBO_ACTION_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.upsert_text";
  definition.description = "Index or replace one text document in the SQLite FTS5 knowledge index.";
  definition.parameters_json = upsert_text_schema;
  definition.kind = TURBO_ACTION_MUTATE;
  definition.idempotent = 1;
  definition.handler = turbo_knowledge_upsert_text_action_handler;
  definition.user_data = store;
  if (turbo_action_tool_registry_add(registry, &definition) != TURBO_ACTION_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.index_file";
  definition.description = "Index one local text file into the SQLite FTS5 knowledge index.";
  definition.parameters_json = index_schema;
  definition.kind = TURBO_ACTION_MUTATE;
  definition.idempotent = 1;
  definition.handler = turbo_knowledge_index_file_action_handler;
  definition.user_data = store;
  if (turbo_action_tool_registry_add(registry, &definition) != TURBO_ACTION_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.index_directory";
  definition.description =
      "Index local text files under a directory into the SQLite FTS5 knowledge index.";
  definition.parameters_json = index_directory_schema;
  definition.kind = TURBO_ACTION_MUTATE;
  definition.idempotent = 1;
  definition.handler = turbo_knowledge_index_directory_action_handler;
  definition.user_data = store;
  if (turbo_action_tool_registry_add(registry, &definition) != TURBO_ACTION_TOOL_OK) {
    return -1;
  }

  memset(&definition, 0, sizeof(definition));
  definition.name = "agent.knowledge.delete_document";
  definition.description = "Delete one document and its chunks from the knowledge index.";
  definition.parameters_json = delete_document_schema;
  definition.kind = TURBO_ACTION_MUTATE;
  definition.idempotent = 1;
  definition.handler = turbo_knowledge_delete_document_action_handler;
  definition.user_data = store;
  if (turbo_action_tool_registry_add(registry, &definition) != TURBO_ACTION_TOOL_OK) {
    return -1;
  }

  return 0;
}
