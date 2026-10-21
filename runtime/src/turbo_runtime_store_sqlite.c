#include "turbo_runtime_store_sqlite.h"

#include <sqlite3.h>
#include <fmt.h>
#include <tstr.h>
#include <vstr.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum { SQLITE_STORE_APPLICATION_ID = 0x54415254, SQLITE_STORE_VERSION = 1,
       SQLITE_STORE_MAX_KEY_BYTES = 1024 };

typedef struct sqlite_store_s {
  sqlite3 *db;
  size_t max_record_bytes, max_list_bytes, max_list_records;
} sqlite_store_t;

static int sqlite_store_status(int rc) {
  switch (rc & 0xff) {
    case SQLITE_OK: case SQLITE_DONE: return SALTS_OK;
    case SQLITE_NOMEM: return SALTS_ENOMEM;
    case SQLITE_BUSY: case SQLITE_LOCKED: return SALTS_EBUSY;
    case SQLITE_FULL: case SQLITE_TOOBIG: return SALTS_ENOSPC;
    case SQLITE_CORRUPT: case SQLITE_NOTADB: case SQLITE_SCHEMA: return SALTS_EPROTO;
    default: return SALTS_EIO;
  }
}

static int sqlite_store_exec(sqlite3 *db, const char *sql) {
  return sqlite_store_status(sqlite3_exec(db, sql, NULL, NULL, NULL));
}

static int sqlite_store_integer(sqlite3 *db, const char *sql, sqlite3_int64 *out) {
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW && sqlite3_column_type(stmt, 0) == SQLITE_INTEGER) {
      *out = sqlite3_column_int64(stmt, 0);
      rc = sqlite3_step(stmt);
      if (rc == SQLITE_ROW) rc = SQLITE_ERROR;
    } else if (rc == SQLITE_ROW || rc == SQLITE_DONE) rc = SQLITE_ERROR;
  }
  sqlite3_finalize(stmt);
  return sqlite_store_status(rc);
}

static int sqlite_store_key(const char *key) {
  return key && key[0] && strlen(key) <= SQLITE_STORE_MAX_KEY_BYTES &&
         vstr_utf8_valid(vstr_from_cstr(key));
}

static int sqlite_store_schema(sqlite3 *db) {
  static const char *const names[] = {"collection", "id", "payload"};
  static const int primary_keys[] = {1, 2, 0};
  sqlite3_stmt *stmt = NULL;
  size_t column = 0;
  int rc = sqlite3_prepare_v2(db, "PRAGMA table_info(runtime_records)", -1, &stmt, NULL);
  int status = sqlite_store_status(rc);
  if (rc != SQLITE_OK) return status;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    const char *type = (const char *)sqlite3_column_text(stmt, 2);
    if (column >= 3 || !name || !type || strcmp(name, names[column]) || strcmp(type, "TEXT") ||
        sqlite3_column_int(stmt, 3) != 1 || sqlite3_column_type(stmt, 4) != SQLITE_NULL ||
        sqlite3_column_int(stmt, 5) != primary_keys[column]) {
      status = SALTS_EPROTO; goto done;
    }
    ++column;
  }
  status = rc == SQLITE_DONE && column != 3 ? SALTS_EPROTO : sqlite_store_status(rc);
done:
  sqlite3_finalize(stmt);
  return status;
}

/* The legacy callback ABI returns malloc-owned strings, not SQLite/JSON buffers. */
static char *sqlite_store_copy(const char *text, size_t bytes) {
  char *copy = malloc(bytes + 1);
  if (copy) { memcpy(copy, text, bytes); copy[bytes] = 0; }
  return copy;
}

static int sqlite_store_put(void *data, const char *collection, const char *id,
                            const char *text) {
  sqlite_store_t *store = data;
  sqlite3_stmt *stmt = NULL;
  json_value_t *record;
  size_t bytes;
  int rc;
  if (!sqlite_store_key(collection) || !sqlite_store_key(id) || !text) return SALTS_EINVAL;
  bytes = strlen(text);
  if (bytes > store->max_record_bytes) return SALTS_ENOSPC;
  if (!vstr_utf8_valid(vstr_from_cstr(text))) return SALTS_EPROTO;
  record = json_parse(text, bytes);
  if (!record) return SALTS_EPROTO;
  json_free(record);
  rc = sqlite3_prepare_v2(store->db,
      "INSERT INTO runtime_records(collection,id,payload) VALUES(?1,?2,?3) "
      "ON CONFLICT(collection,id) DO UPDATE SET payload=excluded.payload", -1, &stmt, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, collection, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, text, (int)bytes, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
  {
    int finish = sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) rc = finish;
  }
  return sqlite_store_status(rc);
}

