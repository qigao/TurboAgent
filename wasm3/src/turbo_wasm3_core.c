#include "turbo_wasm3_internal.h"

m3ApiRawFunction(turbo_wasm3_host_abi_version) {
  m3ApiReturnType(uint32_t)

  m3ApiReturn(TURBO_WASM3_HOST_ABI_VERSION);
}

m3ApiRawFunction(turbo_wasm3_host_clock_time_ms) {
  m3ApiReturnType(uint64_t)

  m3ApiReturn(turbo_realtime_ms());
}


static M3Result turbo_wasm3_vm_link_http_host_cb(turbo_wasm3_vm_t *vm,
                                                 IM3Module module,
                                                 void *user_data) {
  (void)user_data;
  return turbo_wasm3_vm_link_http_host(vm, module);
}

static M3Result turbo_wasm3_vm_link_redis_host_cb(turbo_wasm3_vm_t *vm,
                                                  IM3Module module,
                                                  void *user_data) {
  (void)user_data;
  return turbo_wasm3_vm_link_redis_host(vm, module);
}

static M3Result turbo_wasm3_vm_link_host_cb(turbo_wasm3_vm_t *vm,
                                            IM3Module module,
                                            void *user_data) {
  (void)user_data;
  return turbo_wasm3_vm_link_host(vm, module);
}

turbo_wasm3_vm_t *
turbo_wasm3_vm_create(uint32_t stack_size, void *runtime_user_data,
                      size_t socket_capacity) {
  turbo_wasm3_vm_t *vm;

  vm = (turbo_wasm3_vm_t *)calloc(1, sizeof(*vm));
  if (!vm) {
    return NULL;
  }

  vm->env = m3_NewEnvironment();
  if (!vm->env) {
    turbo_wasm3_vm_destroy(vm);
    return NULL;
  }

  vm->runtime = m3_NewRuntime(vm->env, stack_size ? stack_size : (64U * 1024U),
                              runtime_user_data);
  if (!vm->runtime) {
    turbo_wasm3_vm_destroy(vm);
    return NULL;
  }

  vm->socket_registry =
      turbo_wasm3_socket_registry_create(socket_capacity ? socket_capacity : 16);
  if (!vm->socket_registry) {
    turbo_wasm3_vm_destroy(vm);
    return NULL;
  }

  vm->db_registry = turbo_wasm3_db_registry_create(8);
  if (!vm->db_registry) {
    turbo_wasm3_vm_destroy(vm);
    return NULL;
  }

  vm->http_registry = turbo_wasm3_http_registry_create(4);
  if (!vm->http_registry) {
    turbo_wasm3_vm_destroy(vm);
    return NULL;
  }

  vm->redis_registry = turbo_wasm3_redis_registry_create(4);
  if (!vm->redis_registry) {
    turbo_wasm3_vm_destroy(vm);
    return NULL;
  }

  vm->parser_registry = turbo_wasm3_parser_registry_create(8);
  if (!vm->parser_registry) {
    turbo_wasm3_vm_destroy(vm);
    return NULL;
  }

  vm->wasi_context = m3_NewWasiContext();
  if (!vm->wasi_context) {
    turbo_wasm3_vm_destroy(vm);
    return NULL;
  }

  if (turbo_wasm3_vm_bind_wasi(vm) != m3Err_none) {
    turbo_wasm3_vm_destroy(vm);
    return NULL;
  }

  return vm;
}

void turbo_wasm3_vm_destroy(turbo_wasm3_vm_t *vm) {
  size_t i;

  if (!vm) {
    return;
  }

  if (vm->wasi_context) {
    turbo_wasm3_socket_registry_unbind_wasi(vm->wasi_context);
  }
  turbo_wasm3_vm_clear_wasi_args(vm);
  turbo_wasm3_vm_clear_host_linkers(vm);
  turbo_wasm3_http_registry_destroy(vm->http_registry);
  vm->http_registry = NULL;
  turbo_wasm3_redis_registry_destroy(vm->redis_registry);
  vm->redis_registry = NULL;
  turbo_wasm3_db_registry_destroy(vm->db_registry);
  vm->db_registry = NULL;
  turbo_wasm3_parser_registry_destroy(vm->parser_registry);
  vm->parser_registry = NULL;
  turbo_wasm3_socket_registry_destroy(vm->socket_registry);
  vm->socket_registry = NULL;

  if (vm->runtime) {
    m3_FreeRuntime(vm->runtime);
    vm->runtime = NULL;
  }
  if (vm->wasi_context) {
    m3_FreeWasiContext(vm->wasi_context);
    vm->wasi_context = NULL;
  }
  if (vm->env) {
    m3_FreeEnvironment(vm->env);
    vm->env = NULL;
  }

  for (i = 0; i < vm->blob_count; ++i) {
    free(vm->blobs[i].bytes);
  }
  free(vm->blobs);
  free(vm);
}

