#include "turbo_wasm3_internal.h"

m3ApiRawFunction(turbo_wasm3_host_db_open) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *target = NULL;
  int rc;
  uint32_t handle = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, target_data)
  m3ApiGetArg(uint32_t, target_len)
  m3ApiGetArgMem(uint32_t *, out_handle)

  if (!vm || !vm->db_registry || !out_handle) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_handle, sizeof(uint32_t));
  if (target_len != 0) {
    m3ApiCheckMem(target_data, target_len);
  }
  m3ApiWriteMem32(out_handle, 0);

  target = turbo_wasm3_copy_guest_bytes(target_data, target_len);
  if (!target) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_db_registry_open(vm->db_registry, target, &handle);
  free(target);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_handle, handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_close) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)

  if (!vm || !vm->db_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(turbo_wasm3_db_registry_close(vm->db_registry, handle));
}

m3ApiRawFunction(turbo_wasm3_host_db_exec) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *sql = NULL;
  uint64_t changes = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, sql_data)
  m3ApiGetArg(uint32_t, sql_len)
  m3ApiGetArgMem(uint64_t *, out_changes)

  if (!vm || !vm->db_registry || !out_changes) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_changes, sizeof(uint64_t));
  if (sql_len != 0) {
    m3ApiCheckMem(sql_data, sql_len);
  }
  m3ApiWriteMem64(out_changes, 0);

  sql = turbo_wasm3_copy_guest_bytes(sql_data, sql_len);
  if (!sql) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_db_registry_exec(vm->db_registry, handle, sql, &changes);
  free(sql);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem64(out_changes, changes);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_error) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->db_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_db_registry_error(vm->db_registry, handle, buffer,
                                     (size_t)buffer_size, &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_stmt_error) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->db_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_db_registry_stmt_error(vm->db_registry, stmt_handle, buffer,
                                          (size_t)buffer_size, &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_prepare) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *sql = NULL;
  uint32_t stmt_handle = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, db_handle)
  m3ApiGetArgMem(const uint8_t *, sql_data)
  m3ApiGetArg(uint32_t, sql_len)
  m3ApiGetArgMem(uint32_t *, out_stmt_handle)

  if (!vm || !vm->db_registry || !out_stmt_handle) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_stmt_handle, sizeof(uint32_t));
  if (sql_len != 0) {
    m3ApiCheckMem(sql_data, sql_len);
  }
  m3ApiWriteMem32(out_stmt_handle, 0);

  sql = turbo_wasm3_copy_guest_bytes(sql_data, sql_len);
  if (!sql) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_db_registry_prepare(vm->db_registry, db_handle, sql,
                                       &stmt_handle);
  free(sql);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_stmt_handle, stmt_handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_finalize) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)

  if (!vm || !vm->db_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(turbo_wasm3_db_registry_finalize(vm->db_registry, stmt_handle));
}

m3ApiRawFunction(turbo_wasm3_host_db_reset) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)

  if (!vm || !vm->db_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(turbo_wasm3_db_registry_reset(vm->db_registry, stmt_handle));
}

m3ApiRawFunction(turbo_wasm3_host_db_step) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int32_t state = TURBO_WASM3_DB_STEP_DONE;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArgMem(int32_t *, out_state)

  if (!vm || !vm->db_registry || !out_state) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_state, sizeof(int32_t));
  m3ApiWriteMem32(out_state, (uint32_t)TURBO_WASM3_DB_STEP_DONE);

  rc = turbo_wasm3_db_registry_step(vm->db_registry, stmt_handle, &state);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_state, (uint32_t)state);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_bind_i64) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArg(int64_t, value)

  if (!vm || !vm->db_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(
      turbo_wasm3_db_registry_bind_int64(vm->db_registry, stmt_handle, index, value));
}

m3ApiRawFunction(turbo_wasm3_host_db_bind_f64) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArg(double, value)

  if (!vm || !vm->db_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(
      turbo_wasm3_db_registry_bind_double(vm->db_registry, stmt_handle, index, value));
}

