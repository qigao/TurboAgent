#include "turbo_runtime_history.h"
#include <stdlib.h>
#include <string.h>

static const char *const turbo_agent_runtime_threads_collection = "threads";
static const char *const turbo_agent_runtime_runs_collection = "runs";
static const char *const turbo_agent_runtime_checkpoints_collection = "checkpoints";

static void turbo_runtime_history_json_free(json_value_t **value) {
  if (!value || !*value) {
    return;
  }
  json_free(*value);
  *value = NULL;
}

static char *turbo_runtime_history_strdup(const char *text) {
  size_t len;
  char *copy;

  if (!text) {
    return NULL;
  }
  len = strlen(text);
  copy = (char *)malloc(len + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, text, len + 1);
  return copy;
}

static int turbo_runtime_history_record_string_compare(const char *left, const char *right) {
  if (left && right) {
    return strcmp(left, right);
  }
  if (left) {
    return 1;
  }
  if (right) {
    return -1;
  }
  return 0;
}

static int turbo_runtime_history_json_value_array_append_json(json_value_t *array_json_value,
                                                            const json_value_t *json_value) {
  json_value_t *bound;
  turbo_runtime_json_status_t status;

  if (!array_json_value || !json_value) {
    return -1;
  }
  bound = json_clone(json_value);
  if (!bound) {
    return -1;
  }
  status = turbo_runtime_json_array_append(array_json_value, bound);
  if (status != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(bound);
    return -1;
  }
  return 0;
}

static int turbo_runtime_history_json_value_from_json_value(const json_value_t *value,
                                                          json_value_t **out_value) {
  if (!value || !out_value) {
    return -1;
  }
  *out_value = json_clone(value);
  return *out_value ? 0 : -1;
}

static int turbo_runtime_history_state_from_checkpoint_record(const json_value_t *checkpoint_record,
                                                            json_value_t **out_state) {
  if (out_state) *out_state = NULL;
  const json_value_t *checkpoint_json;
  const json_value_t *state_json;

  if (!checkpoint_record || !out_state) {
    return -1;
  }
  checkpoint_json = json_object_get(checkpoint_record, "checkpoint_json");
  if (!checkpoint_json || json_type(checkpoint_json) != JSON_OBJECT) {
    return -1;
  }
  state_json = json_object_get(checkpoint_json, "state");
  if (!state_json) {
    return -1;
  }
  return turbo_runtime_history_json_value_from_json_value(state_json, out_state);
}

static int turbo_runtime_history_state_from_run_record(turbo_agent_runtime_t *runtime,
                                                     const json_value_t *run_record,
                                                     json_value_t **out_state) {
  if (out_state) *out_state = NULL;
  const json_value_t *state_json;
  const char *latest_checkpoint_id;
  json_value_t *checkpoint_record = NULL;
  int rc;

  if (!runtime || !run_record || !out_state) {
    return -1;
  }
  state_json = json_object_get(run_record, "state_snapshot");
  if (state_json && json_type(state_json) != JSON_NULL) {
    return turbo_runtime_history_json_value_from_json_value(state_json, out_state);
  }
  latest_checkpoint_id = json_get_string(run_record, "latest_checkpoint_id");
  if (!latest_checkpoint_id || latest_checkpoint_id[0] == '\0') {
    return -1;
  }
  rc = turbo_agent_runtime_store_get_json(runtime, turbo_agent_runtime_checkpoints_collection,
                                          latest_checkpoint_id, &checkpoint_record);
  if (rc != 0) {
    return -1;
  }
  rc = turbo_runtime_history_state_from_checkpoint_record(checkpoint_record, out_state);
  turbo_runtime_history_json_free(&checkpoint_record);
  return rc;
}