IM3Runtime turbo_wasm3_vm_get_runtime(turbo_wasm3_vm_t *vm) {
  return vm ? vm->runtime : NULL;
}

m3_wasi_context_t *turbo_wasm3_vm_get_wasi_context(turbo_wasm3_vm_t *vm) {
  return vm ? vm->wasi_context : NULL;
}

turbo_wasm3_db_registry_t *turbo_wasm3_vm_get_db_registry(turbo_wasm3_vm_t *vm) {
  return vm ? vm->db_registry : NULL;
}

turbo_wasm3_http_registry_t *turbo_wasm3_vm_get_http_registry(turbo_wasm3_vm_t *vm) {
  return vm ? vm->http_registry : NULL;
}

turbo_wasm3_redis_registry_t *turbo_wasm3_vm_get_redis_registry(turbo_wasm3_vm_t *vm) {
  return vm ? vm->redis_registry : NULL;
}

turbo_wasm3_parser_registry_t *turbo_wasm3_vm_get_parser_registry(turbo_wasm3_vm_t *vm) {
  return vm ? vm->parser_registry : NULL;
}

void turbo_wasm3_vm_set_host_user_data(turbo_wasm3_vm_t *vm,
                                       void *host_user_data) {
  if (!vm) {
    return;
  }

  vm->host_user_data = host_user_data;
}

void *turbo_wasm3_vm_get_host_user_data(turbo_wasm3_vm_t *vm) {
  return vm ? vm->host_user_data : NULL;
}

int turbo_wasm3_vm_add_host_linker(turbo_wasm3_vm_t *vm,
                                   turbo_wasm3_host_linker_fn linker,
                                   void *user_data) {
  size_t i;
  int rc;

  if (!vm || !linker) {
    return TURBO_EINVAL;
  }

  for (i = 0; i < vm->host_linker_count; ++i) {
    if (vm->host_linkers[i].linker == linker &&
        vm->host_linkers[i].user_data == user_data) {
      return 0;
    }
  }

  rc = turbo_wasm3_vm_reserve_host_linkers(vm, vm->host_linker_count + 1);
  if (rc != 0) {
    return rc;
  }

  vm->host_linkers[vm->host_linker_count].linker = linker;
  vm->host_linkers[vm->host_linker_count].user_data = user_data;
  vm->host_linker_count++;
  return 0;
}

void turbo_wasm3_vm_clear_host_linkers(turbo_wasm3_vm_t *vm) {
  if (!vm) {
    return;
  }

  free(vm->host_linkers);
  vm->host_linkers = NULL;
  vm->host_linker_count = 0;
  vm->host_linker_capacity = 0;
}

M3Result turbo_wasm3_vm_link_host_modules(turbo_wasm3_vm_t *vm,
                                          IM3Module module) {
  size_t i;
  M3Result result = m3Err_none;

  if (!vm || !module) {
    return m3Err_wasmMalformed;
  }

  for (i = 0; i < vm->host_linker_count; ++i) {
    result = vm->host_linkers[i].linker(vm, module, vm->host_linkers[i].user_data);
    if (result) {
      return result;
    }
  }

  return m3Err_none;
}

int turbo_wasm3_vm_enable_host(turbo_wasm3_vm_t *vm) {
  return turbo_wasm3_vm_add_host_linker(vm, turbo_wasm3_vm_link_host_cb, NULL);
}

int turbo_wasm3_vm_enable_http_host(turbo_wasm3_vm_t *vm) {
  return turbo_wasm3_vm_add_host_linker(vm, turbo_wasm3_vm_link_http_host_cb, NULL);
}

int turbo_wasm3_vm_enable_redis_host(turbo_wasm3_vm_t *vm) {
  return turbo_wasm3_vm_add_host_linker(vm, turbo_wasm3_vm_link_redis_host_cb,
                                        NULL);
}

