#include "error_recovery.h"
#include "iris_app.h"
#include "server.h"
#include "turbo_agent_graph.h"
#include "turbo_agent_remote_app.h"
#include "turbo_agent_remote_session.h"
#include "turbo_agent_runtime.h"
#include "turbo_agent_runtime_remote.h"
#include "turbo_agent_runtime_remote_client.h"
#include "turbo_agent_runtime_remote_iris.h"
#include "turbo_agent_state.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  const char *key;
  int value;
} runtime_remote_example_bool_write_t;

typedef struct {
  const char *name;
  turbo_graph_t *graph;
} runtime_remote_example_graph_registry_t;

typedef struct {
  coro_context_t *coro_ctx;
  coro_socket_t *server;
  int server_stopped;
  int exit_code;
  turbo_graph_t *graph;
  turbo_agent_runtime_store_t store;
  turbo_agent_runtime_t *runtime;
  turbo_agent_runtime_remote_t *remote;
  turbo_agent_runtime_remote_iris_t *bridge;
  turbo_agent_runtime_remote_client_t *client;
  turbo_agent_remote_session_t *session;
  turbo_agent_remote_app_t *app;
} runtime_remote_example_state_t;

static const char *runtime_remote_example_text(const char *text) {
  return text ? text : "(null)";
}

static int runtime_remote_example_write_bool_json_value_node(turbo_graph_exec_ctx_t *ctx,
                                                       void *user_data) {
  runtime_remote_example_bool_write_t *write =
      (runtime_remote_example_bool_write_t *)user_data;
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

static int runtime_remote_example_finalize_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *final_output = (const char *)user_data;

  if (!ctx || !ctx->state || !final_output) {
    return -1;
  }
  turbo_json_object_set_bool(ctx->state, "visited_end", true);
  return turbo_agent_state_set_final_answer(ctx->state, final_output);
}

static turbo_graph_t *runtime_remote_example_graph_resolver(const char *graph_name,
                                                            void *user_data) {
  runtime_remote_example_graph_registry_t *registry =
      (runtime_remote_example_graph_registry_t *)user_data;

  if (!registry || !graph_name || !registry->name || !registry->graph) {
    return NULL;
  }
  return strcmp(graph_name, registry->name) == 0 ? registry->graph : NULL;
}

static turbo_graph_t *runtime_remote_example_create_graph(void) {
  turbo_graph_t *graph = turbo_graph_create("runtime-remote-inspect-example");
  static runtime_remote_example_bool_write_t start = {"visited_start", 1};
  static const char *final_output = "{\"ok\":true,\"source\":\"remote-example\"}";

  if (!graph) {
    return NULL;
  }
  if (turbo_graph_add_json_value_node(graph, "start", runtime_remote_example_write_bool_json_value_node,
                                &start) != TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_node(graph, "end", runtime_remote_example_finalize_node,
                           (void *)final_output) != TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_json_value_edge(graph, "start", "end", NULL, NULL) !=
          TURBO_GRAPH_EXEC_OK ||
      turbo_graph_set_entry(graph, "start") != TURBO_GRAPH_EXEC_OK) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  return graph;
}

static json_value_t *runtime_remote_example_create_state_json_value(void) {
  json_value_t *state = turbo_agent_state_create();
  json_value_t *bound;

  if (!state) {
    return NULL;
  }
  if (turbo_agent_state_set_active_agent(state, "planner") != 0 ||
      turbo_agent_state_request_handoff(state, "executor", "delegate execution") != 0 ||
      turbo_agent_state_request_review(state, "need approval") != 0 ||
      turbo_agent_state_set_review_approved(state, 0) != 0) {
    turbo_free_json(&state);
    return NULL;
  }
  bound = turbo_json_clone(state);
  turbo_free_json(&state);
  return bound;
}

