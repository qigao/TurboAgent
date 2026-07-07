#include <stdint.h>

#include "turbo_wasm3_guest_abi.h"

TURBONET_IMPORT("http_client_open")
extern int32_t turbonet_http_client_open(const char *base_url, uint32_t base_url_len,
                                         uint32_t *out_client);

TURBONET_IMPORT("http_client_close")
extern int32_t turbonet_http_client_close(uint32_t client_handle);

TURBONET_IMPORT("http_client_set_timeout")
extern int32_t turbonet_http_client_set_timeout(uint32_t client_handle, int32_t timeout_ms);

TURBONET_IMPORT("http_request")
extern int32_t turbonet_http_request(uint32_t client_handle, int32_t method, const char *url,
                                     uint32_t url_len, const void *body, uint32_t body_len,
                                     uint32_t *out_response);

TURBONET_IMPORT("http_request_with_headers")
extern int32_t turbonet_http_request_with_headers(uint32_t client_handle, int32_t method,
                                                  const char *url, uint32_t url_len,
                                                  const char *headers, uint32_t headers_len,
                                                  const void *body, uint32_t body_len,
                                                  uint32_t *out_response);

TURBONET_IMPORT("http_response_status")
extern int32_t turbonet_http_response_status(uint32_t response_handle, int32_t *out_status);

TURBONET_IMPORT("http_response_error_code")
extern int32_t turbonet_http_response_error_code(uint32_t response_handle, int32_t *out_error_code);

TURBONET_IMPORT("http_response_header")
extern int32_t turbonet_http_response_header(uint32_t response_handle, const char *name,
                                             uint32_t name_len, char *buffer, uint32_t buffer_size,
                                             uint32_t *out_written);

TURBONET_IMPORT("http_response_error")
extern int32_t turbonet_http_response_error(uint32_t response_handle, char *buffer,
                                            uint32_t buffer_size, uint32_t *out_written);

TURBONET_IMPORT("http_response_close")
extern int32_t turbonet_http_response_close(uint32_t response_handle);

enum {
  TURBO_WASM3_HTTP_METHOD_GET = 1,
};

static uint32_t cstrlen(const char *s) {
  uint32_t len = 0;
  while (s[len] != '\0') {
    ++len;
  }
  return len;
}

TURBONET_EXPORT("run_http_demo")
int32_t run_http_demo(void) {
  static const char base_url[] = "http://localhost:8080";
  static const char bad_url[] = "not-a-valid-url";
  static const char headers[] = "Accept: application/json\nX-TurboNet-Demo: wasm3-http";
  static const char content_type[] = "Content-Type";
  uint32_t client = 0;
  uint32_t response = 0;
  uint32_t header_len = 0;
  uint32_t err_len = 0;
  int32_t status = -1;
  int32_t error_code = 0;
  char header_buf[32];
  char err_buf[96];
  int32_t rc = 0;

  rc = turbonet_http_client_open(base_url, cstrlen(base_url), &client);
  if (rc != 0) {
    return 10 + rc;
  }

  rc = turbonet_http_client_set_timeout(client, 100);
  if (rc != 0) {
    return 20 + rc;
  }

  rc = turbonet_http_request_with_headers(client, TURBO_WASM3_HTTP_METHOD_GET, bad_url,
                                          cstrlen(bad_url), headers, cstrlen(headers), 0, 0,
                                          &response);
  if (rc != 0) {
    return 30 + rc;
  }

  rc = turbonet_http_response_status(response, &status);
  if (rc != 0) {
    return 40 + rc;
  }

  rc = turbonet_http_response_error_code(response, &error_code);
  if (rc != 0 || error_code == 0) {
    return 50 + rc;
  }

  rc = turbonet_http_response_header(response, content_type, cstrlen(content_type), header_buf,
                                     sizeof(header_buf), &header_len);
  if (rc != 0 || header_len != 0) {
    return 55 + rc;
  }

  rc = turbonet_http_response_error(response, err_buf, sizeof(err_buf), &err_len);
  if (rc != 0 || err_len == 0 || err_buf[0] == '\0') {
    return 60 + rc;
  }

  rc = turbonet_http_response_close(response);
  if (rc != 0) {
    return 70 + rc;
  }

  rc = turbonet_http_client_close(client);
  if (rc != 0) {
    return 80 + rc;
  }

  return status == 0 ? 17 : 18;
}
