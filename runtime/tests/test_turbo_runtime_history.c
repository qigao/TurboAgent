#include "tinytest.h"
#include "turbo_runtime_history.h"
#include <stdlib.h>
#include <string.h>

static void history_put(turbo_agent_runtime_t *runtime, const char *collection,
                         const char *id, const char *text) {
  json_value_t *record = json_parse(text, strlen(text));
  int rc;
  check_not_null(record);
  rc = turbo_agent_runtime_store_put_json(runtime, collection, id, record);
  json_free(record);
  check_equal(rc, 0);
}

static const char history_checkpoint_one[] =
    "{\"id\":\"cp1\",\"thread_id\":\"thread\",\"run_id\":\"pending\",\"seq\":1,"
    "\"parent_checkpoint_id\":null,\"checkpoint_json\":{\"checkpoint_version\":3,"
    "\"next_node\":\"node\",\"steps\":1,\"topology_id\":\"\",\"state\":{\"value\":\"old\"}},"
    "\"events\":[{\"name\":\"one\"}]}";
static const char history_checkpoint_two[] =
    "{\"id\":\"cp2\",\"thread_id\":\"thread\",\"run_id\":\"pending\",\"seq\":2,"
    "\"parent_checkpoint_id\":\"cp1\",\"checkpoint_json\":{\"checkpoint_version\":3,"
    "\"next_node\":\"node\",\"steps\":2,\"topology_id\":\"\",\"state\":{\"value\":\"second\"}},"
    "\"events\":[{\"name\":\"two\"}]}";

