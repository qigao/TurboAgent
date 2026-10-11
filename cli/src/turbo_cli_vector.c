#include "turbo_cli_vector.h"
#include "turbo_cli_knowledge.h"
#include "turbo_embedding.h"
#include "turbo_vector_store.h"
#include <http_client/http.h>
#include <uri_parser.h>
#include <cmeta_fs.h>
#include <salts/clock.h>
#include <fmt.h>
#include <sqlite3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { VECTOR_APPLICATION_ID = 0x54415643, VECTOR_MAX_ROWS = 8192,
       VECTOR_MAX_BYTES = 64 * 1024 * 1024, VECTOR_MAX_DIMENSIONS = 4096,
       VECTOR_CHUNK_BYTES = 2048, VECTOR_CHUNK_OVERLAP = 3,
       EMBED_MAX_INPUT = 8192, EMBED_MAX_RESPONSE = 256 * 1024,
       EMBED_TIMEOUT_MS = 60000, EMBED_POLL_MS = 50, EMBED_DRAIN_MS = 5000 };

struct turbo_cli_vector_s {
  turbo_vector_store_t *store;
  turbo_embedding_model_t *embedding;
  sqlite3 *metadata;
  cmeta_file_t lock;
  tstr endpoint, model, connection, authority, target, authorization, response;
  const atomic_int *interrupted;
  uint64_t operation_deadline;
  size_t dimensions;
  chttp_async_client client;
  int done, response_status, error;
};

static int endpoint_parse(const char *text, uri_t *uri) {
  if (!text || !text[0] || strlen(text) > 2048 || !uri_parse(text, uri) ||
      !uri->valid || uri->overflow_flags || !uri->host[0] || uri->path[0] != '/' ||
      (uri->component_flags & (URI_COMPONENT_USERINFO | URI_COMPONENT_QUERY | URI_COMPONENT_FRAGMENT)))
    return SALTS_EINVAL;
  if (strcmp(uri->scheme, "https") &&
      (strcmp(uri->scheme, "http") || (strcmp(uri->host, "127.0.0.1") && strcmp(uri->host, "::1"))))
    return SALTS_EINVAL;
  for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
    if (*p <= 32 || *p >= 127 || *p == '\\') return SALTS_EINVAL;
  if ((uri->component_flags & URI_COMPONENT_PORT) && (uri->port <= 0 || uri->port > 65535))
    return SALTS_EINVAL;
  return SALTS_OK;
}

int turbo_cli_vector_validate(const turbo_cli_options_t *options) {
  uri_t uri = {0};
  if (!options->vector_db)
    return options->embedding_url || options->embedding_model ? SALTS_EINVAL : SALTS_OK;
  if (options->knowledge_db || !options->vector_db[0] || strlen(options->vector_db) > 4096 ||
      !strcmp(options->vector_db, ":memory:") || !strncmp(options->vector_db, "file:", 5) ||
      !vstr_utf8_valid(vstr_from_cstr(options->vector_db)) ||
      !options->embedding_model || !options->embedding_model[0] ||
      strlen(options->embedding_model) > 256 || !vstr_utf8_valid(vstr_from_cstr(options->embedding_model)))
    return SALTS_EINVAL;
  return endpoint_parse(options->embedding_url, &uri);
}

static int stopped(turbo_cli_vector_t *vector) {
  if (vector->interrupted && atomic_load_explicit(vector->interrupted, memory_order_relaxed)) return SALTS_ECANCELED;
  return cmeta_monotonic_ms() >= vector->operation_deadline ? SALTS_ETIMEDOUT : SALTS_OK;
}