static int turbo_runtime_history_select_run_id_for_thread(turbo_agent_runtime_t *runtime,
                                                        const char *thread_id,
                                                        const char *status_filter,
                                                        char **out_run_id) {
  if (out_run_id) *out_run_id = NULL;
  json_value_t *runs = NULL;
  const char *best_run_id = NULL;
  const char *best_updated_at = NULL;
  char *owned_run_id = NULL;
  size_t i;
  size_t count;
  int rc = -1;

  if (!runtime || !thread_id || !out_run_id) {
    return -1;
  }
  *out_run_id = NULL;
  if (turbo_agent_runtime_store_list_json(runtime, turbo_agent_runtime_runs_collection, "thread_id",
                                          thread_id, &runs) != 0 ||
      !runs || json_type(runs) != JSON_ARRAY) {
    goto cleanup;
  }
  count = json_array_size(runs);
  for (i = 0; i < count; ++i) {
    const json_value_t *record = json_array_get(runs, i);
    const char *status;
    const char *updated_at;
    const char *run_id;

    if (!record || json_type(record) != JSON_OBJECT) {
      continue;
    }
    status = json_get_string(record, "status");
    run_id = json_get_string(record, "id");
    updated_at = json_get_string(record, "updated_at");
    if (status_filter && (!status || strcmp(status, status_filter) != 0)) {
      continue;
    }
    if (!run_id || run_id[0] == '\0') {
      continue;
    }
    if (!best_run_id ||
        turbo_runtime_history_record_string_compare(updated_at, best_updated_at) > 0) {
      best_run_id = run_id;
      best_updated_at = updated_at;
    }
  }
  if (!best_run_id) {
    goto cleanup;
  }
  owned_run_id = turbo_runtime_history_strdup(best_run_id);
  if (!owned_run_id) {
    goto cleanup;
  }
  *out_run_id = owned_run_id;
  owned_run_id = NULL;
  rc = 0;

cleanup:
  free(owned_run_id);
  turbo_runtime_history_json_free(&runs);
  return rc;
}

int turbo_agent_runtime_latest_run_id_for_thread(turbo_agent_runtime_t *runtime,
                                                 const char *thread_id, char **out_run_id) {
  if (out_run_id) *out_run_id = NULL;
  return turbo_runtime_history_select_run_id_for_thread(runtime, thread_id, NULL, out_run_id);
}

int turbo_agent_runtime_pending_run_id_for_thread(turbo_agent_runtime_t *runtime,
                                                         const char *thread_id, char **out_run_id) {
  if (out_run_id) *out_run_id = NULL;
  return turbo_runtime_history_select_run_id_for_thread(runtime, thread_id, "interrupted",
                                                      out_run_id);
}

static int turbo_runtime_history_latest_checkpoint_id_for_run(turbo_agent_runtime_t *runtime,
                                                            const char *run_id,
                                                            char **out_checkpoint_id) {
  if (out_checkpoint_id) *out_checkpoint_id = NULL;
  json_value_t *run_record = NULL;
  const char *checkpoint_id;
  char *owned_checkpoint_id = NULL;
  int rc = -1;

  if (!runtime || !run_id || !out_checkpoint_id) {
    return -1;
  }
  *out_checkpoint_id = NULL;
  if (turbo_agent_runtime_store_get_json(runtime, turbo_agent_runtime_runs_collection, run_id,
                                         &run_record) != 0 ||
      !run_record) {
    goto cleanup;
  }
  checkpoint_id = json_get_string(run_record, "latest_checkpoint_id");
  if (!checkpoint_id || checkpoint_id[0] == '\0') {
    goto cleanup;
  }
  owned_checkpoint_id = turbo_runtime_history_strdup(checkpoint_id);
  if (!owned_checkpoint_id) {
    goto cleanup;
  }
  *out_checkpoint_id = owned_checkpoint_id;
  owned_checkpoint_id = NULL;
  rc = 0;

cleanup:
  free(owned_checkpoint_id);
  turbo_runtime_history_json_free(&run_record);
  return rc;
}

