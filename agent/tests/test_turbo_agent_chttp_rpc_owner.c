#include "tinytest.h"
#include "turbo_agent_http_test_support.h"
#include <http_server/rpc.h>
#include <http_client/rpc.h>
#include <stdatomic.h>
#include <salts/clock.h>
#include <stdio.h>

typedef struct published_rpc_probe {
  atomic_int calls;
  uint64_t value;
} published_rpc_probe;

static cserde_status published_rpc_encode_value(void *user, cserde_writer *writer) {
  cserde_token token = {0};
  if (user == NULL || writer == NULL) return CSERDE_INVALID_ARGUMENT;
  token.kind = CSERDE_UINT;
  token.value.uint = *(const uint64_t *)user;
  return cserde_writer_write(writer, &token);
}

static int published_rpc_reply(void *user, const crpc_server_request_view *request,
                               crpc_server_response *response) {
  published_rpc_probe *probe = (published_rpc_probe *)user;
  if (probe == NULL || request == NULL || response == NULL ||
      request->notification) return SALTS_EINVAL;
  atomic_fetch_add_explicit(&probe->calls, 1, memory_order_relaxed);
  return crpc_server_response_result(response, published_rpc_encode_value, &probe->value);
}

static chttp_client_config published_rpc_client_config(void) {
  const chttp_server_config server = agent_http_test_server_config();
  chttp_client_config client = {0};
  client.network = server.network;
  client.network.connection_capacity = 2u;
  client.request_capacity = 2u;
  client.max_start_line_bytes = 256u;
  client.max_header_count = 16u;
  client.max_header_bytes = 4096u;
  client.max_request_body_bytes = 8192u;
  client.max_response_body_bytes = 8192u;
  client.max_informational_responses = 2u;
  return client;
}


/* Callback output is only copied during the synchronous poll invocation:
 * borrowed readers or CNet handles are never persisted as live pointers. */
typedef struct published_rpc_async_probe {
  int callbacks;
  int status;
  uint64_t result;
  uint64_t wire_id;
  unsigned int http_status;
  chttp_request handle;
} published_rpc_async_probe;

static void published_rpc_async_complete(
    void *user, chttp_request request, const crpc_response_view *reply,
    const crpc_error *error) {
  published_rpc_async_probe *probe = (published_rpc_async_probe *)user;
  cserde_token token = {0};
  if (probe == NULL) return;
  ++probe->callbacks;
  probe->handle = request;
  if (error != NULL) {
    probe->status = error->status;
    return;
  }
  if (reply == NULL || reply->kind != CRPC_RESPONSE_RESULT ||
      reply->value.result == NULL ||
      cserde_reader_next(reply->value.result, &token) != CSERDE_OK ||
      token.kind != CSERDE_UINT) {
    probe->status = SALTS_EPROTO;
    return;
  }
  probe->status = SALTS_OK;
  probe->wire_id = reply->request_id;
  probe->http_status = reply->http_status;
  probe->result = token.value.uint;
}

static int published_rpc_async_poll_until(
    chttp_async_client *client, const published_rpc_async_probe *probe) {
  const uint64_t deadline = cmeta_monotonic_ms() + AGENT_HTTP_TEST_TIMEOUT_MS;
  while (probe->callbacks == 0) {
    size_t completions = 0u;
    int rc = chttp_async_client_poll(client, 5u, &completions);
    if (rc != SALTS_OK) return rc;
    if (cmeta_monotonic_ms() >= deadline) return SALTS_ETIMEDOUT;
  }
  return SALTS_OK;
}

/* A *downstream* linked and executed consumer of the published CHttp Owner
 * RPC API, not a test of an independent CRPC transport/worker object. */
