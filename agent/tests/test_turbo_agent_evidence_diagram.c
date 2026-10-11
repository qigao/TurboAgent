#include "tinytest.h"
#include "turbo_agent_evidence_diagram.h"
#include "turbo_tool.h"
#include <fmt.h>
#include <string.h>

typedef struct evidence_store_s {
  turbo_agent_runtime_store_t inner;
  int writes, lists, journal_error;
} evidence_store_t;

static int evidence_put(void *user, const char *collection, const char *id, const char *text) {
  evidence_store_t *store = user;
  ++store->writes;
  return store->inner.put(store->inner.user_data, collection, id, text);
}
static int evidence_get(void *user, const char *collection, const char *id, char **out) {
  evidence_store_t *store = user;
  if (store->journal_error && !strcmp(collection, "agent_tool_journal")) {
    *out = NULL; return store->journal_error;
  }
  return store->inner.get(store->inner.user_data, collection, id, out);
}
static int evidence_list(void *user, const char *collection, const char *key, const char *value, char **out) {
  evidence_store_t *store = user;
  ++store->lists;
  return store->inner.list(store->inner.user_data, collection, key, value, out);
}
static int put_text(turbo_agent_runtime_t *runtime, const char *collection, const char *id, const char *text) {
  json_value_t *record = json_parse(text, strlen(text));
  int rc = record ? turbo_agent_runtime_store_put_json(runtime, collection, id, record) : SALTS_ENOMEM;
  json_free(record); return rc;
}
static json_value_t *journal_record(const char *id) {
  static const char text[] = "{\"schema_version\":1,\"thread_id\":\"t\",\"run_id\":\"r\","
      "\"call_id\":\"call\",\"turn_key\":\"turn\",\"tool_name\":\"fs.read\","
      "\"arguments_hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
      "\"phase\":\"committed\",\"status\":0,\"execution_mode\":0,\"idempotency\":0,"
      "\"updated_at\":\"2026-10-11T00:00:00Z\",\"output\":\"private-output\","
      "\"detail\":{\"private\":\"hidden-detail\"}}";
  json_value_t *record = json_parse(text, sizeof(text) - 1);
  if (record && turbo_runtime_json_object_set_string(record, "journal_id", id) != 0) {
    json_free(record); return NULL;
  }
  return record;
}

static int put_origin_state(turbo_agent_runtime_t *runtime, const char *state) {
  tstr head = tstr_format("{{\"schema_version\":1,\"id\":\"t\",\"thread_id\":\"t\",\"run_snapshot\":{{"
      "\"id\":\"r\",\"thread_id\":\"t\",\"status\":\"completed\",\"latest_checkpoint_id\":\"c1\",\"state_snapshot\":{}}}}}", state);
  int rc = head ? put_text(runtime, "thread_heads", "t", head) : SALTS_ENOMEM;
  tstr_free(head); return rc;
}

static json_value_t *workflow_origin_state(void) {
  const char text[] = "{\"plan_versions\":[{\"steps\":[{\"text\":\"old-private-step\"}]},"
      "{\"steps\":[{\"text\":\"current-private-step\"}]}],\"executor_state_versions\":["
      "{\"events\":[{\"kind\":\"model\",\"tool_calls\":[{\"call_id\":\"call\",\"name\":\"fs.read\"}]}]},"
      "{\"events\":[{\"kind\":\"model\",\"tool_calls\":[{\"call_id\":\"call\",\"name\":\"fs.read\"}]}]}]}";
  return json_parse(text, sizeof(text) - 1);
}

static const json_value_t *evidence_node(const json_value_t *view, const char *id) {
  const json_value_t *nodes = json_object_get(view, "nodes");
  for (size_t i = 0; i < json_array_size(nodes); ++i) {
    const json_value_t *node = json_array_get(nodes, i);
    if (!strcmp(json_get_string(node, "id"), id)) return node;
  }
  return NULL;
}

