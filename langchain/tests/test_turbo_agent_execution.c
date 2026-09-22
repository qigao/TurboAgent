#include "tinytest.h"

#include "turbo_agent_execution.h"
#include "turbo_agent_state.h"

#include <salts_uuid.h>

#include <stdatomic.h>
#include <string.h>
#include <salts/clock.h>

typedef struct execution_gate_s {
  atomic_int entered;
  atomic_int open;
} execution_gate_t;

static int execution_write_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *key = (const char *)user_data;
  json_value_t *value = turbo_json_create_bool(1);

  if (!value) {
    return -1;
  }
  if (turbo_runtime_json_object_set(ctx->json_value_state, key, value) != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(value);
    return -1;
  }
  return 0;
}

static int execution_gated_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  execution_gate_t *gate = (execution_gate_t *)user_data;

  atomic_store_explicit(&gate->entered, 1, memory_order_release);
  while (!atomic_load_explicit(&gate->open, memory_order_acquire)) {
    turbo_thread_yield();
  }
  return execution_write_node(ctx, "visited_start");
}

static void execution_blocking_task(void *user_data) {
  execution_gate_t *gate = (execution_gate_t *)user_data;

  atomic_store_explicit(&gate->entered, 1, memory_order_release);
  while (!atomic_load_explicit(&gate->open, memory_order_acquire)) {
    turbo_thread_yield();
  }
}

static turbo_graph_t *execution_create_graph(turbo_graph_json_value_node_fn start_fn,
                                             void *start_user_data) {
  turbo_graph_t *graph = turbo_graph_create("async-execution");

  check_not_null(graph);
  check_int_eq(turbo_graph_add_json_value_node(graph, "start", start_fn, start_user_data),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_json_value_node(graph, "end", execution_write_node, "visited_end"),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_edge(graph, "start", "end", NULL, NULL), TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);
  return graph;
}

