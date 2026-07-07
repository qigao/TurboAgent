#include "turbo_wasm3_internal.h"

m3ApiRawFunction(turbo_wasm3_host_http_client_open) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *base_url = NULL;
  uint32_t client_handle = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, base_url_data)
  m3ApiGetArg(uint32_t, base_url_len)
  m3ApiGetArgMem(uint32_t *, out_client_handle)

  if (!vm || !vm->http_registry || !out_client_handle) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_client_handle, sizeof(uint32_t));
  if (base_url_len != 0) {
    m3ApiCheckMem(base_url_data, base_url_len);
  }
  m3ApiWriteMem32(out_client_handle, 0);

  if (base_url_len != 0) {
    base_url = turbo_wasm3_copy_guest_bytes(base_url_data, base_url_len);
    if (!base_url) {
      m3ApiReturn(TURBO_ENOMEM);
    }
  }

  rc = turbo_wasm3_http_registry_open_client(vm->http_registry, base_url,
                                             &client_handle);
  free(base_url);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_client_handle, client_handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_client_close) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)

  if (!vm || !vm->http_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(
      turbo_wasm3_http_registry_close_client(vm->http_registry, client_handle));
}

m3ApiRawFunction(turbo_wasm3_host_http_client_set_timeout) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArg(int32_t, timeout_ms)

  if (!vm || !vm->http_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(turbo_wasm3_http_registry_set_timeout(vm->http_registry,
                                                    client_handle, timeout_ms));
}

m3ApiRawFunction(turbo_wasm3_host_http_client_set_default_header) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *name = NULL;
  char *value = NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArgMem(const uint8_t *, name_data)
  m3ApiGetArg(uint32_t, name_len)
  m3ApiGetArgMem(const uint8_t *, value_data)
  m3ApiGetArg(uint32_t, value_len)

  if (!vm || !vm->http_registry || name_len == 0 || value_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(name_data, name_len);
  m3ApiCheckMem(value_data, value_len);

  name = turbo_wasm3_copy_guest_bytes(name_data, name_len);
  value = turbo_wasm3_copy_guest_bytes(value_data, value_len);
  if (!name || !value) {
    free(value);
    free(name);
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_http_registry_set_default_header(vm->http_registry,
                                                    client_handle, name, value);
  free(value);
  free(name);
  m3ApiReturn(rc);
}

m3ApiRawFunction(turbo_wasm3_host_http_client_remove_default_header) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *name = NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArgMem(const uint8_t *, name_data)
  m3ApiGetArg(uint32_t, name_len)

  if (!vm || !vm->http_registry || name_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(name_data, name_len);
  name = turbo_wasm3_copy_guest_bytes(name_data, name_len);
  if (!name) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_http_registry_remove_default_header(vm->http_registry,
                                                       client_handle, name);
  free(name);
  m3ApiReturn(rc);
}

m3ApiRawFunction(turbo_wasm3_host_http_client_clear_default_headers) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)

  if (!vm || !vm->http_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(turbo_wasm3_http_registry_clear_default_headers(
      vm->http_registry, client_handle));
}

m3ApiRawFunction(turbo_wasm3_host_http_client_set_basic_auth) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *username = NULL;
  char *password = NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArgMem(const uint8_t *, username_data)
  m3ApiGetArg(uint32_t, username_len)
  m3ApiGetArgMem(const uint8_t *, password_data)
  m3ApiGetArg(uint32_t, password_len)

  if (!vm || !vm->http_registry || username_len == 0 || password_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(username_data, username_len);
  m3ApiCheckMem(password_data, password_len);

  username = turbo_wasm3_copy_guest_bytes(username_data, username_len);
  password = turbo_wasm3_copy_guest_bytes(password_data, password_len);
  if (!username || !password) {
    free(password);
    free(username);
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_http_registry_set_basic_auth(vm->http_registry, client_handle,
                                                username, password);
  free(password);
  free(username);
  m3ApiReturn(rc);
}

m3ApiRawFunction(turbo_wasm3_host_http_client_set_bearer_token) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *token = NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArgMem(const uint8_t *, token_data)
  m3ApiGetArg(uint32_t, token_len)

  if (!vm || !vm->http_registry || token_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(token_data, token_len);
  token = turbo_wasm3_copy_guest_bytes(token_data, token_len);
  if (!token) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_http_registry_set_bearer_token(vm->http_registry,
                                                  client_handle, token);
  free(token);
  m3ApiReturn(rc);
}

m3ApiRawFunction(turbo_wasm3_host_http_client_clear_auth) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)

  if (!vm || !vm->http_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(
      turbo_wasm3_http_registry_clear_auth(vm->http_registry, client_handle));
}

