#include "turbo_state_graph.h"
#include "turbo_state_graph_store.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
  const char *text;
} state_graph_store_file_example_payload_t;

static const char *state_graph_store_file_example_text(const char *text) {
  return text ? text : "(null)";
}

static turbo_runtime_data_bind_value_t *state_graph_store_file_example_make_string_array(
    const char *value) {
  turbo_runtime_data_bind_value_t *array = turbo_runtime_data_bind_value_create_array();

  if (!array) {
    return NULL;
  }
  if (turbo_runtime_data_bind_array_append(
          array, turbo_runtime_data_bind_value_create_string(value)) !=
      TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(array);
    return NULL;
  }
  return array;
}

static int state_graph_store_file_example_start_node(turbo_state_graph_exec_ctx_t *ctx,
                                                     void *user_data) {
  const state_graph_store_file_example_payload_t *payload =
      (const state_graph_store_file_example_payload_t *)user_data;

  if (turbo_runtime_data_bind_object_set(
          ctx->update, "count", turbo_runtime_data_bind_value_create_int64(1)) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(
          ctx->update, "messages",
          state_graph_store_file_example_make_string_array(payload->text)) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    return -1;
  }
  return 0;
}

static int state_graph_store_file_example_done_node(turbo_state_graph_exec_ctx_t *ctx,
                                                    void *user_data) {
  (void)user_data;

  if (turbo_runtime_data_bind_object_set(
          ctx->update, "count", turbo_runtime_data_bind_value_create_int64(5)) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(
          ctx->update, "messages",
          state_graph_store_file_example_make_string_array("done")) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    return -1;
  }
  return 0;
}

static turbo_state_graph_t *state_graph_store_file_example_create_graph(
    const state_graph_store_file_example_payload_t *payload) {
  turbo_state_graph_t *graph = turbo_state_graph_create("state-graph-store-file-example");
  turbo_state_graph_channel_config_t config = {0};
  turbo_runtime_data_bind_value_t *default_value = NULL;

  if (!graph) {
    return NULL;
  }

  default_value = turbo_runtime_data_bind_value_create_int64(0);
  config.reducer = TURBO_STATE_GRAPH_REDUCER_ADD;
  config.value_kind = TURBO_RUNTIME_DATA_BIND_VALUE_INT64;
  config.default_value = default_value;
  if (turbo_state_graph_add_channel(graph, "count", &config) != TURBO_STATE_GRAPH_OK) {
    turbo_runtime_data_bind_value_destroy(default_value);
    turbo_state_graph_destroy(graph);
    return NULL;
  }
  turbo_runtime_data_bind_value_destroy(default_value);

  default_value = turbo_runtime_data_bind_value_create_array();
  config.reducer = TURBO_STATE_GRAPH_REDUCER_APPEND;
  config.value_kind = TURBO_RUNTIME_DATA_BIND_VALUE_STRING;
  config.default_value = default_value;
  if (turbo_state_graph_add_channel(graph, "messages", &config) != TURBO_STATE_GRAPH_OK) {
    turbo_runtime_data_bind_value_destroy(default_value);
    turbo_state_graph_destroy(graph);
    return NULL;
  }
  turbo_runtime_data_bind_value_destroy(default_value);

  default_value = turbo_runtime_data_bind_value_create_bool(0);
  config.reducer = TURBO_STATE_GRAPH_REDUCER_REPLACE;
  config.value_kind = TURBO_RUNTIME_DATA_BIND_VALUE_BOOL;
  config.default_value = default_value;
  if (turbo_state_graph_add_channel(graph, "approved", &config) != TURBO_STATE_GRAPH_OK) {
    turbo_runtime_data_bind_value_destroy(default_value);
    turbo_state_graph_destroy(graph);
    return NULL;
  }
  turbo_runtime_data_bind_value_destroy(default_value);

  if (turbo_state_graph_add_bind_node(graph, "start", state_graph_store_file_example_start_node,
                                      (void *)payload) != TURBO_STATE_GRAPH_OK ||
      turbo_state_graph_add_bind_node(graph, "done", state_graph_store_file_example_done_node,
                                      NULL) != TURBO_STATE_GRAPH_OK ||
      turbo_state_graph_add_bind_edge(graph, "start", "done", NULL, NULL) !=
          TURBO_STATE_GRAPH_OK ||
      turbo_state_graph_set_entry(graph, "start") != TURBO_STATE_GRAPH_OK) {
    turbo_state_graph_destroy(graph);
    return NULL;
  }

  return graph;
}

