#ifndef TURBO_WASM3_DB_H
#define TURBO_WASM3_DB_H

#include "turbo_wasm3_core.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API turbo_wasm3_db_registry_t *turbo_wasm3_db_registry_create(size_t initial_capacity);

CXX_C_API void turbo_wasm3_db_registry_destroy(turbo_wasm3_db_registry_t *registry);

CXX_C_API int turbo_wasm3_db_registry_set_ops(turbo_wasm3_db_registry_t *registry,
                                              const turbo_wasm3_db_ops_t *ops, void *user_data);

CXX_C_API int turbo_wasm3_db_registry_enable_sqlite(turbo_wasm3_db_registry_t *registry);

CXX_C_API int
turbo_wasm3_db_registry_enable_sqlite_with_base_dir(turbo_wasm3_db_registry_t *registry,
                                                    const char *base_dir);
CXX_C_API int turbo_wasm3_vm_enable_sqlite_db(turbo_wasm3_vm_t *vm);

CXX_C_API int turbo_wasm3_vm_enable_sqlite_db_with_base_dir(turbo_wasm3_vm_t *vm,
                                                            const char *base_dir);

CXX_C_API int turbo_wasm3_db_registry_set_limits(turbo_wasm3_db_registry_t *registry,
                                                 size_t max_handles, size_t max_statements);
CXX_C_API int turbo_wasm3_db_registry_open(turbo_wasm3_db_registry_t *registry, const char *target,
                                           uint32_t *db_handle);

CXX_C_API int turbo_wasm3_db_registry_close(turbo_wasm3_db_registry_t *registry,
                                            uint32_t db_handle);

CXX_C_API int turbo_wasm3_db_registry_exec(turbo_wasm3_db_registry_t *registry, uint32_t db_handle,
                                           const char *sql, uint64_t *changes);

CXX_C_API int turbo_wasm3_db_registry_prepare(turbo_wasm3_db_registry_t *registry,
                                              uint32_t db_handle, const char *sql,
                                              uint32_t *stmt_handle);

CXX_C_API int turbo_wasm3_db_registry_finalize(turbo_wasm3_db_registry_t *registry,
                                               uint32_t stmt_handle);

CXX_C_API int turbo_wasm3_db_registry_reset(turbo_wasm3_db_registry_t *registry,
                                            uint32_t stmt_handle);

CXX_C_API int turbo_wasm3_db_registry_step(turbo_wasm3_db_registry_t *registry,
                                           uint32_t stmt_handle, int32_t *out_state);

CXX_C_API int turbo_wasm3_db_registry_bind_int64(turbo_wasm3_db_registry_t *registry,
                                                 uint32_t stmt_handle, uint32_t index,
                                                 int64_t value);

CXX_C_API int turbo_wasm3_db_registry_bind_double(turbo_wasm3_db_registry_t *registry,
                                                  uint32_t stmt_handle, uint32_t index,
                                                  double value);

CXX_C_API int turbo_wasm3_db_registry_bind_null(turbo_wasm3_db_registry_t *registry,
                                                uint32_t stmt_handle, uint32_t index);

CXX_C_API int turbo_wasm3_db_registry_bind_blob(turbo_wasm3_db_registry_t *registry,
                                                uint32_t stmt_handle, uint32_t index,
                                                const void *value, size_t value_len);

CXX_C_API int turbo_wasm3_db_registry_bind_text(turbo_wasm3_db_registry_t *registry,
                                                uint32_t stmt_handle, uint32_t index,
                                                const char *value, size_t value_len);

CXX_C_API int turbo_wasm3_db_registry_column_type(turbo_wasm3_db_registry_t *registry,
                                                  uint32_t stmt_handle, uint32_t index,
                                                  int32_t *out_type);

CXX_C_API int turbo_wasm3_db_registry_column_int64(turbo_wasm3_db_registry_t *registry,
                                                   uint32_t stmt_handle, uint32_t index,
                                                   int64_t *out_value);

CXX_C_API int turbo_wasm3_db_registry_column_double(turbo_wasm3_db_registry_t *registry,
                                                    uint32_t stmt_handle, uint32_t index,
                                                    double *out_value);

CXX_C_API int turbo_wasm3_db_registry_column_blob(turbo_wasm3_db_registry_t *registry,
                                                  uint32_t stmt_handle, uint32_t index,
                                                  void *buffer, size_t buffer_size,
                                                  uint32_t *out_len);

CXX_C_API int turbo_wasm3_db_registry_column_text(turbo_wasm3_db_registry_t *registry,
                                                  uint32_t stmt_handle, uint32_t index,
                                                  char *buffer, size_t buffer_size,
                                                  uint32_t *out_len);

CXX_C_API int turbo_wasm3_db_registry_error(turbo_wasm3_db_registry_t *registry, uint32_t db_handle,
                                            char *buffer, size_t buffer_size, uint32_t *out_len);

CXX_C_API int turbo_wasm3_db_registry_stmt_error(turbo_wasm3_db_registry_t *registry,
                                                 uint32_t stmt_handle, char *buffer,
                                                 size_t buffer_size, uint32_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_WASM3_DB_H */