m3ApiRawFunction(turbo_wasm3_host_http_client_set_proxy) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *host = NULL;
  char *username = NULL;
  char *password = NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArgMem(const uint8_t *, host_data)
  m3ApiGetArg(uint32_t, host_len)
  m3ApiGetArg(uint32_t, port)
  m3ApiGetArgMem(const uint8_t *, username_data)
  m3ApiGetArg(uint32_t, username_len)
  m3ApiGetArgMem(const uint8_t *, password_data)
  m3ApiGetArg(uint32_t, password_len)

  if (!vm || !vm->http_registry || host_len == 0 || port == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(host_data, host_len);
  if (username_len != 0) {
    m3ApiCheckMem(username_data, username_len);
  }
  if (password_len != 0) {
    m3ApiCheckMem(password_data, password_len);
  }

  host = turbo_wasm3_copy_guest_bytes(host_data, host_len);
  if (!host) {
    m3ApiReturn(TURBO_ENOMEM);
  }
  if (username_len != 0) {
    username = turbo_wasm3_copy_guest_bytes(username_data, username_len);
    if (!username) {
      free(host);
      m3ApiReturn(TURBO_ENOMEM);
    }
  }
  if (password_len != 0) {
    password = turbo_wasm3_copy_guest_bytes(password_data, password_len);
    if (!password) {
      free(username);
      free(host);
      m3ApiReturn(TURBO_ENOMEM);
    }
  }

  rc = turbo_wasm3_http_registry_set_proxy(vm->http_registry, client_handle, host,
                                           (uint16_t)port, username, password);
  free(password);
  free(username);
  free(host);
  m3ApiReturn(rc);
}

m3ApiRawFunction(turbo_wasm3_host_http_client_clear_proxy) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)

  if (!vm || !vm->http_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(
      turbo_wasm3_http_registry_clear_proxy(vm->http_registry, client_handle));
}