int turbo_agent_runtime_current_run_id_for_thread(turbo_agent_runtime_t *runtime,
                                                         const char *thread_id, char **out_run_id) {
  if (out_run_id) *out_run_id = NULL;
  if (turbo_agent_runtime_pending_run_id_for_thread(runtime, thread_id, out_run_id) == 0 &&
      out_run_id && *out_run_id) {
    return 0;
  }
  free(out_run_id ? *out_run_id : NULL);
  if (out_run_id) {
    *out_run_id = NULL;
  }
  return turbo_agent_runtime_latest_run_id_for_thread(runtime, thread_id, out_run_id);
}

int turbo_agent_runtime_current_checkpoint_id_for_thread(turbo_agent_runtime_t *runtime,
                                                         const char *thread_id,
                                                         char **out_checkpoint_id) {
  if (out_checkpoint_id) *out_checkpoint_id = NULL;
  char *run_id = NULL;
  int rc;

  if (!runtime || !thread_id || !out_checkpoint_id) {
    return -1;
  }
  *out_checkpoint_id = NULL;
  rc = turbo_agent_runtime_current_run_id_for_thread(runtime, thread_id, &run_id);
  if (rc != 0 || !run_id) {
    free(run_id);
    return -1;
  }
  rc = turbo_runtime_history_latest_checkpoint_id_for_run(runtime, run_id, out_checkpoint_id);
  free(run_id);
  return rc;
}

int turbo_agent_runtime_thread_head_run_id_for_thread(turbo_agent_runtime_t *runtime,
                                                             const char *thread_id,
                                                             char **out_run_id) {
  if (out_run_id) *out_run_id = NULL;
  return turbo_agent_runtime_current_run_id_for_thread(runtime, thread_id, out_run_id);
}

static int
turbo_runtime_history_append_checkpoint_events_json_value(json_value_t *events_json_value,
                                                        const json_value_t *checkpoint_record) {
  json_value_t *events_json;
  size_t i;
  size_t count;

  if (!events_json_value || !checkpoint_record) {
    return -1;
  }
  events_json = json_object_get(checkpoint_record, "events");
  if (!events_json || json_type(events_json) != JSON_ARRAY) {
    return -1;
  }
  count = json_array_size(events_json);
  for (i = 0; i < count; ++i) {
    if (turbo_runtime_history_json_value_array_append_json(
            events_json_value, json_array_get(events_json, i)) != 0) {
      return -1;
    }
  }
  return 0;
}

static int turbo_runtime_history_checkpoint_seq_compare(const void *left, const void *right) {
  const json_value_t *const *a = (const json_value_t *const *)left;
  const json_value_t *const *b = (const json_value_t *const *)right;
  const json_value_t *a_seq = json_object_get(*a, "seq");
  const json_value_t *b_seq = json_object_get(*b, "seq");
  double av =
      (a_seq && json_type(a_seq) == JSON_NUMBER) ? json_number(a_seq) : 0.0;
  double bv =
      (b_seq && json_type(b_seq) == JSON_NUMBER) ? json_number(b_seq) : 0.0;
  return av < bv ? -1 : av > bv ? 1 : 0;
}

CXX_C_API int turbo_agent_runtime_get_thread(turbo_agent_runtime_t *runtime, const char *thread_id,
                                             json_value_t **out_thread_json) {
  if (out_thread_json) *out_thread_json = NULL;
  return turbo_agent_runtime_store_get_json(runtime, turbo_agent_runtime_threads_collection,
                                            thread_id, out_thread_json);
}


CXX_C_API int turbo_agent_runtime_get_run(turbo_agent_runtime_t *runtime, const char *run_id,
                                          json_value_t **out_run_json) {
  if (out_run_json) *out_run_json = NULL;
  return turbo_agent_runtime_store_get_json(runtime, turbo_agent_runtime_runs_collection, run_id,
                                            out_run_json);
}