spec("Selected tool journal evidence diagrams") {
  static evidence_store_t store;
  static turbo_agent_runtime_t *runtime;
  static json_value_t *view, *record, *focused, *fresh, *field;
  static tstr text, id, parent;
  static char *serialized;
  before_each() {
    turbo_agent_runtime_store_t callbacks;
    store.inner = turbo_agent_runtime_store_memory_create(); check_not_null(store.inner.put);
    callbacks = (turbo_agent_runtime_store_t){evidence_put, evidence_get, evidence_list, &store, NULL};
    runtime = turbo_agent_runtime_create(&callbacks); check_not_null(runtime);
    check_equal(put_text(runtime, "thread_heads", "t",
        "{\"schema_version\":1,\"id\":\"t\",\"thread_id\":\"t\",\"run_snapshot\":{"
        "\"id\":\"r\",\"thread_id\":\"t\",\"status\":\"completed\",\"state_snapshot\":{},\"latest_checkpoint_id\":\"c1\"}}"), 0);
    check_equal(put_text(runtime, "checkpoints", "c1",
        "{\"id\":\"c1\",\"thread_id\":\"t\",\"run_id\":\"r\",\"seq\":1,\"parent_checkpoint_id\":null}"), 0);
    record = journal_record("j1"); check_not_null(record);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_tool_journal", "j1", record), 0);
  }
  after_each() {
    turbo_agent_runtime_destroy(runtime); runtime = NULL;
    if (store.inner.user_data_free) store.inner.user_data_free(store.inner.user_data);
    store = (evidence_store_t){0};
    json_free(view); json_free(record); json_free(focused); json_free(fresh); json_free(field);
    view = record = focused = fresh = field = NULL;
    tstr_free(text); tstr_free(id); tstr_free(parent); text = id = parent = NULL;
    json_serialize_free(serialized); serialized = NULL;
  }
  it("links exact run identities without writes, scans, payloads or checkpoint inclusion claims") {
    const char *ids[] = {"j1"};
    const json_value_t *node, *edge;
    int writes = store.writes;
    check_equal(turbo_agent_thread_tool_diagram(runtime, "t", ids, 1, &view), SALTS_OK);
    check_equal(store.writes, writes); check_equal(store.lists, 0);
    check_equal(json_get_string(view, "record_scope"), "published_and_current");
    check_equal(json_get_string(view, "journal_scope"), "selected_records");
    check_equal(json_array_size(json_object_get(view, "nodes")), 3u);
    node = json_array_get(json_object_get(view, "nodes"), 2);
    check_equal(json_get_string(node, "id"), "journal:j1");
    check_equal(json_get_string(node, "collection"), "agent_tool_journal");
    check_equal(json_get_string(node, "freshness"), "current_record");
    check_equal(json_get_string(node, "call_id"), "call");
    check_equal(json_get_string(node, "arguments_hash"), json_get_string(record, "arguments_hash"));
    edge = json_array_get(json_object_get(view, "edges"), 1);
    check_equal(json_get_string(edge, "from"), "run:r");
    check_equal(json_get_string(edge, "to"), "journal:j1");
    check_equal(json_get_string(edge, "relation"), "same_run");
    serialized = json_serialize(view, NULL); check_not_null(serialized);
    check_null(strstr(serialized, "private-output")); check_null(strstr(serialized, "hidden-detail"));
    check_equal(turbo_diagram_neighborhood(view, "run:r", TURBO_DIAGRAM_OUTGOING, 1, &focused), SALTS_OK);
    check_equal(json_array_size(json_object_get(focused, "nodes")), 2u);
    check_equal(turbo_diagram_mermaid(focused, &text), SALTS_OK);
    check_contains(text, "current journal observations (focused)");
    check_contains(text, "same run (current journal)");
  }
  it("deduplicates a published request and retains independent evidence without exporting payloads") {
    const char *ids[] = {"j1", "j2"};
    check_equal(put_origin_state(runtime, "{\"events\":[{\"kind\":\"model\",\"text\":\"private-prompt\",\"tool_calls\":["
        "{\"call_id\":\"call\",\"name\":\"fs.read\",\"arguments\":\"private-arguments\"},"
        "{\"call_id\":\"other\",\"name\":\"fs.read\",\"arguments\":\"different\"}]}]}"), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_string(record, "turn_key", "planner:root:0"), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_tool_journal", "j1", record), 0);
    check_equal(turbo_runtime_json_object_set_string(record, "journal_id", "j2"), 0);
    check_equal(turbo_runtime_json_object_set_string(record, "call_id", "other"), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_tool_journal", "j2", record), 0);
    int writes = store.writes;
    check_equal(turbo_agent_thread_tool_diagram_ex(runtime, "t", ids, 2, TURBO_AGENT_DIAGRAM_CALL_ORIGINS, &view), SALTS_OK);
    check_equal(store.writes, writes); check_equal(store.lists, 0);
    const json_value_t *nodes = json_object_get(view, "nodes"), *edges = json_object_get(view, "edges");
    check_equal(json_array_size(nodes), 5u); check_equal(json_array_size(edges), 6u);
    check_equal(json_get_string(json_array_get(nodes, 2), "origin_status"), "matched");
    check_equal(json_get_string(json_array_get(nodes, 4), "origin_status"), "matched");
    check_equal(json_get_string(json_array_get(nodes, 3), "id"), "event:planner:root:0");
    check_equal(json_get_string(json_array_get(nodes, 3), "source_pointer"), "/run_snapshot/state_snapshot/events/0");
    check_equal(json_get_string(json_array_get(edges, 5), "source_pointer"), "/run_snapshot/state_snapshot/events/0/tool_calls/1");
    turbo_agent_runtime_destroy(runtime); runtime = NULL;
    serialized = json_serialize(view, NULL); check_not_null(serialized);
    check_null(strstr(serialized, "private-prompt")); check_null(strstr(serialized, "private-arguments"));
    check_null(strstr(serialized, "private-output"));
    check_equal(turbo_diagram_neighborhood(view, "journal:j2", TURBO_DIAGRAM_INCOMING, 1, &focused), SALTS_OK);
    check_equal(turbo_diagram_mermaid(focused, &text), SALTS_OK);
    check_contains(text, "published request"); check_contains(text, "same call identity");
  }
  const struct { const char *turn, *state, *status; int rc; } origins[] = {
    {"0", "{}", "unresolved_identity", SALTS_OK},
    {"turn", "{}", "unresolved_identity", SALTS_OK},
    {"executor:0:0", "{}", "not_in_snapshot", SALTS_OK},
    {"executor:1:0", "{\"executor_state_versions\":[{}]}", "not_in_snapshot", SALTS_OK},
    {"planner:root:1", "{\"events\":[]}", "not_in_snapshot", SALTS_OK},
    {"planner:root:0", "{\"events\":[{\"kind\":\"model\",\"tool_calls\":[]}]}", "not_in_snapshot", SALTS_OK},
    {"executor:00:0", "{}", NULL, SALTS_EPROTO},
    {"planner:root:01", "{}", NULL, SALTS_EPROTO},
    {"executor:18446744073709551616:0", "{}", NULL, SALTS_EPROTO},
    {"executor:9223372036854775808:0", "{}", NULL, SALTS_EPROTO},
    {"planner:-1:0", "{}", NULL, SALTS_EPROTO},
    {"executor:0:0junk", "{}", NULL, SALTS_EPROTO},
    {"planner:root:", "{}", NULL, SALTS_EPROTO},
    {"executor:0:0", "{\"executor_state_versions\":{}}", NULL, SALTS_EPROTO},
    {"executor:0:0", "{\"executor_state_versions\":[null]}", NULL, SALTS_EPROTO},
    {"planner:root:0", "{\"events\":null}", NULL, SALTS_EPROTO},
    {"planner:root:0", "{\"events\":[{\"kind\":\"tool_results\"}]}", NULL, SALTS_EPROTO},
    {"planner:root:0", "{\"events\":[{\"kind\":\"model\\u0000tail\",\"tool_calls\":[]}]}", NULL, SALTS_EPROTO},
    {"planner:root:0", "{\"events\":[{\"kind\":\"model\",\"tool_calls\":null}]}", NULL, SALTS_EPROTO},
    {"planner:root:0", "{\"events\":[{\"kind\":\"model\",\"tool_calls\":[{\"call_id\":\"call\",\"name\":\"wrong\"}]}]}", NULL, SALTS_EPROTO},
    {"planner:root:0", "{\"events\":[{\"kind\":\"model\",\"tool_calls\":[{\"call_id\":\"call\\u0000tail\",\"name\":\"fs.read\"}]}]}", NULL, SALTS_EPROTO},
    {"planner:root:0", "{\"events\":[{\"kind\":\"model\",\"tool_calls\":[{\"call_id\":\"call\",\"name\":\"fs.read\"},{\"call_id\":\"call\",\"name\":\"fs.read\"}]}]}", NULL, SALTS_EPROTO}
  };
  for (size_t i = 0; i < sizeof(origins) / sizeof(origins[0]); ++i) {
    it("handles unresolved, absent or contradictory origin case %zu without partial output", i) {
      const char *ids[] = {"j1"};
      check_equal(put_origin_state(runtime, origins[i].state), SALTS_OK);
      check_equal(turbo_runtime_json_object_set_string(record, "turn_key", origins[i].turn), 0);
      check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_tool_journal", "j1", record), 0);
      check_equal(turbo_agent_thread_tool_diagram_ex(runtime, "t", ids, 1, TURBO_AGENT_DIAGRAM_CALL_ORIGINS, &view), origins[i].rc);
      if (origins[i].rc) check_null(view);
      else {
        check_equal(json_array_size(json_object_get(view, "nodes")), 3u);
        check_equal(json_get_string(json_array_get(json_object_get(view, "nodes"), 2), "origin_status"), origins[i].status);
      }
    }
  }
  it("links explicit workflow bindings and deduplicates steps across executor states") {
    const char *ids[] = {"j1", "j2"};
    const char origin[] = "{\"schema_version\":1,\"plan_version\":1,\"step_index\":0}";
    fresh = workflow_origin_state(); check_not_null(fresh);
    field = json_parse(origin, sizeof(origin) - 1); check_not_null(field);
    for (size_t i = 0; i < 2; ++i) {
      check_equal(turbo_runtime_json_object_set_clone(json_array_get(json_object_get(fresh, "executor_state_versions"), i), "plan_step_origin", field), 0);
      check_equal(turbo_runtime_json_object_set_string(record, "journal_id", ids[i]), 0);
      check_equal(turbo_runtime_json_object_set_string(record, "turn_key", i ? "executor:1:0" : "executor:0:0"), 0);
      check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_tool_journal", ids[i], record), 0);
    }
    serialized = json_serialize(fresh, NULL); check_not_null(serialized);
    check_equal(put_origin_state(runtime, serialized), SALTS_OK);
    json_serialize_free(serialized); serialized = NULL;
    int writes = store.writes;
    check_equal(turbo_agent_thread_tool_diagram_ex(runtime, "t", ids, 2,
        TURBO_AGENT_DIAGRAM_CALL_ORIGINS | TURBO_AGENT_DIAGRAM_WORKFLOW_STEPS, &view), SALTS_OK);
    check_equal(store.writes, writes); check_equal(store.lists, 0);
    check_equal(json_array_size(json_object_get(view, "nodes")), 7u);
    check_equal(json_array_size(json_object_get(view, "edges")), 10u);
    const json_value_t *step = evidence_node(view, "workflow-step:1:0"); check_not_null(step);
    check_equal(json_get_string(step, "source_pointer"), "/run_snapshot/state_snapshot/plan_versions/1/steps/0");
    check_null(evidence_node(view, "workflow-step:0:0"));
    check_equal(json_get_string(evidence_node(view, "event:executor:0:0"), "step_origin_status"), "matched");
    check_equal(json_get_string(evidence_node(view, "event:executor:1:0"), "step_origin_status"), "matched");
    turbo_agent_runtime_destroy(runtime); runtime = NULL;
    check_equal(turbo_diagram_shortest_path(view, "workflow-step:1:0", "journal:j2", TURBO_DIAGRAM_OUTGOING, 2, &focused), SALTS_OK);
    check_equal(json_array_size(json_object_get(focused, "nodes")), 3u);
    check_equal(json_array_size(json_object_get(focused, "edges")), 2u);
    serialized = json_serialize(focused, NULL); check_not_null(serialized);
    check_contains(serialized, "/run_snapshot/state_snapshot/executor_state_versions/1/plan_step_origin");
    check_null(strstr(serialized, "private-step")); check_null(strstr(serialized, "private-output"));
    check_equal(turbo_diagram_mermaid(focused, &text), SALTS_OK);
    check_contains(text, "prepared for workflow step");
  }
  const char *invalid_bindings[] = {
    "null", "[]", "{}", "{\"schema_version\":2,\"plan_version\":0,\"step_index\":0}",
    "{\"schema_version\":1,\"plan_version\":-1,\"step_index\":0}",
    "{\"schema_version\":1,\"plan_version\":0.5,\"step_index\":0}",
    "{\"schema_version\":1,\"plan_version\":\"0\",\"step_index\":0}",
    "{\"schema_version\":1,\"plan_version\":9223372036854775808,\"step_index\":0}",
    "{\"schema_version\":1,\"plan_version\":2,\"step_index\":0}",
    "{\"schema_version\":1,\"plan_version\":0,\"step_index\":1}"
  };
  for (size_t i = 0; i < sizeof(invalid_bindings) / sizeof(invalid_bindings[0]); ++i) {
    it("rejects invalid recorded workflow binding %zu without changing default exports", i) {
      const char *ids[] = {"j1"};
      fresh = workflow_origin_state(); check_not_null(fresh);
      field = json_parse(invalid_bindings[i], strlen(invalid_bindings[i])); check_not_null(field);
      check_equal(turbo_runtime_json_object_set(json_array_get(json_object_get(fresh, "executor_state_versions"), 0), "plan_step_origin", field), 0); field = NULL;
      serialized = json_serialize(fresh, NULL); check_not_null(serialized);
      check_equal(put_origin_state(runtime, serialized), SALTS_OK);
      check_equal(turbo_runtime_json_object_set_string(record, "turn_key", "executor:0:0"), 0);
      check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_tool_journal", "j1", record), 0);
      check_equal(turbo_agent_thread_tool_diagram_ex(runtime, "t", ids, 1,
          TURBO_AGENT_DIAGRAM_CALL_ORIGINS | TURBO_AGENT_DIAGRAM_WORKFLOW_STEPS, &view), SALTS_EPROTO);
      check_null(view);
      check_equal(turbo_agent_thread_tool_diagram_ex(runtime, "t", ids, 1, TURBO_AGENT_DIAGRAM_CALL_ORIGINS, &view), SALTS_OK);
      check_null(json_object_get(evidence_node(view, "event:executor:0:0"), "step_origin_status"));
    }
  }
  it("leaves old executor states unrecorded rather than inferring a matching plan step") {
    const char *ids[] = {"j1"};
    fresh = workflow_origin_state(); check_not_null(fresh);
    serialized = json_serialize(fresh, NULL); check_not_null(serialized);
    check_equal(put_origin_state(runtime, serialized), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_string(record, "turn_key", "executor:0:0"), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_tool_journal", "j1", record), 0);
    check_equal(turbo_agent_thread_tool_diagram_ex(runtime, "t", ids, 1,
        TURBO_AGENT_DIAGRAM_CALL_ORIGINS | TURBO_AGENT_DIAGRAM_WORKFLOW_STEPS, &view), SALTS_OK);
    check_equal(json_array_size(json_object_get(view, "nodes")), 4u);
    check_equal(json_get_string(evidence_node(view, "event:executor:0:0"), "step_origin_status"), "unrecorded");
  }
  it("rejects unknown flags and bounds selected event call scans") {
    const char *ids[] = {"j1"};
    check_equal(turbo_agent_thread_tool_diagram_ex(runtime, "t", ids, 1, 2u, &view), SALTS_EINVAL);
    check_equal(turbo_agent_thread_tool_diagram_ex(runtime, "t", ids, 1, 4u, &view), SALTS_EINVAL);
    check_null(view);
    const char empty_calls[] = "{\"events\":[{\"kind\":\"model\",\"tool_calls\":[]}]}";
    fresh = json_parse(empty_calls, sizeof(empty_calls) - 1); check_not_null(fresh);
    json_value_t *calls = json_object_get(json_array_get(json_object_get(fresh, "events"), 0), "tool_calls");
    check_equal(turbo_runtime_json_object_set_string(record, "turn_key", "planner:root:0"), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_tool_journal", "j1", record), 0);
    for (size_t i = 0; i <= TURBO_AGENT_DIAGRAM_MAX_ORIGIN_CALLS; ++i) {
      field = json_create_object(); check_not_null(field);
      id = tstr_format("call-{}", i); check_not_null(id);
      check_equal(turbo_runtime_json_object_set_string(field, "call_id", i ? id : "call"), 0);
      check_equal(turbo_runtime_json_object_set_string(field, "name", "fs.read"), 0);
      check_equal(turbo_runtime_json_array_append(calls, field), 0); field = NULL;
      tstr_free(id); id = NULL;
      if (i + 1 < TURBO_AGENT_DIAGRAM_MAX_ORIGIN_CALLS) continue;
      serialized = json_serialize(fresh, NULL); check_not_null(serialized);
      check_equal(put_origin_state(runtime, serialized), SALTS_OK);
      json_serialize_free(serialized); serialized = NULL;
      check_equal(turbo_agent_thread_tool_diagram_ex(runtime, "t", ids, 1, TURBO_AGENT_DIAGRAM_CALL_ORIGINS, &view),
          i < TURBO_AGENT_DIAGRAM_MAX_ORIGIN_CALLS ? SALTS_OK : SALTS_ENOSPC);
      if (i == TURBO_AGENT_DIAGRAM_MAX_ORIGIN_CALLS) check_null(view);
      json_free(view); view = NULL;
    }
  }
  it("preserves phase uncertainty, committed failures and selection order across later updates") {
    const char *ids[] = {"j3", "j1", "j2"};
    check_equal(turbo_runtime_json_object_set_string(record, "phase", "started"), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_tool_journal", "j1", record), 0);
    check_equal(turbo_runtime_json_object_set_string(record, "journal_id", "j2"), 0);
    check_equal(turbo_runtime_json_object_set_string(record, "phase", "committed"), 0);
    check_equal(turbo_runtime_json_object_set_int64(record, "status", TURBO_TOOL_ERROR), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_tool_journal", "j2", record), 0);
    check_equal(turbo_runtime_json_object_set_string(record, "journal_id", "j3"), 0);
    check_equal(turbo_runtime_json_object_set_string(record, "phase", "planned"), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_tool_journal", "j3", record), 0);
    check_equal(turbo_agent_thread_tool_diagram(runtime, "t", ids, 3, &view), SALTS_OK);
    check_equal(json_get_string(json_array_get(json_object_get(view, "nodes"), 2), "record_id"), "j3");
    check_equal(json_get_int(json_array_get(json_object_get(view, "nodes"), 4), "status", 1), TURBO_TOOL_ERROR);
    check_equal(turbo_runtime_json_object_set_string(record, "journal_id", "j1"), 0);
    check_equal(turbo_runtime_json_object_set_string(record, "phase", "committed"), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_tool_journal", "j1", record), 0);
    check_equal(turbo_agent_thread_tool_diagram(runtime, "t", ids, 3, &fresh), SALTS_OK);
    check_equal(json_get_string(json_array_get(json_object_get(fresh, "nodes"), 3), "phase"), "committed");
    turbo_agent_runtime_destroy(runtime); runtime = NULL;
    check_equal(turbo_diagram_mermaid(view, &text), SALTS_OK);
    check_contains(text, "started (current journal; side effects unknown)");
    check_contains(text, "committed (current journal; recorded status -1)");
  }
  const struct { const char *key, *value; } damage[] = {
      {"journal_id", "\"other\""}, {"thread_id", "\"other\""}, {"run_id", "\"other\""},
      {"schema_version", "2"}, {"phase", "\"done\""}, {"status", "1"}, {"status", "0.5"},
      {"execution_mode", "3"}, {"idempotency", "3"}, {"arguments_hash", "\"bad\""},
      {"tool_name", "\"name\\u0000tail\""}, {"updated_at", "null"}, {"turn_key", "\"\""}};
  for (size_t i = 0; i < sizeof(damage) / sizeof(damage[0]); ++i) {
    it("rejects malformed or unrelated journal metadata case %zu without partial output", i) {
      const char *ids[] = {"j1"};
      field = json_parse(damage[i].value, strlen(damage[i].value)); check_not_null(field);
      check_equal(turbo_runtime_json_object_set(record, damage[i].key, field), 0); field = NULL;
      check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_tool_journal", "j1", record), 0);
      check_equal(turbo_agent_thread_tool_diagram(runtime, "t", ids, 1, &view), SALTS_EPROTO);
      check_null(view);
    }
  }
  it("rejects invalid selection and preserves typed backend errors without a list fallback") {
    const char *ids[] = {"j1", "missing"}, *duplicates[] = {"j1", "j1"};
    check_equal(turbo_agent_thread_tool_diagram(runtime, "t", duplicates, 2, &view), SALTS_EINVAL);
    check_equal(turbo_agent_thread_tool_diagram(runtime, "t", ids, 0, &view), SALTS_EINVAL);
    check_equal(turbo_agent_thread_tool_diagram(runtime, "t", ids, 33, &view), SALTS_EINVAL);
    check_equal(turbo_agent_thread_tool_diagram(runtime, "t", ids, 2, &view), SALTS_ENOENT);
    check_null(view);
    store.journal_error = SALTS_EIO;
    check_equal(turbo_agent_thread_tool_diagram(runtime, "t", ids, 1, &view), SALTS_EIO);
    check_null(view); check_equal(store.lists, 0);
    store.journal_error = SALTS_EBUSY;
    check_equal(turbo_agent_thread_tool_diagram(runtime, "t", ids, 1, &view), SALTS_EBUSY);
    check_null(view);
  }
  it("bounds selected metadata before copying it") {
    const char *ids[] = {"j1"};
    text = tstr_new_len(NULL, TURBO_AGENT_DIAGRAM_MAX_FIELD_BYTES + 1); check_not_null(text);
    memset(text, 'x', tstr_len(text));
    check_equal(turbo_runtime_json_object_set_string(record, "tool_name", text), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_tool_journal", "j1", record), 0);
    check_equal(turbo_agent_thread_tool_diagram(runtime, "t", ids, 1, &view), SALTS_ENOSPC);
    check_null(view);
  }
  it("admits the full selected journal budget in caller order") {
    const char *ids[TURBO_AGENT_DIAGRAM_MAX_JOURNALS];
    /* This array owns JSON strings so borrowed selection keys survive until the
     * query returns; the produced view must then outlive the selection too. */
    fresh = json_create_array(); check_not_null(fresh);
    for (size_t i = 0; i < TURBO_AGENT_DIAGRAM_MAX_JOURNALS; ++i) {
      id = tstr_format("selected-{}", i); check_not_null(id);
      field = json_create_string(id); check_not_null(field);
      check_equal(turbo_runtime_json_array_append(fresh, field), 0); field = NULL;
      ids[i] = json_string(json_array_get(fresh, i));
      check_equal(turbo_runtime_json_object_set_string(record, "journal_id", id), 0);
      check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_tool_journal", id, record), 0);
      tstr_free(id); id = NULL;
    }
    check_equal(turbo_agent_thread_tool_diagram(runtime, "t", ids, TURBO_AGENT_DIAGRAM_MAX_JOURNALS, &view), SALTS_OK);
    json_free(fresh); fresh = NULL;
    check_equal(json_array_size(json_object_get(view, "nodes")), 34u);
    check_equal(json_get_string(json_array_get(json_object_get(view, "nodes"), 33), "record_id"), "selected-31");
  }
  it("requires an actual published head and never infers publication from journals") {
    const char *ids[] = {"j1"};
    check_equal(turbo_agent_thread_tool_diagram(runtime, "absent", ids, 1, &view), SALTS_ENOENT);
    check_null(view);
    check_equal(put_text(runtime, "thread_heads", "t",
        "{\"schema_version\":1,\"id\":\"t\",\"thread_id\":\"t\",\"run_snapshot\":null}"), 0);
    check_equal(turbo_agent_thread_tool_diagram(runtime, "t", ids, 1, &view), SALTS_EBUSY);
    check_null(view);
  }
  it("applies the combined node limit to published ancestry plus selected journals") {
    const char *ids[] = {"j1"};
    json_free(record); record = NULL;
    for (size_t i = 1; i <= TURBO_DIAGRAM_MAX_NODES - 2; ++i) {
      id = tstr_format("c{}", i); parent = tstr_format("c{}", i + 1);
      check_not_null(id); check_not_null(parent);
      record = json_create_object(); check_not_null(record);
      check_equal(turbo_runtime_json_object_set_string(record, "id", id), 0);
      check_equal(turbo_runtime_json_object_set_string(record, "thread_id", "t"), 0);
      check_equal(turbo_runtime_json_object_set_string(record, "run_id", "r"), 0);
      check_equal(turbo_runtime_json_object_set_int64(record, "seq", (int64_t)i), 0);
      if (i == TURBO_DIAGRAM_MAX_NODES - 2) {
        field = json_create_null(); check_not_null(field);
        check_equal(turbo_runtime_json_object_set(record, "parent_checkpoint_id", field), 0); field = NULL;
      } else check_equal(turbo_runtime_json_object_set_string(record, "parent_checkpoint_id", parent), 0);
      check_equal(turbo_agent_runtime_store_put_json(runtime, "checkpoints", id, record), 0);
      json_free(record); record = NULL; tstr_free(id); tstr_free(parent); id = parent = NULL;
    }
    check_equal(turbo_agent_thread_tool_diagram(runtime, "t", ids, 1, &view), SALTS_OK);
    check_equal(json_array_size(json_object_get(view, "nodes")), TURBO_DIAGRAM_MAX_NODES);
    check_equal(put_origin_state(runtime, "{\"events\":[{\"kind\":\"model\",\"tool_calls\":[{\"call_id\":\"call\",\"name\":\"fs.read\"}]}]}"), SALTS_OK);
    record = journal_record("j1"); check_not_null(record);
    check_equal(turbo_runtime_json_object_set_string(record, "turn_key", "planner:root:0"), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_tool_journal", "j1", record), 0);
    check_equal(turbo_agent_thread_tool_diagram_ex(runtime, "t", ids, 1, TURBO_AGENT_DIAGRAM_CALL_ORIGINS, &fresh), SALTS_ENOSPC);
    check_null(fresh);
    /* Leave room for one request but not its recorded workflow step. */
    check_equal(put_text(runtime, "checkpoints", "c125",
        "{\"id\":\"c125\",\"thread_id\":\"t\",\"run_id\":\"r\",\"seq\":125,\"parent_checkpoint_id\":null}"), 0);
    fresh = workflow_origin_state(); check_not_null(fresh);
    field = json_create_object(); check_not_null(field);
    check_equal(turbo_runtime_json_object_set_int64(field, "schema_version", 1), 0);
    check_equal(turbo_runtime_json_object_set_int64(field, "plan_version", 0), 0);
    check_equal(turbo_runtime_json_object_set_int64(field, "step_index", 0), 0);
    check_equal(turbo_runtime_json_object_set(json_array_get(json_object_get(fresh, "executor_state_versions"), 0), "plan_step_origin", field), 0); field = NULL;
    serialized = json_serialize(fresh, NULL); check_not_null(serialized);
    check_equal(put_origin_state(runtime, serialized), SALTS_OK);
    json_free(fresh); fresh = NULL;
    check_equal(turbo_runtime_json_object_set_string(record, "turn_key", "executor:0:0"), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_tool_journal", "j1", record), 0);
    check_equal(turbo_agent_thread_tool_diagram_ex(runtime, "t", ids, 1, TURBO_AGENT_DIAGRAM_CALL_ORIGINS, &fresh), SALTS_OK);
    check_equal(json_array_size(json_object_get(fresh, "nodes")), TURBO_DIAGRAM_MAX_NODES);
    json_free(fresh); fresh = NULL;
    check_equal(turbo_agent_thread_tool_diagram_ex(runtime, "t", ids, 1,
        TURBO_AGENT_DIAGRAM_CALL_ORIGINS | TURBO_AGENT_DIAGRAM_WORKFLOW_STEPS, &fresh), SALTS_ENOSPC);
    check_null(fresh);
    check_equal(put_text(runtime, "checkpoints", "c125",
        "{\"id\":\"c125\",\"thread_id\":\"t\",\"run_id\":\"r\",\"seq\":125,\"parent_checkpoint_id\":\"c126\"}"), 0);
    check_equal(put_text(runtime, "checkpoints", "c126",
        "{\"id\":\"c126\",\"thread_id\":\"t\",\"run_id\":\"r\",\"seq\":126,\"parent_checkpoint_id\":\"c127\"}"), 0);
    check_equal(put_text(runtime, "checkpoints", "c127",
        "{\"id\":\"c127\",\"thread_id\":\"t\",\"run_id\":\"r\",\"seq\":127,\"parent_checkpoint_id\":null}"), 0);
    check_equal(turbo_agent_thread_tool_diagram(runtime, "t", ids, 1, &fresh), SALTS_ENOSPC);
    check_null(fresh);
  }
}
