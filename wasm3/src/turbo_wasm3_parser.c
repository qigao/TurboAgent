#include "turbo_wasm3_internal.h"

static int
turbo_wasm3_parser_registry_reserve(turbo_wasm3_parser_registry_t *registry, size_t new_capacity) {
  turbo_wasm3_parser_entry_t *new_entries;
  size_t checked_capacity;
  int rc;

  if (!registry) {
    return TURBO_EINVAL;
  }

  rc = turbo_wasm3_checked_capacity(registry->capacity, new_capacity, 8,
                                    registry->max_entries,
                                    sizeof(*new_entries), &checked_capacity);
  if (rc != 0) {
    return rc;
  }

  new_entries = (turbo_wasm3_parser_entry_t *)realloc(registry->entries,
                                                       checked_capacity * sizeof(*new_entries));
  if (!new_entries) {
    return TURBO_ENOMEM;
  }

  registry->entries = new_entries;
  registry->capacity = checked_capacity;
  return 0;
}

turbo_wasm3_parser_registry_t *
turbo_wasm3_parser_registry_create(size_t initial_capacity) {
  turbo_wasm3_parser_registry_t *registry;
  int rc;

  registry = (turbo_wasm3_parser_registry_t *)calloc(1, sizeof(*registry));
  if (!registry) {
    return NULL;
  }

  registry->next_handle = 0x1000u;
  registry->max_entries = initial_capacity > TURBO_WASM3_DEFAULT_MAX_PARSER_DOCS
                              ? initial_capacity
                              : TURBO_WASM3_DEFAULT_MAX_PARSER_DOCS;
  rc = turbo_wasm3_parser_registry_reserve(registry,
                                           initial_capacity ? initial_capacity : 8);
  if (rc != 0) {
    free(registry);
    return NULL;
  }

  return registry;
}

int turbo_wasm3_parser_registry_set_limit(
    turbo_wasm3_parser_registry_t *registry, size_t max_entries) {
  if (!registry || max_entries == 0 || max_entries < registry->count) {
    return TURBO_EINVAL;
  }

  registry->max_entries = max_entries;
  return 0;
}

static void
turbo_wasm3_parser_entry_clear(turbo_wasm3_parser_entry_t *entry) {
  if (!entry) {
    return;
  }

  if (entry->doc) {
    switch (entry->type) {
    case TURBO_WASM3_PARSER_TYPE_JSON:
      turbo_free_json(&entry->doc);
      break;
    case TURBO_WASM3_PARSER_TYPE_CSV:
      turbo_free_csv(&entry->doc);
      break;
    case TURBO_WASM3_PARSER_TYPE_XML:
      turbo_free_xml(&entry->doc);
      break;
    case TURBO_WASM3_PARSER_TYPE_INI:
      turbo_free_ini(&entry->doc);
      break;
    case TURBO_WASM3_PARSER_TYPE_TOML:
      turbo_free_toml(&entry->doc);
      break;
    default:
      free(entry->doc);
      break;
    }
    entry->doc = NULL;
  }
  entry->type = TURBO_WASM3_PARSER_TYPE_NONE;
  entry->handle = 0;
}

void
turbo_wasm3_parser_registry_destroy(turbo_wasm3_parser_registry_t *registry) {
  size_t i;

  if (!registry) {
    return;
  }

  for (i = 0; i < registry->count; ++i) {
    turbo_wasm3_parser_entry_clear(&registry->entries[i]);
  }

  free(registry->entries);
  free(registry);
}

static int
turbo_wasm3_parser_registry_find(turbo_wasm3_parser_registry_t *registry, uint32_t handle) {
  size_t i;

  if (!registry) {
    return -1;
  }

  for (i = 0; i < registry->count; ++i) {
    if (registry->entries[i].handle == handle) {
      return (int)i;
    }
  }

  return -1;
}

static int
turbo_wasm3_parser_registry_add(turbo_wasm3_parser_registry_t *registry, int type, void *doc, uint32_t *out_handle) {
  int rc;
  size_t i;

  if (!registry || !doc || !out_handle) {
    return TURBO_EINVAL;
  }

  if (registry->count >= registry->capacity) {
    rc = turbo_wasm3_parser_registry_reserve(registry, registry->capacity * 2);
    if (rc != 0) {
      return rc;
    }
  }

  i = registry->count++;
  registry->entries[i].handle = registry->next_handle++;
  registry->entries[i].type = type;
  registry->entries[i].doc = doc;

  *out_handle = registry->entries[i].handle;
  return 0;
}

