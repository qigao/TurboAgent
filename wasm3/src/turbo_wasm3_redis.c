#include "turbo_wasm3_internal.h"

m3ApiRawFunction(turbo_wasm3_host_redis_client_open) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *host = NULL;
  uint32_t client_handle = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, host_data)
  m3ApiGetArg(uint32_t, host_len)
  m3ApiGetArg(uint32_t, port)
  m3ApiGetArgMem(uint32_t *, out_client_handle)

  if (!vm || !vm->redis_registry || !out_client_handle || host_len == 0 ||
      port == 0 || port > 65535U) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_client_handle, sizeof(uint32_t));
  m3ApiCheckMem(host_data, host_len);
  m3ApiWriteMem32(out_client_handle, 0);

  host = turbo_wasm3_copy_guest_bytes(host_data, host_len);
  if (!host) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_redis_registry_open_client(vm->redis_registry, host,
                                              (uint16_t)port, &client_handle);
  free(host);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_client_handle, client_handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_redis_client_close) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)

  if (!vm || !vm->redis_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(
      turbo_wasm3_redis_registry_close_client(vm->redis_registry, client_handle));
}

m3ApiRawFunction(turbo_wasm3_host_redis_client_error) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->redis_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_redis_registry_client_error(vm->redis_registry, client_handle,
                                               buffer, (size_t)buffer_size,
                                               &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_redis_command) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  const uint32_t *argv_offsets = NULL;
  const uint32_t *argv_lens_mem = NULL;
  const char **argv = NULL;
  uint32_t *argv_lens = NULL;
  uint32_t reply_handle = 0;
  uint32_t i;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArg(uint32_t, argc)
  m3ApiGetArgMem(const uint32_t *, argv_offsets_arg)
  m3ApiGetArgMem(const uint32_t *, argv_lens_arg)
  m3ApiGetArgMem(uint32_t *, out_reply_handle)

  if (!vm || !vm->redis_registry || !out_reply_handle || argc == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_reply_handle, sizeof(uint32_t));
  m3ApiCheckMem(argv_offsets_arg, (size_t)argc * sizeof(uint32_t));
  m3ApiCheckMem(argv_lens_arg, (size_t)argc * sizeof(uint32_t));
  m3ApiWriteMem32(out_reply_handle, 0);

  argv_offsets = argv_offsets_arg;
  argv_lens_mem = argv_lens_arg;
  argv = (const char **)calloc(argc, sizeof(*argv));
  argv_lens = (uint32_t *)calloc(argc, sizeof(*argv_lens));
  if (!argv || !argv_lens) {
    free(argv_lens);
    free(argv);
    m3ApiReturn(TURBO_ENOMEM);
  }

  for (i = 0; i < argc; ++i) {
    uint32_t arg_len = m3ApiReadMem32(&argv_lens_mem[i]);
    uint32_t arg_offset = m3ApiReadMem32(&argv_offsets[i]);

    argv_lens[i] = arg_len;
    if (arg_len == 0) {
      argv[i] = "";
      continue;
    }

    argv[i] = (const char *)m3ApiOffsetToPtr(arg_offset);
    m3ApiCheckMem(argv[i], arg_len);
  }

  rc = turbo_wasm3_redis_registry_command(vm->redis_registry, client_handle, argc,
                                          argv, argv_lens, &reply_handle);
  free(argv_lens);
  free(argv);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_reply_handle, reply_handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_redis_reply_close) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, reply_handle)

  if (!vm || !vm->redis_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(
      turbo_wasm3_redis_registry_close_reply(vm->redis_registry, reply_handle));
}