CXX_C_API int turbo_agent_runtime_get_latest_run(turbo_agent_runtime_t *runtime,
                                                 const char *thread_id,
                                                 json_value_t **out_run_json) {
  if (out_run_json) *out_run_json = NULL;
  char *run_id = NULL;
  int rc;

  if (!runtime || !thread_id || !out_run_json) {
    return -1;
  }
  rc = turbo_agent_runtime_latest_run_id_for_thread(runtime, thread_id, &run_id);
  if (rc != 0 || !run_id) {
    free(run_id);
    return -1;
  }
  rc = turbo_agent_runtime_get_run(runtime, run_id, out_run_json);
  free(run_id);
  return rc;
}


CXX_C_API int turbo_agent_runtime_get_pending_run(turbo_agent_runtime_t *runtime,
                                                  const char *thread_id,
                                                  json_value_t **out_run_json) {
  if (out_run_json) *out_run_json = NULL;
  char *run_id = NULL;
  int rc;

  if (!runtime || !thread_id || !out_run_json) {
    return -1;
  }
  rc = turbo_agent_runtime_pending_run_id_for_thread(runtime, thread_id, &run_id);
  if (rc != 0 || !run_id) {
    free(run_id);
    return -1;
  }
  rc = turbo_agent_runtime_get_run(runtime, run_id, out_run_json);
  free(run_id);
  return rc;
}


CXX_C_API int turbo_agent_runtime_get_checkpoint(turbo_agent_runtime_t *runtime,
                                                 const char *checkpoint_id,
                                                 json_value_t **out_checkpoint_json) {
  if (out_checkpoint_json) *out_checkpoint_json = NULL;
  return turbo_agent_runtime_store_get_json(runtime, turbo_agent_runtime_checkpoints_collection,
                                            checkpoint_id, out_checkpoint_json);
}


CXX_C_API int turbo_agent_runtime_get_latest_checkpoint(turbo_agent_runtime_t *runtime,
                                                        const char *run_id,
                                                        json_value_t **out_checkpoint_json) {
  if (out_checkpoint_json) *out_checkpoint_json = NULL;
  char *checkpoint_id = NULL;
  int rc;

  if (!runtime || !run_id || !out_checkpoint_json) {
    return -1;
  }
  rc = turbo_runtime_history_latest_checkpoint_id_for_run(runtime, run_id, &checkpoint_id);
  if (rc != 0 || !checkpoint_id) {
    free(checkpoint_id);
    return -1;
  }
  rc = turbo_agent_runtime_get_checkpoint(runtime, checkpoint_id, out_checkpoint_json);
  free(checkpoint_id);
  return rc;
}


CXX_C_API int turbo_agent_runtime_get_checkpoint_state_json_value(turbo_agent_runtime_t *runtime,
                                                                  const char *checkpoint_id,
                                                                  json_value_t **out_state) {
  if (out_state) *out_state = NULL;
  json_value_t *checkpoint_record = NULL;
  int rc;

  if (!runtime || !checkpoint_id || !out_state) {
    return -1;
  }
  *out_state = NULL;
  rc = turbo_agent_runtime_get_checkpoint(runtime, checkpoint_id, &checkpoint_record);
  if (rc != 0) {
    return -1;
  }
  rc = turbo_runtime_history_state_from_checkpoint_record(checkpoint_record, out_state);
  turbo_runtime_history_json_free(&checkpoint_record);
  return rc;
}


CXX_C_API int turbo_agent_runtime_get_run_state_json_value(turbo_agent_runtime_t *runtime,
                                                           const char *run_id,
                                                           json_value_t **out_state) {
  if (out_state) *out_state = NULL;
  json_value_t *run_record = NULL;
  int rc;

  if (!runtime || !run_id || !out_state) {
    return -1;
  }
  *out_state = NULL;
  rc = turbo_agent_runtime_get_run(runtime, run_id, &run_record);
  if (rc != 0) {
    return -1;
  }
  rc = turbo_runtime_history_state_from_run_record(runtime, run_record, out_state);
  turbo_runtime_history_json_free(&run_record);
  return rc;
}