m3ApiRawFunction(turbo_wasm3_host_http_request) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *url = NULL;
  char *body = NULL;
  uint32_t response_handle = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArg(int32_t, method)
  m3ApiGetArgMem(const uint8_t *, url_data)
  m3ApiGetArg(uint32_t, url_len)
  m3ApiGetArgMem(const uint8_t *, body_data)
  m3ApiGetArg(uint32_t, body_len)
  m3ApiGetArgMem(uint32_t *, out_response_handle)

  if (!vm || !vm->http_registry || !out_response_handle || !url_len) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_response_handle, sizeof(uint32_t));
  m3ApiCheckMem(url_data, url_len);
  if (body_len != 0) {
    m3ApiCheckMem(body_data, body_len);
  }
  m3ApiWriteMem32(out_response_handle, 0);

  url = turbo_wasm3_copy_guest_bytes(url_data, url_len);
  if (!url) {
    m3ApiReturn(TURBO_ENOMEM);
  }
  if (body_len != 0) {
    body = turbo_wasm3_copy_guest_bytes(body_data, body_len);
    if (!body) {
      free(url);
      m3ApiReturn(TURBO_ENOMEM);
    }
  }

  rc = turbo_wasm3_http_registry_request(vm->http_registry, client_handle, method,
                                         url, body, body_len, &response_handle);
  free(body);
  free(url);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_response_handle, response_handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_request_with_headers) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *url = NULL;
  char *headers = NULL;
  char *body = NULL;
  uint32_t response_handle = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArg(int32_t, method)
  m3ApiGetArgMem(const uint8_t *, url_data)
  m3ApiGetArg(uint32_t, url_len)
  m3ApiGetArgMem(const uint8_t *, headers_data)
  m3ApiGetArg(uint32_t, headers_len)
  m3ApiGetArgMem(const uint8_t *, body_data)
  m3ApiGetArg(uint32_t, body_len)
  m3ApiGetArgMem(uint32_t *, out_response_handle)

  if (!vm || !vm->http_registry || !out_response_handle || !url_len) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_response_handle, sizeof(uint32_t));
  m3ApiCheckMem(url_data, url_len);
  if (headers_len != 0) {
    m3ApiCheckMem(headers_data, headers_len);
  }
  if (body_len != 0) {
    m3ApiCheckMem(body_data, body_len);
  }
  m3ApiWriteMem32(out_response_handle, 0);

  url = turbo_wasm3_copy_guest_bytes(url_data, url_len);
  if (!url) {
    m3ApiReturn(TURBO_ENOMEM);
  }
  if (headers_len != 0) {
    headers = turbo_wasm3_copy_guest_bytes(headers_data, headers_len);
    if (!headers) {
      free(url);
      m3ApiReturn(TURBO_ENOMEM);
    }
  }
  if (body_len != 0) {
    body = turbo_wasm3_copy_guest_bytes(body_data, body_len);
    if (!body) {
      free(headers);
      free(url);
      m3ApiReturn(TURBO_ENOMEM);
    }
  }

  rc = turbo_wasm3_http_registry_request_with_headers(
      vm->http_registry, client_handle, method, url, headers, headers_len, body,
      body_len, &response_handle);
  free(body);
  free(headers);
  free(url);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_response_handle, response_handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_stream_get) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *url = NULL;
  uint32_t response_handle = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArgMem(const uint8_t *, url_data)
  m3ApiGetArg(uint32_t, url_len)
  m3ApiGetArgMem(uint32_t *, out_response_handle)

  if (!vm || !vm->http_registry || !out_response_handle || url_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_response_handle, sizeof(uint32_t));
  m3ApiCheckMem(url_data, url_len);
  m3ApiWriteMem32(out_response_handle, 0);

  url = turbo_wasm3_copy_guest_bytes(url_data, url_len);
  if (!url) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_http_registry_stream_get(vm->http_registry, client_handle, url,
                                            &response_handle);
  free(url);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_response_handle, response_handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_sse_get) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *url = NULL;
  uint32_t response_handle = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, client_handle)
  m3ApiGetArgMem(const uint8_t *, url_data)
  m3ApiGetArg(uint32_t, url_len)
  m3ApiGetArgMem(uint32_t *, out_response_handle)

  if (!vm || !vm->http_registry || !out_response_handle || url_len == 0) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_response_handle, sizeof(uint32_t));
  m3ApiCheckMem(url_data, url_len);
  m3ApiWriteMem32(out_response_handle, 0);

  url = turbo_wasm3_copy_guest_bytes(url_data, url_len);
  if (!url) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_http_registry_sse_get(vm->http_registry, client_handle, url,
                                         &response_handle);
  free(url);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_response_handle, response_handle);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_status) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int32_t status_code = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)
  m3ApiGetArgMem(int32_t *, out_status)

  if (!vm || !vm->http_registry || !out_status) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_status, sizeof(int32_t));
  m3ApiWriteMem32(out_status, 0);

  rc = turbo_wasm3_http_registry_response_status(vm->http_registry, response_handle,
                                                 &status_code);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_status, (uint32_t)status_code);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_error_code) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int32_t error_code = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)
  m3ApiGetArgMem(int32_t *, out_error_code)

  if (!vm || !vm->http_registry || !out_error_code) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_error_code, sizeof(int32_t));
  m3ApiWriteMem32(out_error_code, 0);

  rc = turbo_wasm3_http_registry_response_error_code(vm->http_registry,
                                                     response_handle, &error_code);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_error_code, (uint32_t)error_code);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_is_sse) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  int32_t is_sse = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)
  m3ApiGetArgMem(int32_t *, out_is_sse)

  if (!vm || !vm->http_registry || !out_is_sse) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_is_sse, sizeof(int32_t));
  m3ApiWriteMem32(out_is_sse, 0);

  rc = turbo_wasm3_http_registry_response_is_sse(vm->http_registry,
                                                 response_handle, &is_sse);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_is_sse, (uint32_t)is_sse);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_chunk_count) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t count = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)
  m3ApiGetArgMem(uint32_t *, out_count)

  if (!vm || !vm->http_registry || !out_count) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_count, sizeof(uint32_t));
  m3ApiWriteMem32(out_count, 0);

  rc = turbo_wasm3_http_registry_response_chunk_count(vm->http_registry,
                                                      response_handle, &count);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_count, count);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_chunk) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)
  m3ApiGetArg(uint32_t, chunk_index)
  m3ApiGetArgMem(uint8_t *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->http_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_http_registry_response_chunk(vm->http_registry,
                                                response_handle, chunk_index,
                                                buffer, (size_t)buffer_size,
                                                &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_header) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *header_name = NULL;
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)
  m3ApiGetArgMem(const uint8_t *, header_name_data)
  m3ApiGetArg(uint32_t, header_name_len)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->http_registry || !out_written || !header_name_len) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  m3ApiCheckMem(header_name_data, header_name_len);
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  header_name = turbo_wasm3_copy_guest_bytes(header_name_data, header_name_len);
  if (!header_name) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = turbo_wasm3_http_registry_response_header(vm->http_registry, response_handle,
                                                 header_name, buffer,
                                                 (size_t)buffer_size, &written);
  free(header_name);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_headers) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->http_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_http_registry_response_headers(vm->http_registry, response_handle,
                                                  buffer, (size_t)buffer_size, &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_body) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)
  m3ApiGetArgMem(uint8_t *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->http_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_http_registry_response_body(vm->http_registry, response_handle,
                                               buffer, (size_t)buffer_size, &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_error) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !vm->http_registry || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = turbo_wasm3_http_registry_response_error(vm->http_registry, response_handle,
                                                buffer, (size_t)buffer_size, &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}

m3ApiRawFunction(turbo_wasm3_host_http_response_close) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, response_handle)

  if (!vm || !vm->http_registry) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(
      turbo_wasm3_http_registry_close_response(vm->http_registry, response_handle));
}


