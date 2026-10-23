#include "turbo_agent_evidence_diagram.h"
#include "turbo_agent_tool_journal_internal.h"
#include "turbo_agent_plan_step_origin_internal.h"
#include <cmeta/pp.h>
#include <cmeta_crypto.h>
#include <fmt.h>
#include <string.h>

/* This is the exported string projection, not a second journal storage schema. */
#define JOURNAL_STRINGS(M) Schema(M, (journal_id), (thread_id), (run_id), \
    (call_id), (turn_key), (tool_name), (arguments_hash), (phase), (updated_at))

static int valid_text(const char *text, size_t length) {
  if (!text || !length || length > TURBO_AGENT_DIAGRAM_MAX_FIELD_BYTES ||
      strlen(text) != length || !vstr_utf8_valid(vstr_from_buf(text, length))) return 0;
  for (size_t i = 0; i < length; ++i) {
    unsigned char c = (unsigned char)text[i];
    if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127) return 0;
  }
  return 1;
}

static int same_record_string(const json_value_t *left, const json_value_t *right,
                              const char *key) {
  const json_value_t *left_value = json_object_get(left, key);
  const json_value_t *right_value = json_object_get(right, key);
  size_t length;
  if (json_type(left_value) != JSON_STRING || json_type(right_value) != JSON_STRING)
    return 0;
  length = json_string_len(left_value);
  return length == json_string_len(right_value) &&
         memcmp(json_string(left_value), json_string(right_value), length) == 0;
}

static int admitted_claim_valid(const json_value_t *claim, const json_value_t *journal) {
  static const char *identity[] = {"journal_id", "thread_id", "run_id", "call_id",
                                   "turn_key", "tool_name", "arguments_hash"};
  const json_value_t *phase = json_object_get(claim, "phase");
  const json_value_t *updated = json_object_get(claim, "updated_at");
  if (json_type(claim) != JSON_OBJECT ||
      turbo_runtime_json_value_as_int64(json_object_get(claim, "schema_version"), -1) !=
          TURBO_AGENT_TOOL_JOURNAL_SCHEMA_VERSION ||
      json_type(phase) != JSON_STRING || json_string_len(phase) != sizeof("admitted") - 1 ||
      memcmp(json_string(phase), "admitted", sizeof("admitted") - 1) != 0 ||
      json_type(updated) != JSON_STRING ||
      !valid_text(json_string(updated), json_string_len(updated)) ||
      turbo_runtime_json_value_as_int64(json_object_get(claim, "status"), INT64_MAX) !=
          TURBO_TOOL_OK ||
      turbo_runtime_json_value_as_int64(json_object_get(claim, "execution_mode"), INT64_MIN) !=
          turbo_runtime_json_value_as_int64(json_object_get(journal, "execution_mode"), INT64_MAX) ||
      turbo_runtime_json_value_as_int64(json_object_get(claim, "idempotency"), INT64_MIN) !=
          turbo_runtime_json_value_as_int64(json_object_get(journal, "idempotency"), INT64_MAX) ||
      json_object_get(claim, "output") || json_object_get(claim, "detail"))
    return 0;
  for (size_t i = 0; i < sizeof(identity) / sizeof(identity[0]); ++i)
    if (!same_record_string(claim, journal, identity[i])) return 0;
  return 1;
}