spec("Runtime checkpoint navigation without Agent") {
  static turbo_agent_runtime_t *runtime;
  static turbo_agent_runtime_store_t store;
  static json_value_t *output, *record, *patch, *events;
  static turbo_graph_checkpoint_t *checkpoint;
  static char *root_id, *serialized;

  before_each() {
    output = record = patch = events = NULL;
    checkpoint = NULL;
    root_id = serialized = NULL;
    store = turbo_agent_runtime_store_memory_create();
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    memset(&store, 0, sizeof(store)); /* Backend ownership transferred. */
    history_put(runtime, "checkpoints", "cp1", history_checkpoint_one);
    history_put(runtime, "checkpoints", "cp2", history_checkpoint_two);
    history_put(runtime, "runs", "pending",
        "{\"id\":\"pending\",\"thread_id\":\"thread\",\"status\":\"interrupted\","
        "\"updated_at\":\"2026-10-10T00:00:00Z\",\"latest_checkpoint_id\":\"cp1\"}");
    history_put(runtime, "runs", "new",
        "{\"id\":\"new\",\"thread_id\":\"thread\",\"status\":\"completed\","
        "\"updated_at\":\"2026-10-10T00:00:01Z\",\"latest_checkpoint_id\":\"cp1\","
        "\"state_snapshot\":{\"value\":\"new\"}}");
  }

  after_each() {
    turbo_graph_checkpoint_destroy(checkpoint);
    turbo_agent_runtime_destroy(runtime);
    if (store.user_data_free) store.user_data_free(store.user_data);
    json_free(output); json_free(record); json_free(patch); json_free(events);
    free(root_id);
    json_serialize_free(serialized);
  }

  it("keeps latest and pending-first head selection distinct with legacy state fallback") {
    check_equal(turbo_agent_runtime_get_latest_run(runtime, "thread", &output), 0);
    check_equal(json_get_string(output, "id"), "new");
    json_free(output); output = NULL;
    check_equal(turbo_agent_runtime_get_pending_run(runtime, "thread", &output), 0);
    check_equal(json_get_string(output, "id"), "pending");
    json_free(output); output = NULL;
    check_equal(turbo_agent_runtime_get_thread_state_json_value(runtime, "thread", &output), 0);
    check_equal(json_get_string(output, "value"), "new");
    json_free(output); output = NULL;
    check_equal(turbo_agent_runtime_get_thread_head_state_json_value(runtime, "thread", &output), 0);
    check_equal(json_get_string(output, "value"), "old");
    check_equal(turbo_runtime_json_object_set_string(output, "value", "caller copy"), 0);
    json_free(output); output = NULL;
    check_equal(turbo_agent_runtime_get_checkpoint_state_json_value(runtime, "cp1", &output), 0);
    check_equal(json_get_string(output, "value"), "old");
    check_equal(turbo_agent_runtime_current_checkpoint_id_for_thread(runtime, "thread", &root_id), 0);
    check_equal(root_id, "cp1");
  }

  it("uses backend encounter order for tied run timestamps and clears missing outputs") {
    history_put(runtime, "runs", "tie-z",
        "{\"id\":\"tie-z\",\"thread_id\":\"ties\",\"status\":\"completed\",\"updated_at\":\"same\"}");
    history_put(runtime, "runs", "tie-a",
        "{\"id\":\"tie-a\",\"thread_id\":\"ties\",\"status\":\"completed\",\"updated_at\":\"same\"}");
    check_equal(turbo_agent_runtime_list_runs(runtime, "ties", &record), 0);
    check_equal(turbo_agent_runtime_get_latest_run(runtime, "ties", &output), 0);
    check_equal(json_get_string(output, "id"), json_get_string(json_array_get(record, 0), "id"));
    json_free(output); output = NULL;
    check_equal(turbo_agent_runtime_current_run_id_for_thread(runtime, "ties", &root_id), 0);
    check_equal(root_id, json_get_string(json_array_get(record, 0), "id"));
    output = (json_value_t *)(uintptr_t)1;
    check_equal(turbo_agent_runtime_get_pending_run(runtime, "missing", &output), -1);
    check_null(output);
    check_equal(turbo_agent_runtime_get_latest_checkpoint(runtime, "tie-a", &output), -1);
    check_null(output);
  }

  it("loads checkpoint state overrides independently from borrowed record trees") {
    const char *thread_id, *run_id, *parent_id;
    const json_value_t *checkpoint_json;
    size_t sequence;
    check_equal(turbo_agent_runtime_get_checkpoint(runtime, "cp1", &record), 0);
    check_equal(turbo_runtime_checkpoint_record_read(record, &thread_id, &run_id, &parent_id,
                                                    &sequence, &checkpoint_json), 0);
    check_equal(thread_id, "thread");
    check_equal(run_id, "pending");
    check_null(parent_id);
    check_equal(sequence, 1);
    patch = json_create_object();
    check_not_null(patch);
    check_equal(turbo_runtime_json_object_set_string(patch, "value", "override"), 0);
    check_equal(turbo_runtime_checkpoint_from_json(checkpoint_json, patch, &checkpoint), 0);
    check_equal(json_get_string(json_object_get(checkpoint_json, "state"), "value"), "old");
    check_equal(turbo_runtime_json_object_set_string(patch, "value", "changed"), 0);
    json_free(record); record = NULL;
    json_free(patch); patch = NULL;
    check_equal(turbo_graph_checkpoint_steps(checkpoint), 1);
    check_equal(turbo_graph_checkpoint_next_node(checkpoint), "node");
    serialized = turbo_graph_checkpoint_serialize(checkpoint, NULL);
    check_not_null(serialized);
    output = json_parse(serialized, strlen(serialized));
    check_not_null(output);
    check_equal(json_get_string(json_object_get(output, "state"), "value"), "override");
  }

  it("orders run and parent-chain history without consuming persisted event trees") {
    check_equal(turbo_agent_runtime_load_history_events_json_value(runtime, "pending", NULL, &events), 0);
    check_equal(json_array_size(events), 2);
    check_equal(json_get_string(json_array_get(events, 0), "name"), "one");
    check_equal(json_get_string(json_array_get(events, 1), "name"), "two");
    json_free(events); events = NULL;
    check_equal(turbo_agent_runtime_load_history_events_json_value(runtime, NULL, "cp2", &events), 0);
    check_equal(json_array_size(events), 2);
    check_equal(json_get_string(json_array_get(events, 0), "name"), "one");
    check_equal(turbo_runtime_root_checkpoint_id(runtime, "cp2", &root_id), 0);
    check_equal(root_id, "cp1");
    check_equal(turbo_agent_runtime_get_checkpoint(runtime, "cp1", &record), 0);
    check_equal(json_array_size(json_object_get(record, "events")), 1);
  }

  it("rejects cycles and missing parent records without returning a partial history") {
    check_equal(turbo_agent_runtime_get_checkpoint(runtime, "cp1", &record), 0);
    check_equal(turbo_runtime_json_object_set_string(record, "parent_checkpoint_id", "cp2"), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "checkpoints", "cp1", record), 0);
    check_equal(turbo_runtime_root_checkpoint_id(runtime, "cp2", &root_id), -1);
    check_null(root_id);
    check_equal(turbo_agent_runtime_load_history_events_json_value(runtime, NULL, "cp2", &events), -1);
    check_null(events);
    check_equal(turbo_runtime_json_object_set_string(record, "parent_checkpoint_id", "missing"), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "checkpoints", "cp1", record), 0);
    check_equal(turbo_agent_runtime_load_history_events_json_value(runtime, NULL, "cp2", &events), -1);
    check_null(events);
    check_equal(turbo_runtime_root_checkpoint_id(runtime, "cp2", &root_id), -1);
    check_null(root_id);
  }

  it("rejects fractional, negative, and inexact checkpoint sequence values") {
    const char *thread_id, *run_id, *parent_id;
    const json_value_t *checkpoint_json;
    size_t sequence;
    static const double invalid[] = {-1, 0, 1.5, 9007199254740992.0};
    size_t i;
    check_equal(turbo_agent_runtime_get_checkpoint(runtime, "cp1", &record), 0);
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      check_equal(turbo_runtime_json_object_set(record, "seq", json_create_number(invalid[i])), 0);
      check_equal(turbo_runtime_checkpoint_record_read(record, &thread_id, &run_id, &parent_id,
                                                      &sequence, &checkpoint_json), -1);
      check_null(thread_id); check_null(run_id); check_null(parent_id); check_null(checkpoint_json);
      check_equal(sequence, 0);
      check_equal(turbo_agent_runtime_store_put_json(runtime, "checkpoints", "cp1", record), 0);
      check_equal(turbo_agent_runtime_load_history_events_json_value(runtime, "pending", NULL, &events), -1);
      check_null(events);
    }
  }

  it("rejects malformed graph checkpoint counters before integer conversion") {
    static const double invalid_steps[] = {-1, 1.5, 9007199254740992.0};
    json_value_t *checkpoint_json;
    size_t i;
    check_equal(turbo_agent_runtime_get_checkpoint(runtime, "cp1", &record), 0);
    checkpoint_json = json_object_get(record, "checkpoint_json");
    for (i = 0; i < sizeof(invalid_steps) / sizeof(invalid_steps[0]); ++i) {
      check_equal(turbo_runtime_json_object_set(checkpoint_json, "steps", json_create_number(invalid_steps[i])), 0);
      check_equal(turbo_runtime_checkpoint_from_json(checkpoint_json, NULL, &checkpoint), -1);
      check_null(checkpoint);
    }
    check_equal(turbo_runtime_json_object_set_int64(checkpoint_json, "steps", 1), 0);
    check_equal(turbo_runtime_json_object_set(checkpoint_json, "checkpoint_version", json_create_number(3.5)), 0);
    check_equal(turbo_runtime_checkpoint_from_json(checkpoint_json, NULL, &checkpoint), -1);
    check_null(checkpoint);
  }
}