M3Result turbo_wasm3_vm_link_http_host(turbo_wasm3_vm_t *vm, IM3Module module) {
  const char *mod = "TurboNet";
  M3Result result = m3Err_none;

  if (!vm || !module) {
    return m3Err_wasmMalformed;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_open", "i(*i*)", &turbo_wasm3_host_http_client_open,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_close", "i(i)",
      &turbo_wasm3_host_http_client_close, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_set_timeout", "i(ii)",
      &turbo_wasm3_host_http_client_set_timeout, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_set_default_header", "i(i*i*i)",
      &turbo_wasm3_host_http_client_set_default_header, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_remove_default_header", "i(i*i)",
      &turbo_wasm3_host_http_client_remove_default_header, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_clear_default_headers", "i(i)",
      &turbo_wasm3_host_http_client_clear_default_headers, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_set_basic_auth", "i(i*i*i)",
      &turbo_wasm3_host_http_client_set_basic_auth, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_set_bearer_token", "i(i*i)",
      &turbo_wasm3_host_http_client_set_bearer_token, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_clear_auth", "i(i)",
      &turbo_wasm3_host_http_client_clear_auth, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_set_proxy", "i(i*ii*i*i)",
      &turbo_wasm3_host_http_client_set_proxy, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_client_clear_proxy", "i(i)",
      &turbo_wasm3_host_http_client_clear_proxy, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_request", "i(ii*i*i*)", &turbo_wasm3_host_http_request,
      vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_request_with_headers", "i(ii*i*i*i*)",
      &turbo_wasm3_host_http_request_with_headers, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_stream_get", "i(i*i*)",
      &turbo_wasm3_host_http_stream_get, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_sse_get", "i(i*i*)",
      &turbo_wasm3_host_http_sse_get, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_status", "i(i*)",
      &turbo_wasm3_host_http_response_status, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_error_code", "i(i*)",
      &turbo_wasm3_host_http_response_error_code, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_is_sse", "i(i*)",
      &turbo_wasm3_host_http_response_is_sse, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_chunk_count", "i(i*)",
      &turbo_wasm3_host_http_response_chunk_count, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_chunk", "i(ii*i*)",
      &turbo_wasm3_host_http_response_chunk, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_header", "i(i*i*i*)",
      &turbo_wasm3_host_http_response_header, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_headers", "i(i*i*)",
      &turbo_wasm3_host_http_response_headers, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_body", "i(i*i*)",
      &turbo_wasm3_host_http_response_body, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_error", "i(i*i*)",
      &turbo_wasm3_host_http_response_error, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "http_response_close", "i(i)",
      &turbo_wasm3_host_http_response_close, vm));
  if (result) {
    return result;
  }

  return m3Err_none;
}


