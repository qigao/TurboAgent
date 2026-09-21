#include "tinytest.h"

#include "CoroNet.h"
#include "error_recovery.h"
#include "iris_app.h"
#include "rpc_server.h"
#include "server.h"
#include "turbo_agent_graph.h"
#include "turbo_agent_runtime.h"
#include "turbo_agent_runtime_remote.h"
#include "turbo_agent_runtime_remote_iris.h"
#include "turbo_agent_state.h"
#include <json_parser.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  const char *key;
  int value;
} remote_iris_bool_write_t;

typedef struct {
  const char *name;
  turbo_graph_t *graph;
} remote_iris_graph_registry_t;

typedef struct {
  coro_context_t *coro_ctx;
  coro_socket_t *server;
  int server_stopped;
  rpc_context_t *rpc_ctx;
  turbo_graph_t *graph;
  turbo_agent_runtime_store_t store;
  turbo_agent_runtime_t *runtime;
  turbo_agent_runtime_remote_t *remote;
  turbo_agent_runtime_remote_iris_t *bridge;
  int rpc_ok;
  int remote_ok;
  int method_not_allowed_ok;
} remote_iris_test_state_t;

static void remote_iris_test_state_cleanup(remote_iris_test_state_t *state) {
  if (!state) {
    return;
  }
  if (!state->server_stopped && state->server) {
    coro_socket_destroy(state->server);
    state->server = NULL;
  }
  if (state->bridge) {
    turbo_agent_runtime_remote_iris_destroy(state->bridge);
    state->bridge = NULL;
  }
  if (state->remote) {
    turbo_agent_runtime_remote_destroy(state->remote);
    state->remote = NULL;
  }
  if (state->runtime) {
    turbo_agent_runtime_destroy(state->runtime);
    state->runtime = NULL;
  }
  state->graph = NULL;
  if (state->rpc_ctx) {
    rpc_destroy(state->rpc_ctx);
    state->rpc_ctx = NULL;
  }
}

static int remote_iris_write_bool_json_value_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  remote_iris_bool_write_t *write = (remote_iris_bool_write_t *)user_data;
  json_value_t *value;

  value = turbo_json_create_bool(write->value);
  if (!value) {
    return -1;
  }
  return turbo_runtime_json_object_set(ctx->json_value_state, write->key, value) ==
                 TURBO_RUNTIME_JSON_OK
             ? 0
             : -1;
}

static turbo_graph_t *remote_iris_graph_resolver(const char *graph_name, void *user_data) {
  remote_iris_graph_registry_t *registry = (remote_iris_graph_registry_t *)user_data;

  if (!registry || !graph_name || !registry->name || !registry->graph) {
    return NULL;
  }
  return strcmp(graph_name, registry->name) == 0 ? registry->graph : NULL;
}