spec("TurboAgent installed CHttp 2.1.0-rc.5 RPC Owner integration") {
  it("routes two JSON-RPC requests through an existing HTTP Server and Client") {
    chttp_server server = {0};
    chttp_client client = {0};
    chttp_server_config server_config = agent_http_test_server_config();
    chttp_server_rpc_config rpc_config = CHTTP_SERVER_RPC_CONFIG_INIT;
    chttp_client_config client_config = published_rpc_client_config();
    chttp_client_rpc_limits limits = CHTTP_CLIENT_RPC_LIMITS_INIT;
    published_rpc_probe probe = {0};
    crpc_method method = {.name = "ping"};
    crpc_options options = {0};
    crpc_response reply = {0};
    crpc_error error = {0};
    cserde_token token = {0};
    char uri[64];
    uint16_t port = 0u;
    size_t i;

    atomic_init(&probe.calls, 0);
    probe.value = UINT64_C(42);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_rpc_configure(&server, &rpc_config), SALTS_OK);
    check_equal(chttp_server_rpc_register(&server, "/rpc", &method,
                                          published_rpc_reply, &probe), SALTS_OK);
    check_equal(chttp_server_rpc_register(&server, "/rpc", &method,
                                          published_rpc_reply, &probe), SALTS_EALREADY);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                        (unsigned int)port) > 0);

    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);
    limits.max_method_bytes = 64u;
    limits.max_json_depth = 8u;
    limits.max_request_body_bytes = client_config.max_request_body_bytes;
    limits.max_http_header_count = client_config.max_header_count;
    options.connection_uri = uri;
    options.authority = "localhost";
    options.target = "/rpc";
    options.method = method;
    options.deadline_ms = AGENT_HTTP_TEST_TIMEOUT_MS;

    for (i = 0u; i < 2u; ++i) {
      options.request_id = (uint64_t)(i + 1u);
      check_equal(chttp_client_rpc_request_reply(&client, &limits, &options,
                                                  &reply, &error), SALTS_OK);
      check_equal(reply.kind, CRPC_RESPONSE_RESULT);
      check_equal(reply.request_id, options.request_id);
      check_not_null(reply.value.result);
      check_equal(cserde_reader_next(reply.value.result, &token), CSERDE_OK);
      check_equal(token.kind, CSERDE_UINT);
      check_equal(token.value.uint, UINT64_C(42));
      crpc_response_destroy(&reply);
    }

    check_equal(atomic_load_explicit(&probe.calls, memory_order_relaxed), 2);
    check_equal(chttp_client_destroy(&client, AGENT_HTTP_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_stop(&server, AGENT_HTTP_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  it("uses one HTTP Owner for async RPC, refusing duplicate active wire IDs") {
    chttp_server server = {0};
    chttp_async_client client = {0};
    chttp_server_config server_config = agent_http_test_server_config();
    chttp_server_rpc_config rpc_config = CHTTP_SERVER_RPC_CONFIG_INIT;
    chttp_client_config client_config = published_rpc_client_config();
    chttp_client_rpc_limits limits = CHTTP_CLIENT_RPC_LIMITS_INIT;
    published_rpc_probe server_probe = {0};
    published_rpc_async_probe probe = {0};
    crpc_method method = {.name = "ping"};
    crpc_options options = {0};
    chttp_request request = {0};
    chttp_request duplicate = {0};
    uint16_t port = 0u;
    char uri[64];
    size_t completions = 0u;

    atomic_init(&server_probe.calls, 0);
    server_probe.value = UINT64_C(42);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_rpc_configure(&server, &rpc_config), SALTS_OK);
    check_equal(chttp_server_rpc_register(&server, "/rpc", &method,
                                          published_rpc_reply, &server_probe), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                        (unsigned int)port) > 0);
    check_equal(chttp_async_client_init(&client, &client_config), SALTS_OK);

    limits.max_request_body_bytes = client_config.max_request_body_bytes;
    limits.max_http_header_count = client_config.max_header_count;
    options.connection_uri = uri;
    options.authority = "localhost";
    options.target = "/rpc";
    options.method = method;
    options.request_id = UINT64_C(91);
    options.deadline_ms = AGENT_HTTP_TEST_TIMEOUT_MS;
    check_equal(chttp_async_client_rpc_submit(&client, &limits, &options,
                                              published_rpc_async_complete,
                                              &probe, &request), SALTS_OK);
    check_true(request.slot != 0u && request.generation != 0u);
    check_equal(chttp_async_client_rpc_submit(&client, &limits, &options,
                                              published_rpc_async_complete,
                                              &probe, &duplicate), SALTS_EALREADY);
    check_equal(duplicate.slot, 0u);
    check_equal(duplicate.generation, 0u);
    check_equal(probe.callbacks, 0);

    check_equal(published_rpc_async_poll_until(&client, &probe), SALTS_OK);
    check_equal(probe.callbacks, 1);
    check_equal(probe.status, SALTS_OK);
    check_equal(probe.result, UINT64_C(42));
    check_equal(probe.wire_id, UINT64_C(91));
    check_equal(probe.http_status, 200u);
    check_equal(probe.handle.slot, request.slot);
    check_equal(probe.handle.generation, request.generation);
    check_equal(chttp_async_request_cancel(&client, request), SALTS_ENOENT);
    check_equal(chttp_async_client_poll(&client, 0u, &completions), SALTS_OK);
    check_equal(probe.callbacks, 1);
    check_equal(atomic_load_explicit(&server_probe.calls, memory_order_relaxed), 1);

    check_equal(chttp_async_client_stop(&client, AGENT_HTTP_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(chttp_server_stop(&server, AGENT_HTTP_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(probe.callbacks, 1);
  }

  it("terminally cancels an admitted async RPC exactly once without a second Owner") {
    chttp_async_client client = {0};
    chttp_client_config client_config = published_rpc_client_config();
    chttp_client_rpc_limits limits = CHTTP_CLIENT_RPC_LIMITS_INIT;
    published_rpc_async_probe probe = {0};
    crpc_options options = {0};
    chttp_request request = {0};
    chttp_request duplicate = {0};
    size_t completions = 0u;

    check_equal(chttp_async_client_init(&client, &client_config), SALTS_OK);
    limits.max_request_body_bytes = client_config.max_request_body_bytes;
    limits.max_http_header_count = client_config.max_header_count;
    /* An unconnected loopback endpoint is enough: cancellation occurs before
     * HTTP/CNet poll, and the terminal callback is driven by that same Owner. */
    options.connection_uri = "tcp://127.0.0.1:1";
    options.authority = "localhost";
    options.target = "/rpc";
    options.method = (crpc_method){.name = "cancel"};
    options.request_id = UINT64_C(93);
    options.deadline_ms = AGENT_HTTP_TEST_TIMEOUT_MS;
    check_equal(chttp_async_client_rpc_submit(&client, &limits, &options,
                                              published_rpc_async_complete,
                                              &probe, &request), SALTS_OK);
    check_true(request.slot != 0u && request.generation != 0u);
    check_equal(chttp_async_client_rpc_submit(&client, &limits, &options,
                                              published_rpc_async_complete,
                                              &probe, &duplicate), SALTS_EALREADY);
    check_equal(duplicate.slot, 0u);
    check_equal(chttp_async_request_cancel(&client, request), SALTS_OK);
    check_equal(chttp_async_request_cancel(&client, request), SALTS_EALREADY);
    check_equal(probe.callbacks, 0);
    check_equal(published_rpc_async_poll_until(&client, &probe), SALTS_OK);
    check_equal(probe.callbacks, 1);
    check_equal(probe.status, SALTS_ECANCELED);
    check_equal(probe.handle.slot, request.slot);
    check_equal(probe.handle.generation, request.generation);
    check_equal(chttp_async_request_cancel(&client, request), SALTS_ENOENT);
    check_equal(chttp_async_client_poll(&client, 0u, &completions), SALTS_OK);
    check_equal(probe.callbacks, 1);
    check_equal(chttp_async_client_stop(&client, AGENT_HTTP_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);
    check_equal(probe.callbacks, 1);
  }
}