M3Result turbo_wasm3_vm_link_host(turbo_wasm3_vm_t *vm, IM3Module module) {
  const char *mod = "TurboNet";
  M3Result result = m3Err_none;

  if (!vm || !module) {
    return m3Err_wasmMalformed;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "abi_version", "i()", &turbo_wasm3_host_abi_version, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "clock_time_ms", "I()", &turbo_wasm3_host_clock_time_ms,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "socket_send", "i(i*i*)", &turbo_wasm3_host_socket_send,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "socket_recv", "i(i*i*)", &turbo_wasm3_host_socket_recv,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "socket_release", "i(i)",
      &turbo_wasm3_host_socket_release, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_open", "i(*i*)", &turbo_wasm3_host_db_open, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_close", "i(i)", &turbo_wasm3_host_db_close, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_exec", "i(i*i*)", &turbo_wasm3_host_db_exec, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_error", "i(i*i*)", &turbo_wasm3_host_db_error, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_stmt_error", "i(i*i*)", &turbo_wasm3_host_db_stmt_error,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_prepare", "i(i*i*)", &turbo_wasm3_host_db_prepare, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_finalize", "i(i)", &turbo_wasm3_host_db_finalize, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_reset", "i(i)", &turbo_wasm3_host_db_reset, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_step", "i(i*)", &turbo_wasm3_host_db_step, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_bind_i64", "i(iiI)", &turbo_wasm3_host_db_bind_i64, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_bind_f64", "i(iiF)", &turbo_wasm3_host_db_bind_f64, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_bind_null", "i(ii)", &turbo_wasm3_host_db_bind_null, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_bind_blob", "i(ii*i)", &turbo_wasm3_host_db_bind_blob,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_bind_text", "i(ii*i)", &turbo_wasm3_host_db_bind_text,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_column_type", "i(ii*)", &turbo_wasm3_host_db_column_type,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_column_i64", "i(ii*)", &turbo_wasm3_host_db_column_i64,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_column_f64", "i(ii*)", &turbo_wasm3_host_db_column_f64,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_column_blob", "i(ii*i*)",
      &turbo_wasm3_host_db_column_blob, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "db_column_text", "i(ii*i*)",
      &turbo_wasm3_host_db_column_text, vm));
  if (result) {
    return result;
  }

  /* Parser functions */
  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "parser_json_parse", "i(*i*)", &turbo_wasm3_host_parser_json_parse, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "parser_csv_parse", "i(*i*)", &turbo_wasm3_host_parser_csv_parse, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "parser_xml_parse", "i(*i*)", &turbo_wasm3_host_parser_xml_parse, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "parser_ini_parse", "i(*i*)", &turbo_wasm3_host_parser_ini_parse, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "parser_free", "i(i)", &turbo_wasm3_host_parser_free, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "json_get_string", "i(i*i*i*)", &turbo_wasm3_host_json_get_string, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "json_get_int", "i(i*i*)", &turbo_wasm3_host_json_get_int, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "json_get_bool", "i(i*i*)", &turbo_wasm3_host_json_get_bool, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "json_array_size", "i(i*i*)", &turbo_wasm3_host_json_array_size, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "csv_row_count", "i(i*)", &turbo_wasm3_host_csv_row_count, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "csv_column_count", "i(i*)", &turbo_wasm3_host_csv_column_count, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "csv_get_cell", "i(iii*i*)", &turbo_wasm3_host_csv_get_cell, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "csv_find_column", "i(i*i*)", &turbo_wasm3_host_csv_find_column, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "xml_root_name", "i(i*i*)", &turbo_wasm3_host_xml_root_name, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "xml_get_text", "i(i*i*i*)", &turbo_wasm3_host_xml_get_text, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "xml_count", "i(i*i*)", &turbo_wasm3_host_xml_count, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "ini_get_string", "i(i*i*i*i*)",
      &turbo_wasm3_host_ini_get_string, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "ini_get_int", "i(i*i*i*)", &turbo_wasm3_host_ini_get_int,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "ini_get_bool", "i(i*i*i*)", &turbo_wasm3_host_ini_get_bool,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "ini_get_double", "i(i*i*i*)",
      &turbo_wasm3_host_ini_get_double, vm));
  if (result) {
    return result;
  }

  return m3Err_none;
}