static chttp_client_config embedding_http_config(void) {
  chttp_client_config config = {0};
#ifdef _WIN32
  config.network.backend = NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  config.network.backend = NATIVE_IO_BACKEND_EPOLL;
#else
  config.network.backend = NATIVE_IO_BACKEND_KQUEUE;
#endif
  config.network.connection_capacity = 1;
  config.network.command_capacity = 16; config.network.request_capacity = 16;
  config.network.completion_batch_capacity = 4; config.network.event_capacity = 16;
  config.network.max_send_bytes = 128 * 1024; config.network.receive_buffer_bytes = 64 * 1024;
  config.network.connect_timeout_ms = EMBED_TIMEOUT_MS;
  config.network.read_timeout_ms = EMBED_TIMEOUT_MS;
  config.network.write_timeout_ms = EMBED_TIMEOUT_MS;
  config.network.tls_io_buffer_bytes = 64 * 1024;
  config.network.tls_handshake_timeout_ms = EMBED_TIMEOUT_MS;
  config.request_capacity = 1; config.max_start_line_bytes = 4096;
  config.max_header_count = 32; config.max_header_bytes = 16 * 1024;
  config.max_request_body_bytes = 64 * 1024; config.max_response_body_bytes = EMBED_MAX_RESPONSE;
  config.max_informational_responses = 4;
  return config;
}

static void embedding_complete(void *user, chttp_request request,
    const chttp_response_view *response, const chttp_error *error) {
  turbo_cli_vector_t *vector = user;
  (void)request;
  vector->done = 1;
  vector->error = error ? error->status : SALTS_OK;
  if (vector->error != SALTS_OK) return;
  if (!response || !response->body || !response->body_size || response->body_size > EMBED_MAX_RESPONSE) {
    vector->error = SALTS_EPROTO; return;
  }
  vector->response_status = (int)response->status_code;
  vector->response = tstr_new_len(response->body, response->body_size);
  if (!vector->response) vector->error = SALTS_ENOMEM;
}

static int http_drain(turbo_cli_vector_t *vector) {
  int rc;
  if (!vector->client.impl) return SALTS_OK;
  rc = chttp_async_client_stop(&vector->client, EMBED_DRAIN_MS);
  return rc == SALTS_OK ? chttp_async_client_destroy(&vector->client) : rc;
}

static int embedding_post(turbo_cli_vector_t *vector, const char *body) {
  chttp_client_config config = embedding_http_config();
  chttp_header headers[] = {{"Content-Type", "application/json"}, {"Accept", "application/json"},
                           {"Authorization", vector->authorization}};
  chttp_request request = {0};
  chttp_request_options options = {.connection_uri = vector->connection, .authority = vector->authority,
      .target = vector->target, .method = CHTTP_METHOD_POST, .headers = headers,
      .header_count = vector->authorization ? 3 : 2, .body = body, .body_size = strlen(body),
      .on_complete = embedding_complete, .user = vector, .protocol = CHTTP_HTTP_1_1};
  uint64_t deadline = cmeta_monotonic_ms() + EMBED_TIMEOUT_MS;
  int rc = stopped(vector), admitted = 0;
  if (rc != SALTS_OK || vector->client.impl) return rc != SALTS_OK ? rc : SALTS_ESHUTDOWN;
  vector->done = 0; vector->error = SALTS_OK; vector->response_status = 0;
  tstr_free(vector->response); vector->response = NULL;
  rc = chttp_async_client_init(&vector->client, &config);
  if (rc != SALTS_OK) return rc;
  rc = stopped(vector);
  if (rc == SALTS_OK) { rc = chttp_async_client_submit(&vector->client, &options, &request); admitted = rc == SALTS_OK; }
  while (rc == SALTS_OK) {
    size_t completions = 0;
    rc = stopped(vector);
    if (rc != SALTS_OK) break;
    if (vector->done) { rc = vector->error; break; }
    if (cmeta_monotonic_ms() >= deadline) { rc = SALTS_ETIMEDOUT; break; }
    rc = chttp_async_client_poll(&vector->client, EMBED_POLL_MS, &completions);
  }
  if (admitted && !vector->done) {
    int cancel = chttp_async_request_cancel(&vector->client, request);
    if (cancel != SALTS_OK && cancel != SALTS_ENOENT && cancel != SALTS_EALREADY) rc = cancel;
  }
  {
    int drain = http_drain(vector);
    if (drain != SALTS_OK) rc = drain;
  }
  if (rc == SALTS_OK && vector->response_status != 200) rc = SALTS_EPROTO;
  return rc;
}

