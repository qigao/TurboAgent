#include "tinytest.h"

#include "turbo_agent_graph.h"
#include "turbo_agent_runtime.h"
#include "turbo_agent_runtime_remote.h"
#include "turbo_agent_runtime_remote_chttp.h"
#include "turbo_agent_runtime_remote_client.h"
#include "turbo_agent_state.h"

#include <string.h>

enum {
  REMOTE_CHTTP_CONNECTIONS = 4,
  REMOTE_CHTTP_COMMANDS = 16,
  REMOTE_CHTTP_SEND_BYTES = 64 * 1024,
  REMOTE_CHTTP_BUFFER_BYTES = 512 * 1024,
  REMOTE_CHTTP_TIMEOUT_MS = 5000
};

typedef struct {
  const char *key;
  int value;
} remote_chttp_bool_write_t;

typedef struct {
  const char *name;
  turbo_graph_t *graph;
} remote_chttp_graph_registry_t;

static int remote_chttp_write_bool_json_value_node(turbo_graph_exec_ctx_t *ctx,
                                                   void *user_data) {
  remote_chttp_bool_write_t *write = (remote_chttp_bool_write_t *)user_data;
  json_value_t *value = json_create_bool(write->value != 0);
  if (!value) return -1;
  return turbo_runtime_json_object_set(ctx->json_value_state, write->key, value) ==
                 TURBO_RUNTIME_JSON_OK
             ? 0
             : -1;
}

static turbo_graph_t *remote_chttp_graph_resolver(const char *graph_name, void *user_data) {
  remote_chttp_graph_registry_t *registry = (remote_chttp_graph_registry_t *)user_data;
  if (!registry || !graph_name || !registry->name || !registry->graph) return NULL;
  return strcmp(graph_name, registry->name) == 0 ? registry->graph : NULL;
}

static turbo_graph_t *remote_chttp_graph_create(void) {
  turbo_graph_t *graph = turbo_graph_create("remote-chttp-test");
  static remote_chttp_bool_write_t start = {"visited_start", 1};
  static remote_chttp_bool_write_t end = {"visited_end", 1};

  if (!graph) return NULL;
  if (turbo_graph_add_json_value_node(graph, "start",
                                      remote_chttp_write_bool_json_value_node,
                                      &start) != TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_json_value_node(graph, "end",
                                      remote_chttp_write_bool_json_value_node,
                                      &end) != TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_json_value_edge(graph, "start", "end", NULL, NULL) !=
          TURBO_GRAPH_EXEC_OK ||
      turbo_graph_set_entry(graph, "start") != TURBO_GRAPH_EXEC_OK) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  return graph;
}

static chttp_server_config remote_chttp_server_config(void) {
  chttp_server_config config = {0};
  config.host = "127.0.0.1";
  config.port = 0u;
  config.backlog = REMOTE_CHTTP_CONNECTIONS;
#if defined(_WIN32)
  config.network.backend = NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  config.network.backend = NATIVE_IO_BACKEND_EPOLL;
#else
  config.network.backend = NATIVE_IO_BACKEND_KQUEUE;
#endif
  config.network.connection_capacity = REMOTE_CHTTP_CONNECTIONS;
  config.network.command_capacity = REMOTE_CHTTP_COMMANDS;
  config.network.request_capacity = REMOTE_CHTTP_COMMANDS;
  config.network.completion_batch_capacity = REMOTE_CHTTP_CONNECTIONS;
  config.network.event_capacity = REMOTE_CHTTP_COMMANDS;
  config.network.max_send_bytes = REMOTE_CHTTP_SEND_BYTES;
  config.network.receive_buffer_bytes = REMOTE_CHTTP_SEND_BYTES;
  config.network.connect_timeout_ms = REMOTE_CHTTP_TIMEOUT_MS;
  config.network.read_timeout_ms = REMOTE_CHTTP_TIMEOUT_MS;
  config.network.write_timeout_ms = REMOTE_CHTTP_TIMEOUT_MS;
  config.route_capacity = 2u;
  config.middleware_capacity = 1u;
  config.max_route_middleware_count = 1u;
  config.max_route_param_count = 1u;
  config.max_route_param_bytes = 256u;
  config.max_target_bytes = 256u;
  config.max_header_count = 32u;
  config.max_header_bytes = 8192u;
  config.max_request_body_bytes = REMOTE_CHTTP_SEND_BYTES;
  config.max_response_header_count = 32u;
  config.max_response_header_bytes = 8192u;
  config.max_response_body_bytes = REMOTE_CHTTP_SEND_BYTES;
  config.max_buffered_response_body_bytes = REMOTE_CHTTP_SEND_BYTES;
  config.buffer_capacity_bytes = REMOTE_CHTTP_BUFFER_BYTES;
  config.poll_slice_ms = 1u;
  return config;
}