static int sqlite_store_create_if_absent(void *data, const char *collection, const char *id,
                                         const char *text) {
  sqlite_store_t *store = data;
  sqlite3_stmt *stmt = NULL;
  json_value_t *record;
  size_t bytes;
  int rc;
  int changed;
  if (!store || !sqlite_store_key(collection) || !sqlite_store_key(id) || !text)
    return SALTS_EINVAL;
  bytes = strlen(text);
  if (bytes > store->max_record_bytes) return SALTS_ENOSPC;
  if (!vstr_utf8_valid(vstr_from_cstr(text))) return SALTS_EPROTO;
  record = json_parse(text, bytes);
  if (!record) return SALTS_EPROTO;
  json_free(record);
  rc = sqlite3_prepare_v2(store->db,
      "INSERT INTO runtime_records(collection,id,payload) VALUES(?1,?2,?3) "
      "ON CONFLICT(collection,id) DO NOTHING", -1, &stmt, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, collection, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, text, (int)bytes, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    int finish = sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) rc = finish;
    return sqlite_store_status(rc);
  }
  rc = sqlite3_finalize(stmt);
  if (rc != SQLITE_OK) return sqlite_store_status(rc);
  changed = sqlite3_changes(store->db);
  if (changed == 1) return TURBO_AGENT_RUNTIME_STORE_CREATE_CREATED;
  if (changed == 0) return TURBO_AGENT_RUNTIME_STORE_CREATE_EXISTS;
  return SALTS_EPROTO;
}

static int sqlite_store_row(sqlite_store_t *store, sqlite3_stmt *stmt,
                            json_value_t **out, const char **text, size_t *bytes) {
  *out = NULL;
  if (sqlite3_column_type(stmt, 0) != SQLITE_TEXT) return SALTS_EPROTO;
  *text = (const char *)sqlite3_column_text(stmt, 0);
  *bytes = (size_t)sqlite3_column_bytes(stmt, 0);
  if (!*text) return SALTS_ENOMEM;
  if (*bytes > store->max_record_bytes) return SALTS_ENOSPC;
  if (memchr(*text, 0, *bytes)) return SALTS_EPROTO;
  if (!vstr_utf8_valid(vstr_from_cstr(*text))) return SALTS_EPROTO;
  *out = json_parse(*text, *bytes);
  return *out ? SALTS_OK : SALTS_EPROTO;
}

static int sqlite_store_get(void *data, const char *collection, const char *id, char **out) {
  sqlite_store_t *store = data;
  sqlite3_stmt *stmt = NULL;
  json_value_t *record = NULL;
  const char *text;
  size_t bytes;
  int rc, status;
  if (out) *out = NULL;
  if (!out || !sqlite_store_key(collection) || !sqlite_store_key(id)) return SALTS_EINVAL;
  rc = sqlite3_prepare_v2(store->db,
      "SELECT payload FROM runtime_records WHERE collection=?1 AND id=?2", -1, &stmt, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, collection, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    status = sqlite_store_row(store, stmt, &record, &text, &bytes);
    if (status == SALTS_OK) {
      *out = sqlite_store_copy(text, bytes);
      if (!*out) status = SALTS_ENOMEM;
    }
  } else status = rc == SQLITE_DONE ? SALTS_ENOENT : sqlite_store_status(rc);
  json_free(record);
  sqlite3_finalize(stmt);
  return status;
}

