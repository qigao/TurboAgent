#include <stdint.h>

#include "turbo_wasm3_guest_abi.h"

TURBONET_IMPORT("db_open")
extern int32_t turbonet_db_open(const char *target, uint32_t target_len,
                                uint32_t *out_handle);

TURBONET_IMPORT("db_close")
extern int32_t turbonet_db_close(uint32_t handle);

TURBONET_IMPORT("db_exec")
extern int32_t turbonet_db_exec(uint32_t handle, const char *sql, uint32_t sql_len,
                                uint64_t *out_changes);

TURBONET_IMPORT("db_stmt_error")
extern int32_t turbonet_db_stmt_error(uint32_t stmt_handle, char *buffer,
                                      uint32_t buffer_size,
                                      uint32_t *out_written);

TURBONET_IMPORT("db_prepare")
extern int32_t turbonet_db_prepare(uint32_t db_handle, const char *sql,
                                   uint32_t sql_len, uint32_t *out_stmt);

TURBONET_IMPORT("db_bind_i64")
extern int32_t turbonet_db_bind_i64(uint32_t stmt_handle, uint32_t index,
                                    int64_t value);

TURBONET_IMPORT("db_bind_f64")
extern int32_t turbonet_db_bind_f64(uint32_t stmt_handle, uint32_t index,
                                    double value);

TURBONET_IMPORT("db_bind_null")
extern int32_t turbonet_db_bind_null(uint32_t stmt_handle, uint32_t index);

TURBONET_IMPORT("db_bind_blob")
extern int32_t turbonet_db_bind_blob(uint32_t stmt_handle, uint32_t index,
                                     const void *blob, uint32_t blob_len);

TURBONET_IMPORT("db_bind_text")
extern int32_t turbonet_db_bind_text(uint32_t stmt_handle, uint32_t index,
                                     const char *text, uint32_t text_len);

TURBONET_IMPORT("db_step")
extern int32_t turbonet_db_step(uint32_t stmt_handle, int32_t *out_state);

TURBONET_IMPORT("db_column_type")
extern int32_t turbonet_db_column_type(uint32_t stmt_handle, uint32_t index,
                                       int32_t *out_type);

TURBONET_IMPORT("db_column_i64")
extern int32_t turbonet_db_column_i64(uint32_t stmt_handle, uint32_t index,
                                      int64_t *out_value);

TURBONET_IMPORT("db_column_f64")
extern int32_t turbonet_db_column_f64(uint32_t stmt_handle, uint32_t index,
                                      double *out_value);

TURBONET_IMPORT("db_column_blob")
extern int32_t turbonet_db_column_blob(uint32_t stmt_handle, uint32_t index,
                                       void *buffer, uint32_t buffer_size,
                                       uint32_t *out_written);

TURBONET_IMPORT("db_column_text")
extern int32_t turbonet_db_column_text(uint32_t stmt_handle, uint32_t index,
                                       char *buffer, uint32_t buffer_size,
                                       uint32_t *out_written);

TURBONET_IMPORT("db_reset")
extern int32_t turbonet_db_reset(uint32_t stmt_handle);

TURBONET_IMPORT("db_finalize")
extern int32_t turbonet_db_finalize(uint32_t stmt_handle);

enum {
  TURBO_WASM3_DB_STEP_DONE = 0,
  TURBO_WASM3_DB_STEP_ROW = 1,
  TURBO_WASM3_DB_TYPE_NULL = 0,
  TURBO_WASM3_DB_TYPE_INT64 = 1,
  TURBO_WASM3_DB_TYPE_DOUBLE = 2,
  TURBO_WASM3_DB_TYPE_TEXT = 3,
  TURBO_WASM3_DB_TYPE_BLOB = 4,
};

static uint32_t cstrlen(const char *s) {
  uint32_t len = 0;
  while (s[len] != '\0') {
    ++len;
  }
  return len;
}

static int cstreq(const char *a, const char *b) {
  uint32_t i = 0;
  while (a[i] != '\0' && b[i] != '\0') {
    if (a[i] != b[i]) {
      return 0;
    }
    ++i;
  }
  return a[i] == b[i];
}

static int cbytes_eq(const uint8_t *a, const uint8_t *b, uint32_t len) {
  uint32_t i;

  for (i = 0; i < len; ++i) {
    if (a[i] != b[i]) {
      return 0;
    }
  }
  return 1;
}