static int state_graph_store_file_example_print_thread_view(turbo_state_graph_t *graph,
                                                            const char *thread_id,
                                                            const char *label) {
  turbo_runtime_data_bind_value_t *latest_run = NULL;
  turbo_runtime_data_bind_value_t *thread_state = NULL;
  turbo_runtime_data_bind_value_t *pending_run = NULL;
  turbo_state_graph_status_t pending_status;

  if (turbo_state_graph_get_latest_run(graph, thread_id, &latest_run) != TURBO_STATE_GRAPH_OK ||
      turbo_state_graph_get_thread_state(graph, thread_id, &thread_state) !=
          TURBO_STATE_GRAPH_OK) {
    turbo_runtime_data_bind_value_destroy(latest_run);
    turbo_runtime_data_bind_value_destroy(thread_state);
    return -1;
  }

  pending_status = turbo_state_graph_get_pending_run(graph, thread_id, &pending_run);
  if (pending_status != TURBO_STATE_GRAPH_OK &&
      pending_status != TURBO_STATE_GRAPH_RUN_NOT_FOUND) {
    turbo_runtime_data_bind_value_destroy(pending_run);
    turbo_runtime_data_bind_value_destroy(thread_state);
    turbo_runtime_data_bind_value_destroy(latest_run);
    return -1;
  }

  printf(
      "%s\n"
      "  latest_run_status: %s\n"
      "  approved: %d\n"
      "  message_count: %zu\n"
      "  has_pending_run: %s\n",
      label,
      state_graph_store_file_example_text(turbo_runtime_data_bind_value_as_string(
          turbo_runtime_data_bind_object_get(latest_run, "status"))),
      turbo_runtime_data_bind_value_as_bool(
          turbo_runtime_data_bind_object_get(thread_state, "approved"), 0),
      turbo_runtime_data_bind_value_size(
          turbo_runtime_data_bind_object_get(thread_state, "messages")),
      pending_status == TURBO_STATE_GRAPH_OK ? "yes" : "no");

  turbo_runtime_data_bind_value_destroy(pending_run);
  turbo_runtime_data_bind_value_destroy(thread_state);
  turbo_runtime_data_bind_value_destroy(latest_run);
  return 0;
}