static int sqlite_store_list(void *data, const char *collection, const char *key,
                             const char *value, char **out) {
  sqlite_store_t *store = data;
  sqlite3_stmt *stmt = NULL;
  json_value_t *records = NULL, *record = NULL;
  size_t scanned = 0, included = 0, bytes, total = 2, serialized_bytes;
  const char *text;
  char *serialized = NULL;
  int rc, status;
  if (out) *out = NULL;
  if (!out || !sqlite_store_key(collection)) return SALTS_EINVAL;
  records = json_create_array();
  if (!records) return SALTS_ENOMEM;
  rc = sqlite3_prepare_v2(store->db,
      "SELECT payload FROM runtime_records WHERE collection=?1 ORDER BY id", -1, &stmt, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, collection, -1, SQLITE_STATIC);
  status = sqlite_store_status(rc);
  if (rc != SQLITE_OK) goto done;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const json_value_t *member;
    if (scanned++ == store->max_list_records) { status = SALTS_ENOSPC; goto done; }
    status = sqlite_store_row(store, stmt, &record, &text, &bytes);
    if (status != SALTS_OK) goto done;
    member = key && value ? json_object_get(record, key) : NULL;
    if (key && value && (json_type(member) != JSON_STRING ||
        json_string_len(member) != strlen(value) || strcmp(json_get_string(record, key), value))) {
      json_free(record); record = NULL; continue;
    }
    if (bytes + (included != 0) > store->max_list_bytes - total) {
      status = SALTS_ENOSPC; goto done;
    }
    total += bytes + (included++ != 0);
    if (turbo_runtime_json_array_append(records, record) != TURBO_RUNTIME_JSON_OK) {
      status = SALTS_ENOMEM; goto done;
    }
    record = NULL;
  }
  status = sqlite_store_status(rc);
  if (status != SALTS_OK) goto done;
  serialized = json_serialize(records, &serialized_bytes);
  if (!serialized) { status = SALTS_ENOMEM; goto done; }
  if (serialized_bytes > store->max_list_bytes) { status = SALTS_ENOSPC; goto done; }
  *out = sqlite_store_copy(serialized, serialized_bytes);
  if (!*out) status = SALTS_ENOMEM;
done:
  json_serialize_free(serialized);
  json_free(record); json_free(records);
  sqlite3_finalize(stmt);
  return status;
}

static void sqlite_store_destroy(void *data) {
  sqlite_store_t *store = data;
  if (store) { sqlite3_close(store->db); free(store); }
}

void turbo_runtime_sqlite_config_init(turbo_runtime_sqlite_config_t *config) {
  if (!config) return;
  *config = (turbo_runtime_sqlite_config_t){
      sizeof(*config), TURBO_RUNTIME_SQLITE_ABI_VERSION, NULL,
      16 * 1024 * 1024, 64 * 1024 * 1024, 4096, UINT64_C(256) * 1024 * 1024, 250, 1};
}