static int sqlite_integer(sqlite3 *db, const char *sql, sqlite3_int64 *value) {
  sqlite3_stmt *statement = NULL;
  int rc = SALTS_EPROTO;
  if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) goto done;
  if (sqlite3_step(statement) != SQLITE_ROW || sqlite3_column_type(statement, 0) != SQLITE_INTEGER) goto done;
  *value = sqlite3_column_int64(statement, 0);
  if (sqlite3_step(statement) == SQLITE_DONE) rc = SALTS_OK;
done:
  sqlite3_finalize(statement);
  return rc;
}

static int pin_dimensions(turbo_cli_vector_t *vector, size_t dimensions) {
  sqlite3_stmt *statement = NULL;
  int rc = SALTS_EPROTO;
  if (vector->dimensions) return vector->dimensions == dimensions ? SALTS_OK : SALTS_EPROTO;
  if (sqlite3_prepare_v2(vector->metadata,
      "UPDATE cli_vector_config SET dimensions=? WHERE id=1 AND dimensions=0", -1, &statement, NULL) != SQLITE_OK)
    goto done;
  if (sqlite3_bind_int64(statement, 1, (sqlite3_int64)dimensions) == SQLITE_OK &&
      sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(vector->metadata) == 1) {
    vector->dimensions = dimensions; rc = SALTS_OK;
  }
done:
  sqlite3_finalize(statement);
  return rc;
}

static int embed_text(void *user, const char *text, json_value_t **out) {
  turbo_cli_vector_t *vector = user;
  json_value_t *request = NULL, *response = NULL, *embedding;
  const json_value_t *data, *item;
  char *body = NULL;
  tstr snippet = NULL;
  double scale = 0, norm = 0;
  size_t dimensions;
  int rc = SALTS_EPROTO;
  *out = NULL;
  if (strlen(text) > EMBED_MAX_INPUT) return SALTS_ENOSPC;
  rc = turbo_cli_retrieval_snippet(text, strlen(text), &snippet);
  if (rc != SALTS_OK) return rc;
  if (!tstr_len(snippet)) { rc = SALTS_EINVAL; goto done; }
  request = json_create_object();
  rc = SALTS_ENOMEM;
  if (!request || turbo_runtime_json_object_set_string(request, "model", vector->model) != 0 ||
      turbo_runtime_json_object_set_string(request, "input", snippet) != 0 ||
      turbo_runtime_json_object_set_string(request, "encoding_format", "float") != 0 ||
      !(body = json_serialize(request, NULL))) goto done;
  rc = embedding_post(vector, body);
  if (rc != SALTS_OK) goto done;
  rc = SALTS_EPROTO;
  if (!vector->response || !vstr_utf8_valid(tstr_to_v(vector->response)) ||
      strlen(vector->response) != tstr_len(vector->response)) goto done;
  response = json_parse(vector->response, tstr_len(vector->response));
  data = json_object_get(response, "data");
  item = json_array_get(data, 0);
  embedding = json_object_get(item, "embedding");
  {
    const json_value_t *model = json_object_get(response, "model");
    if (!json_string(model) || json_string_len(model) != tstr_len(vector->model) ||
        memcmp(json_string(model), vector->model, tstr_len(vector->model))) goto done;
  }
  if (json_type(data) != JSON_ARRAY || json_array_size(data) != 1 ||
      json_type(json_object_get(item, "index")) != JSON_NUMBER ||
      json_number(json_object_get(item, "index")) != 0 || json_type(embedding) != JSON_ARRAY) goto done;
  dimensions = json_array_size(embedding);
  if (!dimensions || dimensions > VECTOR_MAX_DIMENSIONS) goto done;
  for (size_t i = 0; i < dimensions; ++i) {
    const json_value_t *value = json_array_get(embedding, i);
    double number = json_number(value);
    if (json_type(value) != JSON_NUMBER || !isfinite(number)) goto done;
    if (fabs(number) > scale) scale = fabs(number);
  }
  if (scale == 0) goto done;
  for (size_t i = 0; i < dimensions; ++i) {
    double number = json_number(json_array_get(embedding, i)) / scale;
    norm += number * number;
  }
  norm = sqrt(norm);
  *out = json_create_array();
  if (!*out) { rc = SALTS_ENOMEM; goto done; }
  for (size_t i = 0; i < dimensions; ++i) {
    json_value_t *value = json_create_number((json_number(json_array_get(embedding, i)) / scale) / norm);
    if (!value) { rc = SALTS_ENOMEM; goto done; }
    json_array_add(*out, value);
  }
  rc = pin_dimensions(vector, dimensions);
done:
  if (rc != SALTS_OK) { json_free(*out); *out = NULL; }
  json_free(request); json_free(response); json_serialize_free(body); tstr_free(snippet);
  return rc;
}