int
turbo_wasm3_parser_free(turbo_wasm3_vm_t *vm, uint32_t handle) {
  int idx;

  if (!vm || !vm->parser_registry) {
    return TURBO_EINVAL;
  }

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    return TURBO_EBADF;
  }

  turbo_wasm3_parser_entry_clear(&vm->parser_registry->entries[idx]);

  if (idx != (int)(vm->parser_registry->count - 1)) {
    vm->parser_registry->entries[idx] = vm->parser_registry->entries[vm->parser_registry->count - 1];
  }
  vm->parser_registry->count--;

  return 0;
}

/* JSON host functions */
m3ApiRawFunction(turbo_wasm3_host_parser_json_parse) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *data = NULL;
  int rc;
  uint32_t handle = 0;
  void *doc = NULL;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, data_ptr)
  m3ApiGetArg(uint32_t, data_len)
  m3ApiGetArgMem(uint32_t *, out_handle)

  if (!vm || !vm->parser_registry || !out_handle) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_handle, sizeof(uint32_t));
  if (data_len != 0) {
    m3ApiCheckMem(data_ptr, data_len);
  }
  m3ApiWriteMem32(out_handle, 0);

  if (data_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  data = turbo_wasm3_copy_guest_bytes(data_ptr, data_len);
  if (!data) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_parse_json((const uint8_t *)data, data_len, &doc);
  free(data);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  rc = turbo_wasm3_parser_registry_add(vm->parser_registry, TURBO_WASM3_PARSER_TYPE_JSON, doc, &handle);
  if (rc != 0) {
    turbo_free_json(&doc);
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_handle, handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_parser_csv_parse) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *data = NULL;
  turbo_csv_options_t opts = {1, ',', '"', 1};
  int rc;
  uint32_t handle = 0;
  void *doc = NULL;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, data_ptr)
  m3ApiGetArg(uint32_t, data_len)
  m3ApiGetArgMem(uint32_t *, out_handle)

  if (!vm || !vm->parser_registry || !out_handle) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_handle, sizeof(uint32_t));
  if (data_len != 0) {
    m3ApiCheckMem(data_ptr, data_len);
  }
  m3ApiWriteMem32(out_handle, 0);

  if (data_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  data = turbo_wasm3_copy_guest_bytes(data_ptr, data_len);
  if (!data) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_parse_csv_opts((const uint8_t *)data, data_len, &opts, &doc);
  free(data);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  rc = turbo_wasm3_parser_registry_add(vm->parser_registry, TURBO_WASM3_PARSER_TYPE_CSV, doc, &handle);
  if (rc != 0) {
    turbo_free_csv(&doc);
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_handle, handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_parser_xml_parse) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *data = NULL;
  int rc;
  uint32_t handle = 0;
  void *doc = NULL;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, data_ptr)
  m3ApiGetArg(uint32_t, data_len)
  m3ApiGetArgMem(uint32_t *, out_handle)

  if (!vm || !vm->parser_registry || !out_handle) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_handle, sizeof(uint32_t));
  if (data_len != 0) {
    m3ApiCheckMem(data_ptr, data_len);
  }
  m3ApiWriteMem32(out_handle, 0);

  if (data_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  data = turbo_wasm3_copy_guest_bytes(data_ptr, data_len);
  if (!data) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_parse_xml((const uint8_t *)data, data_len, &doc);
  free(data);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  rc = turbo_wasm3_parser_registry_add(vm->parser_registry, TURBO_WASM3_PARSER_TYPE_XML, doc, &handle);
  if (rc != 0) {
    turbo_free_xml(&doc);
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_handle, handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_parser_ini_parse) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *data = NULL;
  int rc;
  uint32_t handle = 0;
  void *doc = NULL;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, data_ptr)
  m3ApiGetArg(uint32_t, data_len)
  m3ApiGetArgMem(uint32_t *, out_handle)

  if (!vm || !vm->parser_registry || !out_handle) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_handle, sizeof(uint32_t));
  if (data_len != 0) {
    m3ApiCheckMem(data_ptr, data_len);
  }
  m3ApiWriteMem32(out_handle, 0);

  if (data_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  data = turbo_wasm3_copy_guest_bytes(data_ptr, data_len);
  if (!data) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_parse_ini((const uint8_t *)data, data_len, &doc);
  free(data);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  rc = turbo_wasm3_parser_registry_add(vm->parser_registry, TURBO_WASM3_PARSER_TYPE_INI, doc, &handle);
  if (rc != 0) {
    turbo_free_ini(&doc);
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_handle, handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_parser_free) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)

  if (!vm || !vm->parser_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(turbo_wasm3_parser_free(vm, handle));
}

