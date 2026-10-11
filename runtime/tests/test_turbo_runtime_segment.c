#include "tinytest.h"
#include "turbo_runtime_segment.h"
#include "turbo_runtime_control.h"
#include "turbo_runtime_history.h"
#include "turbo_runtime_diagram.h"
#include <tstr.h>
#include <salts/error_codes.h>
#include <string.h>

typedef struct segment_probe_s {
  turbo_agent_runtime_store_t backend;
  const char *reject_collection;
  int writes;
  int depth;
  int entered;
  int left;
  int scope_errors;
  int nodes;
  int events;
  int checkpoints;
  int snapshot_calls;
  int summary_calls;
  int fail_node;
  int fail_snapshot;
  int reject_published_head;
  int head_read_error;
} segment_probe_t;

static int segment_put(void *data, const char *collection, const char *id, const char *text) {
  segment_probe_t *probe = data;
  ++probe->writes;
  if (probe->reject_published_head && strcmp(collection, "thread_heads") == 0 &&
      strstr(text, "\"state_snapshot\"")) return SALTS_EIO;
  if (probe->reject_collection && strcmp(probe->reject_collection, collection) == 0) return -1;
  return probe->backend.put(probe->backend.user_data, collection, id, text);
}
static int segment_get(void *data, const char *collection, const char *id, char **text) {
  segment_probe_t *probe = data;
  if (probe->head_read_error && strcmp(collection, "thread_heads") == 0) return probe->head_read_error;
  return probe->backend.get(probe->backend.user_data, collection, id, text);
}
static int segment_list(void *data, const char *collection, const char *key,
                        const char *value, char **text) {
  segment_probe_t *probe = data;
  return probe->backend.list(probe->backend.user_data, collection, key, value, text);
}
static void segment_store_free(void *data) {
  segment_probe_t *probe = data;
  probe->backend.user_data_free(probe->backend.user_data);
  memset(&probe->backend, 0, sizeof(probe->backend));
}
static void segment_scope_check(segment_probe_t *probe) {
  if (probe->depth != 1) ++probe->scope_errors;
}
static void segment_enter(const turbo_runtime_segment_t *segment, void *data) {
  segment_probe_t *probe = data;
  (void)segment;
  ++probe->entered;
  ++probe->depth;
}
static void segment_leave(const turbo_runtime_segment_t *segment, void *data) {
  segment_probe_t *probe = data;
  (void)segment;
  segment_scope_check(probe);
  ++probe->left;
  --probe->depth;
}
static int segment_node(turbo_graph_exec_ctx_t *ctx, void *data) {
  segment_probe_t *probe = data;
  int64_t count = turbo_runtime_json_value_as_int64(json_object_get(ctx->json_value_state, "count"), 0);
  segment_scope_check(probe);
  ++probe->nodes;
  if (probe->fail_node) {
    turbo_runtime_json_object_set_string(ctx->json_value_state, "diagnostic", "node failed");
    return -1;
  }
  return turbo_runtime_json_object_set_int64(ctx->json_value_state, "count", count + 1);
}
static void segment_event(const json_value_t *event, void *data) {
  segment_probe_t *probe = data;
  (void)event;
  segment_scope_check(probe);
  ++probe->events;
}
static void segment_checkpoint(const turbo_graph_checkpoint_t *checkpoint, void *data) {
  segment_probe_t *probe = data;
  (void)checkpoint;
  segment_scope_check(probe);
  ++probe->checkpoints;
}
static int segment_snapshots(const json_value_t *state, json_value_t **control,
                             json_value_t **workflow, void *data) {
  segment_probe_t *probe = data;
  segment_scope_check(probe);
  ++probe->snapshot_calls;
  *control = json_create_object();
  *workflow = json_clone(state);
  if (!*control || !*workflow) return -1;
  if (turbo_runtime_json_object_set_string(*control, "owner", "host") != 0) return -1;
  return probe->fail_snapshot ? -1 : 0;
}
static int segment_failed_summary(const turbo_runtime_segment_t *segment,
    const char *checkpoint_id, const turbo_graph_run_result_t *result,
    const json_value_t *state, json_value_t **summary, void *data) {
  segment_probe_t *probe = data;
  (void)segment; (void)checkpoint_id; (void)result; (void)state;
  segment_scope_check(probe);
  ++probe->summary_calls;
  *summary = json_create_object();
  return -1;
}
static uint64_t segment_expired_clock(void *data) { (void)data; return 100; }

