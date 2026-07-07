#ifndef TURBO_WASM3_CORE_H
#define TURBO_WASM3_CORE_H

#include "platform.h"
#include "m3_api_wasi.h"
#include "wasm3.h"
#include "turbo_parser.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct coro_socket_s;
struct m3_wasi_context_t;

typedef struct turbo_wasm3_socket_registry_s turbo_wasm3_socket_registry_t;
typedef struct turbo_wasm3_db_registry_s turbo_wasm3_db_registry_t;
typedef struct turbo_wasm3_http_registry_s turbo_wasm3_http_registry_t;
typedef struct turbo_wasm3_redis_registry_s turbo_wasm3_redis_registry_t;
typedef struct turbo_wasm3_parser_registry_s turbo_wasm3_parser_registry_t;
typedef struct turbo_wasm3_vm_s turbo_wasm3_vm_t;
typedef M3Result (*turbo_wasm3_host_linker_fn)(turbo_wasm3_vm_t *vm,
                                               IM3Module module,
                                               void *user_data);

typedef struct turbo_wasm3_db_ops_s {
  int (*open)(void *user_data, const char *target, void **out_db);
  int (*close)(void *user_data, void *db);
  int (*exec)(void *user_data, void *db, const char *sql, uint64_t *changes);
  int (*error)(void *user_data, void *db, char *buffer, size_t buffer_size,
               uint32_t *out_len);
  int (*prepare)(void *user_data, void *db, const char *sql, void **out_stmt);
  int (*finalize)(void *user_data, void *stmt);
  int (*reset)(void *user_data, void *stmt);
  int (*step)(void *user_data, void *stmt, int32_t *out_state);
  int (*bind_int64)(void *user_data, void *stmt, uint32_t index, int64_t value);
  int (*bind_double)(void *user_data, void *stmt, uint32_t index, double value);
  int (*bind_null)(void *user_data, void *stmt, uint32_t index);
  int (*bind_blob)(void *user_data, void *stmt, uint32_t index, const void *value,
                   size_t value_len);
  int (*bind_text)(void *user_data, void *stmt, uint32_t index, const char *value,
                   size_t value_len);
  int (*column_type)(void *user_data, void *stmt, uint32_t index, int32_t *out_type);
  int (*column_int64)(void *user_data, void *stmt, uint32_t index,
                      int64_t *out_value);
  int (*column_double)(void *user_data, void *stmt, uint32_t index,
                       double *out_value);
  int (*column_blob)(void *user_data, void *stmt, uint32_t index, void *buffer,
                     size_t buffer_size, uint32_t *out_len);
  int (*column_text)(void *user_data, void *stmt, uint32_t index, char *buffer,
                     size_t buffer_size, uint32_t *out_len);
} turbo_wasm3_db_ops_t;

#define TURBO_WASM3_HOST_ABI_VERSION 2u
#define TURBO_WASM3_DB_STEP_DONE 0
#define TURBO_WASM3_DB_STEP_ROW 1
#define TURBO_WASM3_DB_TYPE_NULL 0
#define TURBO_WASM3_DB_TYPE_INT64 1
#define TURBO_WASM3_DB_TYPE_DOUBLE 2
#define TURBO_WASM3_DB_TYPE_TEXT 3
#define TURBO_WASM3_DB_TYPE_BLOB 4
#define TURBO_WASM3_HTTP_METHOD_DELETE 0
#define TURBO_WASM3_HTTP_METHOD_GET 1
#define TURBO_WASM3_HTTP_METHOD_HEAD 2
#define TURBO_WASM3_HTTP_METHOD_POST 3
#define TURBO_WASM3_HTTP_METHOD_PUT 4
#define TURBO_WASM3_HTTP_METHOD_OPTIONS 6
#define TURBO_WASM3_HTTP_METHOD_PATCH 28
#define TURBO_WASM3_REDIS_REPLY_STRING 0
#define TURBO_WASM3_REDIS_REPLY_ERROR 1
#define TURBO_WASM3_REDIS_REPLY_INTEGER 2
#define TURBO_WASM3_REDIS_REPLY_BULK_STRING 3
#define TURBO_WASM3_REDIS_REPLY_ARRAY 4
#define TURBO_WASM3_REDIS_REPLY_NULL 5

/*
 * Create a TurboNet-managed wasm3 VM.
 * The VM owns its environment, runtime, copied wasm bytes, and socket registry.
 */
CXX_C_API turbo_wasm3_vm_t *
turbo_wasm3_vm_create(uint32_t stack_size, void *runtime_user_data,
                      size_t socket_capacity);

CXX_C_API void
turbo_wasm3_vm_destroy(turbo_wasm3_vm_t *vm);

CXX_C_API IM3Runtime
turbo_wasm3_vm_get_runtime(turbo_wasm3_vm_t *vm);

CXX_C_API m3_wasi_context_t *
turbo_wasm3_vm_get_wasi_context(turbo_wasm3_vm_t *vm);