m3ApiRawFunction(turbo_wasm3_host_redis_reply_type) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int32_t type = TURBO_WASM3_REDIS_REPLY_NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, reply_handle)
  m3ApiGetArgMem(int32_t *, out_type)

  if (!vm || !vm->redis_registry || !out_type) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_type, sizeof(int32_t));
  m3ApiWriteMem32(out_type, (uint32_t)TURBO_WASM3_REDIS_REPLY_NULL);

  rc = turbo_wasm3_redis_registry_reply_type(vm->redis_registry, reply_handle,
                                             &type);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_type, (uint32_t)type);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_redis_reply_i64) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int64_t value = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, reply_handle)
  m3ApiGetArgMem(int64_t *, out_value)

  if (!vm || !vm->redis_registry || !out_value) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_value, sizeof(int64_t));
  m3ApiWriteMem64(out_value, 0);

  rc = turbo_wasm3_redis_registry_reply_int64(vm->redis_registry, reply_handle,
                                              &value);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem64(out_value, (uint64_t)value);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_redis_reply_text) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, reply_handle)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->redis_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_redis_registry_reply_text(vm->redis_registry, reply_handle,
                                             buffer, (size_t)buffer_size,
                                             &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_redis_reply_array_len) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t array_len = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, reply_handle)
  m3ApiGetArgMem(uint32_t *, out_len)

  if (!vm || !vm->redis_registry || !out_len) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_len, sizeof(uint32_t));
  m3ApiWriteMem32(out_len, 0);

  rc = turbo_wasm3_redis_registry_reply_array_len(vm->redis_registry,
                                                  reply_handle, &array_len);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_len, array_len);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_redis_reply_array_at) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t child_handle = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, reply_handle)
  m3ApiGetArg(uint32_t, index)
  m3ApiGetArgMem(uint32_t *, out_child_reply_handle)

  if (!vm || !vm->redis_registry || !out_child_reply_handle) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_child_reply_handle, sizeof(uint32_t));
  m3ApiWriteMem32(out_child_reply_handle, 0);

  rc = turbo_wasm3_redis_registry_reply_array_at(
      vm->redis_registry, reply_handle, index, &child_handle);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_child_reply_handle, child_handle);
  m3ApiReturn(0);
}


M3Result turbo_wasm3_vm_link_redis_host(turbo_wasm3_vm_t *vm, IM3Module module) {
  const char *mod = "TurboNet";
  M3Result result = m3Err_none;

  if (!vm || !module) {
    return m3Err_wasmMalformed;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_client_open", "i(*ii*)",
      &turbo_wasm3_host_redis_client_open, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_client_close", "i(i)",
      &turbo_wasm3_host_redis_client_close, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_client_error", "i(i*i*)",
      &turbo_wasm3_host_redis_client_error, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_command", "i(ii***)", &turbo_wasm3_host_redis_command,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_reply_close", "i(i)",
      &turbo_wasm3_host_redis_reply_close, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_reply_type", "i(i*)",
      &turbo_wasm3_host_redis_reply_type, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_reply_i64", "i(i*)",
      &turbo_wasm3_host_redis_reply_i64, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_reply_text", "i(i*i*)",
      &turbo_wasm3_host_redis_reply_text, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_reply_array_len", "i(i*)",
      &turbo_wasm3_host_redis_reply_array_len, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "redis_reply_array_at", "i(ii*)",
      &turbo_wasm3_host_redis_reply_array_at, vm));
  if (result) {
    return result;
  }

  return m3Err_none;
}

/* Parser Registry */

turbo_wasm3_redis_registry_t *
turbo_wasm3_redis_registry_create(size_t initial_capacity) {
  turbo_wasm3_redis_registry_t *registry;
  int rc;

  registry = (turbo_wasm3_redis_registry_t *)calloc(1, sizeof(*registry));
  if (!registry) {
    return NULL;
  }

  registry->next_client_handle = 0x3000u;
  registry->next_reply_handle = 0x4000u;
  registry->max_clients = initial_capacity > TURBO_WASM3_DEFAULT_MAX_REDIS_CLIENTS
                              ? initial_capacity
                              : TURBO_WASM3_DEFAULT_MAX_REDIS_CLIENTS;
  registry->max_replies = TURBO_WASM3_DEFAULT_MAX_REDIS_REPLIES;

  rc = turbo_wasm3_redis_registry_reserve_clients(
      registry, initial_capacity ? initial_capacity : 4);
  if (rc != 0) {
    free(registry);
    return NULL;
  }

  rc = turbo_wasm3_redis_registry_reserve_replies(registry, 4);
  if (rc != 0) {
    free(registry->clients);
    free(registry);
    return NULL;
  }

  return registry;
}

void turbo_wasm3_redis_registry_destroy(turbo_wasm3_redis_registry_t *registry) {
  size_t i;

  if (!registry) {
    return;
  }

  for (i = 0; i < registry->reply_count; ++i) {
    turbo_wasm3_redis_reply_entry_reset(&registry->replies[i]);
  }
  for (i = 0; i < registry->client_count; ++i) {
    turbo_wasm3_redis_client_entry_clear_error(&registry->clients[i]);
    redis_client_destroy(registry->clients[i].client);
    coro_context_destroy(registry->clients[i].ctx);
  }

  free(registry->replies);
  free(registry->clients);
  free(registry);
}

int turbo_wasm3_redis_registry_set_limits(turbo_wasm3_redis_registry_t *registry,
                                          size_t max_clients,
                                          size_t max_replies) {
  if (!registry || max_clients == 0 || max_replies == 0 ||
      max_clients < registry->client_count ||
      max_replies < registry->reply_count) {
    return TURBO_EINVAL;
  }

  registry->max_clients = max_clients;
  registry->max_replies = max_replies;
  return 0;
}