static turbo_graph_t *create_remote_iris_graph(void) {
  turbo_graph_t *graph = turbo_graph_create("remote-iris-test");
  static remote_iris_bool_write_t start = {"visited_start", 1};
  static remote_iris_bool_write_t end = {"visited_end", 1};

  check_not_null(graph);
  check_int_eq(
      turbo_graph_add_json_value_node(graph, "start", remote_iris_write_bool_json_value_node, &start),
      TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_json_value_node(graph, "end", remote_iris_write_bool_json_value_node, &end),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_json_value_edge(graph, "start", "end", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);
  return graph;
}

static json_value_t *create_remote_iris_state_json(void) {
  json_value_t *state = turbo_agent_state_create_json_value();
  json_value_t *state_json;

  check_not_null(state);
  state_json = turbo_json_clone(state);
  turbo_runtime_json_destroy(state);
  return state_json;
}

static int rpc_remote_iris_handler(Req *req, Res *res, rpc_request_t *rpc_req,
                                   rpc_response_t *rpc_res) {
  (void)req;
  (void)res;
  (void)rpc_req;
  rpc_set_result(rpc_res, "{\"origin\":\"iris-rpc\"}");
  return 0;
}

static rpc_context_t *create_remote_iris_rpc_context(void) {
  rpc_config_t config = {0};
  rpc_context_t *ctx;
  rpc_method_t method = {0};

  config.endpoint = "/rpc";
  config.default_protocol = RPC_PROTOCOL_JSON;
  config.enable_introspection = 0;
  config.enable_batch = 0;
  config.max_batch_size = 1;
  config.max_request_size = 4096;

  ctx = rpc_init(&config);
  if (!ctx) {
    return NULL;
  }
  method.name = "ctx.which";
  method.handler = rpc_remote_iris_handler;
  method.description = "Return RPC endpoint identity";
  method.requires_auth = 0;
  if (rpc_register_method(ctx, &method) != 0 || rpc_setup_endpoint(ctx) != 0) {
    rpc_destroy(ctx);
    return NULL;
  }
  return ctx;
}

static int parse_http_content_length(const char *response, size_t header_len,
                                     size_t *out_content_length) {
  const char *line;
  const char *header_end;
  size_t value = 0;

  if (!response || !out_content_length) {
    return -1;
  }

  line = strstr(response, "Content-Length:");
  if (!line) {
    return -1;
  }
  header_end = response + header_len;
  if (line >= header_end) {
    return -1;
  }

  line += strlen("Content-Length:");
  while (line < header_end && (*line == ' ' || *line == '\t')) {
    line++;
  }
  if (line >= header_end || *line < '0' || *line > '9') {
    return -1;
  }

  while (line < header_end && *line >= '0' && *line <= '9') {
    value = value * 10u + (size_t)(*line - '0');
    line++;
  }

  *out_content_length = value;
  return 0;
}

static int send_http_request_capture_body(coro_context_t *ctx, unsigned short port,
                                          const char *request, char **out_body) {
  coro_socket_t *client;
  char *response_data = NULL;
  size_t response_len = 0;
  char *chunk_data = NULL;
  size_t chunk_len = 0;
  const char *header_end = NULL;
  const char *body_start;
  size_t body_len;
  size_t content_length = 0;
  int have_complete_response = 0;
  int rc;

  if (!ctx || !request || !out_body) {
    return -1;
  }
  *out_body = NULL;

  client = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
  if (!client) {
    return -1;
  }
  if (coro_socket_connect(client, "127.0.0.1", port) != 0) {
    coro_socket_destroy(client);
    return -1;
  }
  if (coro_socket_send(client, request, strlen(request)) != 0) {
    coro_socket_destroy(client);
    return -1;
  }

  for (;;) {
    char *new_response_data;

    chunk_data = NULL;
    chunk_len = 0;
    rc = coro_socket_recv(client, &chunk_data, &chunk_len);
    if (rc != 0 || !chunk_data || chunk_len == 0) {
      coro_socket_free_recv(chunk_data);
      break;
    }

    new_response_data = (char *)realloc(response_data, response_len + chunk_len + 1);
    if (!new_response_data) {
      coro_socket_free_recv(chunk_data);
      free(response_data);
      coro_socket_destroy(client);
      return -1;
    }
    response_data = new_response_data;
    memcpy(response_data + response_len, chunk_data, chunk_len);
    response_len += chunk_len;
    response_data[response_len] = '\0';
    coro_socket_free_recv(chunk_data);

    if (!header_end) {
      header_end = strstr(response_data, "\r\n\r\n");
    }
    if (header_end &&
        parse_http_content_length(response_data, (size_t)(header_end + 4 - response_data),
                                  &content_length) == 0) {
      size_t total_needed = (size_t)(header_end + 4 - response_data) + content_length;
      if (response_len >= total_needed) {
        have_complete_response = 1;
        break;
      }
    }
  }

  if (!response_data || response_len == 0) {
    free(response_data);
    coro_socket_destroy(client);
    return -1;
  }

  body_start = header_end ? header_end : strstr(response_data, "\r\n\r\n");
  if (!body_start) {
    free(response_data);
    coro_socket_destroy(client);
    return -1;
  }
  body_start += 4;
  if (have_complete_response) {
    body_len = content_length;
  } else {
    body_len = response_len - (size_t)(body_start - response_data);
  }
  *out_body = (char *)calloc(body_len + 1, sizeof(**out_body));
  if (!*out_body) {
    free(response_data);
    coro_socket_destroy(client);
    return -1;
  }
  memcpy(*out_body, body_start, body_len);
  (*out_body)[body_len] = '\0';

  free(response_data);
  coro_socket_destroy(client);
  return 0;
}

static int send_http_request_contains(coro_context_t *ctx, unsigned short port,
                                      const char *request, const char *needle) {
  char *body = NULL;
  int matched = 0;

  if (send_http_request_capture_body(ctx, port, request, &body) == 0 && body && needle &&
      strstr(body, needle) != NULL) {
    matched = 1;
  }
  free(body);
  return matched;
}

static int send_remote_start_request(coro_context_t *ctx, unsigned short port) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *state_json = create_remote_iris_state_json();
  json_value_t *options_json = turbo_json_create_object();
  json_value_t *interrupt_nodes = turbo_json_create_array();
  char *body_json = NULL;
  char request[4096];
  int written;
  char *response_body = NULL;
  json_value_t *response_json = NULL;
  const json_value_t *result_json;
  const json_value_t *summary_json;
  const json_value_t *state_result_json;
  const char *status;
  const char *thread_id;
  int ok = 0;

  check_not_null(params_json);
  check_not_null(state_json);
  check_not_null(options_json);
  check_not_null(interrupt_nodes);

  turbo_json_object_set_string(params_json, "graph_name", "remote-iris");
  turbo_json_object_set_string(params_json, "thread_id", "thr_remote_iris");
  turbo_json_object_add(params_json, "state", state_json);
  turbo_json_array_add(interrupt_nodes, turbo_json_create_string("end"));
  turbo_json_object_add(options_json, "interrupt_before_nodes", interrupt_nodes);
  turbo_json_object_add(params_json, "options", options_json);

  {
    json_value_t *request_json = turbo_json_create_object();

    check_not_null(request_json);
    turbo_json_object_set_string(request_json, "jsonrpc", "2.0");
    turbo_json_object_set_string(request_json, "id", "remote-start");
    turbo_json_object_set_string(request_json, "method", "runtime.start");
    turbo_json_object_add(request_json, "params", params_json);
    body_json = turbo_json_serialize(request_json, NULL);
    turbo_free_json(&request_json);
  }

  if (!body_json) {
    return 0;
  }

  written = snprintf(request, sizeof(request),
                     "POST /v1/runtime/jsonrpc HTTP/1.1\r\n"
                     "Host: localhost\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n"
                     "%s",
                     strlen(body_json), body_json);
  if (written <= 0 || (size_t)written >= sizeof(request)) {
    turbo_json_serialize_free(body_json);
    return 0;
  }

  if (send_http_request_capture_body(ctx, port, request, &response_body) != 0 || !response_body ||
      turbo_parse_json((const uint8_t *)response_body, strlen(response_body), &response_json) !=
          0 ||
      !response_json) {
    turbo_json_serialize_free(body_json);
    turbo_free_json(&response_json);
    free(response_body);
    return 0;
  }

  result_json = turbo_json_object_get(response_json, "result");
  summary_json = result_json ? turbo_json_object_get(result_json, "summary") : NULL;
  state_result_json = result_json ? turbo_json_object_get(result_json, "state") : NULL;
  status = summary_json ? turbo_json_get_string(summary_json, "status") : NULL;
  thread_id = summary_json ? turbo_json_get_string(summary_json, "thread_id") : NULL;
  if (result_json && summary_json && state_result_json && status && thread_id &&
      strcmp(status, "interrupted") == 0 &&
      strcmp(thread_id, "thr_remote_iris") == 0 &&
      turbo_json_get_bool(state_result_json, "visited_start", false) &&
      !turbo_json_get_bool(state_result_json, "visited_end", false)) {
    ok = 1;
  }

  turbo_json_serialize_free(body_json);
  turbo_free_json(&response_json);
  free(response_body);
  return ok;
}