/* JSON accessors */
m3ApiRawFunction(turbo_wasm3_host_json_get_string) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *path = NULL;
  int idx, rc;
  turbo_wasm3_parser_entry_t *entry;
  json_value_t *val = NULL, *result;
  char *out_str = NULL;
  size_t out_len = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, path_ptr)
  m3ApiGetArg(uint32_t, path_len)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->parser_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (path_len != 0) {
    m3ApiCheckMem(path_ptr, path_len);
  }
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_JSON) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (path_len > 0) {
    path = turbo_wasm3_copy_guest_bytes(path_ptr, path_len);
    if (!path) {
      m3ApiReturn(TURBO_ENOMEM);
    }
    result = turbo_json_object_get((json_value_t *)entry->doc, path);
    free(path);
  } else {
    result = (json_value_t *)entry->doc;
  }

  if (!result) {
    m3ApiReturn(TURBO_ENOENT);
  }

  rc = turbo_json_type(result);
  if (rc == TURBO_JSON_STRING) {
    out_str = (char *)turbo_json_string(result);
    out_len = strlen(out_str);
  } else if (rc == TURBO_JSON_NUMBER) {
    char num_buf[64];
    snprintf(num_buf, sizeof(num_buf), "%g", turbo_json_number(result));
    out_str = num_buf;
    out_len = strlen(num_buf);
  } else if (rc == TURBO_JSON_BOOL) {
    out_str = turbo_json_bool(result) ? "true" : "false";
    out_len = strlen(out_str);
  } else if (rc == TURBO_JSON_NULL) {
    m3ApiReturn(0);
  } else {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (buffer_size > 0) {
    size_t copy_len = out_len < buffer_size - 1 ? out_len : buffer_size - 1;
    memcpy(buffer, out_str, copy_len);
    buffer[copy_len] = '\0';
    m3ApiWriteMem32(out_written, (uint32_t)copy_len);
  } else {
    m3ApiWriteMem32(out_written, (uint32_t)out_len);
  }

  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_json_get_int) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *path = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  json_value_t *result;
  double val = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, path_ptr)
  m3ApiGetArg(uint32_t, path_len)
  m3ApiGetArgMem(int32_t *, out_value)

  if (!vm || !vm->parser_registry || !out_value) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_value, sizeof(int32_t));
  if (path_len != 0) {
    m3ApiCheckMem(path_ptr, path_len);
  }

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_JSON) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (path_len > 0) {
    path = turbo_wasm3_copy_guest_bytes(path_ptr, path_len);
    if (!path) {
      m3ApiReturn(TURBO_ENOMEM);
    }
    result = turbo_json_object_get((json_value_t *)entry->doc, path);
    free(path);
  } else {
    result = (json_value_t *)entry->doc;
  }

  if (!result) {
    m3ApiReturn(TURBO_ENOENT);
  }

  val = turbo_json_number(result);
  m3ApiWriteMem32(out_value, (int32_t)val);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_json_get_bool) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *path = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  json_value_t *result;
  bool val = false;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, path_ptr)
  m3ApiGetArg(uint32_t, path_len)
  m3ApiGetArgMem(int32_t *, out_value)

  if (!vm || !vm->parser_registry || !out_value) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_value, sizeof(int32_t));
  if (path_len != 0) {
    m3ApiCheckMem(path_ptr, path_len);
  }

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_JSON) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (path_len > 0) {
    path = turbo_wasm3_copy_guest_bytes(path_ptr, path_len);
    if (!path) {
      m3ApiReturn(TURBO_ENOMEM);
    }
    result = turbo_json_object_get((json_value_t *)entry->doc, path);
    free(path);
  } else {
    result = (json_value_t *)entry->doc;
  }

  if (!result) {
    m3ApiReturn(TURBO_ENOENT);
  }

  val = turbo_json_bool(result);
  m3ApiWriteMem32(out_value, val ? 1 : 0);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_json_array_size) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *path = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  json_value_t *result;
  size_t size = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, path_ptr)
  m3ApiGetArg(uint32_t, path_len)
  m3ApiGetArgMem(uint32_t *, out_size)

  if (!vm || !vm->parser_registry || !out_size) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_size, sizeof(uint32_t));
  if (path_len != 0) {
    m3ApiCheckMem(path_ptr, path_len);
  }

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_JSON) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (path_len > 0) {
    path = turbo_wasm3_copy_guest_bytes(path_ptr, path_len);
    if (!path) {
      m3ApiReturn(TURBO_ENOMEM);
    }
    result = turbo_json_object_get((json_value_t *)entry->doc, path);
    free(path);
  } else {
    result = (json_value_t *)entry->doc;
  }

  if (!result) {
    m3ApiReturn(TURBO_ENOENT);
  }

  size = turbo_json_array_size(result);
  m3ApiWriteMem32(out_size, (uint32_t)size);
  m3ApiReturn(0);
}

