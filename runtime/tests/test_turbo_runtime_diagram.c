#include "tinytest.h"
#include "turbo_runtime_diagram.h"
#include <fmt.h>
#include <string.h>

static int put(turbo_agent_runtime_t *runtime, const char *collection, const char *id, const char *text) {
  json_value_t *value = json_parse(text, strlen(text));
  int rc = value ? turbo_agent_runtime_store_put_json(runtime, collection, id, value) : -1;
  json_free(value);
  return rc;
}

spec("Published execution diagrams") {
  static turbo_agent_runtime_t *runtime;
  static turbo_agent_runtime_store_t store;
  static json_value_t *view, *record;
  static tstr text, id, parent;
  before_each() {
    store = turbo_agent_runtime_store_memory_create();
    runtime = turbo_agent_runtime_create(&store); check_not_null(runtime);
    store = (turbo_agent_runtime_store_t){0};
    check_equal(put(runtime, "thread_heads", "t",
        "{\"schema_version\":1,\"id\":\"t\",\"thread_id\":\"t\",\"run_snapshot\":{\"id\":\"r\",\"thread_id\":\"t\","
        "\"status\":\"interrupted\",\"state_snapshot\":{\"secret\":\"not-exported\"},\"latest_checkpoint_id\":\"c2\"}}"), 0);
    check_equal(put(runtime, "checkpoints", "c2",
        "{\"id\":\"c2\",\"thread_id\":\"t\",\"run_id\":\"r\",\"seq\":2,\"parent_checkpoint_id\":\"c1\"}"), 0);
    check_equal(put(runtime, "checkpoints", "c1",
        "{\"id\":\"c1\",\"thread_id\":\"t\",\"run_id\":\"r\",\"seq\":1,\"parent_checkpoint_id\":null}"), 0);
  }
  after_each() {
    turbo_agent_runtime_destroy(runtime); runtime = NULL;
    if (store.user_data_free) store.user_data_free(store.user_data);
    store = (turbo_agent_runtime_store_t){0};
    json_free(view); json_free(record); view = record = NULL;
    tstr_free(text); tstr_free(id); tstr_free(parent);
    text = id = parent = NULL;
  }
  it("uses exact published lineage with provenance and excludes unpublished records and state") {
    check_equal(put(runtime, "runs", "r", "{\"latest_checkpoint_id\":\"unpublished\"}"), 0);
    check_equal(turbo_agent_runtime_thread_diagram(runtime, "t", &view), SALTS_OK);
    check_equal(json_array_size(json_object_get(view, "nodes")), 3);
    check_equal(json_array_size(json_object_get(view, "edges")), 2);
    check_equal(json_get_string(json_array_get(json_object_get(view, "nodes"), 0), "collection"), "thread_heads");
    check_equal(json_get_string(json_array_get(json_object_get(view, "nodes"), 0), "record_id"), "t");
    check_equal(turbo_diagram_mermaid(view, &text), SALTS_OK);
    check_contains(text, "n1 -->|\"snapshot checkpoint\"| n0");
    check_contains(text, "n2 -->|\"parent checkpoint\"| n1");
    check_null(strstr(text, "not-exported")); check_null(strstr(text, "unpublished"));
  }
  it("rejects cycles and cross-thread links without returning partial views") {
    check_equal(put(runtime, "checkpoints", "c1",
        "{\"id\":\"c1\",\"thread_id\":\"t\",\"run_id\":\"r\",\"seq\":1,\"parent_checkpoint_id\":\"c2\"}"), 0);
    check_equal(turbo_agent_runtime_thread_diagram(runtime, "t", &view), SALTS_EPROTO); check_null(view);
    check_equal(put(runtime, "checkpoints", "c1",
        "{\"id\":\"c1\",\"thread_id\":\"other\",\"run_id\":\"r\",\"seq\":1,\"parent_checkpoint_id\":null}"), 0);
    check_equal(turbo_agent_runtime_thread_diagram(runtime, "t", &view), SALTS_EPROTO); check_null(view);
  }
  it("focuses checkpoint ancestors while retaining exact store references") {
    check_equal(turbo_agent_runtime_thread_diagram(runtime, "t", &view), SALTS_OK);
    check_equal(turbo_diagram_neighborhood(view, "checkpoint:c2", TURBO_DIAGRAM_INCOMING, 1, &record), SALTS_OK);
    check_equal(json_array_size(json_object_get(record, "nodes")), 2u);
    check_equal(json_array_size(json_object_get(record, "edges")), 1u);
    check_equal(json_get_string(record, "thread_id"), "t");
    check_equal(json_get_string(json_array_get(json_object_get(record, "nodes"), 0), "record_id"), "c2");
    check_equal(json_get_string(json_array_get(json_object_get(record, "nodes"), 1), "record_id"), "c1");
    check_equal(json_get_int(json_array_get(json_object_get(record, "nodes"), 1), "selection_distance", -1), 1);
    check_equal(turbo_diagram_mermaid(record, &text), SALTS_OK);
    check_contains(text, "Published execution records (focused)");
    check_contains(text, "parent checkpoint"); check_null(strstr(text, "snapshot checkpoint"));
    check_equal(json_array_size(json_object_get(view, "nodes")), 3u);
  }
  it("rejects missing parents and does not invent a legacy or empty head") {
    check_equal(put(runtime, "checkpoints", "c1",
        "{\"id\":\"c1\",\"thread_id\":\"t\",\"run_id\":\"r\",\"seq\":1,\"parent_checkpoint_id\":\"missing\"}"), 0);
    check_equal(turbo_agent_runtime_thread_diagram(runtime, "t", &view), SALTS_ENOENT); check_null(view);
    check_equal(turbo_agent_runtime_thread_diagram(runtime, "legacy", &view), SALTS_ENOENT); check_null(view);
    check_equal(put(runtime, "thread_heads", "t", "{\"schema_version\":1,\"id\":\"t\",\"thread_id\":\"t\",\"run_snapshot\":null}"), 0);
    check_equal(turbo_agent_runtime_thread_diagram(runtime, "t", &view), SALTS_EBUSY); check_null(view);
  }
  it("stops a long parent chain at the node budget") {
    for (size_t i = 1; i <= TURBO_DIAGRAM_MAX_NODES; ++i) {
      id = tstr_format("c{}", i); parent = tstr_format("c{}", i + 1);
      record = json_create_object(); check_not_null(id); check_not_null(parent); check_not_null(record);
      check_equal(turbo_runtime_json_object_set_string(record, "id", id), 0);
      check_equal(turbo_runtime_json_object_set_string(record, "thread_id", "t"), 0);
      check_equal(turbo_runtime_json_object_set_string(record, "run_id", "r"), 0);
      check_equal(turbo_runtime_json_object_set_int64(record, "seq", i), 0);
      check_equal(turbo_runtime_json_object_set_string(record, "parent_checkpoint_id", parent), 0);
      check_equal(turbo_agent_runtime_store_put_json(runtime, "checkpoints", id, record), 0);
      json_free(record); record = NULL; tstr_free(id); tstr_free(parent); id = parent = NULL;
    }
    check_equal(turbo_agent_runtime_thread_diagram(runtime, "t", &view), SALTS_ENOSPC); check_null(view);
  }
}