TURBONET_EXPORT("run_db_demo")
int32_t run_db_demo(void) {
  static const char target[] = ":memory:";
  static const char create_sql[] =
      "create table items(id integer primary key, name text, score real, payload blob, note text);";
  static const char insert_sql[] =
      "insert into items(id, name, score, payload, note) values(?, ?, ?, ?, ?);";
  static const char select_sql[] =
      "select id, name, score, payload, note from items where id = ?;";
  static const char expected[] = "academy";
  static const uint8_t payload[] = {0x41, 0x42, 0x00, 0x7f};
  uint32_t db = 0;
  uint32_t stmt = 0;
  uint64_t changes = 0;
  int32_t step_state = TURBO_WASM3_DB_STEP_DONE;
  int32_t column_type = TURBO_WASM3_DB_TYPE_NULL;
  int64_t id_value = 0;
  double score_value = 0.0;
  uint32_t text_len = 0;
  uint32_t blob_len = 0;
  uint32_t err_len = 0;
  char name_buf[32];
  uint8_t payload_buf[8];
  char err_buf[96];
  int32_t rc = 0;

  rc = turbonet_db_open(target, cstrlen(target), &db);
  if (rc != 0) {
    return 10 + rc;
  }

  rc = turbonet_db_exec(db, create_sql, cstrlen(create_sql), &changes);
  if (rc != 0) {
    return 20 + rc;
  }

  rc = turbonet_db_prepare(db, insert_sql, cstrlen(insert_sql), &stmt);
  if (rc != 0) {
    return 30 + rc;
  }

  rc = turbonet_db_bind_i64(stmt, 1, 7);
  if (rc != 0) {
    return 40 + rc;
  }

  rc = turbonet_db_bind_text(stmt, 2, expected, cstrlen(expected));
  if (rc != 0) {
    return 50 + rc;
  }

  rc = turbonet_db_bind_f64(stmt, 3, 4.25);
  if (rc != 0) {
    return 60 + rc;
  }

  rc = turbonet_db_bind_blob(stmt, 4, payload, sizeof(payload));
  if (rc != 0) {
    return 70 + rc;
  }

  rc = turbonet_db_bind_null(stmt, 5);
  if (rc != 0) {
    return 80 + rc;
  }

  rc = turbonet_db_step(stmt, &step_state);
  if (rc != 0 || step_state != TURBO_WASM3_DB_STEP_DONE) {
    return 90 + rc;
  }

  rc = turbonet_db_finalize(stmt);
  if (rc != 0) {
    return 100 + rc;
  }
  stmt = 0;

  rc = turbonet_db_prepare(db, select_sql, cstrlen(select_sql), &stmt);
  if (rc != 0) {
    return 110 + rc;
  }

  rc = turbonet_db_bind_i64(stmt, 1, 7);
  if (rc != 0) {
    return 120 + rc;
  }

  rc = turbonet_db_step(stmt, &step_state);
  if (rc != 0 || step_state != TURBO_WASM3_DB_STEP_ROW) {
    return 130 + rc;
  }

  rc = turbonet_db_column_i64(stmt, 0, &id_value);
  if (rc != 0 || id_value != 7) {
    return 140 + rc;
  }

  rc = turbonet_db_column_text(stmt, 1, name_buf, sizeof(name_buf), &text_len);
  if (rc != 0 || text_len != cstrlen(expected) || !cstreq(name_buf, expected)) {
    return 150 + rc;
  }

  rc = turbonet_db_column_type(stmt, 2, &column_type);
  if (rc != 0 || column_type != TURBO_WASM3_DB_TYPE_DOUBLE) {
    return 160 + rc;
  }

  rc = turbonet_db_column_f64(stmt, 2, &score_value);
  if (rc != 0 || score_value < 4.24 || score_value > 4.26) {
    return 170 + rc;
  }

  rc = turbonet_db_column_type(stmt, 3, &column_type);
  if (rc != 0 || column_type != TURBO_WASM3_DB_TYPE_BLOB) {
    return 180 + rc;
  }

  rc = turbonet_db_column_blob(stmt, 3, payload_buf, sizeof(payload_buf), &blob_len);
  if (rc != 0 || blob_len != sizeof(payload) ||
      !cbytes_eq(payload_buf, payload, (uint32_t)sizeof(payload))) {
    return 190 + rc;
  }

  rc = turbonet_db_column_type(stmt, 4, &column_type);
  if (rc != 0 || column_type != TURBO_WASM3_DB_TYPE_NULL) {
    return 200 + rc;
  }

  rc = turbonet_db_reset(stmt);
  if (rc != 0) {
    return 210 + rc;
  }

  rc = turbonet_db_bind_i64(stmt, 1, 7);
  if (rc != 0) {
    return 220 + rc;
  }

  rc = turbonet_db_step(stmt, &step_state);
  if (rc != 0 || step_state != TURBO_WASM3_DB_STEP_ROW) {
    return 230 + rc;
  }

  rc = turbonet_db_finalize(stmt);
  if (rc != 0) {
    return 240 + rc;
  }
  stmt = 0;

  rc = turbonet_db_prepare(db, insert_sql, cstrlen(insert_sql), &stmt);
  if (rc != 0) {
    return 250 + rc;
  }

  rc = turbonet_db_bind_i64(stmt, 6, 7);
  if (rc == 0) {
    return 260;
  }

  rc = turbonet_db_stmt_error(stmt, err_buf, sizeof(err_buf), &err_len);
  if (rc != 0 || err_len == 0 || err_buf[0] == '\0') {
    return 270 + rc;
  }

  rc = turbonet_db_finalize(stmt);
  if (rc != 0) {
    return 280 + rc;
  }

  rc = turbonet_db_close(db);
  if (rc != 0) {
    return 290 + rc;
  }

  return 7;
}