spec("Durable graph segments without Agent") {
  static turbo_agent_runtime_t *runtime;
  static turbo_graph_t *graph;
  static turbo_runtime_segment_t segment;
  static turbo_graph_run_options_t options;
  static segment_probe_t probe;
  static json_value_t *input, *summary, *state, *record, *records;
  static turbo_graph_checkpoint_t *checkpoint;
  static turbo_cancel_source_t *cancel_source;
  static turbo_cancel_token_t *cancel_token;
  static tstr checkpoint_id;
  static char *serialized;
  static const char *interrupt_before[] = {"end"};

  before_each() {
    turbo_agent_runtime_store_t store;
    memset(&probe, 0, sizeof(probe));
    input = summary = state = record = records = NULL;
    runtime = NULL; graph = NULL; checkpoint = NULL;
    cancel_source = NULL; cancel_token = NULL;
    checkpoint_id = NULL; serialized = NULL;
    memset(&options, 0, sizeof(options));
    probe.backend = turbo_agent_runtime_store_memory_create();
    check_not_null(probe.backend.put);
    store = (turbo_agent_runtime_store_t){segment_put, segment_get, segment_list, &probe, segment_store_free};
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    graph = turbo_graph_create("standalone-segment");
    input = json_create_object();
    check_not_null(graph);
    check_not_null(input);
    check_equal(turbo_graph_add_json_value_node(graph, "start", segment_node, &probe), TURBO_GRAPH_EXEC_OK);
    check_equal(turbo_graph_add_json_value_node(graph, "end", segment_node, &probe), TURBO_GRAPH_EXEC_OK);
    check_equal(turbo_graph_add_json_value_edge(graph, "start", "end", NULL, NULL), TURBO_GRAPH_EXEC_OK);
    check_equal(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);
    options.checkpoint_cb = segment_checkpoint;
    options.checkpoint_user_data = &probe;
    turbo_runtime_segment_init(&segment);
    segment.runtime = runtime;
    segment.graph = graph;
    segment.thread_id = "thread";
    segment.run_id = "run";
    segment.start_state = input;
    segment.options = &options;
    segment.enter = segment_enter;
    segment.leave = segment_leave;
    segment.user_data = &probe;
    segment.event_sink = segment_event;
    segment.event_sink_user_data = &probe;
  }

  after_each() {
    turbo_cancel_token_release(cancel_token);
    turbo_cancel_source_destroy(cancel_source);
    turbo_graph_checkpoint_destroy(checkpoint);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
    if (probe.backend.user_data_free) segment_store_free(&probe);
    json_free(input); json_free(summary); json_free(state);
    json_free(record); json_free(records);
    tstr_free(checkpoint_id);
    json_serialize_free(serialized);
  }

  it("persists projected checkpoints then resumes and forks without Agent policy") {
    options.interrupt_before_nodes = interrupt_before;
    options.interrupt_before_count = 1;
    segment.snapshot = segment_snapshots;
    segment.parent_agent_run_id = "caller-run";
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), 0);
    check_equal(json_get_string(summary, "status"), "interrupted");
    check_equal(probe.nodes, 1);
    check_equal(probe.snapshot_calls, 1);
    check_equal(probe.checkpoints, 1);
    check_greater(probe.events, 0);
    check_equal(probe.depth, 0);
    check_equal(probe.entered, probe.left);
    check_equal(probe.scope_errors, 0);
    check_null(json_object_get(input, "count"));
    check_null(json_object_get(summary, "available_commands"));
    checkpoint_id = tstr_dup(json_get_string(summary, "checkpoint_id"));
    check_not_null(checkpoint_id);
    check_equal(turbo_agent_runtime_thread_diagram(runtime, "thread", &records), SALTS_OK);
    check_equal(json_array_size(json_object_get(records, "nodes")), 2);
    json_free(records); records = NULL;
    check_equal(turbo_agent_runtime_store_get_json(runtime, "checkpoints", checkpoint_id, &record), 0);
    check_equal(json_get_string(json_object_get(record, "control_snapshot"), "owner"), "host");
    check_equal(json_get_string(record, "parent_agent_run_id"), "caller-run");
    check_true(turbo_runtime_json_value_as_int64(json_object_get(
                   json_object_get(record, "workflow_snapshot"), "count"), 0) == 1);
    check_equal((int)json_array_size(json_object_get(record, "events")), probe.events);
    serialized = json_serialize(json_object_get(record, "checkpoint_json"), NULL);
    check_not_null(serialized);
    check_equal(turbo_graph_checkpoint_deserialize(serialized, strlen(serialized), &checkpoint), TURBO_GRAPH_EXEC_OK);
    json_free(record); record = NULL;
    json_free(summary); summary = NULL;
    json_free(state); state = NULL;
    check_equal(turbo_agent_runtime_store_get_json(runtime, "runs", "run", &record), 0);
    segment.run_created_at = json_get_string(record, "created_at");
    segment.start_state = NULL;
    segment.start_checkpoint = checkpoint;
    segment.parent_checkpoint_id = checkpoint_id;
    segment.next_checkpoint_seq = 2;
    options.interrupt_before_count = 0;
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), 0);
    check_equal(json_get_string(summary, "status"), "completed");
    check_true(turbo_runtime_json_value_as_int64(json_object_get(state, "count"), 0) == 2);
    segment.run_created_at = NULL;
    json_free(record); record = NULL;
    check_equal(turbo_agent_runtime_store_get_json(runtime, "runs", "run", &record), 0);
    check_equal(json_get_string(record, "latest_checkpoint_id"), checkpoint_id);
    json_free(record); record = NULL;
    json_free(summary); summary = NULL;
    json_free(state); state = NULL;
    segment.run_id = "fork";
    segment.parent_run_id = "run";
    segment.forked_from_checkpoint_id = checkpoint_id;
    segment.parent_checkpoint_id = NULL;
    segment.next_checkpoint_seq = 1;
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), 0);
    check_equal(turbo_agent_runtime_store_get_json(runtime, "runs", "fork", &record), 0);
    check_equal(json_get_string(record, "parent_run_id"), "run");
    check_equal(json_get_string(record, "forked_from_checkpoint_id"), checkpoint_id);
    check_true(turbo_runtime_json_value_as_int64(json_object_get(state, "count"), 0) == 2);
    check_equal(probe.entered, 3);
    check_equal(probe.left, 3);
    check_equal(probe.scope_errors, 0);
  }

  it("persists cancellation with complete state and empty optional projections") {
    check_equal(turbo_cancel_source_create(NULL, &cancel_source), SALTS_OK);
    check_equal(turbo_cancel_source_token(cancel_source, &cancel_token), SALTS_OK);
    check_equal(turbo_cancel_source_cancel(cancel_source, TURBO_CANCEL_USER), SALTS_OK);
    segment.cancel_token = cancel_token;
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), 0);
    check_equal(json_get_string(summary, "status"), "cancelled");
    check_equal(probe.nodes, 0);
    check_equal(turbo_agent_runtime_store_get_json(runtime, "checkpoints",
                json_get_string(summary, "checkpoint_id"), &record), 0);
    check_equal(json_type(json_object_get(record, "control_snapshot")), JSON_OBJECT);
    check_equal(json_object_size(json_object_get(record, "control_snapshot")), 0);
    check_not_null(json_object_get(json_object_get(record, "checkpoint_json"), "state"));
    check_equal(probe.depth, 0);
    check_equal(probe.left, 1);
  }

  it("persists an expired deadline as a resumable terminal segment") {
    turbo_cancel_source_config_t config = {sizeof(config), TURBO_RUNTIME_CONTROL_ABI_VERSION,
                                           100, segment_expired_clock, NULL};
    check_equal(turbo_cancel_source_create(&config, &cancel_source), SALTS_OK);
    check_equal(turbo_cancel_source_token(cancel_source, &cancel_token), SALTS_OK);
    segment.cancel_token = cancel_token;
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), 0);
    check_equal(json_get_string(summary, "status"), "timed_out");
    check_equal(turbo_agent_runtime_store_get_json(runtime, "checkpoints",
                json_get_string(summary, "checkpoint_id"), &record), 0);
    check_equal(json_get_string(record, "next_node"), "start");
    check_equal(probe.nodes, 0);
    check_equal(probe.depth, 0);
  }

  it("returns diagnostic state and restores scope on a node error") {
    probe.fail_node = 1;
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), -1);
    check_null(summary);
    check_not_null(state);
    check_equal(json_get_string(state, "diagnostic"), "node failed");
    check_equal(probe.left, 1);
    check_equal(probe.depth, 0);
    check_equal(turbo_agent_runtime_store_list_json(runtime, "runs", NULL, NULL, &records), 0);
    check_equal(json_array_size(records), 0);
  }

  it("rejects incompatible configuration before callbacks or record writes") {
    segment.abi_version += 1;
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), -1);
    segment.abi_version = TURBO_RUNTIME_SEGMENT_ABI_VERSION;
    segment.leave = NULL;
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), -1);
    segment.leave = segment_leave;
    segment.run_created_at = "2026-10-10T00:00:00Z-extra-text-too-long";
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), -1);
    segment.run_created_at = NULL;