CXX_C_API int turbo_agent_runtime_get_thread_state_json_value(turbo_agent_runtime_t *runtime,
                                                              const char *thread_id,
                                                              json_value_t **out_state) {
  if (out_state) *out_state = NULL;
  char *run_id = NULL;
  int rc;

  if (!runtime || !thread_id || !out_state) {
    return -1;
  }
  if (turbo_agent_runtime_latest_run_id_for_thread(runtime, thread_id, &run_id) != 0 || !run_id) {
    return -1;
  }
  rc = turbo_agent_runtime_get_run_state_json_value(runtime, run_id, out_state);
  free(run_id);
  return rc;
}


CXX_C_API int turbo_agent_runtime_get_thread_head_state_json_value(turbo_agent_runtime_t *runtime,
                                                                   const char *thread_id,
                                                                   json_value_t **out_state) {
  if (out_state) *out_state = NULL;
  char *run_id = NULL;
  int rc;

  if (!runtime || !thread_id || !out_state) {
    return -1;
  }
  if (turbo_agent_runtime_thread_head_run_id_for_thread(runtime, thread_id, &run_id) != 0 ||
      !run_id) {
    return -1;
  }
  rc = turbo_agent_runtime_get_run_state_json_value(runtime, run_id, out_state);
  free(run_id);
  return rc;
}


CXX_C_API int turbo_agent_runtime_list_runs(turbo_agent_runtime_t *runtime, const char *thread_id,
                                            json_value_t **out_runs_json) {
  return turbo_agent_runtime_store_list_json(runtime, turbo_agent_runtime_runs_collection,
                                             "thread_id", thread_id, out_runs_json);
}


CXX_C_API int turbo_agent_runtime_list_child_runs(turbo_agent_runtime_t *runtime,
                                                  const char *parent_agent_run_id,
                                                  json_value_t **out_runs_json) {
  return turbo_agent_runtime_store_list_json(runtime, turbo_agent_runtime_runs_collection,
                                             "parent_agent_run_id", parent_agent_run_id,
                                             out_runs_json);
}


CXX_C_API int turbo_agent_runtime_list_checkpoints(turbo_agent_runtime_t *runtime,
                                                   const char *run_id,
                                                   json_value_t **out_checkpoints_json) {
  return turbo_agent_runtime_store_list_json(runtime, turbo_agent_runtime_checkpoints_collection,
                                             "run_id", run_id, out_checkpoints_json);
}


static int turbo_runtime_checkpoint_chain(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id,
    json_value_t **out_chain, char **out_root_id) {
  json_value_t *seen = NULL;
  json_value_t *chain = NULL;
  json_value_t *record = NULL;
  char *current_id = NULL;
  char *next_id = NULL;
  int rc = -1;
  if (out_chain) *out_chain = NULL;
  if (out_root_id) *out_root_id = NULL;
  if (!runtime || !checkpoint_id || (!out_chain && !out_root_id)) return -1;
  seen = json_create_object();
  if (out_chain) chain = json_create_array();
  current_id = turbo_runtime_history_strdup(checkpoint_id);
  if (!seen || (out_chain && !chain) || !current_id) goto cleanup;
  for (;;) {
    const json_value_t *parent;
    const char *parent_id;
    if (json_object_get(seen, current_id) ||
        turbo_runtime_json_object_set_int64(seen, current_id, 1) != 0 ||
        turbo_agent_runtime_get_checkpoint(runtime, current_id, &record) != 0 ||
        !record || json_type(record) != JSON_OBJECT) goto cleanup;
    parent = json_object_get(record, "parent_checkpoint_id");
    if (parent && json_type(parent) != JSON_NULL && json_type(parent) != JSON_STRING) goto cleanup;
    parent_id = parent ? turbo_runtime_json_value_as_string(parent) : NULL;
    if (parent_id && parent_id[0] != '\0') {
      next_id = turbo_runtime_history_strdup(parent_id);
      if (!next_id) goto cleanup;
    }
    if (chain) {
      if (turbo_runtime_json_array_append(chain, record) != 0) goto cleanup;
      record = NULL;
    } else {
      turbo_runtime_history_json_free(&record);
    }
    if (!next_id) {
      if (out_root_id) { *out_root_id = current_id; current_id = NULL; }
      if (out_chain) { *out_chain = chain; chain = NULL; }
      rc = 0;
      break;
    }
    free(current_id);
    current_id = next_id;
    next_id = NULL;
  }
cleanup:
  free(current_id);
  free(next_id);
  json_free(record);
  json_free(chain);
  json_free(seen);
  return rc;
}

