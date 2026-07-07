#include "turbo_wasm3_internal.h"

m3ApiRawFunction(turbo_wasm3_host_socket_send) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  turbo_wasm3_socket_entry_t *entry;

  m3ApiReturnType(int32_t) m3ApiGetArg(uint32_t, handle) m3ApiGetArgMem(const uint8_t *, data)
      m3ApiGetArg(uint32_t, data_len) m3ApiGetArgMem(uint32_t *, sent_len)

          if (!vm || !vm->socket_registry || !sent_len) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(sent_len, sizeof(uint32_t));
  m3ApiWriteMem32(sent_len, 0);
  entry = turbo_wasm3_socket_registry_find_entry(vm->socket_registry, handle);
  if (!entry) {
    m3ApiReturn(TURBO_EBADF);
  }
  if (data_len == 0) {
    m3ApiReturn(0);
  }

  m3ApiCheckMem(data, data_len);
  {
    int rc = coro_socket_send(entry->socket, (const char *)data, (size_t)data_len);
    if (rc != 0) {
      m3ApiReturn(rc);
    }
  }

  m3ApiWriteMem32(sent_len, data_len);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_socket_recv) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  turbo_wasm3_socket_entry_t *entry;
  char *recv_data = NULL;
  size_t recv_len = 0;

  m3ApiReturnType(int32_t) m3ApiGetArg(uint32_t, handle) m3ApiGetArgMem(uint8_t *, data)
      m3ApiGetArg(uint32_t, data_len) m3ApiGetArgMem(uint32_t *, recv_len_out)

          if (!vm || !vm->socket_registry || !recv_len_out) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(recv_len_out, sizeof(uint32_t));
  m3ApiWriteMem32(recv_len_out, 0);
  entry = turbo_wasm3_socket_registry_find_entry(vm->socket_registry, handle);
  if (!entry) {
    m3ApiReturn(TURBO_EBADF);
  }
  if (data_len != 0) {
    m3ApiCheckMem(data, data_len);
  }

  {
    int rc = coro_socket_recv(entry->socket, &recv_data, &recv_len);
    size_t copy_len = recv_len;

    if (rc != 0) {
      if (recv_data) {
        coro_socket_free_recv(recv_data);
      }
      m3ApiReturn(rc);
    }

    if (copy_len > data_len) {
      copy_len = data_len;
    }
    if (copy_len != 0 && data && recv_data) {
      memcpy(data, recv_data, copy_len);
    }
    if (recv_data) {
      coro_socket_free_recv(recv_data);
    }

    m3ApiWriteMem32(recv_len_out, (uint32_t)copy_len);
    if (recv_len > data_len) {
      m3ApiReturn(TURBO_EMSGSIZE);
    }
  }

  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_socket_release) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t) m3ApiGetArg(uint32_t, handle)

      if (!vm || !vm->socket_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(turbo_wasm3_socket_registry_unregister(vm->socket_registry, handle));
}

turbo_wasm3_socket_registry_t *turbo_wasm3_socket_registry_create(size_t initial_capacity) {
  turbo_wasm3_socket_registry_t *registry;
  int rc;

  registry = (turbo_wasm3_socket_registry_t *)calloc(1, sizeof(*registry));
  if (!registry) {
    return NULL;
  }

  registry->next_handle = 64;
  registry->max_entries = initial_capacity > TURBO_WASM3_DEFAULT_MAX_SOCKETS
                              ? initial_capacity
                              : TURBO_WASM3_DEFAULT_MAX_SOCKETS;
  rc = turbo_wasm3_socket_registry_reserve(registry, initial_capacity ? initial_capacity : 16);
  if (rc != 0) {
    free(registry);
    return NULL;
  }

  return registry;
}

int turbo_wasm3_socket_registry_set_limit(turbo_wasm3_socket_registry_t *registry,
                                          size_t max_entries) {
  if (!registry || max_entries == 0 || max_entries < registry->count) {
    return TURBO_EINVAL;
  }

  registry->max_entries = max_entries;
  return 0;
}

void turbo_wasm3_socket_registry_destroy(turbo_wasm3_socket_registry_t *registry) {
  if (!registry) {
    return;
  }

  free(registry->entries);
  free(registry);
}

int turbo_wasm3_socket_registry_bind_wasi(turbo_wasm3_socket_registry_t *registry,
                                          m3_wasi_context_t *wasi_context) {
  if (!registry || !wasi_context) {
    return TURBO_EINVAL;
  }

  m3_wasi_context_set_socket_ops(wasi_context, &turbo_wasm3_socket_ops, registry);
  return 0;
}

int turbo_wasm3_socket_registry_unbind_wasi(m3_wasi_context_t *wasi_context) {
  if (!wasi_context) {
    return TURBO_EINVAL;
  }

  m3_wasi_context_set_socket_ops(wasi_context, NULL, NULL);
  return 0;
}

int turbo_wasm3_socket_registry_register(turbo_wasm3_socket_registry_t *registry,
                                         coro_socket_t *socket, uint32_t *wasi_fd) {
  int rc;

  if (!registry || !socket || !wasi_fd) {
    return TURBO_EINVAL;
  }

  rc = turbo_wasm3_socket_registry_reserve(registry, registry->count + 1);
  if (rc != 0) {
    return rc;
  }

  registry->entries[registry->count].handle = registry->next_handle++;
  registry->entries[registry->count].socket = socket;
  *wasi_fd = registry->entries[registry->count].handle;
  registry->count++;
  return 0;
}

int turbo_wasm3_vm_register_socket(turbo_wasm3_vm_t *vm, coro_socket_t *socket, uint32_t *wasi_fd) {
  if (!vm) {
    return TURBO_EINVAL;
  }

  return turbo_wasm3_socket_registry_register(vm->socket_registry, socket, wasi_fd);
}

int turbo_wasm3_socket_registry_unregister(turbo_wasm3_socket_registry_t *registry,
                                           uint32_t wasi_fd) {
  size_t i;

  if (!registry) {
    return TURBO_EINVAL;
  }

  for (i = 0; i < registry->count; ++i) {
    if (registry->entries[i].handle == wasi_fd) {
      registry->entries[i] = registry->entries[registry->count - 1];
      registry->count--;
      return 0;
    }
  }

  return TURBO_EBADF;
}

coro_socket_t *turbo_wasm3_socket_registry_lookup(turbo_wasm3_socket_registry_t *registry,
                                                  uint32_t wasi_fd) {
  turbo_wasm3_socket_entry_t *entry = turbo_wasm3_socket_registry_find_entry(registry, wasi_fd);

  return entry ? entry->socket : NULL;
}