static json_value_t *runtime_remote_example_create_output_item(const char *thread_id,
                                                               const char *run_id,
                                                               const char *checkpoint_id) {
  json_value_t *output_item = turbo_json_create_object();

  if (!output_item) {
    return NULL;
  }
  turbo_json_object_set_string(output_item, "child_thread_id", thread_id);
  turbo_json_object_set_string(output_item, "child_run_id", run_id);
  turbo_json_object_set_string(output_item, "child_checkpoint_id", checkpoint_id);
  turbo_json_object_set_string(output_item, "child_status", "interrupted");
  turbo_json_object_set_string(output_item, "parent_agent_run_id", "run_parent");
  turbo_json_object_set_string(output_item, "parent_tool_call_id", "call_parent");
  turbo_json_object_set_string(output_item, "parent_tool_name", "delegate");
  turbo_json_object_set_string(output_item, "parent_graph_run_id", "run_parent");
  turbo_json_object_set_string(output_item, "call_frame_id", "call_parent");
  return output_item;
}

static void runtime_remote_example_print_supervisor(const char *label,
                                                    const json_value_t *inspect) {
  const json_value_t *supervisor;

  supervisor = inspect ? turbo_json_object_get(inspect, "supervisor") : NULL;
  printf("%s supervisor active_agent: %s\n", label,
         runtime_remote_example_text(
             supervisor ? turbo_json_get_string(supervisor, "active_agent") : NULL));
}

static void runtime_remote_example_print_orchestration(const char *label,
                                                       const json_value_t *inspect) {
  const json_value_t *thread_lineage;

  thread_lineage = inspect ? turbo_json_object_get(inspect, "thread_lineage") : NULL;
  printf("%s orchestration thread_id: %s\n", label,
         runtime_remote_example_text(
             thread_lineage ? turbo_json_get_string(thread_lineage, "thread_id") : NULL));
}

static void runtime_remote_example_print_child_multi_agent(const char *label,
                                                           const json_value_t *inspect) {
  const json_value_t *child_orchestration;

  child_orchestration =
      inspect ? turbo_json_object_get(inspect, "child_orchestration_inspect") : NULL;
  printf("%s child multi-agent parent_tool_name: %s\n", label,
         runtime_remote_example_text(child_orchestration
                                         ? turbo_json_get_string(child_orchestration,
                                                                 "parent_tool_name")
                                         : NULL));
}

static void runtime_remote_example_drain_context(coro_context_t *ctx, uint64_t timeout_ms) {
  uint64_t deadline;

  if (!ctx) {
    return;
  }
  deadline = turbo_monotonic_ms() + timeout_ms;
  while (coro_context_alive(ctx) && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, TURBO_RUN_NOWAIT);
  }
}