/* CSV accessors */
m3ApiRawFunction(turbo_wasm3_host_csv_row_count) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  size_t count = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(uint32_t *, out_count)

  if (!vm || !vm->parser_registry || !out_count) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_count, sizeof(uint32_t));

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_CSV) {
    m3ApiReturn(TURBO_EINVAL);
  }

  count = turbo_csv_row_count((turbo_csv_doc_t *)entry->doc);
  m3ApiWriteMem32(out_count, (uint32_t)count);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_csv_column_count) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  size_t count = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(uint32_t *, out_count)

  if (!vm || !vm->parser_registry || !out_count) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_count, sizeof(uint32_t));

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_CSV) {
    m3ApiReturn(TURBO_EINVAL);
  }

  count = turbo_csv_column_count((turbo_csv_doc_t *)entry->doc);
  m3ApiWriteMem32(out_count, (uint32_t)count);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_csv_get_cell) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  const char *cell;
  size_t cell_len;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArg(uint32_t, row)
  m3ApiGetArg(uint32_t, col)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->parser_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_CSV) {
    m3ApiReturn(TURBO_EINVAL);
  }

  cell = turbo_csv_get((turbo_csv_doc_t *)entry->doc, row, col);
  if (!cell) {
    m3ApiReturn(TURBO_ENOENT);
  }

  cell_len = strlen(cell);
  if (buffer_size > 0) {
    size_t copy_len = cell_len < buffer_size - 1 ? cell_len : buffer_size - 1;
    memcpy(buffer, cell, copy_len);
    buffer[copy_len] = '\0';
    m3ApiWriteMem32(out_written, (uint32_t)copy_len);
  } else {
    m3ApiWriteMem32(out_written, (uint32_t)cell_len);
  }

  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_csv_find_column) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *name = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  size_t col;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, name_ptr)
  m3ApiGetArg(uint32_t, name_len)
  m3ApiGetArgMem(uint32_t *, out_col)

  if (!vm || !vm->parser_registry || !out_col) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_col, sizeof(uint32_t));
  if (name_len != 0) {
    m3ApiCheckMem(name_ptr, name_len);
  }

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_CSV) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (name_len > 0) {
    name = turbo_wasm3_copy_guest_bytes(name_ptr, name_len);
    if (!name) {
      m3ApiReturn(TURBO_ENOMEM);
    }
    col = turbo_csv_find_column((turbo_csv_doc_t *)entry->doc, name);
    free(name);
  } else {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (col == (size_t)-1) {
    m3ApiReturn(TURBO_ENOENT);
  }

  m3ApiWriteMem32(out_col, (uint32_t)col);
  m3ApiReturn(0);
}