int turbo_runtime_store_sqlite_open(const turbo_runtime_sqlite_config_t *config,
                                   turbo_agent_runtime_store_t *out_store) {
  sqlite_store_t *store;
  sqlite3_stmt *stmt = NULL;
  sqlite3_int64 application, version, count, page_size, page_count, actual_max;
  tstr sql = NULL;
  int rc, status, transaction = 0;
  if (out_store) *out_store = (turbo_agent_runtime_store_t){0};
  if (!config || !out_store || config->struct_size != sizeof(*config) ||
      config->abi_version != TURBO_RUNTIME_SQLITE_ABI_VERSION || !config->path ||
      !config->path[0] || !strcmp(config->path, ":memory:") ||
      !vstr_utf8_valid(vstr_from_cstr(config->path)) ||
      !strncmp(config->path, "file:", 5) || !config->max_record_bytes ||
      config->max_record_bytes > INT_MAX - 4096 || config->max_list_bytes < 2 ||
      config->max_list_bytes > INT_MAX || !config->max_list_records ||
      config->max_list_records > INT_MAX || config->busy_timeout_ms > INT_MAX ||
      config->max_database_bytes < 65536 || config->max_database_bytes > INT64_MAX ||
      (config->create_if_missing != 0 && config->create_if_missing != 1)) return SALTS_EINVAL;
  store = calloc(1, sizeof(*store));
  if (!store) return SALTS_ENOMEM;
  rc = sqlite3_open_v2(config->path, &store->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX |
      SQLITE_OPEN_PRIVATECACHE | (config->create_if_missing ? SQLITE_OPEN_CREATE : 0), NULL);
  status = sqlite_store_status(rc);
  if (rc != SQLITE_OK) goto fail;
  store->max_record_bytes = config->max_record_bytes;
  store->max_list_bytes = config->max_list_bytes;
  store->max_list_records = config->max_list_records;
  sqlite3_limit(store->db, SQLITE_LIMIT_LENGTH, (int)config->max_record_bytes + 4096);
  status = sqlite_store_status(sqlite3_busy_timeout(store->db, (int)config->busy_timeout_ms));
  if (status != SALTS_OK) goto fail;
  rc = sqlite3_prepare_v2(store->db, "PRAGMA journal_mode", -1, &stmt, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) { status = sqlite_store_status(rc); if (!status) status = SALTS_EPROTO; goto fail; }
  if (!sqlite3_column_text(stmt, 0) || strcmp((const char *)sqlite3_column_text(stmt, 0), "delete")) {
    status = SALTS_ENOTSUP; goto fail;
  }
  sqlite3_finalize(stmt); stmt = NULL;
  status = sqlite_store_exec(store->db,
      "PRAGMA trusted_schema=OFF; PRAGMA synchronous=FULL; PRAGMA locking_mode=EXCLUSIVE; BEGIN EXCLUSIVE");
  if (status != SALTS_OK) goto fail;
  transaction = 1;
  if ((status = sqlite_store_integer(store->db, "PRAGMA application_id", &application)) != SALTS_OK ||
      (status = sqlite_store_integer(store->db, "PRAGMA user_version", &version)) != SALTS_OK ||
      (status = sqlite_store_integer(store->db, "SELECT count(*) FROM sqlite_schema", &count)) != SALTS_OK)
    goto fail;
  if (!application && !version && !count && config->create_if_missing) {
    status = sqlite_store_exec(store->db,
        "CREATE TABLE runtime_records(collection TEXT NOT NULL,id TEXT NOT NULL,"
        "payload TEXT NOT NULL,PRIMARY KEY(collection,id)) WITHOUT ROWID;"
        "PRAGMA application_id=1413567060; PRAGMA user_version=1");
    if (status != SALTS_OK) goto fail;
  } else if (application != SQLITE_STORE_APPLICATION_ID || version != SQLITE_STORE_VERSION) {
    status = SALTS_ENOTSUP; goto fail;
  } else if (count != 1) {
    status = SALTS_EPROTO; goto fail;
  }
  status = sqlite_store_schema(store->db);
  if (status != SALTS_OK) goto fail;
  if ((status = sqlite_store_integer(store->db, "PRAGMA page_size", &page_size)) != SALTS_OK ||
      (status = sqlite_store_integer(store->db, "PRAGMA page_count", &page_count)) != SALTS_OK) goto fail;
  if (page_size <= 0 || page_count < 0 || (uint64_t)page_count > config->max_database_bytes / (uint64_t)page_size) {
    status = SALTS_ENOSPC; goto fail;
  }
  sql = tstr_format("PRAGMA max_page_count={}", config->max_database_bytes / (uint64_t)page_size);
  if (!sql) { status = SALTS_ENOMEM; goto fail; }
  status = sqlite_store_integer(store->db, sql, &actual_max);
  if (status != SALTS_OK) goto fail;
  if (actual_max <= 0 || (uint64_t)actual_max > config->max_database_bytes / (uint64_t)page_size) {
    status = SALTS_ENOSPC; goto fail;
  }
  status = sqlite_store_exec(store->db, "COMMIT");
  if (status != SALTS_OK) goto fail;
  tstr_free(sql);
  *out_store = (turbo_agent_runtime_store_t){sqlite_store_put, sqlite_store_get, sqlite_store_list,
                                           store, sqlite_store_destroy};
  return SALTS_OK;
fail:
  sqlite3_finalize(stmt);
  if (transaction) (void)sqlite_store_exec(store->db, "ROLLBACK");
  tstr_free(sql);
  sqlite_store_destroy(store);
  return status;
}

int turbo_runtime_store_sqlite_open_ex(const turbo_runtime_sqlite_config_t *config,
                                      turbo_agent_runtime_store_ex_t *out_store) {
  turbo_agent_runtime_store_t legacy = {0};
  int status;
  if (!out_store) return SALTS_EINVAL;
  turbo_agent_runtime_store_ex_init(out_store);
  status = turbo_runtime_store_sqlite_open(config, &legacy);
  if (status != SALTS_OK) return status;
  out_store->store = legacy;
  out_store->create_if_absent = sqlite_store_create_if_absent;
  return SALTS_OK;
}