int turbo_runtime_root_checkpoint_id(turbo_agent_runtime_t *runtime,
                                     const char *checkpoint_id, char **out_root_id) {
  if (!out_root_id) return -1;
  return turbo_runtime_checkpoint_chain(runtime, checkpoint_id, NULL, out_root_id);
}

static int turbo_runtime_checkpoint_sequence(const json_value_t *record, size_t *out_seq) {
  int64_t seq = turbo_runtime_json_value_as_int64(json_object_get(record, "seq"), -1);
  if (seq <= 0 || (uint64_t)seq > SIZE_MAX || seq > INT64_C(9007199254740991)) return -1;
  *out_seq = (size_t)seq;
  return 0;
}

json_value_t *turbo_runtime_checkpoint_records_sorted(const json_value_t *records) {
  const json_value_t **items = NULL;
  json_value_t *sorted = NULL;
  size_t i, count;
  if (!records || json_type(records) != JSON_ARRAY) return NULL;
  count = json_array_size(records);
  items = calloc(count ? count : 1, sizeof(*items));
  sorted = json_create_array();
  if (!items || !sorted) goto fail;
  for (i = 0; i < count; ++i) {
    size_t sequence;
    items[i] = json_array_get(records, i);
    if (turbo_runtime_checkpoint_sequence(items[i], &sequence) != 0) goto fail;
  }
  qsort(items, count, sizeof(*items), turbo_runtime_history_checkpoint_seq_compare);
  for (i = 0; i < count; ++i) {
    if (turbo_runtime_history_json_value_array_append_json(sorted, items[i]) != 0) goto fail;
  }
  free(items);
  return sorted;
fail:
  free(items);
  json_free(sorted);
  return NULL;
}

int turbo_runtime_checkpoint_record_read(
    const json_value_t *record, const char **out_thread_id, const char **out_run_id,
    const char **out_parent_checkpoint_id, size_t *out_seq,
    const json_value_t **out_checkpoint_json) {
  const char *thread_id, *run_id;
  const json_value_t *parent, *checkpoint_json;
  size_t seq;
  if (out_thread_id) *out_thread_id = NULL;
  if (out_run_id) *out_run_id = NULL;
  if (out_parent_checkpoint_id) *out_parent_checkpoint_id = NULL;
  if (out_seq) *out_seq = 0;
  if (out_checkpoint_json) *out_checkpoint_json = NULL;
  if (!record || !out_thread_id || !out_run_id || !out_parent_checkpoint_id ||
      !out_seq || !out_checkpoint_json || json_type(record) != JSON_OBJECT) return -1;
  thread_id = json_get_string(record, "thread_id");
  run_id = json_get_string(record, "run_id");
  parent = json_object_get(record, "parent_checkpoint_id");
  checkpoint_json = json_object_get(record, "checkpoint_json");
  if (!thread_id || !run_id || !checkpoint_json || json_type(checkpoint_json) != JSON_OBJECT ||
      (parent && json_type(parent) != JSON_NULL && json_type(parent) != JSON_STRING) ||
      turbo_runtime_checkpoint_sequence(record, &seq) != 0) return -1;
  *out_thread_id = thread_id;
  *out_run_id = run_id;
  *out_parent_checkpoint_id = parent ? turbo_runtime_json_value_as_string(parent) : NULL;
  *out_seq = seq;
  *out_checkpoint_json = checkpoint_json;
  return 0;
}

