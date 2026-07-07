#ifndef TURBO_WASM3_REDIS_H
#define TURBO_WASM3_REDIS_H

#include "turbo_wasm3_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional TurboNet Redis host module, also linked under "TurboNet":
 *   import "TurboNet" "redis_client_open"  : i32 (host_ptr, host_len, port, out_client)
 *   import "TurboNet" "redis_client_close" : i32 (client)
 *   import "TurboNet" "redis_client_error" : i32 (client, ptr, len, out_written)
 *   import "TurboNet" "redis_command"      : i32 (client, argc, argv_ptr, argv_len_ptr, out_reply)
 *   import "TurboNet" "redis_reply_close"  : i32 (reply)
 *   import "TurboNet" "redis_reply_type"   : i32 (reply, out_type)
 *   import "TurboNet" "redis_reply_i64"    : i32 (reply, out_value)
 *   import "TurboNet" "redis_reply_text"   : i32 (reply, ptr, len, out_written)
 *   import "TurboNet" "redis_reply_array_len": i32 (reply, out_len)
 *   import "TurboNet" "redis_reply_array_at": i32 (reply, index, out_child_reply)
 */
CXX_C_API int
turbo_wasm3_vm_enable_redis_host(turbo_wasm3_vm_t *vm);

CXX_C_API M3Result
turbo_wasm3_vm_link_redis_host(turbo_wasm3_vm_t *vm, IM3Module module);

CXX_C_API turbo_wasm3_redis_registry_t *
turbo_wasm3_redis_registry_create(size_t initial_capacity);

CXX_C_API void
turbo_wasm3_redis_registry_destroy(turbo_wasm3_redis_registry_t *registry);

CXX_C_API int
turbo_wasm3_redis_registry_set_limits(turbo_wasm3_redis_registry_t *registry,
                                      size_t max_clients, size_t max_replies);
CXX_C_API int
turbo_wasm3_redis_registry_open_client(turbo_wasm3_redis_registry_t *registry,
                                       const char *host, uint16_t port,
                                       uint32_t *client_handle);

CXX_C_API int
turbo_wasm3_redis_registry_close_client(turbo_wasm3_redis_registry_t *registry,
                                        uint32_t client_handle);

CXX_C_API int
turbo_wasm3_redis_registry_client_error(
    turbo_wasm3_redis_registry_t *registry, uint32_t client_handle, char *buffer,
    size_t buffer_size, uint32_t *out_len);

CXX_C_API int
turbo_wasm3_redis_registry_command(turbo_wasm3_redis_registry_t *registry,
                                   uint32_t client_handle, uint32_t argc,
                                   const char *const *argv,
                                   const uint32_t *argv_lens,
                                   uint32_t *reply_handle);

CXX_C_API int
turbo_wasm3_redis_registry_close_reply(turbo_wasm3_redis_registry_t *registry,
                                       uint32_t reply_handle);

CXX_C_API int
turbo_wasm3_redis_registry_reply_type(turbo_wasm3_redis_registry_t *registry,
                                      uint32_t reply_handle, int32_t *out_type);

CXX_C_API int
turbo_wasm3_redis_registry_reply_int64(turbo_wasm3_redis_registry_t *registry,
                                       uint32_t reply_handle,
                                       int64_t *out_value);

CXX_C_API int
turbo_wasm3_redis_registry_reply_text(turbo_wasm3_redis_registry_t *registry,
                                      uint32_t reply_handle, char *buffer,
                                      size_t buffer_size, uint32_t *out_len);

CXX_C_API int
turbo_wasm3_redis_registry_reply_array_len(
    turbo_wasm3_redis_registry_t *registry, uint32_t reply_handle,
    uint32_t *out_len);

CXX_C_API int
turbo_wasm3_redis_registry_reply_array_at(
    turbo_wasm3_redis_registry_t *registry, uint32_t reply_handle,
    uint32_t index, uint32_t *out_child_reply_handle);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_WASM3_REDIS_H */