int turbo_wasm3_redis_registry_open_client(turbo_wasm3_redis_registry_t *registry,
                                           const char *host, uint16_t port,
                                           uint32_t *client_handle) {
  coro_context_t *ctx = NULL;
  redis_client_t *client;
  turbo_wasm3_redis_open_task_state_t task_state;
  int rc = 0;

  if (!registry || !host || port == 0 || !client_handle) {
    return TURBO_EINVAL;
  }

  rc = turbo_wasm3_redis_registry_reserve_clients(registry,
                                                  registry->client_count + 1);
  if (rc != 0) {
    return rc;
  }

  client = redis_client_create(host, port);
  if (!client) {
    return TURBO_ENOMEM;
  }

  ctx = coro_context_create(NULL);
  if (!ctx) {
    redis_client_destroy(client);
    return TURBO_ENOMEM;
  }

  memset(&task_state, 0, sizeof(task_state));
  task_state.host = host;
  task_state.port = port;
  rc = coro_context_spawn(ctx, turbo_wasm3_redis_open_task, &task_state);
  if (rc != 0) {
    coro_context_destroy(ctx);
    redis_client_destroy(client);
    return rc;
  }
  rc = turbo_wasm3_redis_run_until_done(ctx, &task_state.done);
  if (rc != 0 || task_state.rc != 0 || !task_state.socket) {
    coro_context_destroy(ctx);
    redis_client_destroy(client);
    return task_state.rc != 0 ? task_state.rc : rc;
  }

  if (redis_client_attach_socket(client, ctx, task_state.socket, 1) != 0) {
    coro_socket_destroy(task_state.socket);
    coro_context_destroy(ctx);
    redis_client_destroy(client);
    return TURBO_EIO;
  }

  registry->clients[registry->client_count].handle = registry->next_client_handle++;
  registry->clients[registry->client_count].ctx = ctx;
  registry->clients[registry->client_count].client = client;
  registry->clients[registry->client_count].last_error = NULL;
  *client_handle = registry->clients[registry->client_count].handle;
  registry->client_count++;
  return 0;
}

int turbo_wasm3_redis_registry_close_client(turbo_wasm3_redis_registry_t *registry,
                                            uint32_t client_handle) {
  turbo_wasm3_redis_client_entry_t *entry;
  size_t i;
  size_t index;

  if (!registry) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_redis_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  for (i = 0; i < registry->reply_count;) {
    if (registry->replies[i].client_handle == client_handle) {
      turbo_wasm3_redis_reply_entry_reset(&registry->replies[i]);
      registry->replies[i] = registry->replies[registry->reply_count - 1];
      registry->reply_count--;
      continue;
    }
    ++i;
  }

  index = (size_t)(entry - registry->clients);
  turbo_wasm3_redis_client_entry_clear_error(entry);
  redis_client_destroy(entry->client);
  coro_context_destroy(entry->ctx);
  registry->clients[index] = registry->clients[registry->client_count - 1];
  registry->client_count--;
  return 0;
}