static void runtime_remote_example_cleanup(runtime_remote_example_state_t *state) {
  if (!state) {
    return;
  }
  if (state->server) {
    state->server_stopped = 1;
    coro_socket_destroy(state->server);
    state->server = NULL;
    runtime_remote_example_drain_context(state->coro_ctx, 1000);
  }
  if (state->app) {
    turbo_agent_remote_app_destroy(state->app);
    state->app = NULL;
  }
  if (state->session) {
    turbo_agent_remote_session_destroy(state->session);
    state->session = NULL;
  }
  if (state->client) {
    turbo_agent_runtime_remote_client_destroy(state->client);
    state->client = NULL;
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
  if (state->graph) {
    turbo_graph_destroy(state->graph);
    state->graph = NULL;
  }
  runtime_remote_example_drain_context(state->coro_ctx, 1000);
}

static void runtime_remote_example_coro(coro_t *co, void *arg) {
  runtime_remote_example_state_t *state = (runtime_remote_example_state_t *)arg;
  iris_app_t *app = iris_app_default();
  runtime_remote_example_graph_registry_t registry = {0};
  turbo_agent_runtime_remote_config_t remote_config = {0};
  turbo_agent_runtime_remote_iris_config_t bridge_config = {0};
  turbo_agent_runtime_remote_client_config_t client_config = {0};
  turbo_agent_remote_session_config_t session_config = {0};
  turbo_agent_remote_session_config_t app_session_config = {0};
  turbo_agent_remote_app_config_t app_config = {0};
  json_value_t *input_state = NULL;
  json_value_t *result_state = NULL;
  json_value_t *summary_json = NULL;
  json_value_t *error_json = NULL;
  json_value_t *output_item = NULL;
  json_value_t *client_supervisor = NULL;
  json_value_t *session_orchestration = NULL;
  json_value_t *app_child_multi_agent = NULL;
  turbo_graph_run_options_t interrupt_options = {0};
  char endpoint_url[256];
  const char *thread_id;
  const char *run_id;
  const char *checkpoint_id;
  int written;
  const unsigned short port = 29891;
  static const char *interrupt_before_end[] = {"end"};

  (void)co;

  state->exit_code = 1;
  state->store = turbo_agent_runtime_store_memory_create();
  state->runtime = turbo_agent_runtime_create(&state->store);
  state->graph = runtime_remote_example_create_graph();
  if (!state->runtime || !state->graph) {
    fprintf(stderr, "failed to initialize remote example runtime\n");
    goto cleanup;
  }

  registry.name = "remote-inspect";
  registry.graph = state->graph;
  remote_config.runtime = state->runtime;
  remote_config.graph_resolver = runtime_remote_example_graph_resolver;
  remote_config.graph_resolver_user_data = &registry;
  state->remote = turbo_agent_runtime_remote_create(&remote_config);
  if (!state->remote) {
    fprintf(stderr, "failed to create remote runtime facade\n");
    goto cleanup;
  }

  bridge_config.remote = state->remote;
  bridge_config.path = "/v1/runtime/jsonrpc";
  state->bridge = turbo_agent_runtime_remote_iris_create(&bridge_config);
  if (!state->bridge || turbo_agent_runtime_remote_iris_mount(state->bridge, app) != 0) {
    fprintf(stderr, "failed to mount remote runtime iris bridge\n");
    goto cleanup;
  }

  if (init_router() != 0) {
    fprintf(stderr, "init_router failed\n");
    goto cleanup;
  }
  state->server = iris_server_start(app, state->coro_ctx, port);
  if (!state->server) {
    fprintf(stderr, "iris_server_start failed\n");
    goto cleanup;
  }

  coro_yield();
  coro_sleep(state->coro_ctx, 50);

  written = snprintf(endpoint_url, sizeof(endpoint_url), "http://127.0.0.1:%u/v1/runtime/jsonrpc",
                     (unsigned)port);
  if (written <= 0 || (size_t)written >= sizeof(endpoint_url)) {
    fprintf(stderr, "failed to format endpoint url\n");
    goto cleanup;
  }

  client_config.url = endpoint_url;
  state->client = turbo_agent_runtime_remote_client_create(&client_config);
  if (!state->client) {
    fprintf(stderr, "failed to create remote client\n");
    goto cleanup;
  }

  interrupt_options.interrupt_before_nodes = interrupt_before_end;
  interrupt_options.interrupt_before_count = 1;
  input_state = runtime_remote_example_create_state_json_value();
  if (!input_state) {
    fprintf(stderr, "failed to create input state\n");
    goto cleanup;
  }
  if (turbo_agent_runtime_remote_client_start_json_value_graph(
          state->client, "remote-inspect", input_state, &interrupt_options,
          "thr_remote_example", &summary_json, &result_state, &error_json) != 0 ||
      !summary_json || !result_state || error_json) {
    fprintf(stderr, "remote client start_json_value_graph failed\n");
    goto cleanup;
  }

  thread_id = turbo_json_get_string(summary_json, "thread_id");
  run_id = turbo_json_get_string(summary_json, "run_id");
  checkpoint_id = turbo_json_get_string(summary_json, "checkpoint_id");
  if (!thread_id || !run_id || !checkpoint_id) {
    fprintf(stderr, "remote client summary missing ids\n");
    goto cleanup;
  }
  output_item = runtime_remote_example_create_output_item(thread_id, run_id, checkpoint_id);
  if (!output_item) {
    fprintf(stderr, "failed to create child output item\n");
    goto cleanup;
  }

  session_config.client_config.url = endpoint_url;
  session_config.thread_id = thread_id;
  state->session = turbo_agent_remote_session_create(&session_config);
  if (!state->session) {
    fprintf(stderr, "failed to create remote session wrapper\n");
    goto cleanup;
  }

  app_session_config.client_config.url = endpoint_url;
  app_session_config.thread_id = thread_id;
  app_config.session_config = &app_session_config;
  app_config.graph_name = "remote-inspect";
  state->app = turbo_agent_remote_app_create(&app_config);
  if (!state->app) {
    fprintf(stderr, "failed to create remote app wrapper\n");
    goto cleanup;
  }

  /* Keep the example stable and short by printing one representative helper per layer.
   * The same layer also exposes the sibling trio listed in README/runtime-v2 docs. */
  if (turbo_agent_runtime_remote_client_get_supervisor_inspect(state->client, thread_id,
                                                               &client_supervisor) != 0 ||
      !client_supervisor) {
    fprintf(stderr, "remote client supervisor helper failed\n");
    goto cleanup;
  }

  if (turbo_agent_remote_session_get_orchestration_inspect(state->session,
                                                           &session_orchestration) != 0 ||
      !session_orchestration) {
    fprintf(stderr, "remote session orchestration helper failed\n");
    goto cleanup;
  }

  if (turbo_agent_remote_app_get_child_multi_agent_inspect(state->app, output_item,
                                                           &app_child_multi_agent) != 0 ||
      !app_child_multi_agent) {
    fprintf(stderr, "remote app child multi-agent helper failed\n");
    goto cleanup;
  }

  printf("remote seed\n");
  printf("  thread_id: %s\n", runtime_remote_example_text(thread_id));
  printf("  run_id: %s\n", runtime_remote_example_text(run_id));
  printf("  checkpoint_id: %s\n", runtime_remote_example_text(checkpoint_id));
  runtime_remote_example_print_supervisor("remote client", client_supervisor);
  runtime_remote_example_print_orchestration("remote session", session_orchestration);
  runtime_remote_example_print_child_multi_agent("remote app", app_child_multi_agent);
  state->exit_code = 0;

cleanup:
  turbo_free_json(&app_child_multi_agent);
  turbo_free_json(&session_orchestration);
  turbo_free_json(&client_supervisor);
  turbo_free_json(&output_item);
  turbo_free_json(&summary_json);
  turbo_free_json(&error_json);
  turbo_runtime_json_destroy(result_state);
  turbo_runtime_json_destroy(input_state);
  coro_sleep(state->coro_ctx, 50);
  if (state->server) {
    state->server_stopped = 1;
    coro_socket_destroy(state->server);
    state->server = NULL;
  }
}

int main(void) {
  runtime_remote_example_state_t state = {0};
  int exit_code = 1;

  iris_app_reset_default();
  reset_router();
  if (iris_error_recovery_init() != 0) {
    fprintf(stderr, "iris_error_recovery_init failed\n");
    return 1;
  }

  state.coro_ctx = coro_context_create(NULL);
  if (!state.coro_ctx) {
    fprintf(stderr, "coro_context_create failed\n");
    iris_error_recovery_cleanup();
    return 1;
  }

  coro_context_spawn(state.coro_ctx, runtime_remote_example_coro, &state);
  coro_context_run(state.coro_ctx, TURBO_RUN_DEFAULT);
  exit_code = state.exit_code;

  runtime_remote_example_cleanup(&state);
  if (state.coro_ctx) {
    coro_context_destroy(state.coro_ctx);
    state.coro_ctx = NULL;
  }
  reset_router();
  iris_app_reset_default();
  iris_error_recovery_cleanup();
  return exit_code;
}
