#ifndef TURBO_AGENT_HTTP_TEST_SUPPORT_H
#define TURBO_AGENT_HTTP_TEST_SUPPORT_H

#include <http_server/http.h>

enum { AGENT_HTTP_TEST_TIMEOUT_MS = 5000, AGENT_HTTP_TEST_BODY_BYTES = 64 * 1024 };

/* Shared loopback fixture limits; never reads credentials or opens a public port. */
static chttp_server_config agent_http_test_server_config(void) {
  chttp_server_config config = {0};
  config.host = "127.0.0.1";
  config.port = 0;
  config.backlog = 4;
#if defined(_WIN32)
  config.network.backend = NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  config.network.backend = NATIVE_IO_BACKEND_EPOLL;
#else
  config.network.backend = NATIVE_IO_BACKEND_KQUEUE;
#endif
  config.network.connection_capacity = 4;
  config.network.command_capacity = 32;
  config.network.request_capacity = 32;
  config.network.completion_batch_capacity = 4;
  config.network.event_capacity = 32;
  config.network.max_send_bytes = AGENT_HTTP_TEST_BODY_BYTES;
  config.network.receive_buffer_bytes = AGENT_HTTP_TEST_BODY_BYTES;
  config.network.connect_timeout_ms = AGENT_HTTP_TEST_TIMEOUT_MS;
  config.network.read_timeout_ms = AGENT_HTTP_TEST_TIMEOUT_MS;
  config.network.write_timeout_ms = AGENT_HTTP_TEST_TIMEOUT_MS;
  config.route_capacity = 1;
  config.middleware_capacity = 1;
  config.max_route_middleware_count = 1;
  config.max_route_param_count = 1;
  config.max_route_param_bytes = 256;
  config.max_target_bytes = 256;
  config.max_header_count = 32;
  config.max_header_bytes = 8192;
  config.max_request_body_bytes = AGENT_HTTP_TEST_BODY_BYTES;
  config.max_response_header_count = 32;
  config.max_response_header_bytes = 8192;
  config.max_response_body_bytes = AGENT_HTTP_TEST_BODY_BYTES;
  config.buffer_capacity_bytes = 1024 * 1024;
  config.poll_slice_ms = 1;
  return config;
}

#endif