static int metadata_open(turbo_cli_vector_t *vector, const char *path) {
  sqlite3_int64 application = 0, count = 0, version = 0;
  sqlite3_stmt *statement = NULL;
  int rc = SALTS_EPROTO;
  if (sqlite3_open_v2(path, &vector->metadata, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK)
    return SALTS_EIO;
  if (sqlite3_exec(vector->metadata, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return SALTS_EBUSY;
  if (sqlite_integer(vector->metadata, "PRAGMA application_id", &application) != SALTS_OK ||
      sqlite_integer(vector->metadata, "SELECT count(*) FROM sqlite_schema", &count) != SALTS_OK ||
      sqlite_integer(vector->metadata, "PRAGMA user_version", &version) != SALTS_OK) goto done;
  if (!application && !count && !version) {
    if (sqlite3_exec(vector->metadata,
        "CREATE TABLE cli_vector_config(id INTEGER PRIMARY KEY CHECK(id=1),endpoint TEXT NOT NULL,"
        "model TEXT NOT NULL,dimensions INTEGER NOT NULL CHECK(dimensions BETWEEN 0 AND 4096));"
        "PRAGMA application_id=1413568067; PRAGMA user_version=1", NULL, NULL, NULL) != SQLITE_OK) goto done;
    if (sqlite3_prepare_v2(vector->metadata,
        "INSERT INTO cli_vector_config VALUES(1,?,?,0)", -1, &statement, NULL) != SQLITE_OK) goto done;
    if (sqlite3_bind_text(statement, 1, vector->endpoint, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_text(statement, 2, vector->model, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_DONE) goto done;
  } else {
    if (application != VECTOR_APPLICATION_ID || version != 1) goto done;
    if (sqlite3_prepare_v2(vector->metadata,
        "SELECT endpoint,model,dimensions FROM cli_vector_config WHERE id=1", -1, &statement, NULL) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_ROW) goto done;
    for (int i = 0; i < 2; ++i) {
      const char *expected = i ? vector->model : vector->endpoint;
      const char *actual = (const char *)sqlite3_column_text(statement, i);
      if (!actual || sqlite3_column_type(statement, i) != SQLITE_TEXT ||
          (size_t)sqlite3_column_bytes(statement, i) != strlen(expected) || strcmp(actual, expected)) goto done;
    }
    count = sqlite3_column_int64(statement, 2);
    if (sqlite3_column_type(statement, 2) != SQLITE_INTEGER || count < 0 || count > VECTOR_MAX_DIMENSIONS) goto done;
    vector->dimensions = (size_t)count;
    if (sqlite3_step(statement) != SQLITE_DONE) goto done;
  }
  sqlite3_finalize(statement); statement = NULL;
  if (sqlite3_exec(vector->metadata, "COMMIT", NULL, NULL, NULL) == SQLITE_OK) rc = SALTS_OK;
done:
  sqlite3_finalize(statement);
  if (rc != SALTS_OK) (void)sqlite3_exec(vector->metadata, "ROLLBACK", NULL, NULL, NULL);
  return rc;
}

/* The SDK exact scan owns all candidates. Check its input budget before it can
 * materialize rows; imported replacements reserve worst-case JSON float bytes. */
static int row_budget(turbo_cli_vector_t *vector, const char *replacing_uri,
    size_t extra_rows, size_t extra_bytes) {
  sqlite3_stmt *statement = NULL;
  int rc = SALTS_EPROTO;
  sqlite3_int64 rows, bytes;
  if (sqlite3_prepare_v2(vector->metadata,
      "SELECT count(*),coalesce(sum(length(CAST(id AS BLOB))+length(CAST(uri AS BLOB))+"
      "coalesce(length(CAST(title AS BLOB)),0)+coalesce(length(CAST(kind AS BLOB)),0)+"
      "length(CAST(text AS BLOB))+length(CAST(embedding_json AS BLOB))+"
      "coalesce(length(CAST(metadata_json AS BLOB)),0)),0),"
      "coalesce(sum(dimensions!=?1 OR typeof(dimensions)!='integer' OR typeof(id)!='text' "
      "OR typeof(uri)!='text' OR typeof(text)!='text' OR typeof(embedding_json)!='text'),0) "
      "FROM vector_documents WHERE ?2 IS NULL OR uri IS NULL OR uri!=?2", -1, &statement, NULL) != SQLITE_OK) goto done;
  if (sqlite3_bind_int64(statement, 1, (sqlite3_int64)vector->dimensions) != SQLITE_OK ||
      (replacing_uri && sqlite3_bind_text(statement, 2, replacing_uri, -1, SQLITE_STATIC) != SQLITE_OK) ||
      sqlite3_step(statement) != SQLITE_ROW) goto done;
  rows = sqlite3_column_int64(statement, 0); bytes = sqlite3_column_int64(statement, 1);
  if (rows < 0 || bytes < 0 || sqlite3_column_int64(statement, 2)) goto done;
  rc = rows > VECTOR_MAX_ROWS || extra_rows > VECTOR_MAX_ROWS - (size_t)rows ||
       bytes > VECTOR_MAX_BYTES || extra_bytes > VECTOR_MAX_BYTES - (size_t)bytes ? SALTS_ENOSPC : SALTS_OK;
done:
  sqlite3_finalize(statement);
  return rc;
}

/* CLI imports store normalized vectors. Admit persisted values before the SDK
 * can silently score a wrong shape or overflow on corrupted numeric content.
 * Row/byte bounds have already been checked; retain only one parsed array. */
static int validate_stored_embeddings(turbo_cli_vector_t *vector) {
  static const double norm_tolerance = 1e-6;
  sqlite3_stmt *statement = NULL;
  json_value_t *embedding = NULL;
  int step, rc = SALTS_EPROTO;
  if (sqlite3_prepare_v2(vector->metadata, "SELECT embedding_json FROM vector_documents",
      -1, &statement, NULL) != SQLITE_OK) goto done;
  while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
    const char *text = (const char *)sqlite3_column_text(statement, 0);
    int length = sqlite3_column_bytes(statement, 0);
    double norm = 0;
    rc = stopped(vector);
    if (rc != SALTS_OK) goto done;
    rc = SALTS_EPROTO;
    if (!text || length <= 0 || length > EMBED_MAX_RESPONSE || memchr(text, 0, (size_t)length)) goto done;
    embedding = json_parse(text, (size_t)length);
    if (!vector->dimensions || json_type(embedding) != JSON_ARRAY ||
        json_array_size(embedding) != vector->dimensions) goto done;
    for (size_t i = 0; i < vector->dimensions; ++i) {
      const json_value_t *component = json_array_get(embedding, i);
      double number = json_number(component);
      if (json_type(component) != JSON_NUMBER || !isfinite(number) || fabs(number) > 1.0) goto done;
      norm += number * number;
    }
    if (fabs(norm - 1.0) > norm_tolerance) goto done;
    json_free(embedding); embedding = NULL;
  }
  rc = step == SQLITE_DONE ? SALTS_OK : SALTS_EIO;
done:
  json_free(embedding);
  sqlite3_finalize(statement);
  return rc;
}

static int vector_ingest(void *owner, const char *uri, const char *title, const char *text) {
  turbo_cli_vector_t *vector = owner;
  turbo_vector_store_document_t document = {.id = uri, .uri = uri, .kind = "file", .title = title, .text = text};
  /* The SDK splits bytes; the adapter trims partial UTF-8 boundaries. Three
   * overlapping bytes retain every complete codepoint in at least one chunk
   * and prevent a final chunk containing only continuation bytes. Keep both
   * preflight and indexing on this same configuration. */
  turbo_text_splitter_options_t splitter = {
      .chunk_size = VECTOR_CHUNK_BYTES, .chunk_overlap = VECTOR_CHUNK_OVERLAP};
  json_value_t *chunks = NULL;
  size_t count, budget;
  int rc = stopped(vector);
  if (rc != SALTS_OK) return rc;
  if (turbo_text_splitter_split_text(text, &splitter, &chunks) != 0) return SALTS_ENOMEM;
  count = json_array_size(chunks); json_free(chunks);
  /* All factors are bounded by the scanner and embedding admission. Reserve
   * the duplicated overlap too (one extra allowance for the first chunk). */
  budget = strlen(text) + count * ((vector->dimensions ? vector->dimensions : VECTOR_MAX_DIMENSIONS) * 32u +
      2 * strlen(uri) + strlen(title) + 128 + VECTOR_CHUNK_OVERLAP);
  rc = row_budget(vector, uri, count, budget);
  if (rc != SALTS_OK) return rc;
  rc = turbo_vector_store_index_text(vector->store, vector->embedding, &document, &splitter, NULL) == 0
      ? SALTS_OK : SALTS_EIO;
  if (stopped(vector) != SALTS_OK) return stopped(vector);
  return rc;
}

int turbo_cli_vector_close(turbo_cli_vector_t *vector) {
  int rc;
  if (!vector) return SALTS_OK;
  rc = http_drain(vector);
  /* A failed backend drain cannot revoke the callback's borrow. Retain its
   * owner and report failure, rather than freeing storage still in use. */
  if (rc != SALTS_OK) return rc;
  turbo_embedding_model_destroy(vector->embedding);
  turbo_vector_store_destroy(vector->store);
  if (vector->metadata && sqlite3_close(vector->metadata) != SQLITE_OK) rc = SALTS_EIO;
  if (vector->lock != SALTS_INVALID_FILE && cmeta_fs_close(vector->lock) != 0) rc = SALTS_EIO;
  tstr_free(vector->endpoint); tstr_free(vector->model); tstr_free(vector->connection);
  tstr_free(vector->authority); tstr_free(vector->target); tstr_free(vector->authorization);
  tstr_free(vector->response); free(vector);
  return rc;
}

int turbo_cli_vector_open(const turbo_cli_options_t *options, const char *index_dir,
    const atomic_int *interrupted, turbo_cli_vector_t **out) {
  turbo_cli_vector_t *vector = NULL;
  turbo_embedding_model_config_t embedding_config = {0};
  uri_t uri = {0};
  tstr lock_path = NULL, host = NULL;
  const char *key = getenv("TURBOAGENT_EMBEDDING_API_KEY");
  int rc = turbo_cli_vector_validate(options), port;
  *out = NULL;
  if (rc != SALTS_OK || !options->vector_db) return SALTS_EINVAL;
  if (interrupted && atomic_load_explicit(interrupted, memory_order_relaxed)) return SALTS_ECANCELED;
  if (key) {
    if (strlen(key) > 4096) return SALTS_EINVAL;
    for (const unsigned char *p = (const unsigned char *)key; *p; ++p)
      if (*p <= 32 || *p >= 127) return SALTS_EINVAL;
  }
  vector = calloc(1, sizeof(*vector));
  if (!vector) return SALTS_ENOMEM;
  vector->lock = SALTS_INVALID_FILE; vector->interrupted = interrupted;
  vector->operation_deadline = cmeta_monotonic_ms() + 600000;
  (void)endpoint_parse(options->embedding_url, &uri);
  port = uri.component_flags & URI_COMPONENT_PORT ? uri.port : (!strcmp(uri.scheme, "https") ? 443 : 80);
  host = uri.host_type == URI_HOST_IPV6ADDR ? tstr_format("[{}]", uri.host) : tstr_dup(uri.host);
  rc = SALTS_ENOMEM;
  if (!host) goto done;
  vector->endpoint = tstr_dup(options->embedding_url); vector->model = tstr_dup(options->embedding_model);
  vector->connection = tstr_format("{}://{}:{}", !strcmp(uri.scheme, "https") ? "tls" : "tcp", host, port);
  vector->authority = tstr_format("{}:{}", host, port); vector->target = tstr_dup(uri.path);
  if (key && key[0]) vector->authorization = tstr_format("Bearer {}", key);
  lock_path = tstr_format("{}.lock", options->vector_db);
  if (!vector->endpoint || !vector->model || !vector->connection || !vector->authority ||
      !vector->target || !lock_path || (key && key[0] && !vector->authorization)) goto done;
  vector->lock = cmeta_fs_open(lock_path, SALTS_FS_O_RDWR | SALTS_FS_O_CREAT, 0600);
  rc = SALTS_EBUSY;
  if (vector->lock == SALTS_INVALID_FILE ||
      cmeta_fs_lock(vector->lock, SALTS_FS_LOCK_EXCLUSIVE | SALTS_FS_LOCK_NONBLOCK, 0, 1) != 0) goto done;
  rc = metadata_open(vector, options->vector_db);
  if (rc != SALTS_OK) goto done;
  vector->store = turbo_vector_store_sqlite_open(options->vector_db);
  rc = SALTS_EIO;
  if (!vector->store) goto done;
  embedding_config.embed_text = embed_text; embedding_config.user_data = vector;
  vector->embedding = turbo_embedding_model_create(&embedding_config);
  if (!vector->embedding) { rc = SALTS_ENOMEM; goto done; }
  rc = row_budget(vector, NULL, 0, 0);
  if (rc == SALTS_OK) rc = validate_stored_embeddings(vector);
  if (rc == SALTS_OK && index_dir) rc = turbo_cli_index_directory(index_dir, interrupted, vector_ingest, vector);
done:
  tstr_free(host); tstr_free(lock_path);
  if (rc == SALTS_OK) *out = vector;
  else if (turbo_cli_vector_close(vector) != SALTS_OK) rc = SALTS_EIO;
  return rc;
}

int turbo_cli_vector_prepare(turbo_cli_vector_t *vector, const char *query, json_value_t *state) {
  turbo_vector_store_query_options_t options = {.limit = 8};
  json_value_t *embedding = NULL, *results = NULL;
  int rc;
  if (!vector) return SALTS_OK;
  vector->operation_deadline = cmeta_monotonic_ms() + EMBED_TIMEOUT_MS;
  rc = stopped(vector);
  if (rc == SALTS_OK) rc = row_budget(vector, NULL, 0, 0);
  if (rc == SALTS_OK) rc = embed_text(vector, query, &embedding);
  if (rc == SALTS_OK)
    rc = turbo_vector_store_query(vector->store, embedding, &options, &results) == 0 ? SALTS_OK : SALTS_EIO;
  if (rc == SALTS_OK) rc = stopped(vector);
  if (rc == SALTS_OK) rc = turbo_cli_retrieval_context(results, state);
  json_free(embedding); json_free(results);
  return rc;
}