m3ApiRawFunction(turbo_wasm3_host_db_bind_null) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)

  if (!vm || !vm->db_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(turbo_wasm3_db_registry_bind_null(vm->db_registry, stmt_handle, index));
}

m3ApiRawFunction(turbo_wasm3_host_db_bind_blob) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *blob = NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArgMem(const uint8_t *, blob_data)
  m3ApiGetArg(uint32_t, blob_len)

  if (!vm || !vm->db_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (blob_len != 0) {
    m3ApiCheckMem(blob_data, blob_len);
  }

  blob = turbo_wasm3_copy_guest_bytes(blob_data, blob_len);
  if (!blob) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_db_registry_bind_blob(vm->db_registry, stmt_handle, index, blob,
                                         blob_len);
  free(blob);
  m3ApiReturn(rc);
}

m3ApiRawFunction(turbo_wasm3_host_db_bind_text) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *text = NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArgMem(const uint8_t *, text_data)
  m3ApiGetArg(uint32_t, text_len)

  if (!vm || !vm->db_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (text_len != 0) {
    m3ApiCheckMem(text_data, text_len);
  }

  text = turbo_wasm3_copy_guest_bytes(text_data, text_len);
  if (!text) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_db_registry_bind_text(vm->db_registry, stmt_handle, index, text,
                                         text_len);
  free(text);
  m3ApiReturn(rc);
}