static void remote_runtime_remote_iris_test_coro(coro_t *co, void *arg) {
  remote_iris_test_state_t *state = (remote_iris_test_state_t *)arg;
  iris_app_t *app = iris_app_default();
  remote_iris_graph_registry_t registry = {0};
  turbo_agent_runtime_remote_config_t remote_config = {0};
  turbo_agent_runtime_remote_iris_config_t bridge_config = {0};
  const unsigned short port = 29883;
  const char *rpc_body =
      "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"ctx.which\",\"params\":{}}";
  const char *get_remote_request =
      "GET /v1/runtime/jsonrpc HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "\r\n";
  char rpc_request[1024];
  char *rpc_response_body = NULL;
  char *method_not_allowed_body = NULL;
  int rpc_written;

  (void)co;

  state->rpc_ctx = create_remote_iris_rpc_context();
  state->store = turbo_agent_runtime_store_memory_create();
  state->runtime = turbo_agent_runtime_create(&state->store);
  state->graph = create_remote_iris_graph();
  if (!state->rpc_ctx || !state->runtime || !state->graph) {
    return;
  }

  registry.name = "remote-iris";
  registry.graph = state->graph;
  remote_config.runtime = state->runtime;
  remote_config.graph_resolver = remote_iris_graph_resolver;
  remote_config.graph_resolver_user_data = &registry;
  state->remote = turbo_agent_runtime_remote_create(&remote_config);
  if (!state->remote) {
    return;
  }

  bridge_config.remote = state->remote;
  bridge_config.path = "/v1/runtime/jsonrpc";
  state->bridge = turbo_agent_runtime_remote_iris_create(&bridge_config);
  if (!state->bridge || turbo_agent_runtime_remote_iris_mount(state->bridge, app) != 0) {
    return;
  }

  rpc_written = snprintf(rpc_request, sizeof(rpc_request),
                         "POST /rpc HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: %zu\r\n"
                         "\r\n"
                         "%s",
                         strlen(rpc_body), rpc_body);
  if (rpc_written <= 0 || (size_t)rpc_written >= sizeof(rpc_request)) {
    return;
  }

  init_router();
  state->server = iris_server_start(app, state->coro_ctx, port);
  if (!state->server) {
    return;
  }

  coro_yield();
  coro_sleep(state->coro_ctx, 50);

  state->rpc_ok = send_http_request_capture_body(state->coro_ctx, port, rpc_request,
                                                 &rpc_response_body) == 0 &&
                  rpc_response_body != NULL &&
                  strstr(rpc_response_body, "\"origin\":\"iris-rpc\"") != NULL;
  state->remote_ok = send_remote_start_request(state->coro_ctx, port);
  state->method_not_allowed_ok =
      send_http_request_capture_body(state->coro_ctx, port, get_remote_request,
                                     &method_not_allowed_body) == 0 &&
      method_not_allowed_body != NULL &&
      strstr(method_not_allowed_body, "\"message\":\"Method not allowed\"") != NULL;

  free(rpc_response_body);
  free(method_not_allowed_body);

  coro_sleep(state->coro_ctx, 50);
  if (state->server) {
    state->server_stopped = 1;
    coro_socket_destroy(state->server);
    state->server = NULL;
  }
  remote_iris_test_state_cleanup(state);
}