spec("turbo agent runtime remote CHTTP api") {
  it("round-trips runtime.start through CHTTP without TurboHttp or CoroNet") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = remote_chttp_graph_create();
    remote_chttp_graph_registry_t registry = {"remote-chttp", graph};
    turbo_agent_runtime_remote_config_t remote_config = {
        .runtime = runtime,
        .graph_resolver = remote_chttp_graph_resolver,
        .graph_resolver_user_data = &registry};
    turbo_agent_runtime_remote_t *remote = NULL;
    chttp_server server = {0};
    chttp_server_config server_config = remote_chttp_server_config();
    uint16_t port = 0u;
    char url[256];
    turbo_agent_runtime_remote_client_config_t client_config = {0};
    turbo_agent_runtime_remote_client_t *client = NULL;
    turbo_graph_run_options_t options = {0};
    static const char *interrupt_before[] = {"end"};
    json_value_t *input = NULL;
    json_value_t *summary = NULL;
    json_value_t *state = NULL;
    json_value_t *error = NULL;
    int status;

    check_not_null(runtime);
    check_not_null(graph);
    remote = turbo_agent_runtime_remote_create(&remote_config);
    check_not_null(remote);

    status = chttp_server_init(&server, &server_config);
    check_int_eq(status, SALTS_OK);
    check_int_eq(turbo_agent_runtime_remote_chttp_mount(
                     remote, &server, "/v1/runtime/jsonrpc"),
                 SALTS_OK);
    check_int_eq(chttp_server_start(&server), SALTS_OK);
    check_int_eq(chttp_server_port(&server, &port), SALTS_OK);
    check_true(port != 0u);
    check_true(snprintf(url, sizeof(url),
                        "http://127.0.0.1:%u/v1/runtime/jsonrpc",
                        (unsigned int)port) > 0);

    client_config.url = url;
    client = turbo_agent_runtime_remote_client_create(&client_config);
    check_not_null(client);

    input = turbo_agent_state_create_json_value();
    check_not_null(input);
    options.interrupt_before_nodes = interrupt_before;
    options.interrupt_before_count = 1u;

    check_int_eq(turbo_agent_runtime_remote_client_start_json_value_graph(
                     client, "remote-chttp", input, &options, "thr_remote_chttp",
                     &summary, &state, &error),
                 0);
    check_null(error);
    check_not_null(summary);
    check_not_null(state);
    check_str_eq(json_get_string(summary, "status"), "interrupted");
    check_str_eq(json_get_string(summary, "thread_id"), "thr_remote_chttp");
    check_true(json_get_bool(state, "visited_start", false));
    check_false(json_get_bool(state, "visited_end", false));

    json_free(error);
    json_free(state);
    json_free(summary);
    turbo_runtime_json_destroy(input);
    turbo_agent_runtime_remote_client_destroy(client);
    check_int_eq(chttp_server_stop(&server, REMOTE_CHTTP_TIMEOUT_MS), SALTS_OK);
    check_int_eq(chttp_server_destroy(&server), SALTS_OK);
    turbo_agent_runtime_remote_destroy(remote);
    turbo_agent_runtime_destroy(runtime);
    turbo_graph_destroy(graph);
  }
}