int turbo_runtime_checkpoint_from_json(const json_value_t *checkpoint_json,
    const json_value_t *state_override, turbo_graph_checkpoint_t **out_checkpoint) {
  json_value_t *copy;
  char *serialized;
  turbo_graph_exec_status_t status;
  if (out_checkpoint) *out_checkpoint = NULL;
  if (!checkpoint_json || !out_checkpoint || json_type(checkpoint_json) != JSON_OBJECT) return -1;
  copy = json_clone(checkpoint_json);
  if (!copy) return -1;
  if (state_override && turbo_runtime_json_object_set_clone(copy, "state", state_override) != 0) {
    json_free(copy);
    return -1;
  }
  serialized = json_serialize(copy, NULL);
  json_free(copy);
  if (!serialized) return -1;
  status = turbo_graph_checkpoint_deserialize(serialized, strlen(serialized), out_checkpoint);
  json_serialize_free(serialized);
  return status == TURBO_GRAPH_EXEC_OK ? 0 : -1;
}

CXX_C_API int
turbo_agent_runtime_load_history_events_json_value(turbo_agent_runtime_t *runtime,
                                                   const char *run_id, const char *checkpoint_id,
                                                   json_value_t **out_events_json_value) {
  if (out_events_json_value) *out_events_json_value = NULL;
  json_value_t *records_json = NULL;
  json_value_t *events_json_value = NULL;
  size_t i;
  size_t count;

  if (!runtime || !out_events_json_value || (!!run_id == !!checkpoint_id)) {
    return -1;
  }
  *out_events_json_value = NULL;
  events_json_value = json_create_array();
  if (!events_json_value) {
    return -1;
  }

  if (run_id) {
    json_value_t *sorted;
    if (turbo_agent_runtime_list_checkpoints(runtime, run_id, &records_json) != 0) goto cleanup;
    sorted = turbo_runtime_checkpoint_records_sorted(records_json);
    if (!sorted) goto cleanup;
    json_free(records_json);
    records_json = sorted;
    count = json_array_size(records_json);
    for (i = 0; i < count; ++i) {
      if (turbo_runtime_history_append_checkpoint_events_json_value(
              events_json_value, json_array_get(records_json, i)) != 0) goto cleanup;
    }
  } else {
    json_value_t *chain = NULL;
    if (turbo_runtime_checkpoint_chain(runtime, checkpoint_id, &chain, NULL) != 0) goto cleanup;
    count = json_array_size(chain);
    while (count > 0) {
      if (turbo_runtime_history_append_checkpoint_events_json_value(
              events_json_value, json_array_get(chain, --count)) != 0) {
        json_free(chain);
        goto cleanup;
      }
    }
    json_free(chain);
  }

  *out_events_json_value = events_json_value;
  events_json_value = NULL;

cleanup:
  turbo_runtime_json_destroy(events_json_value);
  turbo_runtime_history_json_free(&records_json);
  return *out_events_json_value ? 0 : -1;
}


CXX_C_API int turbo_agent_runtime_load_thread_history_events_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id, json_value_t **out_events_json_value) {
  if (out_events_json_value) *out_events_json_value = NULL;
  char *run_id = NULL;
  int rc;

  if (!runtime || !thread_id || !out_events_json_value) {
    return -1;
  }
  rc = turbo_agent_runtime_current_run_id_for_thread(runtime, thread_id, &run_id);
  if (rc != 0 || !run_id) {
    free(run_id);
    return -1;
  }
  rc = turbo_agent_runtime_load_history_events_json_value(runtime, run_id, NULL,
                                                          out_events_json_value);
  free(run_id);
  return rc;
}