static int add_journal(json_value_t *view, const json_value_t *record,
    const json_value_t *claim, const char *requested, const char *thread_id,
    const char *run_node_id) {
  json_value_t *nodes, *node, *edges, *edge;
  tstr node_id = NULL, label = NULL, stored_run = NULL;
  int64_t status, mode, idempotency;
  int rc;
#define READ_STRING(key) \
  const json_value_t *key##_value = json_object_get(record, #key); \
  const char *stored_##key = json_string(key##_value); \
  if (json_type(key##_value) != JSON_STRING) return SALTS_EPROTO; \
  if (json_string_len(key##_value) > TURBO_AGENT_DIAGRAM_MAX_FIELD_BYTES) return SALTS_ENOSPC; \
  if (!valid_text(stored_##key, json_string_len(key##_value))) return SALTS_EPROTO;
  Replay(JOURNAL_STRINGS, READ_STRING)
#undef READ_STRING
  status = turbo_runtime_json_value_as_int64(json_object_get(record, "status"), INT64_MAX);
  mode = turbo_runtime_json_value_as_int64(json_object_get(record, "execution_mode"), -1);
  idempotency = turbo_runtime_json_value_as_int64(json_object_get(record, "idempotency"), -1);
  if (turbo_runtime_json_value_as_int64(json_object_get(record, "schema_version"), -1) !=
          TURBO_AGENT_TOOL_JOURNAL_SCHEMA_VERSION || strcmp(stored_journal_id, requested) ||
      strcmp(stored_thread_id, thread_id) || !turbo_agent_tool_journal_status_valid(status) ||
      (mode != TURBO_TOOL_EXECUTION_SEQUENTIAL && mode != TURBO_TOOL_EXECUTION_PARALLEL_SAFE && mode != TURBO_TOOL_EXECUTION_EXCLUSIVE) ||
      (idempotency != TURBO_TOOL_IDEMPOTENCY_NONE && idempotency != TURBO_TOOL_IDEMPOTENCY_KEYED && idempotency != TURBO_TOOL_IDEMPOTENCY_READ_ONLY) ||
      (strcmp(stored_phase, "planned") && strcmp(stored_phase, "started") && strcmp(stored_phase, "committed")) ||
      strlen(stored_arguments_hash) != 2u * SALTS_SHA256_DIGEST_BYTES) return SALTS_EPROTO;
  for (size_t i = 0; i < 2u * SALTS_SHA256_DIGEST_BYTES; ++i) {
    char c = stored_arguments_hash[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return SALTS_EPROTO;
  }
  stored_run = tstr_format("run:{}", stored_run_id);
  if (!stored_run) return SALTS_ENOMEM;
  if (strcmp(stored_run, run_node_id)) { rc = SALTS_EPROTO; goto done; }
  node_id = tstr_format("journal:{}", requested);
  if (claim && !admitted_claim_valid(claim, record)) { rc = SALTS_EPROTO; goto done; }
  label = (!strcmp(stored_phase, "started") || claim)
      ? tstr_format("{}: {} (current journal; side effects unknown)", stored_tool_name,
                    stored_phase)
      : tstr_format("{}: {} (current journal; recorded status {})", stored_tool_name, stored_phase, status);
  if (!node_id || !label) { rc = SALTS_ENOMEM; goto done; }
  rc = turbo_diagram_add_node(view, node_id, label);
  if (rc != SALTS_OK) goto done;
  nodes = json_object_get(view, "nodes"); node = json_array_get(nodes, json_array_size(nodes) - 1);
#define COPY_STRING(key) \
  if (turbo_runtime_json_object_set_string(node, #key, stored_##key) != 0) { rc = SALTS_ENOMEM; goto done; }
  Replay(JOURNAL_STRINGS, COPY_STRING)
#undef COPY_STRING
  if (turbo_runtime_json_object_set_string(node, "collection", TURBO_AGENT_TOOL_JOURNAL_COLLECTION) != 0 ||
      turbo_runtime_json_object_set_string(node, "record_id", requested) != 0 ||
      turbo_runtime_json_object_set_string(node, "freshness", "current_record") != 0 ||
      turbo_runtime_json_object_set_int64(node, "schema_version", TURBO_AGENT_TOOL_JOURNAL_SCHEMA_VERSION) != 0 ||
      turbo_runtime_json_object_set_int64(node, "status", status) != 0 ||
      turbo_runtime_json_object_set_int64(node, "execution_mode", mode) != 0 ||
      turbo_runtime_json_object_set_int64(node, "idempotency", idempotency) != 0 ||
      turbo_runtime_json_object_set_string(
          node, "effect_status", !strcmp(stored_phase, "committed")
                                     ? "recorded_outcome"
                                     : (!strcmp(stored_phase, "started") || claim)
                                           ? "unknown_side_effect"
                                           : "not_admitted") != 0 ||
      (!strcmp(stored_phase, "planned") &&
       turbo_runtime_json_object_set_string(node, "admission_fence",
                                            claim ? "present" : "absent") != 0)) {
    rc = SALTS_ENOMEM;
    goto done;
  }
  rc = turbo_diagram_add_edge(view, run_node_id, node_id, "same run (current journal)");
  if (rc != SALTS_OK) goto done;
  edges = json_object_get(view, "edges"); edge = json_array_get(edges, json_array_size(edges) - 1);
  if (turbo_runtime_json_object_set_string(edge, "relation", "same_run") != 0 ||
      turbo_runtime_json_object_set_string(edge, "evidence", "agent_tool_journal.thread_id/run_id") != 0)
    rc = SALTS_ENOMEM;
done:
  tstr_free(node_id); tstr_free(label); tstr_free(stored_run);
  return rc;
}
#undef JOURNAL_STRINGS

/* Canonical offsets are shared identities, so aliases such as 00 are invalid. */
static int origin_offset(const char **cursor, size_t *out, char terminator) {
  const char *p = *cursor;
  size_t value = 0;
  if (*p < '0' || *p > '9' || (*p == '0' && p[1] >= '0' && p[1] <= '9')) return SALTS_EPROTO;
  do {
    size_t digit = (size_t)(*p++ - '0');
    if (value > (SIZE_MAX - digit) / 10 || value > ((uint64_t)INT64_MAX - digit) / 10)
      return SALTS_EPROTO;
    value = value * 10 + digit;
  } while (*p >= '0' && *p <= '9');
  if (*p != terminator) return SALTS_EPROTO;
  *cursor = p + (terminator != 0); *out = value;
  return SALTS_OK;
}

static int origin_string(const json_value_t *object, const char *key, const char **out) {
  const json_value_t *value = json_object_get(object, key);
  *out = json_string(value);
  if (json_string_len(value) > TURBO_AGENT_DIAGRAM_MAX_FIELD_BYTES) return SALTS_ENOSPC;
  return json_type(value) == JSON_STRING && valid_text(*out, json_string_len(value))
      ? SALTS_OK : SALTS_EPROTO;
}

static int add_workflow_step(json_value_t *view, json_value_t *event_node,
    const json_value_t *head, const json_value_t *executor_state, size_t executor_version,
    const char *thread, const char *run_node_id) {
  const json_value_t *origin = json_object_get(executor_state, TURBO_AGENT_PLAN_STEP_ORIGIN_KEY);
  const json_value_t *plans, *steps, *step;
  json_value_t *nodes = json_object_get(view, "nodes"), *node = NULL, *edges, *edge;
  tstr step_id = NULL, label = NULL, pointer = NULL, binding_pointer = NULL;
  int rc = SALTS_EPROTO;
  if (!origin) return turbo_runtime_json_object_set_string(event_node, "step_origin_status", "unrecorded") == 0
      ? SALTS_OK : SALTS_ENOMEM;
  if (json_type(origin) != JSON_OBJECT || turbo_runtime_json_value_as_int64(
      json_object_get(origin, "schema_version"), -1) != TURBO_AGENT_PLAN_STEP_ORIGIN_VERSION) return SALTS_EPROTO;
#define READ_ORIGIN_OFFSET(key) \
  int64_t key = turbo_runtime_json_value_as_int64(json_object_get(origin, #key), -1); \
  if (key < 0 || (uint64_t)key > SIZE_MAX) return SALTS_EPROTO;
  Replay(TURBO_AGENT_PLAN_STEP_ORIGIN_FIELDS, READ_ORIGIN_OFFSET)
#undef READ_ORIGIN_OFFSET
  plans = json_object_get(json_object_get(head, "state_snapshot"), "plan_versions");
  if (json_type(plans) != JSON_ARRAY || (size_t)plan_version >= json_array_size(plans)) return SALTS_EPROTO;
  steps = json_object_get(json_array_get(plans, (size_t)plan_version), "steps");
  if (json_type(steps) != JSON_ARRAY || (size_t)step_index >= json_array_size(steps)) return SALTS_EPROTO;
  step = json_array_get(steps, (size_t)step_index);
  if (json_type(step) != JSON_OBJECT || json_type(json_object_get(step, "text")) != JSON_STRING ||
      !json_string_len(json_object_get(step, "text"))) return SALTS_EPROTO;
  step_id = tstr_format("workflow-step:{}:{}", plan_version, step_index);
  label = tstr_format("Workflow plan {} step {} (published)", plan_version, step_index);
  pointer = tstr_format("/run_snapshot/state_snapshot/plan_versions/{}/steps/{}", plan_version, step_index);
  binding_pointer = tstr_format("/run_snapshot/state_snapshot/executor_state_versions/{}/{}",
      executor_version, TURBO_AGENT_PLAN_STEP_ORIGIN_KEY);
  if (!step_id || !label || !pointer || !binding_pointer) { rc = SALTS_ENOMEM; goto done; }
  for (size_t i = 0; i < json_array_size(nodes); ++i) {
    json_value_t *candidate = json_array_get(nodes, i);
    if (!strcmp(json_get_string(candidate, "id"), step_id)) { node = candidate; break; }
  }
  if (!node) {
    rc = turbo_diagram_add_node(view, step_id, label);
    if (rc != SALTS_OK) goto done;
    node = json_array_get(nodes, json_array_size(nodes) - 1);
    if (turbo_runtime_json_object_set_string(node, "collection", "thread_heads") != 0 ||
        turbo_runtime_json_object_set_string(node, "record_id", thread) != 0 ||
        turbo_runtime_json_object_set_string(node, "source_pointer", pointer) != 0 ||
        turbo_runtime_json_object_set_string(node, "freshness", "published_snapshot") != 0) {
      rc = SALTS_ENOMEM; goto done;
    }
#define COPY_ORIGIN_OFFSET(key) \
    if (turbo_runtime_json_object_set_int64(node, #key, key) != 0) { rc = SALTS_ENOMEM; goto done; }
    Replay(TURBO_AGENT_PLAN_STEP_ORIGIN_FIELDS, COPY_ORIGIN_OFFSET)
#undef COPY_ORIGIN_OFFSET
    rc = turbo_diagram_add_edge(view, run_node_id, step_id, "published workflow step");
    if (rc != SALTS_OK) goto done;
    edges = json_object_get(view, "edges"); edge = json_array_get(edges, json_array_size(edges) - 1);
    if (turbo_runtime_json_object_set_string(edge, "relation", "snapshot_contains") != 0) {
      rc = SALTS_ENOMEM; goto done;
    }
  }
  rc = turbo_diagram_add_edge(view, step_id, json_get_string(event_node, "id"), "prepared for workflow step");
  if (rc != SALTS_OK) goto done;
  edges = json_object_get(view, "edges"); edge = json_array_get(edges, json_array_size(edges) - 1);
  if (turbo_runtime_json_object_set_string(edge, "relation", "prepared_for_step") != 0 ||
      turbo_runtime_json_object_set_string(edge, "collection", "thread_heads") != 0 ||
      turbo_runtime_json_object_set_string(edge, "record_id", thread) != 0 ||
      turbo_runtime_json_object_set_string(edge, "source_pointer", binding_pointer) != 0 ||
      turbo_runtime_json_object_set_string(event_node, "step_origin_status", "matched") != 0) rc = SALTS_ENOMEM;
done:
  tstr_free(step_id); tstr_free(label); tstr_free(pointer); tstr_free(binding_pointer);
  return rc;
}

static int add_origin(json_value_t *view, json_value_t *journal,
    const json_value_t *head, const char *thread, const char *run_node_id, unsigned flags) {
  const char *turn = json_get_string(journal, "turn_key"), *cursor = turn;
  const char *versions_key = NULL, *kind, *call_id, *name;
  const char *status = "not_in_snapshot";
  const json_value_t *state, *versions, *events, *event, *calls;
  json_value_t *nodes = json_object_get(view, "nodes"), *node = NULL, *edges, *edge;
  size_t version = 0, event_index = 0, call_index = SIZE_MAX;
  tstr pointer = NULL, event_id = NULL, label = NULL, call_pointer = NULL;
  int rc = SALTS_OK;
  if (!strncmp(cursor, "planner:root:", 13)) cursor += 13;
  else if (!strncmp(cursor, "planner:", 8) || !strncmp(cursor, "executor:", 9)) {
    int planner = cursor[0] == 'p';
    versions_key = planner ? "planner_state_versions" : "executor_state_versions";
    cursor += planner ? 8 : 9;
    rc = origin_offset(&cursor, &version, ':');
    if (rc != SALTS_OK) goto done;
  } else { status = "unresolved_identity"; goto done; }
  rc = origin_offset(&cursor, &event_index, 0);
  if (rc != SALTS_OK) goto done;
  state = json_object_get(head, "state_snapshot");
  if (json_type(state) != JSON_OBJECT) { rc = SALTS_EPROTO; goto done; }
  if (versions_key) {
    versions = json_object_get(state, versions_key);
    if (!versions) goto done;
    if (json_type(versions) != JSON_ARRAY) { rc = SALTS_EPROTO; goto done; }
    if (version >= json_array_size(versions)) goto done;
    state = json_array_get(versions, version);
    if (json_type(state) != JSON_OBJECT) { rc = SALTS_EPROTO; goto done; }
  }
  events = json_object_get(state, "events");
  if (!events) goto done;
  if (json_type(events) != JSON_ARRAY) { rc = SALTS_EPROTO; goto done; }
  if (event_index >= json_array_size(events)) goto done;
  event = json_array_get(events, event_index);
  rc = origin_string(event, "kind", &kind);
  if (rc != SALTS_OK) goto done;
  if (strcmp(kind, "model")) { rc = SALTS_EPROTO; goto done; }
  calls = json_object_get(event, "tool_calls");
  if (json_type(calls) != JSON_ARRAY) { rc = SALTS_EPROTO; goto done; }
  if (json_array_size(calls) > TURBO_AGENT_DIAGRAM_MAX_ORIGIN_CALLS) { rc = SALTS_ENOSPC; goto done; }
  for (size_t i = 0; i < json_array_size(calls); ++i) {
    const json_value_t *call = json_array_get(calls, i);
    rc = origin_string(call, "call_id", &call_id);
    if (rc != SALTS_OK) goto done;
    rc = origin_string(call, "name", &name);
    if (rc != SALTS_OK) goto done;
    if (strcmp(call_id, json_get_string(journal, "call_id"))) continue;
    if (call_index != SIZE_MAX || strcmp(name, json_get_string(journal, "tool_name"))) {
      rc = SALTS_EPROTO; goto done;
    }
    call_index = i;
  }
  if (call_index == SIZE_MAX) goto done;
  pointer = versions_key
      ? tstr_format("/run_snapshot/state_snapshot/{}/{}/events/{}", versions_key, version, event_index)
      : tstr_format("/run_snapshot/state_snapshot/events/{}", event_index);
  event_id = tstr_format("event:{}", turn);
  label = tstr_format("{} (published request)", turn);
  if (!pointer || !event_id || !label) { rc = SALTS_ENOMEM; goto done; }
  for (size_t i = 0; i < json_array_size(nodes); ++i) {
    json_value_t *candidate = json_array_get(nodes, i);
    if (!strcmp(json_get_string(candidate, "id"), event_id)) { node = candidate; break; }
  }
  if (!node) {
    rc = turbo_diagram_add_node(view, event_id, label);
    if (rc != SALTS_OK) goto done;
    node = json_array_get(nodes, json_array_size(nodes) - 1);
    if (turbo_runtime_json_object_set_string(node, "collection", "thread_heads") != 0 ||
        turbo_runtime_json_object_set_string(node, "record_id", thread) != 0 ||
        turbo_runtime_json_object_set_string(node, "source_pointer", pointer) != 0 ||
        turbo_runtime_json_object_set_string(node, "turn_key", turn) != 0 ||
        turbo_runtime_json_object_set_string(node, "freshness", "published_snapshot") != 0) {
      rc = SALTS_ENOMEM; goto done;
    }
    rc = turbo_diagram_add_edge(view, run_node_id, event_id, "published request");
    if (rc != SALTS_OK) goto done;
    edges = json_object_get(view, "edges"); edge = json_array_get(edges, json_array_size(edges) - 1);
    if (turbo_runtime_json_object_set_string(edge, "relation", "snapshot_contains") != 0) {
      rc = SALTS_ENOMEM; goto done;
    }
    if (flags & TURBO_AGENT_DIAGRAM_WORKFLOW_STEPS) {
      rc = versions_key && !strcmp(versions_key, "executor_state_versions")
          ? add_workflow_step(view, node, head, state, version, thread, run_node_id)
          : (turbo_runtime_json_object_set_string(node, "step_origin_status", "not_applicable") == 0
              ? SALTS_OK : SALTS_ENOMEM);
      if (rc != SALTS_OK) goto done;
    }
  }
  call_pointer = tstr_format("{}/tool_calls/{}", pointer, call_index);
  if (!call_pointer) { rc = SALTS_ENOMEM; goto done; }
  rc = turbo_diagram_add_edge(view, event_id, json_get_string(journal, "id"), "same call identity");
  if (rc != SALTS_OK) goto done;
  edges = json_object_get(view, "edges"); edge = json_array_get(edges, json_array_size(edges) - 1);
  if (turbo_runtime_json_object_set_string(edge, "relation", "same_call_identity") != 0 ||
      turbo_runtime_json_object_set_string(edge, "collection", "thread_heads") != 0 ||
      turbo_runtime_json_object_set_string(edge, "record_id", thread) != 0 ||
      turbo_runtime_json_object_set_string(edge, "source_pointer", call_pointer) != 0) {
    rc = SALTS_ENOMEM; goto done;
  }
  status = "matched";
done:
  if (rc == SALTS_OK && turbo_runtime_json_object_set_string(journal, "origin_status", status) != 0)
    rc = SALTS_ENOMEM;
  tstr_free(pointer); tstr_free(event_id); tstr_free(label); tstr_free(call_pointer);
  return rc;
}

int turbo_agent_thread_tool_diagram(turbo_agent_runtime_t *runtime,
    const char *thread_id, const char *const *journal_ids, size_t journal_count,
    json_value_t **out_view) {
  return turbo_agent_thread_tool_diagram_ex(runtime, thread_id, journal_ids, journal_count, 0, out_view);
}

int turbo_agent_thread_tool_diagram_ex(turbo_agent_runtime_t *runtime,
    const char *thread_id, const char *const *journal_ids, size_t journal_count,
    unsigned flags, json_value_t **out_view) {
  json_value_t *view = NULL, *record = NULL, *claim = NULL, *head = NULL;
  const char *run_node_id;
  int rc;
  if (out_view) *out_view = NULL;
  if ((flags & ~(TURBO_AGENT_DIAGRAM_CALL_ORIGINS | TURBO_AGENT_DIAGRAM_WORKFLOW_STEPS)) ||
      ((flags & TURBO_AGENT_DIAGRAM_WORKFLOW_STEPS) && !(flags & TURBO_AGENT_DIAGRAM_CALL_ORIGINS)) ||
      !runtime || !thread_id || !valid_text(thread_id, strlen(thread_id)) ||
      !journal_ids || !journal_count || journal_count > TURBO_AGENT_DIAGRAM_MAX_JOURNALS || !out_view)
    return SALTS_EINVAL;
  for (size_t i = 0; i < journal_count; ++i) {
    if (!journal_ids[i] || !valid_text(journal_ids[i], strlen(journal_ids[i]))) return SALTS_EINVAL;
    for (size_t j = 0; j < i; ++j) if (!strcmp(journal_ids[i], journal_ids[j])) return SALTS_EINVAL;
  }
  rc = turbo_agent_runtime_thread_diagram(runtime, thread_id, &view);
  if (rc != SALTS_OK) return rc;
  if (journal_count > TURBO_DIAGRAM_MAX_NODES - json_array_size(json_object_get(view, "nodes"))) {
    rc = SALTS_ENOSPC; goto done;
  }
  /* Nodes are separate owned objects; appending array entries does not move the
   * first node's ID string. It remains borrowed only until this view is freed. */
  run_node_id = json_get_string(json_array_get(json_object_get(view, "nodes"), 0), "id");
  if (flags & TURBO_AGENT_DIAGRAM_CALL_ORIGINS) {
    const char *head_id;
    rc = turbo_agent_runtime_get_published_thread_head(runtime, thread_id, &head);
    if (rc != SALTS_OK) goto done;
    rc = origin_string(head, "id", &head_id);
    if (rc != SALTS_OK) goto done;
    if (strcmp(head_id, run_node_id + 4)) { rc = SALTS_EPROTO; goto done; }
  }
  for (size_t i = 0; i < journal_count; ++i) {
    rc = turbo_agent_runtime_store_get_json_status(runtime, TURBO_AGENT_TOOL_JOURNAL_COLLECTION, journal_ids[i], &record);
    if (rc != SALTS_OK) goto done;
    {
      const json_value_t *phase = json_object_get(record, "phase");
      if (json_type(phase) == JSON_STRING && json_string_len(phase) == sizeof("planned") - 1 &&
          memcmp(json_string(phase), "planned", sizeof("planned") - 1) == 0) {
        rc = turbo_agent_runtime_store_get_json_status(
            runtime, TURBO_AGENT_TOOL_CLAIM_COLLECTION, journal_ids[i], &claim);
        if (rc != SALTS_OK && rc != SALTS_ENOENT) goto done;
      }
    }
    rc = add_journal(view, record, claim, journal_ids[i], thread_id, run_node_id);
    json_free(record); record = NULL;
    json_free(claim); claim = NULL;
    if (rc != SALTS_OK) goto done;
    if (head) {
      json_value_t *nodes = json_object_get(view, "nodes");
      rc = add_origin(view, json_array_get(nodes, json_array_size(nodes) - 1), head, thread_id, run_node_id, flags);
      if (rc != SALTS_OK) goto done;
    }
  }
  if (turbo_runtime_json_object_set_string(view, "record_scope", "published_and_current") != 0 ||
      turbo_runtime_json_object_set_string(view, "journal_scope", "selected_records") != 0 ||
      turbo_runtime_json_object_set_string(view, "journal_consistency", "caller_quiescent_read") != 0) {
    rc = SALTS_ENOMEM; goto done;
  }
  *out_view = view; view = NULL;
done:
  json_free(record); json_free(claim); json_free(view); json_free(head);
  return rc;
}