turbo_wasm3_http_registry_t *
turbo_wasm3_http_registry_create(size_t initial_capacity) {
  turbo_wasm3_http_registry_t *registry;
  int rc;

  registry = (turbo_wasm3_http_registry_t *)calloc(1, sizeof(*registry));
  if (!registry) {
    return NULL;
  }

  registry->next_client_handle = 1;
  registry->next_response_handle = 0x20000u;
  registry->max_clients = initial_capacity > TURBO_WASM3_DEFAULT_MAX_HTTP_CLIENTS
                              ? initial_capacity
                              : TURBO_WASM3_DEFAULT_MAX_HTTP_CLIENTS;
  registry->max_responses = TURBO_WASM3_DEFAULT_MAX_HTTP_RESPONSES;
  registry->max_chunks_per_response =
      TURBO_WASM3_DEFAULT_MAX_HTTP_CHUNKS_PER_RESPONSE;
  registry->max_stream_bytes = TURBO_WASM3_DEFAULT_MAX_HTTP_STREAM_BYTES;

  rc = turbo_wasm3_http_registry_reserve_clients(registry,
                                                 initial_capacity ? initial_capacity : 4);
  if (rc != 0) {
    free(registry);
    return NULL;
  }

  rc = turbo_wasm3_http_registry_reserve_responses(registry, 4);
  if (rc != 0) {
    free(registry->clients);
    free(registry);
    return NULL;
  }

  return registry;
}

void turbo_wasm3_http_registry_destroy(turbo_wasm3_http_registry_t *registry) {
  size_t i;

  if (!registry) {
    return;
  }

  for (i = 0; i < registry->response_count; ++i) {
    turbo_wasm3_http_response_entry_reset(&registry->responses[i]);
  }

  for (i = 0; i < registry->client_count; ++i) {
    turbo_wasm3_http_client_entry_clear_error(&registry->clients[i]);
    http_client_destroy(registry->clients[i].client);
  }

  free(registry->responses);
  free(registry->clients);
  free(registry);
}