/* XML accessors */
m3ApiRawFunction(turbo_wasm3_host_xml_root_name) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  turbo_xml_node_t *root;
  const char *name;
  size_t name_len;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->parser_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_XML) {
    m3ApiReturn(TURBO_EINVAL);
  }

  root = turbo_xml_root_element((turbo_xml_doc_t *)entry->doc);
  if (!root) {
    m3ApiReturn(TURBO_ENOENT);
  }

  name = turbo_xml_node_name(root);
  if (!name) {
    m3ApiReturn(TURBO_ENOENT);
  }

  name_len = strlen(name);
  if (buffer_size > 0) {
    size_t copy_len = name_len < buffer_size - 1 ? name_len : buffer_size - 1;
    memcpy(buffer, name, copy_len);
    buffer[copy_len] = '\0';
    m3ApiWriteMem32(out_written, (uint32_t)copy_len);
  } else {
    m3ApiWriteMem32(out_written, (uint32_t)name_len);
  }

  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_xml_get_text) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *xpath = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  const char *text;
  size_t text_len;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, xpath_ptr)
  m3ApiGetArg(uint32_t, xpath_len)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->parser_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (xpath_len != 0) {
    m3ApiCheckMem(xpath_ptr, xpath_len);
  }
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_XML) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (xpath_len > 0) {
    xpath = turbo_wasm3_copy_guest_bytes(xpath_ptr, xpath_len);
    if (!xpath) {
      m3ApiReturn(TURBO_ENOMEM);
    }
    text = turbo_xml_get_text((turbo_xml_doc_t *)entry->doc, xpath);
    free(xpath);
  } else {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (!text) {
    m3ApiReturn(TURBO_ENOENT);
  }

  text_len = strlen(text);
  if (buffer_size > 0) {
    size_t copy_len = text_len < buffer_size - 1 ? text_len : buffer_size - 1;
    memcpy(buffer, text, copy_len);
    buffer[copy_len] = '\0';
    m3ApiWriteMem32(out_written, (uint32_t)copy_len);
  } else {
    m3ApiWriteMem32(out_written, (uint32_t)text_len);
  }

  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_xml_count) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *xpath = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  size_t count = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, xpath_ptr)
  m3ApiGetArg(uint32_t, xpath_len)
  m3ApiGetArgMem(uint32_t *, out_count)

  if (!vm || !vm->parser_registry || !out_count) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_count, sizeof(uint32_t));
  if (xpath_len != 0) {
    m3ApiCheckMem(xpath_ptr, xpath_len);
  }

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_XML) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (xpath_len > 0) {
    xpath = turbo_wasm3_copy_guest_bytes(xpath_ptr, xpath_len);
    if (!xpath) {
      m3ApiReturn(TURBO_ENOMEM);
    }
    count = turbo_xml_count((turbo_xml_doc_t *)entry->doc, xpath);
    free(xpath);
  } else {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiWriteMem32(out_count, (uint32_t)count);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_ini_get_string) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *section = NULL;
  char *key = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  const char *value;
  size_t value_len;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, section_ptr)
  m3ApiGetArg(uint32_t, section_len)
  m3ApiGetArgMem(const uint8_t *, key_ptr)
  m3ApiGetArg(uint32_t, key_len)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->parser_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (section_len != 0) {
    m3ApiCheckMem(section_ptr, section_len);
  }
  if (key_len != 0) {
    m3ApiCheckMem(key_ptr, key_len);
  }
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_INI) {
    m3ApiReturn(TURBO_EINVAL);
  }
  if (section_len == 0 || key_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  section = turbo_wasm3_copy_guest_bytes(section_ptr, section_len);
  if (!section) {
    m3ApiReturn(TURBO_ENOMEM);
  }
  key = turbo_wasm3_copy_guest_bytes(key_ptr, key_len);
  if (!key) {
    free(section);
    m3ApiReturn(TURBO_ENOMEM);
  }

  value = turbo_ini_get((const turbo_ini_t *)entry->doc, section, key);
  free(key);
  free(section);
  if (!value) {
    m3ApiReturn(TURBO_ENOENT);
  }

  value_len = strlen(value);
  if (buffer_size > 0) {
    size_t copy_len = value_len < buffer_size - 1 ? value_len : buffer_size - 1;
    memcpy(buffer, value, copy_len);
    buffer[copy_len] = '\0';
    m3ApiWriteMem32(out_written, (uint32_t)copy_len);
  } else {
    m3ApiWriteMem32(out_written, (uint32_t)value_len);
  }

  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_ini_get_int) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *section = NULL;
  char *key = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  const char *value;
  int32_t parsed = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, section_ptr)
  m3ApiGetArg(uint32_t, section_len)
  m3ApiGetArgMem(const uint8_t *, key_ptr)
  m3ApiGetArg(uint32_t, key_len)
  m3ApiGetArgMem(int32_t *, out_value)

  if (!vm || !vm->parser_registry || !out_value) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_value, sizeof(int32_t));
  if (section_len != 0) {
    m3ApiCheckMem(section_ptr, section_len);
  }
  if (key_len != 0) {
    m3ApiCheckMem(key_ptr, key_len);
  }
  m3ApiWriteMem32(out_value, 0);

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_INI) {
    m3ApiReturn(TURBO_EINVAL);
  }
  if (section_len == 0 || key_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  section = turbo_wasm3_copy_guest_bytes(section_ptr, section_len);
  if (!section) {
    m3ApiReturn(TURBO_ENOMEM);
  }
  key = turbo_wasm3_copy_guest_bytes(key_ptr, key_len);
  if (!key) {
    free(section);
    m3ApiReturn(TURBO_ENOMEM);
  }

  value = turbo_ini_get((const turbo_ini_t *)entry->doc, section, key);
  parsed = (int32_t)turbo_ini_get_int((const turbo_ini_t *)entry->doc, section,
                                      key, 0);
  free(key);
  free(section);
  if (!value) {
    m3ApiReturn(TURBO_ENOENT);
  }

  m3ApiWriteMem32(out_value, (uint32_t)parsed);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_ini_get_bool) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *section = NULL;
  char *key = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  const char *value;
  int32_t parsed = 0;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, section_ptr)
  m3ApiGetArg(uint32_t, section_len)
  m3ApiGetArgMem(const uint8_t *, key_ptr)
  m3ApiGetArg(uint32_t, key_len)
  m3ApiGetArgMem(int32_t *, out_value)

  if (!vm || !vm->parser_registry || !out_value) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_value, sizeof(int32_t));
  if (section_len != 0) {
    m3ApiCheckMem(section_ptr, section_len);
  }
  if (key_len != 0) {
    m3ApiCheckMem(key_ptr, key_len);
  }
  m3ApiWriteMem32(out_value, 0);

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_INI) {
    m3ApiReturn(TURBO_EINVAL);
  }
  if (section_len == 0 || key_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  section = turbo_wasm3_copy_guest_bytes(section_ptr, section_len);
  if (!section) {
    m3ApiReturn(TURBO_ENOMEM);
  }
  key = turbo_wasm3_copy_guest_bytes(key_ptr, key_len);
  if (!key) {
    free(section);
    m3ApiReturn(TURBO_ENOMEM);
  }

  value = turbo_ini_get((const turbo_ini_t *)entry->doc, section, key);
  parsed = turbo_ini_get_bool((const turbo_ini_t *)entry->doc, section, key, false)
               ? 1
               : 0;
  free(key);
  free(section);
  if (!value) {
    m3ApiReturn(TURBO_ENOENT);
  }

  m3ApiWriteMem32(out_value, (uint32_t)parsed);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_ini_get_double) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *section = NULL;
  char *key = NULL;
  int idx;
  turbo_wasm3_parser_entry_t *entry;
  const char *value;
  double parsed = 0.0;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, section_ptr)
  m3ApiGetArg(uint32_t, section_len)
  m3ApiGetArgMem(const uint8_t *, key_ptr)
  m3ApiGetArg(uint32_t, key_len)
  m3ApiGetArgMem(double *, out_value)

  if (!vm || !vm->parser_registry || !out_value) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_value, sizeof(double));
  if (section_len != 0) {
    m3ApiCheckMem(section_ptr, section_len);
  }
  if (key_len != 0) {
    m3ApiCheckMem(key_ptr, key_len);
  }
  memset(out_value, 0, sizeof(double));

  idx = turbo_wasm3_parser_registry_find(vm->parser_registry, handle);
  if (idx < 0) {
    m3ApiReturn(TURBO_EBADF);
  }

  entry = &vm->parser_registry->entries[idx];
  if (entry->type != TURBO_WASM3_PARSER_TYPE_INI) {
    m3ApiReturn(TURBO_EINVAL);
  }
  if (section_len == 0 || key_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  section = turbo_wasm3_copy_guest_bytes(section_ptr, section_len);
  if (!section) {
    m3ApiReturn(TURBO_ENOMEM);
  }
  key = turbo_wasm3_copy_guest_bytes(key_ptr, key_len);
  if (!key) {
    free(section);
    m3ApiReturn(TURBO_ENOMEM);
  }

  value = turbo_ini_get((const turbo_ini_t *)entry->doc, section, key);
  parsed =
      turbo_ini_get_double((const turbo_ini_t *)entry->doc, section, key, 0.0);
  free(key);
  free(section);
  if (!value) {
    m3ApiReturn(TURBO_ENOENT);
  }

  memcpy(out_value, &parsed, sizeof(parsed));
  m3ApiReturn(0);
}