int turbo_wasm3_vm_set_wasi_args(turbo_wasm3_vm_t *vm, uint32_t argc,
                                 const char *const *argv) {
  char **argv_copy = NULL;
  uint32_t i;

  if (!vm || !vm->wasi_context) {
    return TURBO_EINVAL;
  }
  if (argc != 0 && !argv) {
    return TURBO_EINVAL;
  }

  turbo_wasm3_vm_clear_wasi_args(vm);

  if (argc == 0) {
    return 0;
  }

  argv_copy = (char **)calloc(argc, sizeof(*argv_copy));
  if (!argv_copy) {
    return TURBO_ENOMEM;
  }

  for (i = 0; i < argc; ++i) {
    uint32_t j;

    if (!argv[i]) {
      for (j = 0; j < i; ++j) {
        free(argv_copy[j]);
      }
      free(argv_copy);
      return TURBO_EINVAL;
    }

    argv_copy[i] = turbo_wasm3_strdup(argv[i]);
    if (!argv_copy[i]) {
      for (j = 0; j < i; ++j) {
        free(argv_copy[j]);
      }
      free(argv_copy);
      return TURBO_ENOMEM;
    }
  }

  vm->wasi_argv = argv_copy;
  vm->wasi_argc = argc;
  vm->wasi_context->argc = argc;
  vm->wasi_context->argv = (ccstr_t *)vm->wasi_argv;
  return 0;
}

void turbo_wasm3_vm_reset_preopens(turbo_wasm3_vm_t *vm) {
  if (!vm || !vm->wasi_context) {
    return;
  }

  m3_wasi_context_reset_preopens(vm->wasi_context);
}

int turbo_wasm3_vm_set_preopen(turbo_wasm3_vm_t *vm, uint32_t fd,
                               const char *guest_path, const char *host_path) {
  if (!vm || !vm->wasi_context) {
    return TURBO_EINVAL;
  }

  return m3_wasi_context_set_preopen(vm->wasi_context, fd, guest_path, host_path);
}

int turbo_wasm3_vm_remove_preopen(turbo_wasm3_vm_t *vm, uint32_t fd) {
  if (!vm || !vm->wasi_context) {
    return TURBO_EINVAL;
  }

  return m3_wasi_context_remove_preopen(vm->wasi_context, fd);
}

M3Result turbo_wasm3_vm_load_module(turbo_wasm3_vm_t *vm, const uint8_t *wasm_bytes,
                                    uint32_t wasm_size,
                                    const char *module_name,
                                    IM3Module *out_module) {
  size_t blob_index;
  const uint8_t *owned_bytes = NULL;
  IM3Module module = NULL;
  M3Result result;

  if (!vm || !vm->env || !vm->runtime || !wasm_bytes || !wasm_size) {
    return m3Err_wasmMalformed;
  }

  blob_index = vm->blob_count;
  result = turbo_wasm3_vm_store_blob(vm, wasm_bytes, wasm_size, &owned_bytes);
  if (result) {
    return result;
  }

  result = m3_ParseModule(vm->env, &module, owned_bytes, wasm_size);
  if (result) {
    free(vm->blobs[blob_index].bytes);
    vm->blob_count = blob_index;
    return result;
  }

  result = m3_LoadModule(vm->runtime, module);
  if (result) {
    m3_FreeModule(module);
    free(vm->blobs[blob_index].bytes);
    vm->blob_count = blob_index;
    return result;
  }

  if (module_name && module_name[0] != '\0') {
    m3_SetModuleName(module, module_name);
  }

  result = m3_LinkWASIWithContext(module, vm->wasi_context);
  if (result) {
    return result;
  }

  result = turbo_wasm3_vm_bind_wasi(vm);
  if (result) {
    return result;
  }

  result = turbo_wasm3_vm_link_host_modules(vm, module);
  if (result) {
    return result;
  }

  if (out_module) {
    *out_module = module;
  }
  return m3Err_none;
}

M3Result turbo_wasm3_vm_load_module_file(turbo_wasm3_vm_t *vm, const char *path,
                                         const char *module_name,
                                         IM3Module *out_module) {
  turbo_fs_buf_t wasm = {0};
  M3Result result;

  if (!vm || !path) {
    return m3Err_wasmMalformed;
  }

  if (turbo_fs_read_file(path, &wasm) != 0) {
    return "failed to read wasm module";
  }
  if (wasm.len == 0 || wasm.len > UINT32_MAX) {
    turbo_fs_buf_free(&wasm);
    return m3Err_wasmOverrun;
  }

  result = turbo_wasm3_vm_load_module(vm, (const uint8_t *)wasm.base,
                                      (uint32_t)wasm.len, module_name,
                                      out_module);
  turbo_fs_buf_free(&wasm);
  return result;
}