spec("turbo agent execution") {
  it("runs a durable operation asynchronously and moves its result once") {
    turbo_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 4};
    turbo_threadpool_t *pool = turbo_threadpool_create_with_config(&pool_config);
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = execution_create_graph(execution_write_node, "visited_start");
    json_value_t *input = turbo_agent_state_create_json_value();
    json_value_t *summary = NULL;
    json_value_t *state = NULL;
    turbo_agent_execution_t *execution = NULL;
    turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_FAILED;
    int operation_rc = -1;

    check_not_null(pool);
    check_not_null(runtime);
    check_not_null(input);
    check_int_eq(turbo_agent_execution_start(
                     pool, runtime, graph, input, NULL,
                     &(turbo_agent_runtime_exec_options_t){.thread_id = "async-complete"}, NULL,
                     &execution),
                 SALTS_OK);
    check_not_null(execution);
    check_not_null(turbo_agent_execution_id(execution));
    check_size_eq(strlen(turbo_agent_execution_id(execution)), SALTS_UUID_STRING_LENGTH);
    check_int_eq(turbo_agent_execution_wait(execution, UINT64_MAX), SALTS_OK);
    check_int_eq(turbo_agent_execution_get_status(execution, &status), SALTS_OK);
    check_int_eq(status, TURBO_AGENT_EXECUTION_COMPLETED);
    check_int_eq(turbo_agent_execution_result_code(execution, &operation_rc), SALTS_OK);
    check_int_eq(operation_rc, 0);
    check_int_eq(turbo_agent_execution_take_result(execution, &summary, &state), SALTS_OK);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_true(turbo_runtime_json_value_as_bool(turbo_json_object_get(state, "visited_end"), 0));
    check_int_eq(turbo_agent_execution_take_result(execution, &summary, &state), SALTS_EALREADY);

    turbo_runtime_json_destroy(summary);
    turbo_runtime_json_destroy(state);
    turbo_agent_execution_release(execution);
    turbo_runtime_json_destroy(input);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
    turbo_threadpool_destroy(pool);
  }

  it("cancels after a running node and returns a resumable checkpoint") {
    turbo_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 4};
    turbo_threadpool_t *pool = turbo_threadpool_create_with_config(&pool_config);
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    execution_gate_t gate;
    turbo_graph_t *graph;
    json_value_t *input = turbo_agent_state_create_json_value();
    json_value_t *summary = NULL;
    json_value_t *state = NULL;
    turbo_agent_execution_t *execution = NULL;
    turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_FAILED;

    atomic_init(&gate.entered, 0);
    atomic_init(&gate.open, 0);
    graph = execution_create_graph(execution_gated_node, &gate);
    check_not_null(pool);
    check_not_null(runtime);
    check_not_null(input);

    check_int_eq(turbo_agent_execution_start(
                     pool, runtime, graph, input, NULL,
                     &(turbo_agent_runtime_exec_options_t){.thread_id = "async-cancel"}, NULL,
                     &execution),
                 SALTS_OK);
    while (!atomic_load_explicit(&gate.entered, memory_order_acquire)) {
      turbo_thread_yield();
    }
    check_int_eq(turbo_agent_execution_wait(execution, 0), SALTS_ETIMEDOUT);
    check_int_eq(turbo_agent_execution_cancel(execution, TURBO_CANCEL_USER), SALTS_OK);
    atomic_store_explicit(&gate.open, 1, memory_order_release);
    check_int_eq(turbo_agent_execution_wait(execution, UINT64_MAX), SALTS_OK);
    check_int_eq(turbo_agent_execution_get_status(execution, &status), SALTS_OK);
    check_int_eq(status, TURBO_AGENT_EXECUTION_CANCELLED);
    check_int_eq(turbo_agent_execution_cancel(execution, TURBO_CANCEL_SHUTDOWN), SALTS_EALREADY);
    check_int_eq(turbo_agent_execution_take_result(execution, &summary, &state), SALTS_OK);
    check_str_eq(turbo_json_get_string(summary, "status"), "cancelled");
    check_not_null(turbo_json_get_string(summary, "checkpoint_id"));
    check_true(turbo_runtime_json_value_as_bool(turbo_json_object_get(state, "visited_start"), 0));
    check_false(turbo_runtime_json_value_as_bool(turbo_json_object_get(state, "visited_end"), 0));

    turbo_runtime_json_destroy(summary);
    turbo_runtime_json_destroy(state);
    turbo_agent_execution_release(execution);
    turbo_runtime_json_destroy(input);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
    turbo_threadpool_destroy(pool);
  }

  it("turns an expired execution deadline into a timed-out run") {
    turbo_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 4};
    turbo_threadpool_t *pool = turbo_threadpool_create_with_config(&pool_config);
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = execution_create_graph(execution_write_node, "visited_start");
    json_value_t *input = turbo_agent_state_create_json_value();
    json_value_t *summary = NULL;
    json_value_t *state = NULL;
    turbo_agent_execution_t *execution = NULL;
    turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_FAILED;
    turbo_agent_execution_options_t options = {sizeof(options), TURBO_AGENT_EXECUTION_ABI_VERSION,
                                               salts_monotonic_ms()};

    check_int_eq(turbo_agent_execution_start(
                     pool, runtime, graph, input, NULL,
                     &(turbo_agent_runtime_exec_options_t){.thread_id = "async-deadline"}, &options,
                     &execution),
                 SALTS_OK);
    check_int_eq(turbo_agent_execution_wait(execution, UINT64_MAX), SALTS_OK);
    check_int_eq(turbo_agent_execution_get_status(execution, &status), SALTS_OK);
    check_int_eq(status, TURBO_AGENT_EXECUTION_TIMED_OUT);
    check_int_eq(turbo_agent_execution_take_result(execution, &summary, &state), SALTS_OK);
    check_str_eq(turbo_json_get_string(summary, "status"), "timed_out");
    check_false(turbo_runtime_json_value_as_bool(turbo_json_object_get(state, "visited_start"), 0));

    turbo_runtime_json_destroy(summary);
    turbo_runtime_json_destroy(state);
    turbo_agent_execution_release(execution);
    turbo_runtime_json_destroy(input);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
    turbo_threadpool_destroy(pool);
  }

  it("fails fast when the caller-owned executor queue is full") {
    turbo_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    turbo_threadpool_t *pool = turbo_threadpool_create_with_config(&pool_config);
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = execution_create_graph(execution_write_node, "visited_start");
    json_value_t *input = turbo_agent_state_create_json_value();
    turbo_agent_execution_t *execution = NULL;
    execution_gate_t gate;

    atomic_init(&gate.entered, 0);
    atomic_init(&gate.open, 0);
    check_not_null(pool);
    check_not_null(runtime);
    check_not_null(input);
    check_int_eq(turbo_threadpool_try_submit(pool, execution_blocking_task, &gate), 0);
    while (!atomic_load_explicit(&gate.entered, memory_order_acquire)) {
      turbo_thread_yield();
    }
    check_int_eq(turbo_threadpool_try_submit(pool, execution_blocking_task, &gate), 0);
    check_int_eq(turbo_agent_execution_start(
                     pool, runtime, graph, input, NULL,
                     &(turbo_agent_runtime_exec_options_t){.thread_id = "async-rejected"}, NULL,
                     &execution),
                 SALTS_EBUSY);
    check_null(execution);

    atomic_store_explicit(&gate.open, 1, memory_order_release);
    turbo_threadpool_wait(pool);
    turbo_runtime_json_destroy(input);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
    turbo_threadpool_destroy(pool);
  }
}
