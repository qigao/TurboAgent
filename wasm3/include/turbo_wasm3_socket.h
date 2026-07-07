#ifndef TURBO_WASM3_SOCKET_H
#define TURBO_WASM3_SOCKET_H

#include "turbo_wasm3_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Registered sockets are borrowed, not owned.
 * Execute wasm3 from the same CoroNet scheduling context that owns them.
 */
CXX_C_API turbo_wasm3_socket_registry_t *
turbo_wasm3_socket_registry_create(size_t initial_capacity);

CXX_C_API void
turbo_wasm3_socket_registry_destroy(turbo_wasm3_socket_registry_t *registry);

CXX_C_API int
turbo_wasm3_socket_registry_set_limit(turbo_wasm3_socket_registry_t *registry,
                                      size_t max_sockets);

CXX_C_API int
turbo_wasm3_socket_registry_bind_wasi(turbo_wasm3_socket_registry_t *registry,
                                      struct m3_wasi_context_t *wasi_context);

CXX_C_API int
turbo_wasm3_socket_registry_unbind_wasi(struct m3_wasi_context_t *wasi_context);

CXX_C_API int
turbo_wasm3_socket_registry_register(turbo_wasm3_socket_registry_t *registry,
                                     struct coro_socket_s *socket,
                                     uint32_t *wasi_fd);

CXX_C_API int
turbo_wasm3_socket_registry_unregister(turbo_wasm3_socket_registry_t *registry,
                                       uint32_t wasi_fd);

CXX_C_API struct coro_socket_s *
turbo_wasm3_socket_registry_lookup(turbo_wasm3_socket_registry_t *registry,
                                   uint32_t wasi_fd);

CXX_C_API int
turbo_wasm3_vm_register_socket(turbo_wasm3_vm_t *vm, struct coro_socket_s *socket,
                               uint32_t *wasi_fd);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_WASM3_SOCKET_H */