m3ApiRawFunction(turbo_wasm3_host_db_column_type) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int32_t type = TURBO_WASM3_DB_TYPE_NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArgMem(int32_t *, out_type)

  if (!vm || !vm->db_registry || !out_type) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_type, sizeof(int32_t));
  m3ApiWriteMem32(out_type, (uint32_t)TURBO_WASM3_DB_TYPE_NULL);

  rc = turbo_wasm3_db_registry_column_type(vm->db_registry, stmt_handle, index, &type);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_type, (uint32_t)type);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_column_i64) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int64_t value = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArgMem(int64_t *, out_value)

  if (!vm || !vm->db_registry || !out_value) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_value, sizeof(int64_t));
  m3ApiWriteMem64(out_value, 0);

  rc = turbo_wasm3_db_registry_column_int64(vm->db_registry, stmt_handle, index,
                                            &value);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem64(out_value, (uint64_t)value);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_column_f64) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  double value = 0.0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArgMem(double *, out_value)

  if (!vm || !vm->db_registry || !out_value) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_value, sizeof(double));
  memset(out_value, 0, sizeof(double));

  rc = turbo_wasm3_db_registry_column_double(vm->db_registry, stmt_handle, index,
                                             &value);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  memcpy(out_value, &value, sizeof(value));
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_column_blob) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArgMem(uint8_t *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->db_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_db_registry_column_blob(vm->db_registry, stmt_handle, index,
                                           buffer, (size_t)buffer_size, &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_db_column_text) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, stmt_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->db_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_db_registry_column_text(vm->db_registry, stmt_handle, index,
                                           buffer, buffer_size, &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}


turbo_wasm3_db_registry_t *
turbo_wasm3_db_registry_create(size_t initial_capacity) {
  turbo_wasm3_db_registry_t *registry;
  int rc;

  registry = (turbo_wasm3_db_registry_t *)calloc(1, sizeof(*registry));
  if (!registry) {
    return NULL;
  }

  registry->next_handle = 1;
  registry->next_stmt_handle = 0x10000u;
  registry->max_entries = initial_capacity > TURBO_WASM3_DEFAULT_MAX_DB_HANDLES
                              ? initial_capacity
                              : TURBO_WASM3_DEFAULT_MAX_DB_HANDLES;
  registry->max_statements = TURBO_WASM3_DEFAULT_MAX_DB_STATEMENTS;
  rc = turbo_wasm3_db_registry_reserve(registry,
                                       initial_capacity ? initial_capacity : 4);
  if (rc != 0) {
    free(registry);
    return NULL;
  }

  rc = turbo_wasm3_db_registry_reserve_statements(registry, 4);
  if (rc != 0) {
    free(registry->entries);
    free(registry);
    return NULL;
  }

  return registry;
}

void turbo_wasm3_db_registry_destroy(turbo_wasm3_db_registry_t *registry) {
  size_t i;

  if (!registry) {
    return;
  }

  if (registry->ops && registry->ops->finalize) {
    for (i = 0; i < registry->stmt_count; ++i) {
      turbo_wasm3_db_stmt_entry_clear_error(&registry->stmt_entries[i]);
      registry->ops->finalize(registry->user_data, registry->stmt_entries[i].stmt);
    }
  }

  if (registry->ops && registry->ops->close) {
    for (i = 0; i < registry->count; ++i) {
      turbo_wasm3_db_entry_clear_error(&registry->entries[i]);
      registry->ops->close(registry->user_data, registry->entries[i].db);
    }
  }

  free(registry->stmt_entries);
  free(registry->entries);
  free(registry->sqlite_base_dir);
  free(registry);
}

int turbo_wasm3_db_registry_set_ops(turbo_wasm3_db_registry_t *registry,
                                    const turbo_wasm3_db_ops_t *ops,
                                    void *user_data) {
  if (!registry || !ops || !ops->open || !ops->close || !ops->exec ||
      !ops->error || !ops->prepare || !ops->finalize || !ops->reset ||
      !ops->step || !ops->bind_int64 || !ops->bind_double || !ops->bind_null ||
      !ops->bind_blob || !ops->bind_text || !ops->column_type ||
      !ops->column_int64 || !ops->column_double || !ops->column_blob ||
      !ops->column_text) {
    return TURBO_EINVAL;
  }
  if (registry->count != 0 || registry->stmt_count != 0) {
    return TURBO_EBUSY;
  }

  free(registry->sqlite_base_dir);
  registry->sqlite_base_dir = NULL;
  registry->ops = ops;
  registry->user_data = user_data;
  return 0;
}

int turbo_wasm3_db_registry_enable_sqlite(turbo_wasm3_db_registry_t *registry) {
  if (!registry) {
    return TURBO_EINVAL;
  }
  if (registry->count != 0 || registry->stmt_count != 0) {
    return TURBO_EBUSY;
  }
  free(registry->sqlite_base_dir);
  registry->sqlite_base_dir = NULL;
  return turbo_wasm3_db_registry_set_ops(registry, &turbo_wasm3_sqlite_db_ops,
                                         registry);
}

int turbo_wasm3_db_registry_enable_sqlite_with_base_dir(
    turbo_wasm3_db_registry_t *registry, const char *base_dir) {
  char *base_copy;

  if (!registry || !base_dir || base_dir[0] == '\0' ||
      !turbo_fs_path_is_absolute(base_dir)) {
    return TURBO_EINVAL;
  }
  if (turbo_wasm3_path_has_parent_segment(base_dir)) {
    return TURBO_EPERM;
  }

  base_copy = turbo_wasm3_strdup(base_dir);
  if (!base_copy) {
    return TURBO_ENOMEM;
  }

  if (registry->count != 0 || registry->stmt_count != 0) {
    free(base_copy);
    return TURBO_EBUSY;
  }

  free(registry->sqlite_base_dir);
  registry->sqlite_base_dir = base_copy;
  registry->ops = &turbo_wasm3_sqlite_db_ops;
  registry->user_data = registry;
  return 0;
}

int turbo_wasm3_vm_enable_sqlite_db(turbo_wasm3_vm_t *vm) {
  if (!vm) {
    return TURBO_EINVAL;
  }

  return turbo_wasm3_db_registry_enable_sqlite(vm->db_registry);
}

int turbo_wasm3_vm_enable_sqlite_db_with_base_dir(turbo_wasm3_vm_t *vm,
                                                  const char *base_dir) {
  if (!vm) {
    return TURBO_EINVAL;
  }

  return turbo_wasm3_db_registry_enable_sqlite_with_base_dir(vm->db_registry,
                                                            base_dir);
}

int turbo_wasm3_db_registry_set_limits(turbo_wasm3_db_registry_t *registry,
                                       size_t max_databases,
                                       size_t max_statements) {
  if (!registry || max_databases == 0 || max_statements == 0 ||
      max_databases < registry->count ||
      max_statements < registry->stmt_count) {
    return TURBO_EINVAL;
  }

  registry->max_entries = max_databases;
  registry->max_statements = max_statements;
  return 0;
}

int turbo_wasm3_db_registry_open(turbo_wasm3_db_registry_t *registry,
                                 const char *target, uint32_t *db_handle) {
  int rc;
  void *db = NULL;

  if (!registry || !target || !db_handle || !registry->ops || !registry->ops->open) {
    return TURBO_EINVAL;
  }

  rc = turbo_wasm3_db_registry_reserve(registry, registry->count + 1);
  if (rc != 0) {
    return rc;
  }

  rc = registry->ops->open(registry->user_data, target, &db);
  if (rc != 0) {
    return rc;
  }

  registry->entries[registry->count].handle = registry->next_handle++;
  registry->entries[registry->count].db = db;
  registry->entries[registry->count].last_error = NULL;
  *db_handle = registry->entries[registry->count].handle;
  registry->count++;
  return 0;
}

int turbo_wasm3_db_registry_close(turbo_wasm3_db_registry_t *registry,
                                  uint32_t db_handle) {
  turbo_wasm3_db_entry_t *entry;
  size_t index;
  size_t i;
  int rc;

  if (!registry || !registry->ops || !registry->ops->close) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_db_registry_find_entry(registry, db_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  index = (size_t)(entry - registry->entries);
  for (i = registry->stmt_count; i > 0; --i) {
    turbo_wasm3_db_stmt_entry_t *stmt_entry = &registry->stmt_entries[i - 1];
    if (stmt_entry->db_handle == db_handle) {
      rc = registry->ops->finalize(registry->user_data, stmt_entry->stmt);
      if (rc != 0) {
        turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, entry);
        return rc;
      }
      turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
      registry->stmt_entries[i - 1] = registry->stmt_entries[registry->stmt_count - 1];
      registry->stmt_count--;
    }
  }

  rc = registry->ops->close(registry->user_data, entry->db);
  if (rc != 0) {
    turbo_wasm3_db_entry_capture_provider_error(registry, entry);
    return rc;
  }

  turbo_wasm3_db_entry_clear_error(entry);
  registry->entries[index] = registry->entries[registry->count - 1];
  registry->count--;
  return 0;
}

int turbo_wasm3_db_registry_exec(turbo_wasm3_db_registry_t *registry,
                                 uint32_t db_handle, const char *sql,
                                 uint64_t *changes) {
  turbo_wasm3_db_entry_t *entry;

  if (!registry || !sql || !changes || !registry->ops || !registry->ops->exec) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_db_registry_find_entry(registry, db_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_db_entry_clear_error(entry);
  {
    int rc = registry->ops->exec(registry->user_data, entry->db, sql, changes);
    if (rc != 0) {
      turbo_wasm3_db_entry_capture_provider_error(registry, entry);
      return rc;
    }
  }

  return 0;
}

int turbo_wasm3_db_registry_prepare(turbo_wasm3_db_registry_t *registry,
                                    uint32_t db_handle, const char *sql,
                                    uint32_t *stmt_handle) {
  turbo_wasm3_db_entry_t *entry;
  void *stmt = NULL;
  int rc;

  if (!registry || !sql || !stmt_handle || !registry->ops || !registry->ops->prepare) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_db_registry_find_entry(registry, db_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  rc = turbo_wasm3_db_registry_reserve_statements(registry, registry->stmt_count + 1);
  if (rc != 0) {
    return rc;
  }

  turbo_wasm3_db_entry_clear_error(entry);
  rc = registry->ops->prepare(registry->user_data, entry->db, sql, &stmt);
  if (rc != 0) {
    turbo_wasm3_db_entry_capture_provider_error(registry, entry);
    return rc;
  }

  registry->stmt_entries[registry->stmt_count].handle = registry->next_stmt_handle++;
  registry->stmt_entries[registry->stmt_count].db_handle = db_handle;
  registry->stmt_entries[registry->stmt_count].stmt = stmt;
  registry->stmt_entries[registry->stmt_count].last_error = NULL;
  *stmt_handle = registry->stmt_entries[registry->stmt_count].handle;
  registry->stmt_count++;
  return 0;
}

int turbo_wasm3_db_registry_finalize(turbo_wasm3_db_registry_t *registry,
                                     uint32_t stmt_handle) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  size_t index;
  int rc;

  if (!registry || !registry->ops || !registry->ops->finalize) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  index = (size_t)(stmt_entry - registry->stmt_entries);
  rc = registry->ops->finalize(registry->user_data, stmt_entry->stmt);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
    return rc;
  }

  turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
  registry->stmt_entries[index] = registry->stmt_entries[registry->stmt_count - 1];
  registry->stmt_count--;
  return 0;
}

int turbo_wasm3_db_registry_reset(turbo_wasm3_db_registry_t *registry,
                                  uint32_t stmt_handle) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !registry->ops || !registry->ops->reset) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
  if (db_entry) {
    turbo_wasm3_db_entry_clear_error(db_entry);
  }
  rc = registry->ops->reset(registry->user_data, stmt_entry->stmt);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_step(turbo_wasm3_db_registry_t *registry,
                                 uint32_t stmt_handle, int32_t *out_state) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !out_state || !registry->ops || !registry->ops->step) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
  if (db_entry) {
    turbo_wasm3_db_entry_clear_error(db_entry);
  }
  rc = registry->ops->step(registry->user_data, stmt_entry->stmt, out_state);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_bind_int64(turbo_wasm3_db_registry_t *registry,
                                       uint32_t stmt_handle, uint32_t index,
                                       int64_t value) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !registry->ops || !registry->ops->bind_int64) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
  if (db_entry) {
    turbo_wasm3_db_entry_clear_error(db_entry);
  }
  rc = registry->ops->bind_int64(registry->user_data, stmt_entry->stmt, index,
                                 value);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_bind_double(turbo_wasm3_db_registry_t *registry,
                                        uint32_t stmt_handle, uint32_t index,
                                        double value) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !registry->ops || !registry->ops->bind_double) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
  if (db_entry) {
    turbo_wasm3_db_entry_clear_error(db_entry);
  }
  rc = registry->ops->bind_double(registry->user_data, stmt_entry->stmt, index,
                                  value);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_bind_null(turbo_wasm3_db_registry_t *registry,
                                      uint32_t stmt_handle, uint32_t index) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !registry->ops || !registry->ops->bind_null) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
  if (db_entry) {
    turbo_wasm3_db_entry_clear_error(db_entry);
  }
  rc = registry->ops->bind_null(registry->user_data, stmt_entry->stmt, index);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_bind_blob(turbo_wasm3_db_registry_t *registry,
                                      uint32_t stmt_handle, uint32_t index,
                                      const void *value, size_t value_len) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || (!value && value_len != 0) || !registry->ops ||
      !registry->ops->bind_blob) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
  if (db_entry) {
    turbo_wasm3_db_entry_clear_error(db_entry);
  }
  rc = registry->ops->bind_blob(registry->user_data, stmt_entry->stmt, index,
                                value, value_len);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_bind_text(turbo_wasm3_db_registry_t *registry,
                                      uint32_t stmt_handle, uint32_t index,
                                      const char *value, size_t value_len) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || (!value && value_len != 0) || !registry->ops ||
      !registry->ops->bind_text) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
  if (db_entry) {
    turbo_wasm3_db_entry_clear_error(db_entry);
  }
  rc = registry->ops->bind_text(registry->user_data, stmt_entry->stmt, index,
                                value, value_len);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_column_type(turbo_wasm3_db_registry_t *registry,
                                        uint32_t stmt_handle, uint32_t index,
                                        int32_t *out_type) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !out_type || !registry->ops || !registry->ops->column_type) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  rc = registry->ops->column_type(registry->user_data, stmt_entry->stmt, index,
                                  out_type);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_column_int64(turbo_wasm3_db_registry_t *registry,
                                         uint32_t stmt_handle, uint32_t index,
                                         int64_t *out_value) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !out_value || !registry->ops || !registry->ops->column_int64) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  rc = registry->ops->column_int64(registry->user_data, stmt_entry->stmt, index,
                                   out_value);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_column_double(turbo_wasm3_db_registry_t *registry,
                                          uint32_t stmt_handle, uint32_t index,
                                          double *out_value) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !out_value || !registry->ops || !registry->ops->column_double) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  rc = registry->ops->column_double(registry->user_data, stmt_entry->stmt, index,
                                    out_value);
  if (rc != 0) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_column_blob(turbo_wasm3_db_registry_t *registry,
                                        uint32_t stmt_handle, uint32_t index,
                                        void *buffer, size_t buffer_size,
                                        uint32_t *out_len) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !buffer || buffer_size == 0 || !out_len || !registry->ops ||
      !registry->ops->column_blob) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  rc = registry->ops->column_blob(registry->user_data, stmt_entry->stmt, index,
                                  buffer, buffer_size, out_len);
  if (rc != 0 && rc != TURBO_EMSGSIZE) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_column_text(turbo_wasm3_db_registry_t *registry,
                                        uint32_t stmt_handle, uint32_t index,
                                        char *buffer, size_t buffer_size,
                                        uint32_t *out_len) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;
  int rc;

  if (!registry || !buffer || buffer_size == 0 || !out_len || !registry->ops ||
      !registry->ops->column_text) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  rc = registry->ops->column_text(registry->user_data, stmt_entry->stmt, index,
                                  buffer, buffer_size, out_len);
  if (rc != 0 && rc != TURBO_EMSGSIZE) {
    turbo_wasm3_db_stmt_entry_capture_provider_error(registry, stmt_entry, db_entry);
  }
  return rc;
}