CXX_C_API turbo_wasm3_db_registry_t *
turbo_wasm3_vm_get_db_registry(turbo_wasm3_vm_t *vm);

CXX_C_API turbo_wasm3_http_registry_t *
turbo_wasm3_vm_get_http_registry(turbo_wasm3_vm_t *vm);

CXX_C_API turbo_wasm3_redis_registry_t *
turbo_wasm3_vm_get_redis_registry(turbo_wasm3_vm_t *vm);

CXX_C_API turbo_wasm3_parser_registry_t *
turbo_wasm3_vm_get_parser_registry(turbo_wasm3_vm_t *vm);

CXX_C_API int
turbo_wasm3_vm_set_wasi_args(turbo_wasm3_vm_t *vm, uint32_t argc,
                             const char *const *argv);

CXX_C_API void
turbo_wasm3_vm_reset_preopens(turbo_wasm3_vm_t *vm);

CXX_C_API int
turbo_wasm3_vm_set_preopen(turbo_wasm3_vm_t *vm, uint32_t fd,
                           const char *guest_path, const char *host_path);

CXX_C_API int
turbo_wasm3_vm_remove_preopen(turbo_wasm3_vm_t *vm, uint32_t fd);

CXX_C_API M3Result
turbo_wasm3_vm_load_module(turbo_wasm3_vm_t *vm, const uint8_t *wasm_bytes,
                           uint32_t wasm_size, const char *module_name,
                           IM3Module *out_module);

CXX_C_API M3Result
turbo_wasm3_vm_load_module_file(turbo_wasm3_vm_t *vm, const char *path,
                                const char *module_name, IM3Module *out_module);

/*
 * Host linkers let TurboNet bind stable host imports without coupling guests to
 * backend libraries directly. Add them before loading a module.
 */
CXX_C_API void
turbo_wasm3_vm_set_host_user_data(turbo_wasm3_vm_t *vm, void *host_user_data);

CXX_C_API void *
turbo_wasm3_vm_get_host_user_data(turbo_wasm3_vm_t *vm);

CXX_C_API int
turbo_wasm3_vm_add_host_linker(turbo_wasm3_vm_t *vm,
                               turbo_wasm3_host_linker_fn linker,
                               void *user_data);

CXX_C_API void
turbo_wasm3_vm_clear_host_linkers(turbo_wasm3_vm_t *vm);

CXX_C_API M3Result
turbo_wasm3_vm_link_host_modules(turbo_wasm3_vm_t *vm, IM3Module module);

/*
 * Built-in TurboNet host module:
 *   import "TurboNet" "abi_version"    : i32 () -> ABI version
 *   import "TurboNet" "clock_time_ms"  : i64 () -> monotonic-ish host time in ms
 *   import "TurboNet" "socket_send"    : i32 (fd, ptr, len, out_sent)
 *   import "TurboNet" "socket_recv"    : i32 (fd, ptr, len, out_recv)
 *   import "TurboNet" "socket_release" : i32 (fd)
 *   import "TurboNet" "db_open"        : i32 (ptr, len, out_handle)
 *   import "TurboNet" "db_close"       : i32 (handle)
 *   import "TurboNet" "db_exec"        : i32 (handle, ptr, len, out_changes)
 *   import "TurboNet" "db_error"       : i32 (handle, ptr, len, out_written)
 *   import "TurboNet" "db_stmt_error"  : i32 (stmt, ptr, len, out_written)
 *   import "TurboNet" "db_prepare"     : i32 (db, ptr, len, out_stmt)
 *   import "TurboNet" "db_bind_i64"    : i32 (stmt, index, value)
 *   import "TurboNet" "db_bind_f64"    : i32 (stmt, index, value)
 *   import "TurboNet" "db_bind_null"   : i32 (stmt, index)
 *   import "TurboNet" "db_bind_blob"   : i32 (stmt, index, ptr, len)
 *   import "TurboNet" "db_bind_text"   : i32 (stmt, index, ptr, len)
 *   import "TurboNet" "db_step"        : i32 (stmt, out_state)
 *   import "TurboNet" "db_column_type" : i32 (stmt, index, out_type)
 *   import "TurboNet" "db_column_i64"  : i32 (stmt, index, out_value)
 *   import "TurboNet" "db_column_f64"  : i32 (stmt, index, out_value)
 *   import "TurboNet" "db_column_blob" : i32 (stmt, index, ptr, len, out_written)
 *   import "TurboNet" "db_column_text" : i32 (stmt, index, ptr, len, out_written)
 *   import "TurboNet" "db_reset"       : i32 (stmt)
 *   import "TurboNet" "db_finalize"    : i32 (stmt)
 */
CXX_C_API int
turbo_wasm3_vm_enable_host(turbo_wasm3_vm_t *vm);

CXX_C_API M3Result
turbo_wasm3_vm_link_host(turbo_wasm3_vm_t *vm, IM3Module module);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_WASM3_CORE_H */