#if SIZE_MAX > 9007199254740991ULL
    segment.next_checkpoint_seq = (size_t)UINT64_C(9007199254740992);
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), -1);
#endif
    check_null(summary);
    check_null(state);
    check_equal(probe.entered, 0);
    check_equal(probe.writes, 0);
  }

  it("releases failed projections before any checkpoint or run commit") {
    options.interrupt_before_nodes = interrupt_before;
    options.interrupt_before_count = 1;
    segment.snapshot = segment_snapshots;
    probe.fail_snapshot = 1;
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), -1);
    check_null(summary);
    check_null(state);
    check_equal(probe.snapshot_calls, 1);
    check_equal(probe.left, 1);
    check_equal(probe.depth, 0);
    check_equal(probe.writes, 2); /* Thread upsert and initial head marker. */
  }

  for (int fail_run = 0; fail_run < 2; ++fail_run) {
  it("keeps the published snapshot when a same-run update fails at %s", fail_run ? "run" : "head") {
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), 0);
    json_free(summary); summary = NULL;
    json_free(state); state = NULL;
    check_equal(turbo_runtime_json_object_set_int64(input, "count", 10), 0);
    probe.reject_published_head = !fail_run;
    probe.reject_collection = fail_run ? "runs" : NULL;
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), -1);
    check_null(summary); check_null(state);
    check_equal(turbo_agent_runtime_get_run_state_json_value(runtime, "run", &record), 0);
    check_equal(turbo_runtime_json_value_as_int64(json_object_get(record, "count"), 0), fail_run ? 2 : 12);
    json_free(record); record = NULL;
    check_equal(turbo_agent_runtime_get_thread_head_state_json_value(runtime, "thread", &record), 0);
    check_equal(turbo_runtime_json_value_as_int64(json_object_get(record, "count"), 0), 2);
  }
  }

  it("excludes unpublished checkpoints from thread history and checkpoint selection") {
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), 0);
    json_free(summary); summary = NULL;
    json_free(state); state = NULL;
    options.interrupt_before_nodes = interrupt_before;
    options.interrupt_before_count = 1;
    probe.reject_published_head = 1;
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), -1);
    check_equal(turbo_agent_runtime_store_list_json(runtime, "checkpoints", NULL, NULL, &records), 0);
    check_equal(json_array_size(records), 1);
    check_equal(turbo_agent_runtime_current_checkpoint_id_for_thread(runtime, "thread", &serialized), -1);
    check_null(serialized);
    check_equal(turbo_agent_runtime_load_thread_history_events_json_value(runtime, "thread", &record), 0);
    check_equal(json_array_size(record), 0);
    json_free(record); record = NULL;
    check_equal(turbo_agent_runtime_thread_diagram(runtime, "thread", &record), SALTS_OK);
    check_equal(json_array_size(json_object_get(record, "nodes")), 1);
    check_equal(json_array_size(json_object_get(record, "edges")), 0);
  }

  it("does not select an unpublished first run through legacy fallback") {
    probe.reject_published_head = 1;
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), -1);
    check_equal(turbo_agent_runtime_get_published_thread_head(runtime, "thread", &record), SALTS_EBUSY);
    check_null(record);
    check_equal(turbo_agent_runtime_get_thread_head_state_json_value(runtime, "thread", &record), -1);
    check_null(record);
  }

  it("does not confuse a busy backend with a confirmed empty head marker") {
    probe.head_read_error = SALTS_EBUSY;
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), -1);
    check_equal(probe.nodes, 0);
    check_equal(probe.entered, 0);
    check_equal(turbo_agent_runtime_store_list_json(runtime, "runs", NULL, NULL, &records), 0);
    check_equal(json_array_size(records), 0);
  }

  it("does not enter execution when head adoption fails") {
    probe.reject_collection = "thread_heads";
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), -1);
    check_equal(probe.nodes, 0);
    check_equal(probe.entered, 0);
  }

  it("rejects a corrupt head instead of selecting an older run") {
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), 0);
    check_equal(probe.backend.put(probe.backend.user_data, "thread_heads", "thread", "{}"), 0);
    check_equal(turbo_agent_runtime_get_published_thread_head(runtime, "thread", &record), SALTS_EPROTO);
    check_equal(turbo_agent_runtime_get_thread_head_state_json_value(runtime, "thread", &record), -1);
    check_null(record);
  }

  it("does not enter execution when the initial thread write fails") {
    probe.reject_collection = "threads";
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), -1);
    check_equal(probe.entered, 0);
    check_equal(probe.nodes, 0);
    check_null(summary);
    check_null(state);
  }

  it("stops run finalization after a failed checkpoint commit") {
    probe.reject_collection = "checkpoints";
    options.interrupt_before_nodes = interrupt_before;
    options.interrupt_before_count = 1;
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), -1);
    check_null(summary);
    check_null(state);
    check_equal(probe.left, 1);
    check_equal(probe.depth, 0);
    check_equal(turbo_agent_runtime_store_list_json(runtime, "runs", NULL, NULL, &records), 0);
    check_equal(json_array_size(records), 0);
  }

  it("retains the committed checkpoint if the later run write fails") {
    probe.reject_collection = "runs";
    options.interrupt_before_nodes = interrupt_before;
    options.interrupt_before_count = 1;
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), -1);
    check_null(summary);
    check_null(state);
    check_equal(probe.depth, 0);
    check_equal(turbo_agent_runtime_store_list_json(runtime, "checkpoints", NULL, NULL, &records), 0);
    check_equal(json_array_size(records), 1);
    check_equal(turbo_agent_runtime_store_get_json(runtime, "runs", "run", &record), -1);
    check_null(record);
  }

  it("reports summary failure after persistence and closes the execution scope") {
    segment.summary = segment_failed_summary;
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), -1);
    check_null(summary);
    check_null(state);
    check_equal(probe.summary_calls, 1);
    check_equal(probe.depth, 0);
    check_equal(probe.left, 1);
    check_equal(probe.scope_errors, 0);
    check_equal(turbo_agent_runtime_store_get_json(runtime, "runs", "run", &record), 0);
    check_equal(json_get_string(record, "status"), "completed");
  }
}