int turbo_wasm3_http_registry_set_limits(turbo_wasm3_http_registry_t *registry,
                                         size_t max_clients,
                                         size_t max_responses,
                                         size_t max_chunks_per_response,
                                         size_t max_stream_bytes) {
  size_t i;

  if (!registry || max_clients == 0 || max_responses == 0 ||
      max_chunks_per_response == 0 || max_stream_bytes == 0 ||
      max_clients < registry->client_count ||
      max_responses < registry->response_count) {
    return TURBO_EINVAL;
  }
  for (i = 0; i < registry->response_count; ++i) {
    if (registry->responses[i].chunk_count > max_chunks_per_response ||
        registry->responses[i].chunk_bytes > max_stream_bytes) {
      return TURBO_EINVAL;
    }
  }

  registry->max_clients = max_clients;
  registry->max_responses = max_responses;
  registry->max_chunks_per_response = max_chunks_per_response;
  registry->max_stream_bytes = max_stream_bytes;
  return 0;
}

int turbo_wasm3_http_registry_open_client(turbo_wasm3_http_registry_t *registry,
                                          const char *base_url,
                                          uint32_t *client_handle) {
  http_client_t *client;
  int rc;

  if (!registry || !client_handle) {
    return TURBO_EINVAL;
  }

  rc = turbo_wasm3_http_registry_reserve_clients(registry, registry->client_count + 1);
  if (rc != 0) {
    return rc;
  }

  client = http_client_create(base_url && base_url[0] != '\0' ? base_url : NULL);
  if (!client) {
    return TURBO_ENOMEM;
  }

  registry->clients[registry->client_count].handle = registry->next_client_handle++;
  registry->clients[registry->client_count].client = client;
  registry->clients[registry->client_count].last_error = NULL;
  *client_handle = registry->clients[registry->client_count].handle;
  registry->client_count++;
  return 0;
}

