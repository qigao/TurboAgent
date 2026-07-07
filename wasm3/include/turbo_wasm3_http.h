#ifndef TURBO_WASM3_HTTP_H
#define TURBO_WASM3_HTTP_H

#include "turbo_wasm3_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional TurboNet HTTP host module, also linked under "TurboNet":
 *   import "TurboNet" "http_client_open"       : i32 (ptr, len, out_client)
 *   import "TurboNet" "http_client_close"      : i32 (client)
 *   import "TurboNet" "http_client_set_timeout": i32 (client, timeout_ms)
 *   import "TurboNet" "http_client_set_default_header": i32 (client, name_ptr, name_len, value_ptr, value_len)
 *   import "TurboNet" "http_client_remove_default_header": i32 (client, name_ptr, name_len)
 *   import "TurboNet" "http_client_clear_default_headers": i32 (client)
 *   import "TurboNet" "http_client_set_basic_auth": i32 (client, user_ptr, user_len, pass_ptr, pass_len)
 *   import "TurboNet" "http_client_set_bearer_token": i32 (client, token_ptr, token_len)
 *   import "TurboNet" "http_client_clear_auth": i32 (client)
 *   import "TurboNet" "http_client_set_proxy": i32 (client, host_ptr, host_len, port, user_ptr, user_len, pass_ptr, pass_len)
 *   import "TurboNet" "http_client_clear_proxy": i32 (client)
 *   import "TurboNet" "http_request"           : i32 (client, method, url_ptr, url_len, body_ptr, body_len, out_resp)
 *   import "TurboNet" "http_request_with_headers": i32 (client, method, url_ptr, url_len, headers_ptr, headers_len, body_ptr, body_len, out_resp)
 *   import "TurboNet" "http_stream_get"        : i32 (client, url_ptr, url_len, out_resp)
 *   import "TurboNet" "http_sse_get"           : i32 (client, url_ptr, url_len, out_resp)
 *   import "TurboNet" "http_response_status"   : i32 (resp, out_status)
 *   import "TurboNet" "http_response_error_code": i32 (resp, out_code)
 *   import "TurboNet" "http_response_is_sse"   : i32 (resp, out_flag)
 *   import "TurboNet" "http_response_chunk_count": i32 (resp, out_count)
 *   import "TurboNet" "http_response_chunk"    : i32 (resp, index, ptr, len, out_written)
 *   import "TurboNet" "http_response_header"   : i32 (resp, name_ptr, name_len, ptr, len, out_written)
 *   import "TurboNet" "http_response_headers"  : i32 (resp, ptr, len, out_written)
 *   import "TurboNet" "http_response_body"     : i32 (resp, ptr, len, out_written)
 *   import "TurboNet" "http_response_error"    : i32 (resp, ptr, len, out_written)
 *   import "TurboNet" "http_response_close"    : i32 (resp)
 */
CXX_C_API int
turbo_wasm3_vm_enable_http_host(turbo_wasm3_vm_t *vm);

CXX_C_API M3Result
turbo_wasm3_vm_link_http_host(turbo_wasm3_vm_t *vm, IM3Module module);

CXX_C_API turbo_wasm3_http_registry_t *
turbo_wasm3_http_registry_create(size_t initial_capacity);

CXX_C_API void
turbo_wasm3_http_registry_destroy(turbo_wasm3_http_registry_t *registry);

CXX_C_API int
turbo_wasm3_http_registry_set_limits(turbo_wasm3_http_registry_t *registry,
                                     size_t max_clients,
                                     size_t max_responses,
                                     size_t max_chunks_per_response,
                                     size_t max_stream_bytes);
CXX_C_API int
turbo_wasm3_http_registry_open_client(turbo_wasm3_http_registry_t *registry,
                                      const char *base_url,
                                      uint32_t *client_handle);

CXX_C_API int
turbo_wasm3_http_registry_close_client(turbo_wasm3_http_registry_t *registry,
                                       uint32_t client_handle);

CXX_C_API int
turbo_wasm3_http_registry_set_timeout(turbo_wasm3_http_registry_t *registry,
                                      uint32_t client_handle, int timeout_ms);

CXX_C_API int
turbo_wasm3_http_registry_set_default_header(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *name, const char *value);

CXX_C_API int
turbo_wasm3_http_registry_remove_default_header(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *name);

CXX_C_API int
turbo_wasm3_http_registry_clear_default_headers(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle);

CXX_C_API int
turbo_wasm3_http_registry_set_basic_auth(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *username, const char *password);

CXX_C_API int
turbo_wasm3_http_registry_set_bearer_token(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *token);

CXX_C_API int
turbo_wasm3_http_registry_clear_auth(turbo_wasm3_http_registry_t *registry,
                                     uint32_t client_handle);

CXX_C_API int
turbo_wasm3_http_registry_set_proxy(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *host, uint16_t port, const char *username,
    const char *password);

CXX_C_API int
turbo_wasm3_http_registry_clear_proxy(turbo_wasm3_http_registry_t *registry,
                                      uint32_t client_handle);

CXX_C_API int
turbo_wasm3_http_registry_request(turbo_wasm3_http_registry_t *registry,
                                  uint32_t client_handle, int method,
                                  const char *url, const void *body,
                                  size_t body_len, uint32_t *response_handle);

CXX_C_API int
turbo_wasm3_http_registry_request_with_headers(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle, int method,
    const char *url, const char *headers, size_t headers_len, const void *body,
    size_t body_len, uint32_t *response_handle);

CXX_C_API int
turbo_wasm3_http_registry_stream_get(turbo_wasm3_http_registry_t *registry,
                                     uint32_t client_handle, const char *url,
                                     uint32_t *response_handle);

CXX_C_API int
turbo_wasm3_http_registry_sse_get(turbo_wasm3_http_registry_t *registry,
                                  uint32_t client_handle, const char *url,
                                  uint32_t *response_handle);

CXX_C_API int
turbo_wasm3_http_registry_response_status(turbo_wasm3_http_registry_t *registry,
                                          uint32_t response_handle,
                                          int32_t *out_status);

CXX_C_API int
turbo_wasm3_http_registry_response_error_code(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    int32_t *out_error_code);

CXX_C_API int
turbo_wasm3_http_registry_response_is_sse(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    int32_t *out_is_sse);

CXX_C_API int
turbo_wasm3_http_registry_response_chunk_count(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    uint32_t *out_count);

CXX_C_API int
turbo_wasm3_http_registry_response_chunk(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    uint32_t chunk_index, void *buffer, size_t buffer_size, uint32_t *out_len);

CXX_C_API int
turbo_wasm3_http_registry_response_header(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    const char *header_name, char *buffer, size_t buffer_size, uint32_t *out_len);

CXX_C_API int
turbo_wasm3_http_registry_response_headers(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle, char *buffer,
    size_t buffer_size, uint32_t *out_len);

CXX_C_API int
turbo_wasm3_http_registry_response_body(turbo_wasm3_http_registry_t *registry,
                                        uint32_t response_handle, void *buffer,
                                        size_t buffer_size, uint32_t *out_len);

CXX_C_API int
turbo_wasm3_http_registry_response_error(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle, char *buffer,
    size_t buffer_size, uint32_t *out_len);

CXX_C_API int
turbo_wasm3_http_registry_close_response(turbo_wasm3_http_registry_t *registry,
                                         uint32_t response_handle);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_WASM3_HTTP_H */