int turbo_wasm3_db_registry_error(turbo_wasm3_db_registry_t *registry,
                                  uint32_t db_handle, char *buffer,
                                  size_t buffer_size, uint32_t *out_len) {
  turbo_wasm3_db_entry_t *entry;

  if (!registry || !buffer || buffer_size == 0 || !out_len || !registry->ops ||
      !registry->ops->error) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_db_registry_find_entry(registry, db_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  if (entry->last_error) {
    return turbo_wasm3_error_buffer_write(entry->last_error, buffer, buffer_size,
                                          out_len);
  }

  return registry->ops->error(registry->user_data, entry->db, buffer,
                              buffer_size, out_len);
}

int turbo_wasm3_db_registry_stmt_error(turbo_wasm3_db_registry_t *registry,
                                       uint32_t stmt_handle, char *buffer,
                                       size_t buffer_size, uint32_t *out_len) {
  turbo_wasm3_db_stmt_entry_t *stmt_entry;
  turbo_wasm3_db_entry_t *db_entry;

  if (!registry || !buffer || buffer_size == 0 || !out_len || !registry->ops ||
      !registry->ops->error) {
    return TURBO_EINVAL;
  }

  stmt_entry = turbo_wasm3_db_registry_find_stmt_entry(registry, stmt_handle);
  if (!stmt_entry) {
    return TURBO_EBADF;
  }

  if (stmt_entry->last_error) {
    return turbo_wasm3_error_buffer_write(stmt_entry->last_error, buffer,
                                          buffer_size, out_len);
  }

  db_entry = turbo_wasm3_db_registry_find_entry(registry, stmt_entry->db_handle);
  if (!db_entry) {
    return TURBO_EBADF;
  }

  if (db_entry->last_error) {
    return turbo_wasm3_error_buffer_write(db_entry->last_error, buffer, buffer_size,
                                          out_len);
  }

  return registry->ops->error(registry->user_data, db_entry->db, buffer,
                              buffer_size, out_len);
}