int turbo_wasm3_http_registry_close_client(turbo_wasm3_http_registry_t *registry,
                                           uint32_t client_handle) {
  turbo_wasm3_http_client_entry_t *entry;
  size_t index;
  size_t i;

  if (!registry) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  index = (size_t)(entry - registry->clients);
  for (i = registry->response_count; i > 0; --i) {
    turbo_wasm3_http_response_entry_t *response_entry = &registry->responses[i - 1];
    if (response_entry->client_handle == client_handle) {
      turbo_wasm3_http_response_entry_reset(response_entry);
      registry->responses[i - 1] = registry->responses[registry->response_count - 1];
      registry->response_count--;
    }
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_destroy(entry->client);
  registry->clients[index] = registry->clients[registry->client_count - 1];
  registry->client_count--;
  return 0;
}

int turbo_wasm3_http_registry_set_timeout(turbo_wasm3_http_registry_t *registry,
                                          uint32_t client_handle,
                                          int timeout_ms) {
  turbo_wasm3_http_client_entry_t *entry;

  if (!registry) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_set_timeout(entry->client, timeout_ms);
  return 0;
}

int turbo_wasm3_http_registry_set_default_header(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *name, const char *value) {
  turbo_wasm3_http_client_entry_t *entry;

  if (!registry || !name || !value) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_set_default_header(entry->client, name, value);
  return 0;
}

int turbo_wasm3_http_registry_remove_default_header(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *name) {
  turbo_wasm3_http_client_entry_t *entry;

  if (!registry || !name) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_remove_default_header(entry->client, name);
  return 0;
}

int turbo_wasm3_http_registry_clear_default_headers(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle) {
  turbo_wasm3_http_client_entry_t *entry;

  if (!registry) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_clear_default_headers(entry->client);
  return 0;
}

int turbo_wasm3_http_registry_set_basic_auth(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *username, const char *password) {
  turbo_wasm3_http_client_entry_t *entry;

  if (!registry || !username || !password) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_set_basic_auth(entry->client, username, password);
  return 0;
}

int turbo_wasm3_http_registry_set_bearer_token(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *token) {
  turbo_wasm3_http_client_entry_t *entry;

  if (!registry || !token) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_set_bearer_token(entry->client, token);
  return 0;
}

int turbo_wasm3_http_registry_clear_auth(turbo_wasm3_http_registry_t *registry,
                                         uint32_t client_handle) {
  turbo_wasm3_http_client_entry_t *entry;

  if (!registry) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_clear_auth(entry->client);
  return 0;
}

int turbo_wasm3_http_registry_set_proxy(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *host, uint16_t port, const char *username,
    const char *password) {
  turbo_wasm3_http_client_entry_t *entry;

  if (!registry || !host || port == 0) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_set_proxy(entry->client, host, port, username, password);
  return 0;
}

int turbo_wasm3_http_registry_clear_proxy(turbo_wasm3_http_registry_t *registry,
                                          uint32_t client_handle) {
  turbo_wasm3_http_client_entry_t *entry;

  if (!registry) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  turbo_wasm3_http_client_entry_clear_error(entry);
  http_client_clear_proxy(entry->client);
  return 0;
}

int turbo_wasm3_http_registry_request(turbo_wasm3_http_registry_t *registry,
                                      uint32_t client_handle, int method,
                                      const char *url, const void *body,
                                      size_t body_len, uint32_t *response_handle) {
  return turbo_wasm3_http_registry_request_with_headers(
      registry, client_handle, method, url, NULL, 0, body, body_len, response_handle);
}

int turbo_wasm3_http_registry_request_with_headers(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle, int method,
    const char *url, const char *headers, size_t headers_len, const void *body,
    size_t body_len, uint32_t *response_handle) {
  turbo_wasm3_http_header_block_t header_block = {0};
  int rc;

  if (!registry || !url || !response_handle) {
    return TURBO_EINVAL;
  }

  rc = turbo_wasm3_http_parse_headers(headers, headers_len, &header_block);
  if (rc != 0) {
    return rc;
  }

  rc = turbo_wasm3_http_registry_request_core(
      registry, client_handle, method, url, header_block.lines,
      header_block.count, body, body_len, 0, 0, response_handle);
  turbo_wasm3_http_header_block_clear(&header_block);
  return rc;
}

int turbo_wasm3_http_registry_stream_get(turbo_wasm3_http_registry_t *registry,
                                         uint32_t client_handle, const char *url,
                                         uint32_t *response_handle) {
  return turbo_wasm3_http_registry_request_core(registry, client_handle,
                                                TURBO_WASM3_HTTP_METHOD_GET, url,
                                                NULL, 0, NULL, 0, 1, 0,
                                                response_handle);
}

int turbo_wasm3_http_registry_sse_get(turbo_wasm3_http_registry_t *registry,
                                      uint32_t client_handle, const char *url,
                                      uint32_t *response_handle) {
  return turbo_wasm3_http_registry_request_core(registry, client_handle,
                                                TURBO_WASM3_HTTP_METHOD_GET, url,
                                                NULL, 0, NULL, 0, 1, 1,
                                                response_handle);
}

int turbo_wasm3_http_registry_response_status(turbo_wasm3_http_registry_t *registry,
                                              uint32_t response_handle,
                                              int32_t *out_status) {
  turbo_wasm3_http_response_entry_t *entry;

  if (!registry || !out_status) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  *out_status = entry->response ? (int32_t)entry->response->status_code : 0;
  return 0;
}

int turbo_wasm3_http_registry_response_error_code(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    int32_t *out_error_code) {
  turbo_wasm3_http_response_entry_t *entry;

  if (!registry || !out_error_code) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  *out_error_code =
      entry->response ? (int32_t)entry->response->error_code : (int32_t)TURBO_EIO;
  return 0;
}

int turbo_wasm3_http_registry_response_is_sse(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    int32_t *out_is_sse) {
  turbo_wasm3_http_response_entry_t *entry;

  if (!registry || !out_is_sse) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  *out_is_sse = (int32_t)(entry->is_sse ? 1 : 0);
  return 0;
}

int turbo_wasm3_http_registry_response_chunk_count(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    uint32_t *out_count) {
  turbo_wasm3_http_response_entry_t *entry;

  if (!registry || !out_count) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  *out_count = (uint32_t)entry->chunk_count;
  return 0;
}

int turbo_wasm3_http_registry_response_chunk(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    uint32_t chunk_index, void *buffer, size_t buffer_size, uint32_t *out_len) {
  turbo_wasm3_http_response_entry_t *entry;
  turbo_wasm3_blob_t *chunk;

  if (!registry || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }
  if (chunk_index >= entry->chunk_count) {
    return TURBO_EINVAL;
  }

  chunk = &entry->chunks[chunk_index];
  return turbo_wasm3_http_copy_buffer(chunk->bytes, chunk->size, buffer,
                                      buffer_size, out_len);
}

int turbo_wasm3_http_registry_response_header(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    const char *header_name, char *buffer, size_t buffer_size, uint32_t *out_len) {
  turbo_wasm3_http_response_entry_t *entry;
  char *value;
  int rc;

  if (!registry || !header_name || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  if (!entry->response) {
    buffer[0] = '\0';
    *out_len = 0;
    return 0;
  }

  value = http_response_get_header(entry->response, header_name);
  if (!value) {
    buffer[0] = '\0';
    *out_len = 0;
    return 0;
  }

  rc = turbo_wasm3_http_copy_string(value, buffer, buffer_size, out_len);
  free(value);
  if (rc != 0 && rc != TURBO_EMSGSIZE) {
    turbo_wasm3_http_capture_response_error(entry, entry->response);
  }
  return rc;
}

int turbo_wasm3_http_registry_response_headers(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle, char *buffer,
    size_t buffer_size, uint32_t *out_len) {
  turbo_wasm3_http_response_entry_t *entry;
  int rc;

  if (!registry || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  rc = turbo_wasm3_http_copy_string(
      entry->response ? entry->response->headers : NULL, buffer, buffer_size, out_len);
  if (rc != 0 && rc != TURBO_EMSGSIZE) {
    turbo_wasm3_http_capture_response_error(entry, entry->response);
  }
  return rc;
}

int turbo_wasm3_http_registry_response_body(turbo_wasm3_http_registry_t *registry,
                                            uint32_t response_handle, void *buffer,
                                            size_t buffer_size, uint32_t *out_len) {
  turbo_wasm3_http_response_entry_t *entry;
  int rc;

  if (!registry || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }
  if (entry->is_stream) {
    return TURBO_ENOTSUP;
  }

  rc = turbo_wasm3_http_copy_buffer(entry->response ? entry->response->body : NULL,
                                    entry->response ? entry->response->body_len : 0,
                                    buffer, buffer_size, out_len);
  if (rc != 0 && rc != TURBO_EMSGSIZE) {
    turbo_wasm3_http_capture_response_error(entry, entry->response);
  }
  return rc;
}

int turbo_wasm3_http_registry_response_error(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle, char *buffer,
    size_t buffer_size, uint32_t *out_len) {
  turbo_wasm3_http_response_entry_t *entry;

  if (!registry || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  if (entry->last_error) {
    return turbo_wasm3_http_copy_string(entry->last_error, buffer, buffer_size,
                                        out_len);
  }

  return turbo_wasm3_http_copy_string(
      entry->response ? entry->response->error : NULL, buffer, buffer_size, out_len);
}

int turbo_wasm3_http_registry_close_response(turbo_wasm3_http_registry_t *registry,
                                             uint32_t response_handle) {
  turbo_wasm3_http_response_entry_t *entry;
  size_t index;

  if (!registry) {
    return TURBO_EINVAL;
  }

  entry = turbo_wasm3_http_registry_find_response(registry, response_handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  index = (size_t)(entry - registry->responses);
  turbo_wasm3_http_response_entry_reset(entry);
  registry->responses[index] = registry->responses[registry->response_count - 1];
  registry->response_count--;
  return 0;
}