spec("turbo agent runtime remote iris api") {
  remote_iris_test_state_t state = {0};

  before_each() {
    memset(&state, 0, sizeof(state));
    iris_app_reset_default();
    reset_router();
    iris_error_recovery_init();
  }

  after_each() {
    remote_iris_test_state_cleanup(&state);
    if (state.coro_ctx) {
      coro_context_destroy(state.coro_ctx);
      state.coro_ctx = NULL;
    }
    reset_router();
    iris_app_reset_default();
    iris_error_recovery_cleanup();
  }

  it("should coexist with iris rpc endpoints on the same app without one global rpc slot") {
    state.coro_ctx = coro_context_create(NULL);
    check_not_null(state.coro_ctx);

    coro_context_spawn(state.coro_ctx, remote_runtime_remote_iris_test_coro, &state);
    coro_context_run(state.coro_ctx, TURBO_RUN_DEFAULT);
    remote_iris_test_state_cleanup(&state);

    check_true(state.rpc_ok);
    check_true(state.remote_ok);
    check_true(state.method_not_allowed_ok);
  }

  it("should tolerate app registry teardown before bridge teardown") {
    turbo_agent_runtime_remote_config_t remote_config = {0};
    turbo_agent_runtime_remote_iris_config_t bridge_config = {0};
    iris_app_t *app = iris_app_default();

    state.store = turbo_agent_runtime_store_memory_create();
    state.runtime = turbo_agent_runtime_create(&state.store);
    check_not_null(state.runtime);

    remote_config.runtime = state.runtime;
    state.remote = turbo_agent_runtime_remote_create(&remote_config);
    check_not_null(state.remote);

    bridge_config.remote = state.remote;
    bridge_config.path = "/v1/runtime/jsonrpc";
    state.bridge = turbo_agent_runtime_remote_iris_create(&bridge_config);
    check_not_null(state.bridge);
    check_int_eq(turbo_agent_runtime_remote_iris_mount(state.bridge, app), 0);
    check_not_null(iris_app_lookup_rpc_context(app, "/v1/runtime/jsonrpc"));

    iris_app_reset_default();

    turbo_agent_runtime_remote_iris_destroy(state.bridge);
    state.bridge = NULL;
  }
}