int main(void) {
  state_graph_store_file_example_payload_t payload = {"review"};
  turbo_state_graph_t *graph = NULL;
  turbo_state_graph_t *restored = NULL;
  turbo_state_graph_store_t store = {0};
  turbo_state_graph_run_result_t interrupted = {0};
  turbo_state_graph_run_result_t updated = {0};
  turbo_state_graph_run_result_t resumed = {0};
  turbo_state_graph_run_options_t options = {0};
  turbo_state_graph_store_list_options_t filter = {0};
  turbo_runtime_data_bind_value_t *state = NULL;
  turbo_runtime_data_bind_value_t *patch = NULL;
  json_value_t *snapshot_ids = NULL;
  char *latest_snapshot_id = NULL;
  int exit_code = 1;
  const char *interrupt_before[] = {"done"};

  graph = state_graph_store_file_example_create_graph(&payload);
  restored = state_graph_store_file_example_create_graph(&payload);
  store = turbo_state_graph_store_file_create("state-graph-store-file-example");
  patch = turbo_runtime_data_bind_value_create_object();
  if (!graph || !restored || !store.user_data || !patch) {
    fprintf(stderr, "failed to initialize state graph file-store example\n");
    goto cleanup;
  }

  (void)turbo_state_graph_store_delete(&store, "file-snapshot-1");
  (void)turbo_state_graph_store_delete(&store, "file-snapshot-2");
  (void)turbo_state_graph_store_delete(&store, "file-snapshot-3");

  options.interrupt_before_nodes = interrupt_before;
  options.interrupt_before_count = 1;
  if (turbo_state_graph_start(graph, "thread-file-example", NULL, &options, &interrupted,
                              &state) != TURBO_STATE_GRAPH_INTERRUPTED) {
    fprintf(stderr, "expected initial file-store run to interrupt before done\n");
    goto cleanup;
  }
  turbo_runtime_data_bind_value_destroy(state);
  state = NULL;

  if (turbo_state_graph_store_save_snapshot(&store, "file-snapshot-1", graph) !=
      TURBO_STATE_GRAPH_OK) {
    fprintf(stderr, "save file-snapshot-1 failed\n");
    goto cleanup;
  }

  if (turbo_runtime_data_bind_object_set(
          patch, "approved", turbo_runtime_data_bind_value_create_bool(1)) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(
          patch, "messages", state_graph_store_file_example_make_string_array("manual")) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    fprintf(stderr, "failed to build file-store patch\n");
    goto cleanup;
  }

  if (turbo_state_graph_update_state(graph, interrupted.run_id, patch, NULL, &updated, &state) !=
      TURBO_STATE_GRAPH_OK) {
    fprintf(stderr, "file-store update_state failed\n");
    goto cleanup;
  }
  turbo_runtime_data_bind_value_destroy(state);
  state = NULL;

  if (turbo_state_graph_store_save_snapshot(&store, "file-snapshot-2", graph) !=
      TURBO_STATE_GRAPH_OK) {
    fprintf(stderr, "save file-snapshot-2 failed\n");
    goto cleanup;
  }

  if (turbo_state_graph_resume(graph, interrupted.run_id, NULL, &resumed, &state) !=
      TURBO_STATE_GRAPH_OK) {
    fprintf(stderr, "file-store resume failed\n");
    goto cleanup;
  }
  turbo_runtime_data_bind_value_destroy(state);
  state = NULL;

  if (turbo_state_graph_store_save_snapshot(&store, "file-snapshot-3", graph) !=
      TURBO_STATE_GRAPH_OK) {
    fprintf(stderr, "save file-snapshot-3 failed\n");
    goto cleanup;
  }

  if (turbo_state_graph_store_list(&store, &snapshot_ids) != 0) {
    fprintf(stderr, "file-store list failed\n");
    goto cleanup;
  }
  printf("file store snapshot count: %d\n", (int)turbo_json_array_size(snapshot_ids));
  turbo_free_json(&snapshot_ids);
  snapshot_ids = NULL;

  if (turbo_state_graph_store_get_latest_snapshot_id_for_thread(
          &store, "thread-file-example", &latest_snapshot_id) != 0) {
    fprintf(stderr, "get_latest_snapshot_id_for_thread(file) failed\n");
    goto cleanup;
  }
  printf("latest snapshot for thread-file-example: %s\n", latest_snapshot_id);
  free(latest_snapshot_id);
  latest_snapshot_id = NULL;

  filter.run_status = "interrupted";
  if (turbo_state_graph_store_get_latest_snapshot_id(&store, &filter, &latest_snapshot_id) != 0) {
    fprintf(stderr, "get_latest_snapshot_id(interrupted file) failed\n");
    goto cleanup;
  }
  printf("latest interrupted file snapshot: %s\n", latest_snapshot_id);
  free(latest_snapshot_id);
  latest_snapshot_id = NULL;

  if (turbo_state_graph_store_load_latest_snapshot(&store, &filter, restored) !=
      TURBO_STATE_GRAPH_OK) {
    fprintf(stderr, "load_latest_snapshot(interrupted file) failed\n");
    goto cleanup;
  }
  if (state_graph_store_file_example_print_thread_view(
          restored, "thread-file-example", "restored interrupted file snapshot") != 0) {
    fprintf(stderr, "print interrupted file snapshot failed\n");
    goto cleanup;
  }

  if (turbo_state_graph_store_load_latest_snapshot_for_thread(
          &store, "thread-file-example", restored) != TURBO_STATE_GRAPH_OK) {
    fprintf(stderr, "load_latest_snapshot_for_thread(file) failed\n");
    goto cleanup;
  }
  if (state_graph_store_file_example_print_thread_view(
          restored, "thread-file-example", "restored latest file snapshot") != 0) {
    fprintf(stderr, "print latest file snapshot failed\n");
    goto cleanup;
  }

  if (turbo_state_graph_store_delete(&store, "file-snapshot-1") != 0 ||
      turbo_state_graph_store_delete(&store, "file-snapshot-2") != 0 ||
      turbo_state_graph_store_delete(&store, "file-snapshot-3") != 0) {
    fprintf(stderr, "file-store cleanup delete failed\n");
    goto cleanup;
  }
  if (turbo_state_graph_store_list(&store, &snapshot_ids) != 0) {
    fprintf(stderr, "file-store final list failed\n");
    goto cleanup;
  }
  printf("file store snapshot count after cleanup: %d\n",
         (int)turbo_json_array_size(snapshot_ids));
  turbo_free_json(&snapshot_ids);
  snapshot_ids = NULL;

  exit_code = 0;

cleanup:
  turbo_free_json(&snapshot_ids);
  free(latest_snapshot_id);
  turbo_runtime_data_bind_value_destroy(patch);
  turbo_runtime_data_bind_value_destroy(state);
  turbo_state_graph_store_destroy(&store);
  turbo_state_graph_destroy(restored);
  turbo_state_graph_destroy(graph);
  return exit_code;
}