int turbo_wasm3_redis_registry_client_error(
    turbo_wasm3_redis_registry_t *registry, uint32_t client_handle, char *buffer,
    size_t buffer_size, uint32_t *out_len) {
  turbo_wasm3_redis_client_entry_t *entry;

  if (!registry || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_redis_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  return turbo_wasm3_error_buffer_write(entry->last_error, buffer, buffer_size,
                                        out_len);
}

int turbo_wasm3_redis_registry_command(turbo_wasm3_redis_registry_t *registry,
                                       uint32_t client_handle, uint32_t argc,
                                       const char *const *argv,
                                       const uint32_t *argv_lens,
                                       uint32_t *reply_handle) {
  turbo_wasm3_redis_client_entry_t *entry;
  turbo_wasm3_redis_command_task_state_t task_state;
  int rc;

  if (!registry || !reply_handle || argc == 0 || !argv || !argv_lens) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_redis_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_redis_client_entry_clear_error(entry);
  memset(&task_state, 0, sizeof(task_state));
  task_state.entry = entry;
  task_state.argc = argc;
  task_state.argv = argv;
  task_state.argv_lens = argv_lens;
  rc = coro_context_spawn(entry->ctx, turbo_wasm3_redis_command_task, &task_state);
  if (rc != 0) {
    turbo_wasm3_redis_client_entry_set_error(entry, "redis command failed", 20);
    return rc;
  }
  rc = turbo_wasm3_redis_run_until_done(entry->ctx, &task_state.done);
  if (rc != 0 || task_state.rc != 0) {
    turbo_wasm3_redis_client_entry_set_error(entry, "redis command failed", 20);
    return task_state.rc != 0 ? task_state.rc : rc;
  }
  if (!task_state.reply) {
    turbo_wasm3_redis_client_entry_set_error(entry, "redis reply missing", 19);
    return TURBO_EIO;
  }

  return turbo_wasm3_redis_registry_store_reply(registry, client_handle,
                                                task_state.reply, reply_handle);
}

int turbo_wasm3_redis_registry_close_reply(turbo_wasm3_redis_registry_t *registry,
                                           uint32_t reply_handle) {
  turbo_wasm3_redis_reply_entry_t *entry;
  size_t index;

  if (!registry) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_redis_registry_find_reply(registry, reply_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  index = (size_t)(entry - registry->replies);
  turbo_wasm3_redis_reply_entry_reset(entry);
  registry->replies[index] = registry->replies[registry->reply_count - 1];
  registry->reply_count--;
  return 0;
}

int turbo_wasm3_redis_registry_reply_type(turbo_wasm3_redis_registry_t *registry,
                                          uint32_t reply_handle,
                                          int32_t *out_type) {
  turbo_wasm3_redis_reply_entry_t *entry;

  if (!registry || !out_type) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_redis_registry_find_reply(registry, reply_handle);
  if (!entry || !entry->reply) {
    return TURBO_EBADF;
  }

  *out_type = (int32_t)entry->reply->type;
  return 0;
}

int turbo_wasm3_redis_registry_reply_int64(turbo_wasm3_redis_registry_t *registry,
                                           uint32_t reply_handle,
                                           int64_t *out_value) {
  turbo_wasm3_redis_reply_entry_t *entry;

  if (!registry || !out_value) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_redis_registry_find_reply(registry, reply_handle);
  if (!entry || !entry->reply) {
    return TURBO_EBADF;
  }
  if (entry->reply->type != REDIS_REPLY_INTEGER) {
    return TURBO_EINVAL;
  }

  *out_value = entry->reply->integer;
  return 0;
}

int turbo_wasm3_redis_registry_reply_text(turbo_wasm3_redis_registry_t *registry,
                                          uint32_t reply_handle, char *buffer,
                                          size_t buffer_size,
                                          uint32_t *out_len) {
  turbo_wasm3_redis_reply_entry_t *entry;

  if (!registry || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_redis_registry_find_reply(registry, reply_handle);
  if (!entry || !entry->reply) {
    return TURBO_EBADF;
  }

  switch (entry->reply->type) {
  case REDIS_REPLY_STRING:
  case REDIS_REPLY_ERROR:
  case REDIS_REPLY_BULK_STRING:
    return turbo_wasm3_http_copy_buffer(entry->reply->str, entry->reply->len,
                                        buffer, buffer_size, out_len);
  case REDIS_REPLY_NULL:
    buffer[0] = '\0';
    *out_len = 0;
    return 0;
  default:
    return TURBO_EINVAL;
  }
}

int turbo_wasm3_redis_registry_reply_array_len(
    turbo_wasm3_redis_registry_t *registry, uint32_t reply_handle,
    uint32_t *out_len) {
  turbo_wasm3_redis_reply_entry_t *entry;

  if (!registry || !out_len) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_redis_registry_find_reply(registry, reply_handle);
  if (!entry || !entry->reply) {
    return TURBO_EBADF;
  }
  if (entry->reply->type != REDIS_REPLY_ARRAY) {
    return TURBO_EINVAL;
  }

  *out_len = (uint32_t)entry->reply->element_count;
  return 0;
}

int turbo_wasm3_redis_registry_reply_array_at(
    turbo_wasm3_redis_registry_t *registry, uint32_t reply_handle, uint32_t index,
    uint32_t *out_child_reply_handle) {
  turbo_wasm3_redis_reply_entry_t *entry;
  const redis_reply_t *child_reply;
  uint32_t client_handle;
  redis_reply_t *copy;

  if (!registry || !out_child_reply_handle) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_redis_registry_find_reply(registry, reply_handle);
  if (!entry || !entry->reply) {
    return TURBO_EBADF;
  }
  if (entry->reply->type != REDIS_REPLY_ARRAY ||
      index >= entry->reply->element_count) {
    return TURBO_EINVAL;
  }

  child_reply = entry->reply->elements[index];
  client_handle = entry->client_handle;
  copy = turbo_wasm3_redis_reply_clone(child_reply);
  if (!copy) {
    return TURBO_ENOMEM;
  }

  {
    int rc = turbo_wasm3_redis_registry_store_reply(
        registry, client_handle, copy, out_child_reply_handle);
    if (rc != 0) {
      redis_reply_free(copy);
    }
    return rc;
  }
}
