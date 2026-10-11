#include "turbo_runtime_diagram.h"
#include <fmt.h>
#include <string.h>

static const char *record_string(const json_value_t *record, const char *key) {
  const json_value_t *value = json_object_get(record, key);
  const char *text = json_string(value);
  return json_type(value) == JSON_STRING && text && json_string_len(value) <= 1024 &&
      strlen(text) == json_string_len(value) && text[0] ? text : NULL;
}

static int add_record(json_value_t *view, const char *collection, const char *record_id,
                      const char *node_id, const char *label) {
  json_value_t *nodes, *node;
  int rc = turbo_diagram_add_node(view, node_id, label);
  if (rc != 0) return rc;
  nodes = json_object_get(view, "nodes"); node = json_array_get(nodes, json_array_size(nodes) - 1);
  if (turbo_runtime_json_object_set_string(node, "collection", collection) != 0 ||
      turbo_runtime_json_object_set_string(node, "record_id", record_id) != 0) return SALTS_ENOMEM;
  return SALTS_OK;
}

int turbo_agent_runtime_thread_diagram(turbo_agent_runtime_t *runtime,
                                       const char *thread_id, json_value_t **out_view) {
  json_value_t *head = NULL, *record = NULL, *view = NULL, *seen = NULL;
  tstr current = NULL, child = NULL, node = NULL, label = NULL;
  const char *id, *checkpoint;
  int rc;
  if (out_view) *out_view = NULL;
  if (!runtime || !thread_id || !thread_id[0] || !out_view) return SALTS_EINVAL;
  rc = turbo_agent_runtime_get_published_thread_head(runtime, thread_id, &head);
  if (rc != 0) return rc;
  id = record_string(head, "id");
  if (!id) { rc = SALTS_EPROTO; goto done; }
  child = tstr_format("run:{}", id);
  label = tstr_format("Run {}: {} (published snapshot)", id, json_get_string(head, "status"));
  view = turbo_diagram_create("execution"); seen = json_create_object();
  if (!child || !label || !view || !seen) { rc = SALTS_ENOMEM; goto done; }
  rc = add_record(view, "thread_heads", thread_id, child, label);
  tstr_free(label); label = NULL;
  if (rc != 0) goto done;
  if (turbo_runtime_json_object_set_string(view, "thread_id", thread_id) != 0) {
    rc = SALTS_ENOMEM; goto done;
  }
  checkpoint = json_get_string(head, "latest_checkpoint_id");
  if (checkpoint) {
    if (!record_string(head, "latest_checkpoint_id")) { rc = SALTS_EPROTO; goto done; }
    current = tstr_dup(checkpoint);
    if (!current) { rc = SALTS_ENOMEM; goto done; }
  }
  while (current) {
    const json_value_t *parent;
    const char *parent_id, *stored_id, *stored_thread, *run_id;
    int64_t sequence;
    if (json_object_get(seen, current)) { rc = SALTS_EPROTO; goto done; }
    if (json_array_size(json_object_get(view, "nodes")) >= TURBO_DIAGRAM_MAX_NODES) {
      rc = SALTS_ENOSPC; goto done;
    }
    if (turbo_runtime_json_object_set_int64(seen, current, 1) != 0) { rc = SALTS_ENOMEM; goto done; }
    rc = turbo_agent_runtime_store_get_json_status(runtime, "checkpoints", current, &record);
    if (rc != 0) goto done;
    stored_id = record_string(record, "id"); stored_thread = record_string(record, "thread_id");
    run_id = record_string(record, "run_id");
    sequence = turbo_runtime_json_value_as_int64(json_object_get(record, "seq"), -1);
    parent = json_object_get(record, "parent_checkpoint_id");
    parent_id = record_string(record, "parent_checkpoint_id");
    if (!stored_id || strcmp(stored_id, current) || !stored_thread || strcmp(stored_thread, thread_id) ||
        !run_id || sequence < 1 || !parent || (json_type(parent) != JSON_NULL && !parent_id)) {
      rc = SALTS_EPROTO; goto done;
    }
    node = tstr_format("checkpoint:{}", current);
    label = tstr_format("Checkpoint {}: run {}, seq {}", current, run_id, sequence);
    if (!node || !label) { rc = SALTS_ENOMEM; goto done; }
    rc = add_record(view, "checkpoints", current, node, label);
    tstr_free(label); label = NULL;
    if (rc != 0) goto done;
    rc = turbo_diagram_add_edge(view, node, child,
        json_array_size(json_object_get(view, "nodes")) == 2 ? "snapshot checkpoint" : "parent checkpoint");
    if (rc != 0) goto done;
    tstr_free(child); child = node; node = NULL;
    tstr_free(current); current = parent_id ? tstr_dup(parent_id) : NULL;
    if (parent_id && !current) { rc = SALTS_ENOMEM; goto done; }
    json_free(record); record = NULL;
  }
  *out_view = view; view = NULL; rc = SALTS_OK;
done:
  json_free(head); json_free(record); json_free(view); json_free(seen);
  tstr_free(current); tstr_free(child); tstr_free(node); tstr_free(label);
  return rc;
}
