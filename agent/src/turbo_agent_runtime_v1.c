#include "turbo_agent_runtime.h"

#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_runtime_v1_internal.h"
#include "turbo_agent_state.h"
#include "turbo_agent_util_internal.h"
#include <json_parser.h>
#include "turbo_runtime_segment.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void turbo_agent_runtime_v1_json_free(json_value_t **value) {
  if (!value || !*value) {
    return;
  }
  json_free(*value);
  *value = NULL;
}

const char *const turbo_agent_runtime_threads_collection = "threads";
const char *const turbo_agent_runtime_runs_collection = "runs";
const char *const turbo_agent_runtime_checkpoints_collection = "checkpoints";

/* Forward declarations are in turbo_agent_runtime_v1_internal.h */
int turbo_agent_runtime_json_value_object_set_string(
    json_value_t *object, const char *key, const char *value) {
  return turbo_runtime_json_object_set_string(object, key, value) ==
                 TURBO_RUNTIME_JSON_OK ? 0 : -1;
}

int turbo_agent_runtime_json_value_object_set_int64(
    json_value_t *object, const char *key, int64_t value) {
  return turbo_runtime_json_object_set_int64(object, key, value) ==
                 TURBO_RUNTIME_JSON_OK ? 0 : -1;
}

int turbo_agent_runtime_json_value_object_set_clone(
    json_value_t *object, const char *key, const json_value_t *value) {
  return turbo_runtime_json_object_set_clone(object, key, value) ==
                 TURBO_RUNTIME_JSON_OK ? 0 : -1;
}

int turbo_agent_runtime_json_value_object_set_optional_string(json_value_t *object, const char *key,
                                                              const json_value_t *value) {
  const char *text = turbo_runtime_json_value_as_string(value);

  if (!text) {
    return 0;
  }
  return turbo_agent_runtime_json_value_object_set_string(object, key, text);
}

static const char *turbo_agent_runtime_observer_type_for_trace_name(const char *name) {
  if (!name || name[0] == '\0') {
    return NULL;
  }
  if (strcmp(name, "model_request") == 0 || strcmp(name, "model_response") == 0) {
    return "model_delta";
  }
  if (strcmp(name, "tool_dispatch") == 0) {
    return "tool_call_started";
  }
  if (strcmp(name, "tool_result") == 0) {
    return "tool_result";
  }
  if (strcmp(name, "structured_retry") == 0 || strcmp(name, "replan_requested") == 0 ||
      strcmp(name, "review_required") == 0 || strcmp(name, "review_approved") == 0 ||
      strcmp(name, "guardrail_rejected") == 0 || strcmp(name, "memory_load") == 0 ||
      strcmp(name, "memory_save") == 0) {
    return "state_updated";
  }
  return NULL;
}

static int turbo_agent_runtime_observer_event_from_json_value(const json_value_t *raw_event,
                                                              json_value_t **out_observer_event) {
  const json_value_t *kind_value;
  const json_value_t *type_value;
  const char *kind;
  const char *type;
  const char *observer_type = NULL;
  json_value_t *observer_event = NULL;

  if (!raw_event || !out_observer_event) {
    return -1;
  }
  *out_observer_event = NULL;

  kind_value = json_object_get(raw_event, "kind");
  kind = turbo_runtime_json_value_as_string(kind_value);
  if (kind && strcmp(kind, "model") == 0) {
    observer_type = "model_delta";
  } else if (kind && strcmp(kind, "tool_result") == 0) {
    observer_type = "tool_result";
  } else if (kind && strcmp(kind, "trace") == 0) {
    observer_type = turbo_agent_runtime_observer_type_for_trace_name(
        turbo_runtime_json_value_as_string(json_object_get(raw_event, "name")));
  } else {
    type_value = json_object_get(raw_event, "type");
    type = turbo_runtime_json_value_as_string(type_value);
    if (type && strcmp(type, "interrupted") == 0) {
      observer_type = "interrupted";
    } else if (type && strcmp(type, "completed") == 0) {
      observer_type = "completed";
    } else if (type && type[0] != '\0') {
      observer_type = "state_updated";
    }
  }

  if (!observer_type) {
    observer_type = "state_updated";
  }

  observer_event = json_create_object();
  if (!observer_event ||
      turbo_agent_runtime_json_value_object_set_string(observer_event, "kind", "observer") != 0 ||
      turbo_agent_runtime_json_value_object_set_string(observer_event, "type", observer_type) !=
          0 ||
      turbo_agent_runtime_json_value_object_set_clone(observer_event, "event", raw_event) != 0) {
    turbo_runtime_json_destroy(observer_event);
    return -1;
  }

  if (kind && strcmp(kind, "trace") == 0) {
    const json_value_t *status_value = json_object_get(raw_event, "status");

    if (turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "name", json_object_get(raw_event, "name")) != 0 ||
        turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "detail", json_object_get(raw_event, "detail")) != 0 ||
        turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "payload", json_object_get(raw_event, "payload")) != 0) {
      turbo_runtime_json_destroy(observer_event);
      return -1;
    }
    if (status_value &&
        turbo_agent_runtime_json_value_object_set_int64(
            observer_event, "status", turbo_runtime_json_value_as_int64(status_value, 0)) != 0) {
      turbo_runtime_json_destroy(observer_event);
      return -1;
    }
  } else if (kind && strcmp(kind, "model") == 0) {
    if (turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "response_id", json_object_get(raw_event, "response_id")) != 0 ||
        turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "output_text", json_object_get(raw_event, "output_text")) != 0) {
      turbo_runtime_json_destroy(observer_event);
      return -1;
    }
    if (json_object_get(raw_event, "tool_calls") &&
        turbo_agent_runtime_json_value_object_set_clone(
            observer_event, "tool_calls", json_object_get(raw_event, "tool_calls")) != 0) {
      turbo_runtime_json_destroy(observer_event);
      return -1;
    }
  } else if (kind && strcmp(kind, "tool_result") == 0) {
    const json_value_t *status_value = json_object_get(raw_event, "status");

    if (turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "name", json_object_get(raw_event, "name")) != 0 ||
        turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "arguments_json", json_object_get(raw_event, "arguments_json")) !=
            0 ||
        turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "output", json_object_get(raw_event, "output")) != 0) {
      turbo_runtime_json_destroy(observer_event);
      return -1;
    }
    if (status_value &&
        turbo_agent_runtime_json_value_object_set_int64(
            observer_event, "status", turbo_runtime_json_value_as_int64(status_value, 0)) != 0) {
      turbo_runtime_json_destroy(observer_event);
      return -1;
    }
  } else {
    if (turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "last_node", json_object_get(raw_event, "last_node")) != 0 ||
        turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "next_node", json_object_get(raw_event, "next_node")) != 0) {
      turbo_runtime_json_destroy(observer_event);
      return -1;
    }
    if (json_object_get(raw_event, "steps") &&
        turbo_agent_runtime_json_value_object_set_int64(
            observer_event, "steps",
            turbo_runtime_json_value_as_int64(json_object_get(raw_event, "steps"), 0)) != 0) {
      turbo_runtime_json_destroy(observer_event);
      return -1;
    }
  }

  *out_observer_event = observer_event;
  return 1;
}

static int
turbo_agent_runtime_events_json_value_has_terminal_type(const json_value_t *events_json_value,
                                                        const char *terminal_type) {
  size_t i;
  size_t count;

  if (!events_json_value || !terminal_type ||
      json_type(events_json_value) != JSON_ARRAY) {
    return 0;
  }
  count = turbo_runtime_json_value_size(events_json_value);
  for (i = 0; i < count; ++i) {
    const json_value_t *raw_event = json_array_get(events_json_value, i);
    const char *type;

    if (!raw_event) {
      continue;
    }
    type = turbo_runtime_json_value_as_string(json_object_get(raw_event, "type"));
    if (type && strcmp(type, terminal_type) == 0) {
      return 1;
    }
  }
  return 0;
}

static int
turbo_agent_runtime_observer_terminal_event_from_record_json(const json_value_t *record_json,
                                                             json_value_t **out_observer_event) {
  const char *status;
  json_value_t *record_json_value = NULL;
  json_value_t *observer_event = NULL;

  if (!record_json || !out_observer_event) {
    return -1;
  }
  *out_observer_event = NULL;
  status = json_get_string(record_json, "status");
  if (!status || (strcmp(status, "interrupted") != 0 && strcmp(status, "completed") != 0)) {
    return 0;
  }
  record_json_value = json_clone(record_json);
  if (!record_json_value) {
    return -1;
  }
  observer_event = json_create_object();
  if (!observer_event ||
      turbo_agent_runtime_json_value_object_set_string(observer_event, "kind", "observer") != 0 ||
      turbo_agent_runtime_json_value_object_set_string(observer_event, "type", status) != 0 ||
      turbo_agent_runtime_json_value_object_set_clone(observer_event, "event", record_json_value) !=
          0) {
    turbo_runtime_json_destroy(record_json_value);
    turbo_runtime_json_destroy(observer_event);
    return -1;
  }
  turbo_runtime_json_destroy(record_json_value);
  *out_observer_event = observer_event;
  return 1;
}

static int turbo_agent_runtime_observe_terminal_record_json(
    const json_value_t *events_json_value, const json_value_t *record_json,
    const turbo_agent_observer_json_value_sink_t *sink) {
  json_value_t *observer_event = NULL;
  int rc;

  if (!record_json || !sink || !sink->callback) {
    return -1;
  }
  rc = turbo_agent_runtime_observer_terminal_event_from_record_json(record_json, &observer_event);
  if (rc <= 0) {
    turbo_runtime_json_destroy(observer_event);
    return rc;
  }
  if (!events_json_value ||
      !turbo_agent_runtime_events_json_value_has_terminal_type(
          events_json_value,
          turbo_runtime_json_value_as_string(json_object_get(observer_event, "type")))) {
    sink->callback(observer_event, sink->user_data);
  }
  turbo_runtime_json_destroy(observer_event);
  return 1;
}

char *turbo_agent_runtime_strdup(const char *text) {
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

const char *turbo_agent_runtime_status_text(turbo_graph_exec_status_t status) {
  return turbo_runtime_execution_status_text(status);
}

int turbo_agent_runtime_result_to_json(const turbo_graph_run_result_t *result,
                                      json_value_t **out_result) {
  return turbo_runtime_execution_result_json(result, out_result);
}

char *turbo_agent_runtime_make_id(const char *prefix) {
  return turbo_runtime_record_id(prefix);
}

int turbo_agent_runtime_parse_json_string(const char *json_text, json_value_t **out_json) {
  json_value_t *json_root = NULL;

  if (!json_text || !out_json) {
    return -1;
  }

  *out_json = NULL;
  json_root = json_parse(json_text, strlen(json_text));
  if (!json_root) {
    turbo_agent_runtime_v1_json_free(&json_root);
    return -1;
  }
  *out_json = json_root;
  return 0;
}

static int turbo_agent_runtime_json_value_array_append_json(json_value_t *array_json_value,
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

static int turbo_agent_runtime_capture_snapshots(const json_value_t *state,
                                                 json_value_t **out_control_snapshot,
                                                 json_value_t **out_workflow_snapshot) {
  json_value_t *control_json_value;
  json_value_t *workflow_json_value;

  if (!state || !out_control_snapshot || !out_workflow_snapshot) {
    return -1;
  }
  *out_control_snapshot = NULL;
  *out_workflow_snapshot = NULL;

  control_json_value = turbo_agent_state_control_snapshot_json_value(state);
  workflow_json_value = turbo_agent_state_workflow_snapshot_json_value(state);
  if (!control_json_value || !workflow_json_value) {
    turbo_runtime_json_destroy(control_json_value);
    turbo_runtime_json_destroy(workflow_json_value);
    return -1;
  }

  *out_control_snapshot = json_clone(control_json_value);
  *out_workflow_snapshot = json_clone(workflow_json_value);
  turbo_runtime_json_destroy(control_json_value);
  turbo_runtime_json_destroy(workflow_json_value);
  if (!*out_control_snapshot || !*out_workflow_snapshot) {
    turbo_agent_runtime_v1_json_free(out_control_snapshot);
    turbo_agent_runtime_v1_json_free(out_workflow_snapshot);
    return -1;
  }
  return 0;
}

typedef struct turbo_agent_segment_scope_s {
  turbo_agent_execution_context_t saved;
} turbo_agent_segment_scope_t;

static void turbo_agent_segment_enter(const turbo_runtime_segment_t *segment, void *user_data) {
  turbo_agent_segment_scope_t *scope = user_data;
  turbo_agent_execution_context_t current;
  turbo_agent_execution_context_get(&scope->saved);
  current = scope->saved;
  current.thread_id = segment->thread_id;
  current.run_id = segment->run_id;
  current.runtime = segment->runtime;
  current.cancel_token = segment->cancel_token;
  turbo_agent_execution_context_set(&current);
}

static void turbo_agent_segment_leave(const turbo_runtime_segment_t *segment, void *user_data) {
  turbo_agent_segment_scope_t *scope = user_data;
  (void)segment;
  turbo_agent_execution_context_set(&scope->saved);
}

static int turbo_agent_segment_snapshots(const json_value_t *state, json_value_t **control,
                                       json_value_t **workflow, void *user_data) {
  (void)user_data;
  return turbo_agent_runtime_capture_snapshots(state, control, workflow);
}

static int turbo_agent_segment_summary(const turbo_runtime_segment_t *segment,
    const char *checkpoint_id, const turbo_graph_run_result_t *result,
    const json_value_t *state, json_value_t **summary, void *user_data) {
  (void)user_data;
  return turbo_agent_runtime_make_summary(segment->thread_id, segment->run_id,
      turbo_runtime_execution_status_text(result->status), checkpoint_id,
      segment->parent_agent_run_id, segment->parent_tool_call_id, segment->parent_tool_name,
      segment->parent_graph_run_id, segment->call_frame_id, result, state, summary);
}

static int turbo_agent_runtime_run_segment(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *thread_id, const char *run_id,
    const char *parent_run_id, const char *parent_agent_run_id, const char *parent_tool_call_id,
    const char *parent_tool_name, const char *parent_graph_run_id, const char *call_frame_id,
    const char *forked_from_checkpoint_id, const char *parent_checkpoint_id,
    size_t next_checkpoint_seq, const char *run_created_at, const json_value_t *start_state,
    const turbo_graph_checkpoint_t *start_checkpoint, const turbo_graph_run_options_t *options,
    const turbo_cancel_token_t *cancel_token, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data, json_value_t **out_summary_json, json_value_t **out_state) {
  turbo_runtime_segment_t segment;
  turbo_agent_segment_scope_t scope = {0};
  turbo_runtime_segment_init(&segment);
  segment.runtime = runtime;
  segment.graph = graph;
  segment.thread_id = thread_id;
  segment.run_id = run_id;
  segment.parent_run_id = parent_run_id;
  segment.parent_agent_run_id = parent_agent_run_id;
  segment.parent_tool_call_id = parent_tool_call_id;
  segment.parent_tool_name = parent_tool_name;
  segment.parent_graph_run_id = parent_graph_run_id;
  segment.call_frame_id = call_frame_id;
  segment.forked_from_checkpoint_id = forked_from_checkpoint_id;
  segment.parent_checkpoint_id = parent_checkpoint_id;
  segment.run_created_at = run_created_at;
  segment.next_checkpoint_seq = next_checkpoint_seq;
  segment.start_state = start_state;
  segment.start_checkpoint = start_checkpoint;
  segment.options = options;
  segment.cancel_token = cancel_token;
  segment.event_sink = event_sink;
  segment.event_sink_user_data = event_sink_user_data;
  segment.snapshot = turbo_agent_segment_snapshots;
  segment.summary = turbo_agent_segment_summary;
  segment.enter = turbo_agent_segment_enter;
  segment.leave = turbo_agent_segment_leave;
  segment.user_data = &scope;
  return turbo_runtime_segment_execute(&segment, out_summary_json, out_state);
}

int turbo_agent_runtime_record_string_compare(const char *left, const char *right) {
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

static int turbo_agent_runtime_observability_summary_thread_id_compare_asc(const void *left,
                                                                           const void *right) {
  const json_value_t *a = *(const json_value_t *const *)left;
  const json_value_t *b = *(const json_value_t *const *)right;
  const json_value_t *a_thread_json = json_object_get(a, "thread");
  const json_value_t *b_thread_json = json_object_get(b, "thread");
  const char *a_thread_id = (a_thread_json && json_type(a_thread_json) == JSON_OBJECT)
                                ? json_get_string(a_thread_json, "id")
                                : NULL;
  const char *b_thread_id = (b_thread_json && json_type(b_thread_json) == JSON_OBJECT)
                                ? json_get_string(b_thread_json, "id")
                                : NULL;

  return turbo_agent_runtime_record_string_compare(a_thread_id, b_thread_id);
}

static int turbo_agent_runtime_observability_summary_thread_id_compare_desc(const void *left,
                                                                            const void *right) {
  return turbo_agent_runtime_observability_summary_thread_id_compare_asc(right, left);
}

static int
turbo_agent_runtime_observability_summary_latest_run_updated_at_compare_asc(const void *left,
                                                                            const void *right) {
  const json_value_t *a = *(const json_value_t *const *)left;
  const json_value_t *b = *(const json_value_t *const *)right;
  const char *a_updated_at = json_get_string(a, "latest_run_updated_at");
  const char *b_updated_at = json_get_string(b, "latest_run_updated_at");
  int rc = turbo_agent_runtime_record_string_compare(a_updated_at, b_updated_at);

  if (rc != 0) {
    return rc;
  }
  return turbo_agent_runtime_observability_summary_thread_id_compare_asc(left, right);
}

static int
turbo_agent_runtime_observability_summary_latest_run_updated_at_compare_desc(const void *left,
                                                                             const void *right) {
  return turbo_agent_runtime_observability_summary_latest_run_updated_at_compare_asc(right, left);
}

int turbo_agent_runtime_run_updated_at_compare_desc(const void *left, const void *right) {
  const json_value_t *a = *(const json_value_t *const *)left;
  const json_value_t *b = *(const json_value_t *const *)right;
  const char *a_updated_at = json_get_string(a, "updated_at");
  const char *b_updated_at = json_get_string(b, "updated_at");
  int rc = turbo_agent_runtime_record_string_compare(b_updated_at, a_updated_at);

  if (rc != 0) {
    return rc;
  }
  return turbo_agent_runtime_record_string_compare(json_get_string(a, "id"),
                                                   json_get_string(b, "id"));
}

json_value_t *turbo_agent_runtime_sorted_json_array_clone(const json_value_t *records_json,
                                                          int (*compare)(const void *,
                                                                         const void *)) {
  json_value_t *sorted_json = NULL;
  const json_value_t **items = NULL;
  size_t count;
  size_t i;

  if (!records_json || json_type(records_json) != JSON_ARRAY || !compare) {
    return NULL;
  }
  sorted_json = json_create_array();
  if (!sorted_json) {
    return NULL;
  }
  count = json_array_size(records_json);
  if (count == 0) {
    return sorted_json;
  }
  items = (const json_value_t **)calloc(count, sizeof(*items));
  if (!items) {
    turbo_agent_runtime_v1_json_free(&sorted_json);
    return NULL;
  }
  for (i = 0; i < count; ++i) {
    items[i] = json_array_get(records_json, i);
  }
  qsort(items, count, sizeof(*items), compare);
  for (i = 0; i < count; ++i) {
    json_value_t *clone = json_clone(items[i]);
    if (!clone) {
      free(items);
      turbo_agent_runtime_v1_json_free(&sorted_json);
      return NULL;
    }
    json_array_add(sorted_json, clone);
  }
  free(items);
  return sorted_json;
}

static json_value_t *
turbo_agent_runtime_checkpoint_summary_from_record(const json_value_t *checkpoint_record) {
  json_value_t *summary;
  const char *string_fields[] = {"id",     "run_id",     "parent_checkpoint_id",
                                 "status", "created_at", "next_node"};
  const char *number_fields[] = {"seq", "steps"};
  size_t i;

  if (!checkpoint_record || json_type(checkpoint_record) != JSON_OBJECT) {
    return NULL;
  }
  summary = json_create_object();
  if (!summary) {
    return NULL;
  }

  for (i = 0; i < sizeof(string_fields) / sizeof(string_fields[0]); ++i) {
    const char *value = json_get_string(checkpoint_record, string_fields[i]);

    if (value && value[0] != '\0') {
      json_object_set_string(summary, string_fields[i], value);
    } else {
      json_object_set_null(summary, string_fields[i]);
    }
  }
  for (i = 0; i < sizeof(number_fields) / sizeof(number_fields[0]); ++i) {
    const json_value_t *value = json_object_get(checkpoint_record, number_fields[i]);

    if (value && json_type(value) == JSON_NUMBER) {
      json_object_set_number(summary, number_fields[i],
                                   json_get_double(checkpoint_record, number_fields[i], 0));
    } else {
      json_object_set_null(summary, number_fields[i]);
    }
  }

  return summary;
}

static int turbo_agent_runtime_load_checkpoint_summary_by_id(turbo_agent_runtime_t *runtime,
                                                             const char *checkpoint_id,
                                                             json_value_t **out_summary) {
  json_value_t *checkpoint_json = NULL;
  json_value_t *summary = NULL;

  /* Host-facing inspect views reuse one stable lightweight checkpoint summary. */
  if (!runtime || !out_summary) {
    return -1;
  }
  *out_summary = NULL;
  if (!checkpoint_id || checkpoint_id[0] == '\0') {
    return 0;
  }
  if (turbo_agent_runtime_get_checkpoint(runtime, checkpoint_id, &checkpoint_json) != 0) {
    goto cleanup;
  }
  summary = turbo_agent_runtime_checkpoint_summary_from_record(checkpoint_json);
  if (!summary) {
    goto cleanup;
  }
  *out_summary = summary;
  summary = NULL;

cleanup:
  turbo_agent_runtime_v1_json_free(&summary);
  turbo_agent_runtime_v1_json_free(&checkpoint_json);
  return *out_summary || !checkpoint_id || checkpoint_id[0] == '\0' ? 0 : -1;
}

static int turbo_agent_runtime_attach_checkpoint_summary(turbo_agent_runtime_t *runtime,
                                                         json_value_t *object,
                                                         const char *field_name,
                                                         const char *checkpoint_id) {
  json_value_t *summary = NULL;

  if (!runtime || !object || !field_name) {
    return -1;
  }
  if (!checkpoint_id || checkpoint_id[0] == '\0') {
    json_object_set_null(object, field_name);
    return 0;
  }
  if (turbo_agent_runtime_load_checkpoint_summary_by_id(runtime, checkpoint_id, &summary) != 0) {
    return -1;
  }
  if (summary) {
    json_object_add(object, field_name, summary);
  } else {
    json_object_set_null(object, field_name);
  }
  return 0;
}

static int turbo_agent_runtime_branch_from_run_record(turbo_agent_runtime_t *runtime,
                                                      const json_value_t *run_record,
                                                      json_value_t **out_branch_json,
                                                      char **out_root_checkpoint_id) {
  json_value_t *branch_json = NULL;
  json_value_t *checkpoint_record = NULL;
  const char *run_id;
  const char *checkpoint_id;
  const char *parent_run_id;
  const char *forked_from_checkpoint_id;
  const char *parent_checkpoint_id = NULL;
  const char *status;
  const char *updated_at;
  const char *created_at;
  char *root_checkpoint_id = NULL;
  const char *root_source_checkpoint_id = NULL;

  if (!runtime || !run_record || !out_branch_json || !out_root_checkpoint_id) {
    return -1;
  }
  *out_branch_json = NULL;
  *out_root_checkpoint_id = NULL;

  run_id = json_get_string(run_record, "id");
  if (!run_id || run_id[0] == '\0') {
    return -1;
  }
  checkpoint_id = json_get_string(run_record, "latest_checkpoint_id");
  parent_run_id = json_get_string(run_record, "parent_run_id");
  forked_from_checkpoint_id = json_get_string(run_record, "forked_from_checkpoint_id");
  status = json_get_string(run_record, "status");
  updated_at = json_get_string(run_record, "updated_at");
  created_at = json_get_string(run_record, "created_at");

  if (checkpoint_id && checkpoint_id[0] != '\0') {
    if (turbo_agent_runtime_get_checkpoint(runtime, checkpoint_id, &checkpoint_record) != 0 ||
        !checkpoint_record) {
      goto cleanup;
    }
    root_source_checkpoint_id = checkpoint_id;
  } else if (forked_from_checkpoint_id && forked_from_checkpoint_id[0] != '\0') {
    root_source_checkpoint_id = forked_from_checkpoint_id;
  }

  if (forked_from_checkpoint_id && forked_from_checkpoint_id[0] != '\0') {
    parent_checkpoint_id = forked_from_checkpoint_id;
  }

  if (root_source_checkpoint_id &&
      turbo_runtime_root_checkpoint_id(runtime, root_source_checkpoint_id,
                                                            &root_checkpoint_id) != 0) {
    goto cleanup;
  }

  branch_json = json_create_object();
  if (!branch_json) {
    goto cleanup;
  }
  json_object_set_string(branch_json, "run_id", run_id);
  if (checkpoint_id && checkpoint_id[0] != '\0') {
    json_object_set_string(branch_json, "checkpoint_id", checkpoint_id);
  } else {
    json_object_set_null(branch_json, "checkpoint_id");
  }
  if (parent_checkpoint_id && parent_checkpoint_id[0] != '\0') {
    json_object_set_string(branch_json, "parent_checkpoint_id", parent_checkpoint_id);
  } else {
    json_object_set_null(branch_json, "parent_checkpoint_id");
  }
  if (parent_run_id && parent_run_id[0] != '\0') {
    json_object_set_string(branch_json, "parent_run_id", parent_run_id);
  } else {
    json_object_set_null(branch_json, "parent_run_id");
  }
  if (forked_from_checkpoint_id && forked_from_checkpoint_id[0] != '\0') {
    json_object_set_string(branch_json, "forked_from_checkpoint_id",
                                 forked_from_checkpoint_id);
  } else {
    json_object_set_null(branch_json, "forked_from_checkpoint_id");
  }
  if (status && status[0] != '\0') {
    json_object_set_string(branch_json, "status", status);
  } else {
    json_object_set_null(branch_json, "status");
  }
  if (updated_at && updated_at[0] != '\0') {
    json_object_set_string(branch_json, "updated_at", updated_at);
  } else {
    json_object_set_null(branch_json, "updated_at");
  }
  if (root_checkpoint_id && root_checkpoint_id[0] != '\0') {
    json_object_set_string(branch_json, "branch_root_checkpoint_id", root_checkpoint_id);
  } else {
    json_object_set_null(branch_json, "branch_root_checkpoint_id");
  }
  if (created_at && created_at[0] != '\0') {
    json_object_set_string(branch_json, "created_at", created_at);
  } else {
    json_object_set_null(branch_json, "created_at");
  }

  *out_branch_json = branch_json;
  branch_json = NULL;
  *out_root_checkpoint_id = root_checkpoint_id;
  root_checkpoint_id = NULL;

cleanup:
  free(root_checkpoint_id);
  turbo_agent_runtime_v1_json_free(&checkpoint_record);
  turbo_agent_runtime_v1_json_free(&branch_json);
  return *out_branch_json ? 0 : -1;
}

static int turbo_agent_runtime_history_events_json_for_run(turbo_agent_runtime_t *runtime,
                                                           const char *run_id,
                                                           json_value_t **out_events_json) {
  json_value_t *events_json_value = NULL;
  json_value_t *events_json = NULL;

  if (!runtime || !run_id || !out_events_json) {
    return -1;
  }
  *out_events_json = NULL;
  if (turbo_agent_runtime_load_history_events_json_value(runtime, run_id, NULL,
                                                         &events_json_value) != 0 ||
      !events_json_value) {
    return -1;
  }
  events_json = json_clone(events_json_value);
  turbo_runtime_json_destroy(events_json_value);
  if (!events_json) {
    return -1;
  }
  *out_events_json = events_json;
  return 0;
}

static const char *turbo_agent_runtime_command_string(const json_value_t *command_json,
                                                      const char *primary_key) {
  const char *value;

  if (!command_json) {
    return NULL;
  }
  value = json_get_string(command_json, primary_key);
  if (value && value[0] != '\0') {
    return value;
  }
  return NULL;
}

static char *turbo_agent_runtime_command_text_owned(const json_value_t *command_json,
                                                    const char *primary_key) {
  const char *text_value;
  const json_value_t *json_value;
  char *serialized = NULL;
  char *owned_value = NULL;

  if (!command_json) {
    return NULL;
  }
  text_value = turbo_agent_runtime_command_string(command_json, primary_key);
  if (text_value && text_value[0] != '\0') {
    return turbo_agent_runtime_strdup(text_value);
  }

  json_value = primary_key ? json_object_get(command_json, primary_key) : NULL;
  if (!json_value || json_type(json_value) == JSON_NULL) {
    return NULL;
  }
  if (json_type(json_value) == JSON_STRING) {
    text_value = json_string(json_value);
    return text_value ? turbo_agent_runtime_strdup(text_value) : NULL;
  }
  serialized = json_serialize(json_value, NULL);
  if (!serialized) {
    return NULL;
  }
  owned_value = turbo_agent_runtime_strdup(serialized);
  json_serialize_free(serialized);
  return owned_value;
}

static int
turbo_agent_runtime_merge_state_patch_object_json_value(json_value_t *target_object,
                                                        const json_value_t *patch_object) {
  size_t i;
  size_t count;

  if (!target_object || !patch_object || json_type(target_object) != JSON_OBJECT ||
      json_type(patch_object) != JSON_OBJECT) {
    return -1;
  }

  count = turbo_runtime_json_value_size(patch_object);
  for (i = 0; i < count; ++i) {
    const char *key = json_object_key(patch_object, i);
    const json_value_t *patch_value;
    const json_value_t *target_value;
    json_value_t *copy;

    if (!key) {
      return -1;
    }
    patch_value = json_object_get(patch_object, key);
    if (!patch_value) {
      return -1;
    }
    target_value = json_object_get(target_object, key);
    if (target_value && json_type(patch_value) == JSON_OBJECT &&
        json_type(target_value) == JSON_OBJECT) {
      if (turbo_agent_runtime_merge_state_patch_object_json_value((json_value_t *)target_value,
                                                                  patch_value) != 0) {
        return -1;
      }
      continue;
    }
    copy = json_clone(patch_value);
    if (!copy) {
      return -1;
    }
    if (turbo_runtime_json_object_set(target_object, key, copy) != TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_json_destroy(copy);
      return -1;
    }
  }
  return 0;
}

static int turbo_agent_runtime_apply_state_patch_to_json_value(const json_value_t *state,
                                                               const json_value_t *patch,
                                                               json_value_t **out_state_override) {
  json_value_t *updated_state = NULL;
  json_type_t state_kind;
  json_type_t patch_kind;
  int rc = -1;

  if (!state || !patch || !out_state_override) {
    return -1;
  }
  *out_state_override = NULL;

  state_kind = json_type(state);
  patch_kind = json_type(patch);
  if (state_kind == JSON_OBJECT && patch_kind == JSON_OBJECT) {
    updated_state = json_clone(state);
    if (!updated_state) {
      return -1;
    }
    if (turbo_agent_runtime_merge_state_patch_object_json_value(updated_state, patch) != 0) {
      goto cleanup;
    }
  } else {
    updated_state = json_clone(patch);
    if (!updated_state) {
      goto cleanup;
    }
  }

  *out_state_override = updated_state;
  updated_state = NULL;
  rc = 0;

cleanup:
  turbo_runtime_json_destroy(updated_state);
  return rc;
}

static int turbo_agent_runtime_apply_command_json(json_value_t *state,
                                                  const json_value_t *command_json) {
  const char *kind;
  const json_value_t *approved_value;
  int approved;
  const char *text;
  char *owned_text = NULL;
  int rc = -1;

  if (!state || !command_json || json_type(command_json) != JSON_OBJECT) {
    return -1;
  }
  kind = json_get_string(command_json, "kind");
  if (!kind || kind[0] == '\0') {
    return -1;
  }

  if (strcmp(kind, "approve_review") == 0) {
    approved_value = json_object_get(command_json, "approved");
    approved = approved_value ? json_get_bool(command_json, "approved", 1) : 1;
    return turbo_agent_state_set_review_approved(state, approved);
  }
  if (strcmp(kind, "reject_review") == 0) {
    owned_text = turbo_agent_runtime_command_text_owned(command_json, "reason");
    if (owned_text && owned_text[0] != '\0') {
      rc = turbo_agent_state_request_review(state, owned_text);
      free(owned_text);
      return rc;
    }
    free(owned_text);
    return turbo_agent_state_set_review_approved(state, 0);
  }
  if (strcmp(kind, "request_replan") == 0) {
    text = turbo_agent_runtime_command_string(command_json, "reason");
    rc = turbo_agent_state_clear_review(state);
    if (rc != 0) {
      return rc;
    }
    return turbo_agent_state_request_replan(state, text);
  }
  if (strcmp(kind, "append_feedback") == 0) {
    owned_text = turbo_agent_runtime_command_text_owned(command_json, "text");
    if (!owned_text || owned_text[0] == '\0') {
      free(owned_text);
      return -1;
    }
    rc = turbo_agent_state_add_user_message(state, owned_text);
    free(owned_text);
    return rc;
  }
  if (strcmp(kind, "append_user_message") == 0) {
    owned_text = turbo_agent_runtime_command_text_owned(command_json, "text");
    if (!owned_text || owned_text[0] == '\0') {
      free(owned_text);
      return -1;
    }
    rc = turbo_agent_state_add_user_message(state, owned_text);
    free(owned_text);
    return rc;
  }
  if (strcmp(kind, "override_final_output") == 0) {
    owned_text = turbo_agent_runtime_command_text_owned(command_json, "text");
    if (!owned_text || owned_text[0] == '\0') {
      free(owned_text);
      owned_text = turbo_agent_runtime_command_text_owned(command_json, "output_json");
    }
    if (!owned_text || owned_text[0] == '\0') {
      free(owned_text);
      return -1;
    }
    rc = turbo_agent_state_set_final_answer(state, owned_text);
    free(owned_text);
    return rc;
  }
  return -1;
}

static int turbo_agent_runtime_run_command_json_value(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state, int fork_run) {
  json_value_t *state_override = NULL;
  int rc;

  if (!runtime || !graph || !checkpoint_id || !command || !out_summary_json || !out_state) {
    return -1;
  }
  rc = turbo_agent_runtime_prepare_checkpoint_command_override_json_value(runtime, checkpoint_id,
                                                                          command, &state_override);
  if (rc != 0) {
    return rc;
  }
  if (fork_run) {
    rc = turbo_agent_runtime_fork_json_value_graph_stream(runtime, graph, checkpoint_id,
                                                          state_override, options, NULL, NULL,
                                                          out_summary_json, out_state);
  } else {
    rc = turbo_agent_runtime_resume_json_value_graph_stream(runtime, graph, checkpoint_id,
                                                            state_override, options, NULL, NULL,
                                                            out_summary_json, out_state);
  }
  turbo_runtime_json_destroy(state_override);
  return rc;
}

int turbo_agent_runtime_start_json_value_graph_linked_stream(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const json_value_t *state,
    const turbo_graph_run_options_t *options, const char *thread_id,
    const turbo_agent_runtime_parent_link_t *parent_link, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data, json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_runtime_start_json_value_graph_linked_stream_controlled(
      runtime, graph, state, options, thread_id, parent_link, NULL, event_sink,
      event_sink_user_data, out_summary_json, out_state);
}

int turbo_agent_runtime_start_json_value_graph_linked_stream_controlled(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const json_value_t *state,
    const turbo_graph_run_options_t *options, const char *thread_id,
    const turbo_agent_runtime_parent_link_t *parent_link, const turbo_cancel_token_t *cancel_token,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  char *owned_thread_id = NULL;
  char *run_id = NULL;
  int rc;

  if (!runtime || !graph || !state || !out_summary_json || !out_state) {
    return -1;
  }
  if (thread_id && thread_id[0] != '\0') {
    owned_thread_id = turbo_agent_runtime_strdup(thread_id);
  } else {
    owned_thread_id = turbo_agent_runtime_make_id("thr");
  }
  run_id = turbo_agent_runtime_make_id("run");
  if (!owned_thread_id || !run_id) {
    free(owned_thread_id);
    free(run_id);
    return -1;
  }
  rc = turbo_agent_runtime_run_segment(runtime, graph, owned_thread_id, run_id, NULL,
                                       parent_link ? parent_link->parent_agent_run_id : NULL,
                                       parent_link ? parent_link->parent_tool_call_id : NULL,
                                       parent_link ? parent_link->parent_tool_name : NULL,
                                       parent_link ? parent_link->parent_graph_run_id : NULL,
                                       parent_link ? parent_link->call_frame_id : NULL, NULL, NULL,
                                       1, NULL, state, NULL, options, cancel_token, event_sink,
                                       event_sink_user_data, out_summary_json, out_state);
  free(owned_thread_id);
  free(run_id);
  return rc;
}

int turbo_agent_runtime_resume_json_value_graph_stream(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *state_override, const turbo_graph_run_options_t *options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_runtime_resume_json_value_graph_stream_controlled(
      runtime, graph, checkpoint_id, state_override, options, NULL, event_sink,
      event_sink_user_data, out_summary_json, out_state);
}

int turbo_agent_runtime_resume_json_value_graph_stream_controlled(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *state_override, const turbo_graph_run_options_t *options,
    const turbo_cancel_token_t *cancel_token, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data, json_value_t **out_summary_json, json_value_t **out_state) {
  json_value_t *checkpoint_record = NULL;
  json_value_t *run_record = NULL;
  const char *thread_id;
  const char *run_id;
  const char *parent_checkpoint_id;
  const char *created_at;
  size_t seq = 0;
  const json_value_t *checkpoint_json;
  turbo_graph_checkpoint_t *checkpoint = NULL;
  int rc = -1;

  if (!runtime || !graph || !checkpoint_id || !out_summary_json || !out_state) {
    return -1;
  }
  if (turbo_agent_runtime_store_get_json(runtime, turbo_agent_runtime_checkpoints_collection,
                                         checkpoint_id, &checkpoint_record) != 0 ||
      turbo_runtime_checkpoint_record_read(checkpoint_record, &thread_id, &run_id,
                                                    &parent_checkpoint_id, &seq,
                                                    &checkpoint_json) != 0 ||
      turbo_agent_runtime_store_get_json(runtime, turbo_agent_runtime_runs_collection, run_id,
                                         &run_record) != 0) {
    goto cleanup;
  }
  created_at = json_get_string(run_record, "created_at");
  if (!created_at ||
      turbo_runtime_checkpoint_from_json(checkpoint_json, state_override, &checkpoint) != 0) {
    goto cleanup;
  }
  rc = turbo_agent_runtime_run_segment(
      runtime, graph, thread_id, run_id, json_get_string(run_record, "parent_run_id"),
      json_get_string(run_record, "parent_agent_run_id"),
      json_get_string(run_record, "parent_tool_call_id"),
      json_get_string(run_record, "parent_tool_name"),
      json_get_string(run_record, "parent_graph_run_id"),
      json_get_string(run_record, "call_frame_id"),
      json_get_string(run_record, "forked_from_checkpoint_id"), checkpoint_id, seq + 1,
      created_at, NULL, checkpoint, options, cancel_token, event_sink, event_sink_user_data,
      out_summary_json, out_state);

cleanup:
  turbo_graph_checkpoint_destroy(checkpoint);
  turbo_agent_runtime_v1_json_free(&checkpoint_record);
  turbo_agent_runtime_v1_json_free(&run_record);
  return rc;
}

CXX_C_API int turbo_agent_runtime_fork_json_value_graph(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *state_override, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_runtime_fork_json_value_graph_stream(runtime, graph, checkpoint_id,
                                                          state_override, options, NULL, NULL,
                                                          out_summary_json, out_state);
}

int turbo_agent_runtime_fork_json_value_graph_stream(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *state_override, const turbo_graph_run_options_t *options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_runtime_fork_json_value_graph_stream_controlled(
      runtime, graph, checkpoint_id, state_override, options, NULL, event_sink,
      event_sink_user_data, out_summary_json, out_state);
}

int turbo_agent_runtime_fork_json_value_graph_stream_controlled(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *state_override, const turbo_graph_run_options_t *options,
    const turbo_cancel_token_t *cancel_token, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data, json_value_t **out_summary_json, json_value_t **out_state) {
  json_value_t *checkpoint_record = NULL;
  json_value_t *source_run_record = NULL;
  const char *thread_id;
  const char *source_run_id;
  const char *parent_checkpoint_id;
  size_t seq = 0;
  const json_value_t *checkpoint_json;
  turbo_graph_checkpoint_t *checkpoint = NULL;
  char *new_run_id = NULL;
  int rc = -1;

  if (!runtime || !graph || !checkpoint_id || !out_summary_json || !out_state) {
    return -1;
  }
  if (turbo_agent_runtime_store_get_json(runtime, turbo_agent_runtime_checkpoints_collection,
                                         checkpoint_id, &checkpoint_record) != 0 ||
      turbo_runtime_checkpoint_record_read(checkpoint_record, &thread_id, &source_run_id,
                                                    &parent_checkpoint_id, &seq,
                                                    &checkpoint_json) != 0 ||
      turbo_agent_runtime_get_run(runtime, source_run_id, &source_run_record) != 0) {
    goto cleanup;
  }
  (void)parent_checkpoint_id;
  (void)seq;
  new_run_id = turbo_agent_runtime_make_id("run");
  if (!new_run_id ||
      turbo_runtime_checkpoint_from_json(checkpoint_json, state_override, &checkpoint) != 0) {
    goto cleanup;
  }
  rc = turbo_agent_runtime_run_segment(
      runtime, graph, thread_id, new_run_id, source_run_id,
      json_get_string(source_run_record, "parent_agent_run_id"),
      json_get_string(source_run_record, "parent_tool_call_id"),
      json_get_string(source_run_record, "parent_tool_name"),
      json_get_string(source_run_record, "parent_graph_run_id"),
      json_get_string(source_run_record, "call_frame_id"), checkpoint_id, NULL, 1, NULL, NULL,
      checkpoint, options, cancel_token, event_sink, event_sink_user_data, out_summary_json,
      out_state);

cleanup:
  free(new_run_id);
  turbo_graph_checkpoint_destroy(checkpoint);
  turbo_agent_runtime_v1_json_free(&source_run_record);
  turbo_agent_runtime_v1_json_free(&checkpoint_record);
  return rc;
}

static int turbo_agent_runtime_trace_events_from_state_json_value(json_value_t *state,
                                                                  json_value_t **out_events) {
  json_value_t *events = NULL;

  if (!state || !out_events) {
    return -1;
  }
  *out_events = NULL;
  events = turbo_agent_state_trace_events_json_value(state);
  if (!events) {
    events = json_create_array();
  }
  if (!events) {
    return -1;
  }
  *out_events = events;
  return 0;
}

CXX_C_API int turbo_agent_runtime_get_checkpoint_trace_events_json_value(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id, json_value_t **out_events) {
  json_value_t *state = NULL;
  int rc;

  if (!runtime || !checkpoint_id || !out_events) {
    return -1;
  }
  rc = turbo_agent_runtime_get_checkpoint_state_json_value(runtime, checkpoint_id, &state);
  if (rc != 0 || !state) {
    turbo_runtime_json_destroy(state);
    return -1;
  }
  rc = turbo_agent_runtime_trace_events_from_state_json_value(state, out_events);
  turbo_runtime_json_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_runtime_get_run_trace_events_json_value(turbo_agent_runtime_t *runtime,
                                                                  const char *run_id,
                                                                  json_value_t **out_events) {
  json_value_t *state = NULL;
  int rc;

  if (!runtime || !run_id || !out_events) {
    return -1;
  }
  rc = turbo_agent_runtime_get_run_state_json_value(runtime, run_id, &state);
  if (rc != 0 || !state) {
    turbo_runtime_json_destroy(state);
    return -1;
  }
  rc = turbo_agent_runtime_trace_events_from_state_json_value(state, out_events);
  turbo_runtime_json_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_runtime_get_thread_trace_events_json_value(turbo_agent_runtime_t *runtime,
                                                                     const char *thread_id,
                                                                     json_value_t **out_events) {
  json_value_t *state = NULL;
  int rc;

  if (!runtime || !thread_id || !out_events) {
    return -1;
  }
  rc = turbo_agent_runtime_get_thread_state_json_value(runtime, thread_id, &state);
  if (rc != 0 || !state) {
    turbo_runtime_json_destroy(state);
    return -1;
  }
  rc = turbo_agent_runtime_trace_events_from_state_json_value(state, out_events);
  turbo_runtime_json_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_runtime_get_thread_head_trace_events_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id, json_value_t **out_events) {
  json_value_t *state = NULL;
  int rc;

  if (!runtime || !thread_id || !out_events) {
    return -1;
  }
  rc = turbo_agent_runtime_get_thread_head_state_json_value(runtime, thread_id, &state);
  if (rc != 0 || !state) {
    turbo_runtime_json_destroy(state);
    return -1;
  }
  rc = turbo_agent_runtime_trace_events_from_state_json_value(state, out_events);
  turbo_runtime_json_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_runtime_list_thread_lineage(turbo_agent_runtime_t *runtime,
                                                      const char *thread_id,
                                                      json_value_t **out_lineage_json) {
  json_value_t *runs_json = NULL;
  json_value_t *sorted_runs_json = NULL;
  json_value_t *branches_json = NULL;
  json_value_t *lineage_json = NULL;
  char *latest_run_id = NULL;
  char *pending_run_id = NULL;
  char *root_checkpoint_id = NULL;
  size_t i;
  size_t count;

  if (!runtime || !thread_id || !out_lineage_json) {
    return -1;
  }
  *out_lineage_json = NULL;

  if (turbo_agent_runtime_list_runs(runtime, thread_id, &runs_json) != 0 || !runs_json ||
      json_type(runs_json) != JSON_ARRAY) {
    goto cleanup;
  }
  sorted_runs_json = turbo_agent_runtime_sorted_json_array_clone(
      runs_json, turbo_agent_runtime_run_updated_at_compare_desc);
  if (!sorted_runs_json) {
    goto cleanup;
  }
  branches_json = json_create_array();
  lineage_json = json_create_object();
  if (!branches_json || !lineage_json) {
    goto cleanup;
  }

  if (turbo_agent_runtime_latest_run_id_for_thread(runtime, thread_id, &latest_run_id) != 0) {
    free(latest_run_id);
    latest_run_id = NULL;
  }
  if (turbo_agent_runtime_pending_run_id_for_thread(runtime, thread_id, &pending_run_id) != 0) {
    free(pending_run_id);
    pending_run_id = NULL;
  }

  count = json_array_size(sorted_runs_json);
  for (i = 0; i < count; ++i) {
    const json_value_t *run_record = json_array_get(sorted_runs_json, i);
    json_value_t *branch_json = NULL;
    char *branch_root_checkpoint_id = NULL;

    if (!run_record || json_type(run_record) != JSON_OBJECT) {
      continue;
    }
    if (turbo_agent_runtime_branch_from_run_record(runtime, run_record, &branch_json,
                                                   &branch_root_checkpoint_id) != 0) {
      free(branch_root_checkpoint_id);
      goto cleanup;
    }
    if (!root_checkpoint_id && branch_root_checkpoint_id) {
      root_checkpoint_id = branch_root_checkpoint_id;
      branch_root_checkpoint_id = NULL;
    }
    free(branch_root_checkpoint_id);
    json_array_add(branches_json, branch_json);
  }

  json_object_set_string(lineage_json, "thread_id", thread_id);
  if (latest_run_id) {
    json_object_set_string(lineage_json, "latest_run_id", latest_run_id);
  } else {
    json_object_set_null(lineage_json, "latest_run_id");
  }
  if (pending_run_id) {
    json_object_set_string(lineage_json, "pending_run_id", pending_run_id);
  } else {
    json_object_set_null(lineage_json, "pending_run_id");
  }
  if (root_checkpoint_id) {
    json_object_set_string(lineage_json, "root_checkpoint_id", root_checkpoint_id);
  } else {
    json_object_set_null(lineage_json, "root_checkpoint_id");
  }
  json_object_add(lineage_json, "branches", branches_json);
  branches_json = NULL;

  *out_lineage_json = lineage_json;
  lineage_json = NULL;

cleanup:
  free(root_checkpoint_id);
  free(pending_run_id);
  free(latest_run_id);
  turbo_agent_runtime_v1_json_free(&lineage_json);
  turbo_agent_runtime_v1_json_free(&branches_json);
  turbo_agent_runtime_v1_json_free(&sorted_runs_json);
  turbo_agent_runtime_v1_json_free(&runs_json);
  return *out_lineage_json ? 0 : -1;
}

static json_value_t *
turbo_agent_runtime_branch_tree_edges_from_branches(turbo_agent_runtime_t *runtime,
                                                    const json_value_t *branches_json) {
  json_value_t *edges_json = NULL;
  size_t i;
  size_t count;

  if (!branches_json || json_type(branches_json) != JSON_ARRAY) {
    return NULL;
  }
  edges_json = json_create_array();
  if (!edges_json) {
    return NULL;
  }

  count = json_array_size(branches_json);
  for (i = 0; i < count; ++i) {
    const json_value_t *branch = json_array_get(branches_json, i);
    const char *forked_from_checkpoint_id;
    const char *target_run_id;
    const char *parent_run_id;
    json_value_t *edge_json;

    if (!branch || json_type(branch) != JSON_OBJECT) {
      continue;
    }
    forked_from_checkpoint_id = json_get_string(branch, "forked_from_checkpoint_id");
    if (!forked_from_checkpoint_id || forked_from_checkpoint_id[0] == '\0') {
      continue;
    }
    target_run_id = json_get_string(branch, "run_id");
    if (!target_run_id || target_run_id[0] == '\0') {
      turbo_agent_runtime_v1_json_free(&edges_json);
      return NULL;
    }
    parent_run_id = json_get_string(branch, "parent_run_id");
    edge_json = json_create_object();
    if (!edge_json) {
      turbo_agent_runtime_v1_json_free(&edges_json);
      return NULL;
    }
    json_object_set_string(edge_json, "kind", "fork");
    json_object_set_string(edge_json, "source_checkpoint_id", forked_from_checkpoint_id);
    json_object_set_string(edge_json, "target_run_id", target_run_id);
    if (parent_run_id && parent_run_id[0] != '\0') {
      json_object_set_string(edge_json, "parent_run_id", parent_run_id);
    } else {
      json_object_set_null(edge_json, "parent_run_id");
    }
    if (turbo_agent_runtime_attach_checkpoint_summary(
            runtime, edge_json, "source_checkpoint_summary", forked_from_checkpoint_id) != 0) {
      turbo_agent_runtime_v1_json_free(&edge_json);
      turbo_agent_runtime_v1_json_free(&edges_json);
      return NULL;
    }
    json_array_add(edges_json, edge_json);
  }

  return edges_json;
}

static json_value_t *
turbo_agent_runtime_branch_tree_branches_from_lineage(turbo_agent_runtime_t *runtime,
                                                      const json_value_t *branches_json) {
  json_value_t *tree_branches_json = NULL;
  size_t i;
  size_t count;

  if (!branches_json || json_type(branches_json) != JSON_ARRAY) {
    return NULL;
  }
  tree_branches_json = json_create_array();
  if (!tree_branches_json) {
    return NULL;
  }

  count = json_array_size(branches_json);
  for (i = 0; i < count; ++i) {
    const json_value_t *branch = json_array_get(branches_json, i);
    json_value_t *branch_copy;
    const char *source_checkpoint_id;

    if (!branch || json_type(branch) != JSON_OBJECT) {
      continue;
    }
    branch_copy = json_clone(branch);
    if (!branch_copy) {
      turbo_agent_runtime_v1_json_free(&tree_branches_json);
      return NULL;
    }
    source_checkpoint_id = json_get_string(branch, "forked_from_checkpoint_id");
    if (source_checkpoint_id && source_checkpoint_id[0] != '\0') {
      json_object_set_string(branch_copy, "source_checkpoint_id", source_checkpoint_id);
    } else {
      json_object_set_null(branch_copy, "source_checkpoint_id");
    }
    if (turbo_agent_runtime_attach_checkpoint_summary(
            runtime, branch_copy, "checkpoint_summary",
            json_get_string(branch_copy, "checkpoint_id")) != 0) {
      turbo_agent_runtime_v1_json_free(&branch_copy);
      turbo_agent_runtime_v1_json_free(&tree_branches_json);
      return NULL;
    }
    if (turbo_agent_runtime_attach_checkpoint_summary(
            runtime, branch_copy, "source_checkpoint_summary", source_checkpoint_id) != 0) {
      turbo_agent_runtime_v1_json_free(&branch_copy);
      turbo_agent_runtime_v1_json_free(&tree_branches_json);
      return NULL;
    }
    json_array_add(tree_branches_json, branch_copy);
  }

  return tree_branches_json;
}

static const json_value_t *
turbo_agent_runtime_branch_tree_find_branch_by_run_id(const json_value_t *branches_json,
                                                      const char *run_id) {
  size_t i;

  if (!branches_json || json_type(branches_json) != JSON_ARRAY || !run_id ||
      run_id[0] == '\0') {
    return NULL;
  }

  for (i = 0; i < json_array_size(branches_json); ++i) {
    const json_value_t *branch = json_array_get(branches_json, i);
    const char *branch_run_id;

    if (!branch || json_type(branch) != JSON_OBJECT) {
      continue;
    }
    branch_run_id = json_get_string(branch, "run_id");
    if (branch_run_id && strcmp(branch_run_id, run_id) == 0) {
      return branch;
    }
  }

  return NULL;
}

CXX_C_API int turbo_agent_runtime_get_branch_tree(turbo_agent_runtime_t *runtime,
                                                  const char *thread_id,
                                                  json_value_t **out_branch_tree_json) {
  json_value_t *lineage_json = NULL;
  json_value_t *branch_tree_json = NULL;
  json_value_t *tree_branches_json = NULL;
  json_value_t *edges_json = NULL;
  json_value_t *current_checkpoint_json = NULL;
  const json_value_t *branches_json;
  const json_value_t *current_branch_json = NULL;
  const char *current_checkpoint_id = NULL;
  char *latest_run_id = NULL;
  char *pending_run_id = NULL;

  if (!runtime || !thread_id || !out_branch_tree_json) {
    return -1;
  }
  *out_branch_tree_json = NULL;

  if (turbo_agent_runtime_list_thread_lineage(runtime, thread_id, &lineage_json) != 0 ||
      !lineage_json) {
    goto cleanup;
  }
  branches_json = json_object_get(lineage_json, "branches");
  if (!branches_json) {
    goto cleanup;
  }
  tree_branches_json =
      turbo_agent_runtime_branch_tree_branches_from_lineage(runtime, branches_json);
  if (!tree_branches_json) {
    goto cleanup;
  }
  edges_json = turbo_agent_runtime_branch_tree_edges_from_branches(runtime, branches_json);
  if (!edges_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_latest_run_id_for_thread(runtime, thread_id, &latest_run_id) != 0) {
    free(latest_run_id);
    latest_run_id = NULL;
  }
  if (turbo_agent_runtime_pending_run_id_for_thread(runtime, thread_id, &pending_run_id) != 0) {
    free(pending_run_id);
    pending_run_id = NULL;
  }
  if (latest_run_id) {
    current_branch_json =
        turbo_agent_runtime_branch_tree_find_branch_by_run_id(tree_branches_json, latest_run_id);
    if (current_branch_json) {
      current_checkpoint_id = json_get_string(current_branch_json, "checkpoint_id");
      if (turbo_agent_runtime_load_checkpoint_summary_by_id(runtime, current_checkpoint_id,
                                                            &current_checkpoint_json) != 0) {
        goto cleanup;
      }
    }
  }

  branch_tree_json = json_create_object();
  if (!branch_tree_json) {
    goto cleanup;
  }
  json_object_set_string(branch_tree_json, "thread_id",
                               json_get_string(lineage_json, "thread_id"));
  if (latest_run_id) {
    json_object_set_string(branch_tree_json, "current_run_id", latest_run_id);
  } else {
    json_object_set_null(branch_tree_json, "current_run_id");
  }
  if (latest_run_id) {
    json_object_set_string(branch_tree_json, "latest_run_id", latest_run_id);
  } else {
    json_object_set_null(branch_tree_json, "latest_run_id");
  }
  if (pending_run_id && latest_run_id && strcmp(pending_run_id, latest_run_id) == 0) {
    json_object_set_string(branch_tree_json, "pending_run_id", pending_run_id);
  } else {
    json_object_set_null(branch_tree_json, "pending_run_id");
  }
  if (!json_is_null(json_object_get(lineage_json, "root_checkpoint_id"))) {
    json_object_set_string(branch_tree_json, "root_checkpoint_id",
                                 json_get_string(lineage_json, "root_checkpoint_id"));
  } else {
    json_object_set_null(branch_tree_json, "root_checkpoint_id");
  }
  if (current_checkpoint_id && current_checkpoint_id[0] != '\0') {
    json_object_set_string(branch_tree_json, "current_checkpoint_id", current_checkpoint_id);
  } else {
    json_object_set_null(branch_tree_json, "current_checkpoint_id");
  }
  if (current_checkpoint_json) {
    json_object_add(branch_tree_json, "current_checkpoint_summary", current_checkpoint_json);
    current_checkpoint_json = NULL;
  } else {
    json_object_set_null(branch_tree_json, "current_checkpoint_summary");
  }
  if (current_branch_json) {
    json_value_t *current_branch_copy = json_clone(current_branch_json);

    if (!current_branch_copy) {
      goto cleanup;
    }
    json_object_add(branch_tree_json, "current_branch", current_branch_copy);
  } else {
    json_object_set_null(branch_tree_json, "current_branch");
  }
  json_object_add(branch_tree_json, "branches", tree_branches_json);
  tree_branches_json = NULL;
  json_object_add(branch_tree_json, "edges", edges_json);
  edges_json = NULL;

  *out_branch_tree_json = branch_tree_json;
  branch_tree_json = NULL;

cleanup:
  free(pending_run_id);
  free(latest_run_id);
  turbo_agent_runtime_v1_json_free(&edges_json);
  turbo_agent_runtime_v1_json_free(&tree_branches_json);
  turbo_agent_runtime_v1_json_free(&current_checkpoint_json);
  turbo_agent_runtime_v1_json_free(&branch_tree_json);
  turbo_agent_runtime_v1_json_free(&lineage_json);
  return *out_branch_tree_json ? 0 : -1;
}

CXX_C_API int turbo_agent_runtime_get_checkpoint_context(turbo_agent_runtime_t *runtime,
                                                         const char *checkpoint_id,
                                                         json_value_t **out_context_json) {
  json_value_t *checkpoint_json = NULL;
  json_value_t *checkpoint_summary_json = NULL;
  json_value_t *state_json = NULL;
  json_value_t *run_json = NULL;
  json_value_t *thread_json = NULL;
  json_value_t *history_events_json = NULL;
  json_value_t *context_json = NULL;
  json_value_t *state_json_value = NULL;
  json_value_t *history_events_json_value = NULL;
  const char *run_id;
  const char *thread_id;

  if (!runtime || !checkpoint_id || !checkpoint_id[0] || !out_context_json) {
    return -1;
  }
  *out_context_json = NULL;

  if (turbo_agent_runtime_get_checkpoint(runtime, checkpoint_id, &checkpoint_json) != 0 ||
      !checkpoint_json) {
    goto cleanup;
  }
  checkpoint_summary_json = turbo_agent_runtime_checkpoint_summary_from_record(checkpoint_json);
  if (!checkpoint_summary_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_get_checkpoint_state_json_value(runtime, checkpoint_id,
                                                          &state_json_value) != 0 ||
      !state_json_value) {
    goto cleanup;
  }
  state_json = json_clone(state_json_value);
  if (!state_json) {
    goto cleanup;
  }
  run_id = json_get_string(checkpoint_summary_json, "run_id");
  if (!run_id || !run_id[0] || turbo_agent_runtime_get_run(runtime, run_id, &run_json) != 0 ||
      !run_json) {
    goto cleanup;
  }
  thread_id = json_get_string(run_json, "thread_id");
  if (!thread_id || !thread_id[0] ||
      turbo_agent_runtime_get_thread(runtime, thread_id, &thread_json) != 0 || !thread_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_load_history_events_json_value(runtime, NULL, checkpoint_id,
                                                         &history_events_json_value) != 0 ||
      !history_events_json_value) {
    goto cleanup;
  }
  history_events_json = json_clone(history_events_json_value);
  if (!history_events_json) {
    goto cleanup;
  }

  context_json = json_create_object();
  if (!context_json) {
    goto cleanup;
  }
  json_object_add(context_json, "checkpoint_summary", checkpoint_summary_json);
  checkpoint_summary_json = NULL;
  json_object_add(context_json, "state", state_json);
  state_json = NULL;
  json_object_add(context_json, "run", run_json);
  run_json = NULL;
  json_object_add(context_json, "thread", thread_json);
  thread_json = NULL;
  json_object_add(context_json, "history_events", history_events_json);
  history_events_json = NULL;

  *out_context_json = context_json;
  context_json = NULL;

cleanup:
  turbo_agent_runtime_v1_json_free(&context_json);
  turbo_agent_runtime_v1_json_free(&history_events_json);
  turbo_runtime_json_destroy(history_events_json_value);
  turbo_agent_runtime_v1_json_free(&thread_json);
  turbo_agent_runtime_v1_json_free(&run_json);
  turbo_agent_runtime_v1_json_free(&state_json);
  turbo_runtime_json_destroy(state_json_value);
  turbo_agent_runtime_v1_json_free(&checkpoint_summary_json);
  turbo_agent_runtime_v1_json_free(&checkpoint_json);
  return *out_context_json ? 0 : -1;
}

static int turbo_agent_runtime_replay_events_json_value(const json_value_t *events_json_value,
                                                        turbo_event_sink_json_value_fn event_sink,
                                                        void *event_sink_user_data) {
  size_t i;
  size_t count;

  if (!events_json_value || !event_sink || json_type(events_json_value) != JSON_ARRAY) {
    return -1;
  }
  count = turbo_runtime_json_value_size(events_json_value);
  for (i = 0; i < count; ++i) {
    const json_value_t *event = json_array_get(events_json_value, i);

    if (!event) {
      return -1;
    }
    event_sink(event, event_sink_user_data);
  }
  return 0;
}

static int
turbo_agent_runtime_observe_events_json_value(const json_value_t *events_json_value,
                                              const turbo_agent_observer_json_value_sink_t *sink) {
  size_t i;
  size_t count;

  if (!events_json_value || !sink || !sink->callback ||
      json_type(events_json_value) != JSON_ARRAY) {
    return -1;
  }
  count = turbo_runtime_json_value_size(events_json_value);
  for (i = 0; i < count; ++i) {
    const json_value_t *raw_event = json_array_get(events_json_value, i);
    json_value_t *observer_event = NULL;
    int rc;

    if (!raw_event) {
      return -1;
    }
    rc = turbo_agent_runtime_observer_event_from_json_value(raw_event, &observer_event);
    if (rc < 0) {
      turbo_runtime_json_destroy(observer_event);
      return -1;
    }
    if (rc == 0 || !observer_event) {
      turbo_runtime_json_destroy(observer_event);
      continue;
    }
    sink->callback(observer_event, sink->user_data);
    turbo_runtime_json_destroy(observer_event);
  }
  return 0;
}

CXX_C_API int turbo_agent_runtime_replay_history_json_value(
    turbo_agent_runtime_t *runtime, const char *run_id, const char *checkpoint_id,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data) {
  json_value_t *events_json_value = NULL;
  int rc;

  if (!runtime || !event_sink || (!!run_id == !!checkpoint_id)) {
    return -1;
  }
  rc = turbo_agent_runtime_load_history_events_json_value(runtime, run_id, checkpoint_id,
                                                          &events_json_value);
  if (rc != 0 || !events_json_value) {
    turbo_runtime_json_destroy(events_json_value);
    return -1;
  }
  rc = turbo_agent_runtime_replay_events_json_value(events_json_value, event_sink,
                                                    event_sink_user_data);
  turbo_runtime_json_destroy(events_json_value);
  return rc;
}

CXX_C_API int turbo_agent_runtime_replay_thread_history_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data) {
  json_value_t *events_json_value = NULL;
  int rc;

  if (!runtime || !thread_id || !event_sink) {
    return -1;
  }
  rc = turbo_agent_runtime_load_thread_history_events_json_value(runtime, thread_id,
                                                                 &events_json_value);
  if (rc != 0 || !events_json_value) {
    turbo_runtime_json_destroy(events_json_value);
    return -1;
  }
  rc = turbo_agent_runtime_replay_events_json_value(events_json_value, event_sink,
                                                    event_sink_user_data);
  turbo_runtime_json_destroy(events_json_value);
  return rc;
}

CXX_C_API int
turbo_agent_runtime_observe_history_json_value(turbo_agent_runtime_t *runtime, const char *run_id,
                                               const char *checkpoint_id,
                                               const turbo_agent_observer_json_value_sink_t *sink) {
  json_value_t *events_json_value = NULL;
  json_value_t *record_json = NULL;
  int rc;

  if (!runtime || !sink || !sink->callback || (!!run_id == !!checkpoint_id)) {
    return -1;
  }
  rc = turbo_agent_runtime_load_history_events_json_value(runtime, run_id, checkpoint_id,
                                                          &events_json_value);
  if (rc != 0 || !events_json_value) {
    turbo_runtime_json_destroy(events_json_value);
    return -1;
  }
  rc = turbo_agent_runtime_observe_events_json_value(events_json_value, sink);
  if (rc == 0) {
    rc = checkpoint_id ? turbo_agent_runtime_get_checkpoint(runtime, checkpoint_id, &record_json)
                       : turbo_agent_runtime_get_run(runtime, run_id, &record_json);
    if (rc == 0 && record_json) {
      rc = turbo_agent_runtime_observe_terminal_record_json(events_json_value, record_json, sink);
      if (rc > 0) {
        rc = 0;
      }
    }
  }
  turbo_agent_runtime_v1_json_free(&record_json);
  turbo_runtime_json_destroy(events_json_value);
  return rc;
}

CXX_C_API int turbo_agent_runtime_observe_thread_history_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    const turbo_agent_observer_json_value_sink_t *sink) {
  json_value_t *events_json_value = NULL;
  json_value_t *record_json = NULL;
  char *checkpoint_id = NULL;
  char *run_id = NULL;
  int rc;

  if (!runtime || !thread_id || !sink || !sink->callback) {
    return -1;
  }
  rc = turbo_agent_runtime_load_thread_history_events_json_value(runtime, thread_id,
                                                                 &events_json_value);
  if (rc != 0 || !events_json_value) {
    turbo_runtime_json_destroy(events_json_value);
    return -1;
  }
  rc = turbo_agent_runtime_observe_events_json_value(events_json_value, sink);
  if (rc == 0 &&
      turbo_agent_runtime_current_checkpoint_id_for_thread(runtime, thread_id, &checkpoint_id) ==
          0 &&
      checkpoint_id) {
    rc = turbo_agent_runtime_get_checkpoint(runtime, checkpoint_id, &record_json);
  } else if (rc == 0 &&
             turbo_agent_runtime_current_run_id_for_thread(runtime, thread_id, &run_id) == 0 &&
             run_id) {
    rc = turbo_agent_runtime_get_run(runtime, run_id, &record_json);
  }
  if (rc == 0 && record_json) {
    rc = turbo_agent_runtime_observe_terminal_record_json(events_json_value, record_json, sink);
    if (rc > 0) {
      rc = 0;
    }
  }
  free(checkpoint_id);
  free(run_id);
  turbo_agent_runtime_v1_json_free(&record_json);
  turbo_runtime_json_destroy(events_json_value);
  return rc;
}

CXX_C_API int turbo_agent_runtime_get_thread_timeline_json_value(turbo_agent_runtime_t *runtime,
                                                                 const char *thread_id,
                                                                 json_value_t **out_timeline) {
  json_value_t *timeline_json = NULL;
  json_value_t *thread_json = NULL;
  json_value_t *latest_run_json = NULL;
  json_value_t *pending_run_json = NULL;
  json_value_t *resolved_current_run_json = NULL;
  json_value_t *resolved_current_checkpoint_json = NULL;
  json_value_t *runs_json = NULL;
  json_value_t *current_run_checkpoints_json = NULL;
  json_value_t *history_events_json = NULL;
  json_value_t *sorted_json = NULL;
  json_value_t *timeline_json_value = NULL;
  char *resolved_current_run_id = NULL;
  const char *resolved_current_checkpoint_id = NULL;
  const char *resolved_current_run_source = NULL;

  if (!runtime || !thread_id || !out_timeline) {
    return -1;
  }
  *out_timeline = NULL;

  if (turbo_agent_runtime_get_thread(runtime, thread_id, &thread_json) != 0 || !thread_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_get_latest_run(runtime, thread_id, &latest_run_json) != 0) {
    turbo_agent_runtime_v1_json_free(&latest_run_json);
  }
  if (turbo_agent_runtime_get_pending_run(runtime, thread_id, &pending_run_json) != 0) {
    turbo_agent_runtime_v1_json_free(&pending_run_json);
  }
  if (turbo_agent_runtime_list_runs(runtime, thread_id, &runs_json) != 0 || !runs_json) {
    goto cleanup;
  }
  sorted_json = turbo_agent_runtime_sorted_json_array_clone(
      runs_json, turbo_agent_runtime_run_updated_at_compare_desc);
  if (!sorted_json) {
    goto cleanup;
  }
  turbo_agent_runtime_v1_json_free(&runs_json);
  runs_json = sorted_json;
  sorted_json = NULL;

  if (pending_run_json) {
    const char *run_id = json_get_string(pending_run_json, "id");
    if (!run_id || run_id[0] == '\0') {
      goto cleanup;
    }
    resolved_current_run_id = turbo_agent_runtime_strdup(run_id);
    resolved_current_run_source = "pending_run";
    resolved_current_run_json = json_clone(pending_run_json);
  } else if (latest_run_json) {
    const char *run_id = json_get_string(latest_run_json, "id");
    if (!run_id || run_id[0] == '\0') {
      goto cleanup;
    }
    resolved_current_run_id = turbo_agent_runtime_strdup(run_id);
    resolved_current_run_source = "latest_run";
    resolved_current_run_json = json_clone(latest_run_json);
  }
  if ((resolved_current_run_id && !resolved_current_run_json) ||
      (!resolved_current_run_id && resolved_current_run_json)) {
    goto cleanup;
  }
  if (resolved_current_run_json) {
    resolved_current_checkpoint_id =
        json_get_string(resolved_current_run_json, "latest_checkpoint_id");
    if (resolved_current_checkpoint_id && resolved_current_checkpoint_id[0] == '\0') {
      resolved_current_checkpoint_id = NULL;
    }
  }

  if (resolved_current_run_id) {
    if (turbo_agent_runtime_list_checkpoints(runtime, resolved_current_run_id,
                                             &current_run_checkpoints_json) != 0 ||
        !current_run_checkpoints_json) {
      goto cleanup;
    }
    sorted_json = turbo_runtime_checkpoint_records_sorted(current_run_checkpoints_json);
    if (!sorted_json) {
      goto cleanup;
    }
    turbo_agent_runtime_v1_json_free(&current_run_checkpoints_json);
    current_run_checkpoints_json = sorted_json;
    sorted_json = NULL;
    if (turbo_agent_runtime_history_events_json_for_run(runtime, resolved_current_run_id,
                                                        &history_events_json) != 0 ||
        !history_events_json) {
      goto cleanup;
    }
    if (turbo_agent_runtime_load_checkpoint_summary_by_id(runtime, resolved_current_checkpoint_id,
                                                          &resolved_current_checkpoint_json) != 0) {
      goto cleanup;
    }
  } else {
    current_run_checkpoints_json = json_create_array();
    history_events_json = json_create_array();
    if (!current_run_checkpoints_json || !history_events_json) {
      goto cleanup;
    }
  }

  timeline_json = json_create_object();
  if (!timeline_json) {
    goto cleanup;
  }
  json_object_add(timeline_json, "thread", thread_json);
  thread_json = NULL;
  if (resolved_current_run_id) {
    json_object_set_string(timeline_json, "resolved_current_run_id", resolved_current_run_id);
  } else {
    json_object_set_null(timeline_json, "resolved_current_run_id");
  }
  if (resolved_current_checkpoint_id) {
    json_object_set_string(timeline_json, "resolved_current_checkpoint_id",
                                 resolved_current_checkpoint_id);
  } else {
    json_object_set_null(timeline_json, "resolved_current_checkpoint_id");
  }
  if (resolved_current_run_source) {
    json_object_set_string(timeline_json, "resolved_current_run_source",
                                 resolved_current_run_source);
  } else {
    json_object_set_null(timeline_json, "resolved_current_run_source");
  }
  if (resolved_current_run_json) {
    json_object_add(timeline_json, "resolved_current_run", resolved_current_run_json);
    resolved_current_run_json = NULL;
  } else {
    json_object_set_null(timeline_json, "resolved_current_run");
  }
  if (resolved_current_checkpoint_json) {
    json_object_add(timeline_json, "resolved_current_checkpoint",
                          resolved_current_checkpoint_json);
    resolved_current_checkpoint_json = NULL;
  } else {
    json_object_set_null(timeline_json, "resolved_current_checkpoint");
  }
  if (latest_run_json) {
    json_object_add(timeline_json, "latest_run", latest_run_json);
    latest_run_json = NULL;
  } else {
    json_object_set_null(timeline_json, "latest_run");
  }
  if (pending_run_json) {
    json_object_add(timeline_json, "pending_run", pending_run_json);
    pending_run_json = NULL;
  } else {
    json_object_set_null(timeline_json, "pending_run");
  }
  json_object_add(timeline_json, "runs", runs_json);
  runs_json = NULL;
  json_object_add(timeline_json, "current_run_checkpoints", current_run_checkpoints_json);
  current_run_checkpoints_json = NULL;
  json_object_add(timeline_json, "history_events", history_events_json);
  history_events_json = NULL;

  timeline_json_value = json_clone(timeline_json);
  if (!timeline_json_value) {
    goto cleanup;
  }
  *out_timeline = timeline_json_value;
  timeline_json_value = NULL;

cleanup:
  free(resolved_current_run_id);
  turbo_runtime_json_destroy(timeline_json_value);
  turbo_agent_runtime_v1_json_free(&sorted_json);
  turbo_agent_runtime_v1_json_free(&history_events_json);
  turbo_agent_runtime_v1_json_free(&current_run_checkpoints_json);
  turbo_agent_runtime_v1_json_free(&runs_json);
  turbo_agent_runtime_v1_json_free(&resolved_current_run_json);
  turbo_agent_runtime_v1_json_free(&resolved_current_checkpoint_json);
  turbo_agent_runtime_v1_json_free(&pending_run_json);
  turbo_agent_runtime_v1_json_free(&latest_run_json);
  turbo_agent_runtime_v1_json_free(&thread_json);
  turbo_agent_runtime_v1_json_free(&timeline_json);
  return *out_timeline ? 0 : -1;
}

static void turbo_agent_runtime_observability_counts_from_runs(const json_value_t *runs_json,
                                                               size_t *out_runs,
                                                               size_t *out_interrupted_runs,
                                                               size_t *out_completed_runs) {
  size_t i;
  size_t count = 0;
  size_t interrupted = 0;
  size_t completed = 0;

  if (out_runs) {
    *out_runs = 0;
  }
  if (out_interrupted_runs) {
    *out_interrupted_runs = 0;
  }
  if (out_completed_runs) {
    *out_completed_runs = 0;
  }
  if (!runs_json || json_type(runs_json) != JSON_ARRAY) {
    return;
  }

  count = json_array_size(runs_json);
  for (i = 0; i < count; ++i) {
    const json_value_t *run_record = json_array_get(runs_json, i);
    const char *status;

    if (!run_record || json_type(run_record) != JSON_OBJECT) {
      continue;
    }
    status = json_get_string(run_record, "status");
    if (status && strcmp(status, "interrupted") == 0) {
      interrupted += 1;
    } else if (status && strcmp(status, "completed") == 0) {
      completed += 1;
    }
  }

  if (out_runs) {
    *out_runs = count;
  }
  if (out_interrupted_runs) {
    *out_interrupted_runs = interrupted;
  }
  if (out_completed_runs) {
    *out_completed_runs = completed;
  }
}

static json_value_t *turbo_agent_runtime_build_observability_counts_json(
    const json_value_t *timeline_json, const json_value_t *lineage_json,
    const json_value_t *branch_tree_json, const json_value_t *history_events_json,
    const json_value_t *trace_events_json) {
  const json_value_t *runs_json = NULL;
  const json_value_t *current_run_checkpoints_json = NULL;
  const json_value_t *branches_json = NULL;
  const json_value_t *edges_json = NULL;
  size_t runs = 0;
  size_t interrupted_runs = 0;
  size_t completed_runs = 0;
  json_value_t *counts_json = json_create_object();

  if (!counts_json) {
    return NULL;
  }

  if (timeline_json && json_type(timeline_json) == JSON_OBJECT) {
    runs_json = json_object_get(timeline_json, "runs");
    current_run_checkpoints_json = json_object_get(timeline_json, "current_run_checkpoints");
  }
  if (branch_tree_json && json_type(branch_tree_json) == JSON_OBJECT) {
    branches_json = json_object_get(branch_tree_json, "branches");
    edges_json = json_object_get(branch_tree_json, "edges");
  } else if (lineage_json && json_type(lineage_json) == JSON_OBJECT) {
    branches_json = json_object_get(lineage_json, "branches");
  }

  turbo_agent_runtime_observability_counts_from_runs(runs_json, &runs, &interrupted_runs,
                                                     &completed_runs);
  json_object_set_number(counts_json, "runs", (double)runs);
  json_object_set_number(counts_json, "interrupted_runs", (double)interrupted_runs);
  json_object_set_number(counts_json, "completed_runs", (double)completed_runs);
  json_object_set_number(
      counts_json, "current_run_checkpoints",
      (double)((current_run_checkpoints_json &&
                json_type(current_run_checkpoints_json) == JSON_ARRAY)
                   ? json_array_size(current_run_checkpoints_json)
                   : 0));
  json_object_set_number(
      counts_json, "branches",
      (double)((branches_json && json_type(branches_json) == JSON_ARRAY)
                   ? json_array_size(branches_json)
                   : 0));
  json_object_set_number(
      counts_json, "edges",
      (double)((edges_json && json_type(edges_json) == JSON_ARRAY)
                   ? json_array_size(edges_json)
                   : 0));
  json_object_set_number(
      counts_json, "history_events",
      (double)((history_events_json && json_type(history_events_json) == JSON_ARRAY)
                   ? json_array_size(history_events_json)
                   : 0));
  json_object_set_number(
      counts_json, "trace_events",
      (double)((trace_events_json && json_type(trace_events_json) == JSON_ARRAY)
                   ? json_array_size(trace_events_json)
                   : 0));
  return counts_json;
}

static const char *turbo_agent_runtime_observability_string_or_null(const json_value_t *object_json,
                                                                    const char *key) {
  const char *value;

  if (!object_json || json_type(object_json) != JSON_OBJECT || !key) {
    return NULL;
  }
  value = json_get_string(object_json, key);
  return (value && value[0] != '\0') ? value : NULL;
}

static const char *
turbo_agent_runtime_observability_current_status(const json_value_t *timeline_json,
                                                 const json_value_t *pending_run_json,
                                                 const json_value_t *latest_run_json) {
  const json_value_t *resolved_current_run_json = NULL;
  const char *status = NULL;

  if (timeline_json && json_type(timeline_json) == JSON_OBJECT) {
    resolved_current_run_json = json_object_get(timeline_json, "resolved_current_run");
  }
  status = turbo_agent_runtime_observability_string_or_null(resolved_current_run_json, "status");
  if (status) {
    return status;
  }
  status = turbo_agent_runtime_observability_string_or_null(pending_run_json, "status");
  if (status) {
    return status;
  }
  return turbo_agent_runtime_observability_string_or_null(latest_run_json, "status");
}

static int turbo_agent_runtime_observability_summary_copy_field(json_value_t *target_json,
                                                                const json_value_t *source_json,
                                                                const char *key) {
  const json_value_t *value_json;
  json_value_t *clone_json;

  if (!target_json || !source_json || !key) {
    return -1;
  }
  value_json = json_object_get(source_json, key);
  if (!value_json) {
    json_object_set_null(target_json, key);
    return 0;
  }
  clone_json = json_clone(value_json);
  if (!clone_json) {
    return -1;
  }
  json_object_add(target_json, key, clone_json);
  return 0;
}

static int turbo_agent_runtime_build_observability_index_summary(const json_value_t *index_json,
                                                                 json_value_t **out_summary_json) {
  static const char *const summary_keys[] = {"thread",
                                             "latest_run",
                                             "pending_run",
                                             "current_status",
                                             "current_interrupt_reason",
                                             "current_pending_action",
                                             "current_checkpoint_summary",
                                             "latest_run_status",
                                             "latest_run_updated_at",
                                             "pending_run_id",
                                             "pending_checkpoint_id",
                                             "has_failure",
                                             "has_model_error",
                                             "has_guardrail_rejection",
                                             "replan_requested",
                                             "current_failure_reason",
                                             "current_review_note",
                                             "has_pending_review",
                                             "has_handoff",
                                             "active_agent",
                                             "counts"};
  json_value_t *summary_json = NULL;
  size_t i;

  if (!index_json || json_type(index_json) != JSON_OBJECT || !out_summary_json) {
    return -1;
  }
  *out_summary_json = NULL;
  summary_json = json_create_object();
  if (!summary_json) {
    return -1;
  }
  for (i = 0; i < sizeof(summary_keys) / sizeof(summary_keys[0]); ++i) {
    if (turbo_agent_runtime_observability_summary_copy_field(summary_json, index_json,
                                                             summary_keys[i]) != 0) {
      turbo_agent_runtime_v1_json_free(&summary_json);
      return -1;
    }
  }
  *out_summary_json = summary_json;
  return 0;
}

static int turbo_agent_runtime_observability_summary_matches_filters(
    const json_value_t *summary_json, const json_value_t *filters_json, bool *out_matches) {
  const json_value_t *value_json;
  const char *summary_text;
  const char *summary_thread_id;
  bool expected_bool;

  if (!summary_json || json_type(summary_json) != JSON_OBJECT || !out_matches) {
    return -1;
  }
  *out_matches = true;
  if (!filters_json || json_is_null(filters_json)) {
    return 0;
  }
  if (json_type(filters_json) != JSON_OBJECT) {
    return -1;
  }
  summary_thread_id = turbo_agent_runtime_observability_string_or_null(
      json_object_get(summary_json, "thread"), "id");

  value_json = json_object_get(filters_json, "status");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_STRING) {
      return -1;
    }
    summary_text = turbo_agent_runtime_observability_string_or_null(summary_json, "current_status");
    if (!summary_text || strcmp(summary_text, json_string(value_json)) != 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "has_pending_review");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_BOOL) {
      return -1;
    }
    expected_bool = json_bool(value_json);
    if (json_get_bool(summary_json, "has_pending_review", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "has_failure");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_BOOL) {
      return -1;
    }
    expected_bool = json_bool(value_json);
    if (json_get_bool(summary_json, "has_failure", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "has_handoff");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_BOOL) {
      return -1;
    }
    expected_bool = json_bool(value_json);
    if (json_get_bool(summary_json, "has_handoff", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "has_model_error");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_BOOL) {
      return -1;
    }
    expected_bool = json_bool(value_json);
    if (json_get_bool(summary_json, "has_model_error", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "has_guardrail_rejection");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_BOOL) {
      return -1;
    }
    expected_bool = json_bool(value_json);
    if (json_get_bool(summary_json, "has_guardrail_rejection", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "replan_requested");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_BOOL) {
      return -1;
    }
    expected_bool = json_bool(value_json);
    if (json_get_bool(summary_json, "replan_requested", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "active_agent");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_STRING) {
      return -1;
    }
    summary_text = turbo_agent_runtime_observability_string_or_null(summary_json, "active_agent");
    if (!summary_text || strcmp(summary_text, json_string(value_json)) != 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "current_interrupt_reason");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_STRING) {
      return -1;
    }
    summary_text =
        turbo_agent_runtime_observability_string_or_null(summary_json, "current_interrupt_reason");
    if (!summary_text || strcmp(summary_text, json_string(value_json)) != 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "latest_run_status");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_STRING) {
      return -1;
    }
    summary_text =
        turbo_agent_runtime_observability_string_or_null(summary_json, "latest_run_status");
    if (!summary_text || strcmp(summary_text, json_string(value_json)) != 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "latest_run_updated_after");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_STRING) {
      return -1;
    }
    summary_text =
        turbo_agent_runtime_observability_string_or_null(summary_json, "latest_run_updated_at");
    if (!summary_text || turbo_agent_runtime_record_string_compare(
                             summary_text, json_string(value_json)) <= 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "latest_run_updated_before");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_STRING) {
      return -1;
    }
    summary_text =
        turbo_agent_runtime_observability_string_or_null(summary_json, "latest_run_updated_at");
    if (!summary_text || turbo_agent_runtime_record_string_compare(
                             summary_text, json_string(value_json)) >= 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "thread_id_prefix");
  if (value_json && !json_is_null(value_json)) {
    const char *prefix;

    if (json_type(value_json) != JSON_STRING) {
      return -1;
    }
    prefix = json_string(value_json);
    if (!summary_thread_id || strncmp(summary_thread_id, prefix, strlen(prefix)) != 0) {
      *out_matches = false;
      return 0;
    }
  }

  return 0;
}

static int turbo_agent_runtime_observability_sort_options(const json_value_t *filters_json,
                                                          const char **out_sort_by,
                                                          const char **out_sort_order,
                                                          int *out_limit) {
  const json_value_t *value_json;

  if (!out_sort_by || !out_sort_order || !out_limit) {
    return -1;
  }
  *out_sort_by = NULL;
  *out_sort_order = NULL;
  *out_limit = -1;
  if (!filters_json || json_is_null(filters_json)) {
    return 0;
  }
  if (json_type(filters_json) != JSON_OBJECT) {
    return -1;
  }

  value_json = json_object_get(filters_json, "sort_by");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_STRING) {
      return -1;
    }
    *out_sort_by = json_string(value_json);
    if (strcmp(*out_sort_by, "latest_run_updated_at") != 0 &&
        strcmp(*out_sort_by, "thread_id") != 0) {
      return -1;
    }
  }

  value_json = json_object_get(filters_json, "sort_order");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_STRING) {
      return -1;
    }
    *out_sort_order = json_string(value_json);
    if (strcmp(*out_sort_order, "asc") != 0 && strcmp(*out_sort_order, "desc") != 0) {
      return -1;
    }
  }

  value_json = json_object_get(filters_json, "limit");
  if (value_json && !json_is_null(value_json)) {
    double limit_value;

    if (json_type(value_json) != JSON_NUMBER) {
      return -1;
    }
    limit_value = json_number(value_json);
    if (limit_value < 0.0) {
      return -1;
    }
    *out_limit = (int)limit_value;
  }

  if (!*out_sort_by && *out_sort_order) {
    *out_sort_by = "latest_run_updated_at";
  }
  if (*out_sort_by && !*out_sort_order) {
    *out_sort_order = (strcmp(*out_sort_by, "thread_id") == 0) ? "asc" : "desc";
  }
  return 0;
}

static int (*turbo_agent_runtime_observability_sort_compare(const char *sort_by,
                                                            const char *sort_order))(const void *,
                                                                                     const void *) {
  if (!sort_by || !sort_order) {
    return NULL;
  }
  if (strcmp(sort_by, "thread_id") == 0) {
    return (strcmp(sort_order, "desc") == 0)
               ? turbo_agent_runtime_observability_summary_thread_id_compare_desc
               : turbo_agent_runtime_observability_summary_thread_id_compare_asc;
  }
  return (strcmp(sort_order, "asc") == 0)
             ? turbo_agent_runtime_observability_summary_latest_run_updated_at_compare_asc
             : turbo_agent_runtime_observability_summary_latest_run_updated_at_compare_desc;
}

CXX_C_API int turbo_agent_runtime_get_thread_observability_index(turbo_agent_runtime_t *runtime,
                                                                 const char *thread_id,
                                                                 json_value_t **out_index_json) {
  json_value_t *index_json = NULL;
  json_value_t *thread_json = NULL;
  json_value_t *latest_run_json = NULL;
  json_value_t *pending_run_json = NULL;
  json_value_t *thread_timeline_json = NULL;
  json_value_t *thread_lineage_json = NULL;
  json_value_t *branch_tree_json = NULL;
  json_value_t *history_events_json = NULL;
  json_value_t *trace_events_json = NULL;
  json_value_t *counts_json = NULL;
  json_value_t *control_snapshot_json = NULL;
  json_value_t *current_checkpoint_summary_json = NULL;
  json_value_t *thread_state_json = NULL;
  json_value_t *thread_timeline_json_value = NULL;
  json_value_t *history_events_json_value = NULL;
  json_value_t *trace_events_json_value = NULL;
  json_value_t *thread_state_json_value = NULL;
  json_value_t *control_snapshot_json_value = NULL;
  const json_value_t *review_json = NULL;
  const json_value_t *replan_json = NULL;
  const json_value_t *failure_json = NULL;
  const json_value_t *model_error_json = NULL;
  const json_value_t *guardrail_json = NULL;
  const json_value_t *supervisor_json = NULL;
  const char *current_status = NULL;
  const char *current_interrupt_reason = NULL;
  const char *current_pending_action = NULL;
  const char *active_agent = NULL;
  const char *latest_run_status = NULL;
  const char *latest_run_updated_at = NULL;
  const char *pending_run_id = NULL;
  const char *pending_checkpoint_id = NULL;
  char *owned_executor_failure_reason = NULL;
  const char *handoff_target_agent = NULL;
  const char *handoff_reason = NULL;
  const char *current_failure_reason = NULL;
  const char *current_review_note = NULL;
  bool has_pending_review = false;
  bool has_handoff = false;
  bool has_failure = false;
  bool has_model_error = false;
  bool has_guardrail_rejection = false;
  bool replan_requested = false;

  if (!runtime || !thread_id || !out_index_json) {
    return -1;
  }
  *out_index_json = NULL;

  if (turbo_agent_runtime_get_thread(runtime, thread_id, &thread_json) != 0 || !thread_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_get_latest_run(runtime, thread_id, &latest_run_json) != 0) {
    turbo_agent_runtime_v1_json_free(&latest_run_json);
  }
  if (turbo_agent_runtime_get_pending_run(runtime, thread_id, &pending_run_json) != 0) {
    turbo_agent_runtime_v1_json_free(&pending_run_json);
  }
  if (turbo_agent_runtime_get_thread_timeline_json_value(runtime, thread_id,
                                                         &thread_timeline_json_value) != 0 ||
      !thread_timeline_json_value) {
    goto cleanup;
  }
  thread_timeline_json = json_clone(thread_timeline_json_value);
  if (!thread_timeline_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_list_thread_lineage(runtime, thread_id, &thread_lineage_json) != 0 ||
      !thread_lineage_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_get_branch_tree(runtime, thread_id, &branch_tree_json) != 0 ||
      !branch_tree_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_load_thread_history_events_json_value(runtime, thread_id,
                                                                &history_events_json_value) != 0 ||
      !history_events_json_value) {
    goto cleanup;
  }
  history_events_json = json_clone(history_events_json_value);
  if (!history_events_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_get_thread_trace_events_json_value(runtime, thread_id,
                                                             &trace_events_json_value) != 0 ||
      !trace_events_json_value) {
    goto cleanup;
  }
  trace_events_json = json_clone(trace_events_json_value);
  if (!trace_events_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_get_thread_state_json_value(runtime, thread_id,
                                                      &thread_state_json_value) == 0 &&
      thread_state_json_value) {
    thread_state_json = json_clone(thread_state_json_value);
    if (!thread_state_json) {
      goto cleanup;
    }
    control_snapshot_json_value =
        turbo_agent_state_control_snapshot_json_value(thread_state_json_value);
    if (control_snapshot_json_value) {
      control_snapshot_json = json_clone(control_snapshot_json_value);
      if (!control_snapshot_json) {
        goto cleanup;
      }
    }
  }

  counts_json = turbo_agent_runtime_build_observability_counts_json(
      thread_timeline_json, thread_lineage_json, branch_tree_json, history_events_json,
      trace_events_json);
  if (!counts_json) {
    goto cleanup;
  }
  current_status = turbo_agent_runtime_observability_current_status(
      thread_timeline_json, pending_run_json, latest_run_json);
  latest_run_status = turbo_agent_runtime_observability_string_or_null(latest_run_json, "status");
  latest_run_updated_at =
      turbo_agent_runtime_observability_string_or_null(latest_run_json, "updated_at");
  pending_run_id = turbo_agent_runtime_observability_string_or_null(pending_run_json, "id");
  pending_checkpoint_id =
      turbo_agent_runtime_observability_string_or_null(pending_run_json, "latest_checkpoint_id");
  if (thread_timeline_json && json_type(thread_timeline_json) == JSON_OBJECT) {
    const json_value_t *resolved_current_checkpoint_json =
        json_object_get(thread_timeline_json, "resolved_current_checkpoint");
    const char *next_node = turbo_agent_runtime_observability_string_or_null(
        resolved_current_checkpoint_json, "next_node");
    if (resolved_current_checkpoint_json &&
        json_type(resolved_current_checkpoint_json) == JSON_OBJECT) {
      current_checkpoint_summary_json = json_clone(resolved_current_checkpoint_json);
      if (!current_checkpoint_summary_json) {
        goto cleanup;
      }
    }
    turbo_agent_runtime_interrupt_metadata(current_status, next_node, thread_state_json,
                                           &current_interrupt_reason, &current_pending_action, NULL,
                                           NULL, &owned_executor_failure_reason);
  }
  if (control_snapshot_json && json_type(control_snapshot_json) == JSON_OBJECT) {
    review_json = json_object_get(control_snapshot_json, "review");
    replan_json = json_object_get(control_snapshot_json, "replan");
    failure_json = json_object_get(control_snapshot_json, "failure");
    model_error_json = json_object_get(control_snapshot_json, "model_error");
    guardrail_json = json_object_get(control_snapshot_json, "guardrail");
    supervisor_json = json_object_get(control_snapshot_json, "supervisor");
  }
  if (review_json && json_type(review_json) == JSON_OBJECT) {
    has_pending_review = json_get_bool(review_json, "required", false) &&
                         !json_get_bool(review_json, "approved", false);
    current_review_note = turbo_agent_runtime_observability_string_or_null(review_json, "note");
  }
  if (replan_json && json_type(replan_json) == JSON_OBJECT) {
    replan_requested = json_get_bool(replan_json, "requested", false);
  }
  if (failure_json && json_type(failure_json) == JSON_OBJECT) {
    current_failure_reason =
        turbo_agent_runtime_observability_string_or_null(failure_json, "reason");
    has_failure = current_failure_reason && current_failure_reason[0] != '\0';
  }
  if (model_error_json && json_type(model_error_json) == JSON_OBJECT) {
    has_model_error =
        turbo_agent_runtime_observability_string_or_null(model_error_json, "detail") != NULL;
  }
  if (guardrail_json && json_type(guardrail_json) == JSON_OBJECT) {
    has_guardrail_rejection =
        turbo_agent_runtime_observability_string_or_null(guardrail_json, "reason") != NULL;
  }
  active_agent = turbo_agent_runtime_observability_string_or_null(supervisor_json, "active_agent");
  handoff_target_agent =
      turbo_agent_runtime_observability_string_or_null(supervisor_json, "target_agent");
  handoff_reason =
      turbo_agent_runtime_observability_string_or_null(supervisor_json, "handoff_reason");
  has_handoff = (handoff_target_agent && handoff_target_agent[0] != '\0') ||
                (handoff_reason && handoff_reason[0] != '\0');

  index_json = json_create_object();
  if (!index_json) {
    goto cleanup;
  }
  json_object_add(index_json, "thread", thread_json);
  thread_json = NULL;
  if (latest_run_json) {
    json_object_add(index_json, "latest_run", latest_run_json);
    latest_run_json = NULL;
  } else {
    json_object_set_null(index_json, "latest_run");
  }
  if (pending_run_json) {
    json_object_add(index_json, "pending_run", pending_run_json);
    pending_run_json = NULL;
  } else {
    json_object_set_null(index_json, "pending_run");
  }
  json_object_add(index_json, "thread_timeline", thread_timeline_json);
  thread_timeline_json = NULL;
  json_object_add(index_json, "thread_lineage", thread_lineage_json);
  thread_lineage_json = NULL;
  json_object_add(index_json, "branch_tree", branch_tree_json);
  branch_tree_json = NULL;
  if (current_status) {
    json_object_set_string(index_json, "current_status", current_status);
  } else {
    json_object_set_null(index_json, "current_status");
  }
  if (current_interrupt_reason) {
    json_object_set_string(index_json, "current_interrupt_reason", current_interrupt_reason);
  } else {
    json_object_set_null(index_json, "current_interrupt_reason");
  }
  if (current_pending_action) {
    json_object_set_string(index_json, "current_pending_action", current_pending_action);
  } else {
    json_object_set_null(index_json, "current_pending_action");
  }
  if (current_checkpoint_summary_json) {
    json_object_add(index_json, "current_checkpoint_summary",
                          current_checkpoint_summary_json);
    current_checkpoint_summary_json = NULL;
  } else {
    json_object_set_null(index_json, "current_checkpoint_summary");
  }
  if (latest_run_status) {
    json_object_set_string(index_json, "latest_run_status", latest_run_status);
  } else {
    json_object_set_null(index_json, "latest_run_status");
  }
  if (latest_run_updated_at) {
    json_object_set_string(index_json, "latest_run_updated_at", latest_run_updated_at);
  } else {
    json_object_set_null(index_json, "latest_run_updated_at");
  }
  if (pending_run_id) {
    json_object_set_string(index_json, "pending_run_id", pending_run_id);
  } else {
    json_object_set_null(index_json, "pending_run_id");
  }
  if (pending_checkpoint_id) {
    json_object_set_string(index_json, "pending_checkpoint_id", pending_checkpoint_id);
  } else {
    json_object_set_null(index_json, "pending_checkpoint_id");
  }
  json_object_set_bool(index_json, "has_failure", has_failure ? true : false);
  json_object_set_bool(index_json, "has_model_error", has_model_error ? true : false);
  json_object_set_bool(index_json, "has_guardrail_rejection",
                             has_guardrail_rejection ? true : false);
  json_object_set_bool(index_json, "replan_requested", replan_requested ? true : false);
  if (current_failure_reason) {
    json_object_set_string(index_json, "current_failure_reason", current_failure_reason);
  } else {
    json_object_set_null(index_json, "current_failure_reason");
  }
  if (current_review_note) {
    json_object_set_string(index_json, "current_review_note", current_review_note);
  } else {
    json_object_set_null(index_json, "current_review_note");
  }
  json_object_set_bool(index_json, "has_pending_review", has_pending_review ? true : false);
  json_object_set_bool(index_json, "has_handoff", has_handoff ? true : false);
  if (active_agent) {
    json_object_set_string(index_json, "active_agent", active_agent);
  } else {
    json_object_set_null(index_json, "active_agent");
  }
  json_object_add(index_json, "history_events", history_events_json);
  history_events_json = NULL;
  json_object_add(index_json, "trace_events", trace_events_json);
  trace_events_json = NULL;
  json_object_add(index_json, "counts", counts_json);
  counts_json = NULL;

  *out_index_json = index_json;
  index_json = NULL;
  free(owned_executor_failure_reason);
  turbo_runtime_json_destroy(control_snapshot_json_value);
  turbo_runtime_json_destroy(thread_state_json_value);
  turbo_runtime_json_destroy(trace_events_json_value);
  turbo_runtime_json_destroy(history_events_json_value);
  turbo_runtime_json_destroy(thread_timeline_json_value);
  turbo_agent_runtime_v1_json_free(&thread_state_json);
  turbo_agent_runtime_v1_json_free(&control_snapshot_json);
  return 0;

cleanup:
  free(owned_executor_failure_reason);
  turbo_runtime_json_destroy(control_snapshot_json_value);
  turbo_runtime_json_destroy(thread_state_json_value);
  turbo_runtime_json_destroy(trace_events_json_value);
  turbo_runtime_json_destroy(history_events_json_value);
  turbo_runtime_json_destroy(thread_timeline_json_value);
  turbo_agent_runtime_v1_json_free(&thread_state_json);
  turbo_agent_runtime_v1_json_free(&current_checkpoint_summary_json);
  turbo_agent_runtime_v1_json_free(&control_snapshot_json);
  turbo_agent_runtime_v1_json_free(&counts_json);
  turbo_agent_runtime_v1_json_free(&trace_events_json);
  turbo_agent_runtime_v1_json_free(&history_events_json);
  turbo_agent_runtime_v1_json_free(&branch_tree_json);
  turbo_agent_runtime_v1_json_free(&thread_lineage_json);
  turbo_agent_runtime_v1_json_free(&thread_timeline_json);
  turbo_agent_runtime_v1_json_free(&pending_run_json);
  turbo_agent_runtime_v1_json_free(&latest_run_json);
  turbo_agent_runtime_v1_json_free(&thread_json);
  turbo_agent_runtime_v1_json_free(&index_json);
  return -1;
}

CXX_C_API int
turbo_agent_runtime_list_observability_indexes_filtered(turbo_agent_runtime_t *runtime,
                                                        const json_value_t *filters_json,
                                                        json_value_t **out_indexes_json) {
  json_value_t *threads_json = NULL;
  json_value_t *sorted_threads_json = NULL;
  json_value_t *indexes_json = NULL;
  json_value_t *sorted_indexes_json = NULL;
  json_value_t *limited_indexes_json = NULL;
  const char *sort_by = NULL;
  const char *sort_order = NULL;
  int limit = -1;
  int (*compare)(const void *, const void *) = NULL;
  size_t count;
  size_t i;

  if (!runtime || !out_indexes_json) {
    return -1;
  }
  *out_indexes_json = NULL;
  if (turbo_agent_runtime_observability_sort_options(filters_json, &sort_by, &sort_order, &limit) !=
      0) {
    return -1;
  }
  compare = turbo_agent_runtime_observability_sort_compare(sort_by, sort_order);
  if (turbo_agent_runtime_store_list_json(runtime, turbo_agent_runtime_threads_collection, NULL,
                                          NULL, &threads_json) != 0 ||
      !threads_json) {
    goto cleanup;
  }
  sorted_threads_json = turbo_agent_runtime_sorted_json_array_clone(
      threads_json, turbo_agent_runtime_run_updated_at_compare_desc);
  if (!sorted_threads_json) {
    goto cleanup;
  }
  indexes_json = json_create_array();
  if (!indexes_json) {
    goto cleanup;
  }

  count = json_array_size(sorted_threads_json);
  for (i = 0; i < count; ++i) {
    const json_value_t *thread_json = json_array_get(sorted_threads_json, i);
    const char *thread_id;
    json_value_t *index_json = NULL;
    json_value_t *summary_json = NULL;
    bool matches_filters = false;

    if (!thread_json || json_type(thread_json) != JSON_OBJECT) {
      goto cleanup;
    }
    thread_id = json_get_string(thread_json, "id");
    if (!thread_id || !thread_id[0]) {
      goto cleanup;
    }
    if (turbo_agent_runtime_get_thread_observability_index(runtime, thread_id, &index_json) != 0 ||
        !index_json ||
        turbo_agent_runtime_build_observability_index_summary(index_json, &summary_json) != 0 ||
        !summary_json) {
      turbo_agent_runtime_v1_json_free(&index_json);
      turbo_agent_runtime_v1_json_free(&summary_json);
      goto cleanup;
    }
    if (turbo_agent_runtime_observability_summary_matches_filters(summary_json, filters_json,
                                                                  &matches_filters) != 0) {
      turbo_agent_runtime_v1_json_free(&index_json);
      turbo_agent_runtime_v1_json_free(&summary_json);
      goto cleanup;
    }
    turbo_agent_runtime_v1_json_free(&index_json);
    if (!matches_filters) {
      turbo_agent_runtime_v1_json_free(&summary_json);
      continue;
    }
    json_array_add(indexes_json, summary_json);
    summary_json = NULL;
  }

  if (compare) {
    sorted_indexes_json = turbo_agent_runtime_sorted_json_array_clone(indexes_json, compare);
    if (!sorted_indexes_json) {
      goto cleanup;
    }
    turbo_agent_runtime_v1_json_free(&indexes_json);
    indexes_json = sorted_indexes_json;
    sorted_indexes_json = NULL;
  }

  if (limit >= 0) {
    limited_indexes_json = json_create_array();
    if (!limited_indexes_json) {
      goto cleanup;
    }
    count = json_array_size(indexes_json);
    if ((size_t)limit < count) {
      count = (size_t)limit;
    }
    for (i = 0; i < count; ++i) {
      json_value_t *clone_json = json_clone(json_array_get(indexes_json, i));

      if (!clone_json) {
        goto cleanup;
      }
      json_array_add(limited_indexes_json, clone_json);
    }
    turbo_agent_runtime_v1_json_free(&indexes_json);
    indexes_json = limited_indexes_json;
    limited_indexes_json = NULL;
  }

  *out_indexes_json = indexes_json;
  indexes_json = NULL;
  turbo_agent_runtime_v1_json_free(&threads_json);
  turbo_agent_runtime_v1_json_free(&sorted_threads_json);
  return 0;

cleanup:
  turbo_agent_runtime_v1_json_free(&threads_json);
  turbo_agent_runtime_v1_json_free(&sorted_threads_json);
  turbo_agent_runtime_v1_json_free(&sorted_indexes_json);
  turbo_agent_runtime_v1_json_free(&limited_indexes_json);
  turbo_agent_runtime_v1_json_free(&indexes_json);
  return -1;
}

CXX_C_API int turbo_agent_runtime_list_observability_indexes(turbo_agent_runtime_t *runtime,
                                                             json_value_t **out_indexes_json) {
  return turbo_agent_runtime_list_observability_indexes_filtered(runtime, NULL, out_indexes_json);
}

CXX_C_API int turbo_agent_runtime_apply_command_json_value(turbo_agent_runtime_t *runtime,
                                                           const char *checkpoint_id,
                                                           const json_value_t *command,
                                                           json_value_t **out_state_override) {
  return turbo_agent_runtime_prepare_checkpoint_command_override_json_value(
      runtime, checkpoint_id, command, out_state_override);
}

CXX_C_API int turbo_agent_runtime_prepare_checkpoint_command_override_json_value(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id, const json_value_t *command,
    json_value_t **out_state_override) {
  json_value_t *state = NULL;
  json_value_t *state_json = NULL;
  json_value_t *command_json = NULL;
  json_value_t *updated_state = NULL;
  int rc = -1;

  if (!runtime || !checkpoint_id || !command || !out_state_override) {
    return -1;
  }
  *out_state_override = NULL;
  if (turbo_agent_runtime_get_checkpoint_state_json_value(runtime, checkpoint_id, &state) != 0 ||
      !state) {
    goto cleanup;
  }
  state_json = json_clone(state);
  command_json = json_clone(command);
  if (!state_json || !command_json ||
      turbo_agent_runtime_apply_command_json(state_json, command_json) != 0) {
    goto cleanup;
  }
  updated_state = json_clone(state_json);
  if (!updated_state) {
    goto cleanup;
  }
  *out_state_override = updated_state;
  updated_state = NULL;
  rc = 0;

cleanup:
  turbo_runtime_json_destroy(updated_state);
  turbo_agent_runtime_v1_json_free(&command_json);
  turbo_agent_runtime_v1_json_free(&state_json);
  turbo_runtime_json_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_runtime_prepare_thread_command_override_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id, const json_value_t *command,
    json_value_t **out_state_override) {
  char *checkpoint_id = NULL;
  int rc;

  if (!runtime || !thread_id || !thread_id[0] || !command || !out_state_override) {
    return -1;
  }
  rc = turbo_agent_runtime_current_checkpoint_id_for_thread(runtime, thread_id, &checkpoint_id);
  if (rc != 0 || !checkpoint_id) {
    free(checkpoint_id);
    return -1;
  }
  rc = turbo_agent_runtime_prepare_checkpoint_command_override_json_value(
      runtime, checkpoint_id, command, out_state_override);
  free(checkpoint_id);
  return rc;
}

CXX_C_API int turbo_agent_runtime_apply_state_patch_json_value(turbo_agent_runtime_t *runtime,
                                                               const char *checkpoint_id,
                                                               const json_value_t *state_patch,
                                                               json_value_t **out_state_override) {
  return turbo_agent_runtime_prepare_checkpoint_state_override_json_value(
      runtime, checkpoint_id, state_patch, out_state_override);
}

CXX_C_API int turbo_agent_runtime_prepare_checkpoint_state_override_json_value(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id, const json_value_t *state_patch,
    json_value_t **out_state_override) {
  json_value_t *state = NULL;
  int rc;

  if (!runtime || !checkpoint_id || !checkpoint_id[0] || !state_patch || !out_state_override) {
    return -1;
  }
  *out_state_override = NULL;
  rc = turbo_agent_runtime_get_checkpoint_state_json_value(runtime, checkpoint_id, &state);
  if (rc != 0 || !state) {
    turbo_runtime_json_destroy(state);
    return -1;
  }
  rc = turbo_agent_runtime_apply_state_patch_to_json_value(state, state_patch, out_state_override);
  turbo_runtime_json_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_runtime_prepare_thread_state_override_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id, const json_value_t *state_patch,
    json_value_t **out_state_override) {
  char *checkpoint_id = NULL;
  int rc;

  if (!runtime || !thread_id || !thread_id[0] || !state_patch || !out_state_override) {
    return -1;
  }
  rc = turbo_agent_runtime_current_checkpoint_id_for_thread(runtime, thread_id, &checkpoint_id);
  if (rc != 0 || !checkpoint_id) {
    free(checkpoint_id);
    return -1;
  }
  rc = turbo_agent_runtime_prepare_checkpoint_state_override_json_value(
      runtime, checkpoint_id, state_patch, out_state_override);
  free(checkpoint_id);
  return rc;
}

/* ── Unified API Implementations ───────────────────────────────────────────── */

static int turbo_agent_runtime_exec_resolve_override(
    turbo_agent_runtime_t *runtime, const turbo_agent_runtime_exec_options_t *exec_options,
    const json_value_t *input, char **out_checkpoint_id, json_value_t **out_override) {
  char *checkpoint_id = NULL;
  int rc;

  if (!exec_options) return -1;

  if (exec_options->scope == TURBO_RUNTIME_SCOPE_THREAD) {
    if (!exec_options->thread_id) return -1;
    rc = turbo_agent_runtime_current_checkpoint_id_for_thread(runtime, exec_options->thread_id,
                                                              &checkpoint_id);
    if (rc != 0 || !checkpoint_id) return -1;
  } else {
    if (!exec_options->checkpoint_id) return -1;
    checkpoint_id = turbo_agent_runtime_strdup(exec_options->checkpoint_id);
    if (!checkpoint_id) return -1;
  }

  if (exec_options->input_kind == TURBO_RUNTIME_INPUT_OVERRIDE) {
    *out_override = NULL;
    if (input) {
      *out_override = json_clone(input);
      if (!*out_override) {
        free(checkpoint_id);
        return -1;
      }
    }
  } else if (exec_options->input_kind == TURBO_RUNTIME_INPUT_PATCH) {
    rc = turbo_agent_runtime_prepare_checkpoint_state_override_json_value(runtime, checkpoint_id,
                                                                          input, out_override);
    if (rc != 0) {
      free(checkpoint_id);
      return -1;
    }
  } else if (exec_options->input_kind == TURBO_RUNTIME_INPUT_COMMAND) {
    rc = turbo_agent_runtime_prepare_checkpoint_command_override_json_value(runtime, checkpoint_id,
                                                                            input, out_override);
    if (rc != 0) {
      free(checkpoint_id);
      return -1;
    }
  } else {
    free(checkpoint_id);
    return -1;
  }

  *out_checkpoint_id = checkpoint_id;
  return 0;
}

CXX_C_API int turbo_agent_runtime_exec_start(turbo_agent_runtime_t *runtime, turbo_graph_t *graph,
                                             const json_value_t *state,
                                             const turbo_graph_run_options_t *options,
                                             const turbo_agent_runtime_exec_options_t *exec_options,
                                             json_value_t **out_summary_json,
                                             json_value_t **out_state) {
  return turbo_agent_runtime_exec_start_controlled(runtime, graph, state, options, exec_options,
                                                   NULL, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_runtime_exec_start_controlled(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const json_value_t *state,
    const turbo_graph_run_options_t *options,
    const turbo_agent_runtime_exec_options_t *exec_options,
    const turbo_cancel_token_t *cancel_token, json_value_t **out_summary_json,
    json_value_t **out_state) {
  const char *thread_id = NULL;
  turbo_event_sink_json_value_fn event_sink = NULL;
  void *event_sink_user_data = NULL;
  const turbo_agent_runtime_parent_link_t *parent_link = NULL;

  if (exec_options) {
    thread_id = exec_options->thread_id;
    event_sink = exec_options->event_sink;
    event_sink_user_data = exec_options->event_sink_user_data;
    parent_link = exec_options->parent_link;
  }

  return turbo_agent_runtime_start_json_value_graph_linked_stream_controlled(
      runtime, graph, state, options, thread_id, parent_link, cancel_token, event_sink,
      event_sink_user_data, out_summary_json, out_state);
}

CXX_C_API int
turbo_agent_runtime_exec_resume(turbo_agent_runtime_t *runtime, turbo_graph_t *graph,
                                const json_value_t *input, const turbo_graph_run_options_t *options,
                                const turbo_agent_runtime_exec_options_t *exec_options,
                                json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_runtime_exec_resume_controlled(runtime, graph, input, options, exec_options,
                                                    NULL, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_runtime_exec_resume_controlled(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const json_value_t *input,
    const turbo_graph_run_options_t *options,
    const turbo_agent_runtime_exec_options_t *exec_options,
    const turbo_cancel_token_t *cancel_token, json_value_t **out_summary_json,
    json_value_t **out_state) {
  char *checkpoint_id = NULL;
  json_value_t *override_state = NULL;
  turbo_event_sink_json_value_fn event_sink = NULL;
  void *event_sink_user_data = NULL;
  int rc;

  if (!runtime || !graph || !exec_options) {
    return -1;
  }

  rc = turbo_agent_runtime_exec_resolve_override(runtime, exec_options, input, &checkpoint_id,
                                                 &override_state);
  if (rc != 0) {
    return -1;
  }

  event_sink = exec_options->event_sink;
  event_sink_user_data = exec_options->event_sink_user_data;

  rc = turbo_agent_runtime_resume_json_value_graph_stream_controlled(
      runtime, graph, checkpoint_id, override_state, options, cancel_token, event_sink,
      event_sink_user_data, out_summary_json, out_state);

  turbo_runtime_json_destroy(override_state);
  free(checkpoint_id);
  return rc;
}

CXX_C_API int turbo_agent_runtime_exec_fork(turbo_agent_runtime_t *runtime, turbo_graph_t *graph,
                                            const json_value_t *input,
                                            const turbo_graph_run_options_t *options,
                                            const turbo_agent_runtime_exec_options_t *exec_options,
                                            json_value_t **out_summary_json,
                                            json_value_t **out_state) {
  return turbo_agent_runtime_exec_fork_controlled(runtime, graph, input, options, exec_options,
                                                  NULL, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_runtime_exec_fork_controlled(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const json_value_t *input,
    const turbo_graph_run_options_t *options,
    const turbo_agent_runtime_exec_options_t *exec_options,
    const turbo_cancel_token_t *cancel_token, json_value_t **out_summary_json,
    json_value_t **out_state) {
  char *checkpoint_id = NULL;
  json_value_t *override_state = NULL;
  turbo_event_sink_json_value_fn event_sink = NULL;
  void *event_sink_user_data = NULL;
  int rc;

  if (!runtime || !graph || !exec_options) {
    return -1;
  }

  rc = turbo_agent_runtime_exec_resolve_override(runtime, exec_options, input, &checkpoint_id,
                                                 &override_state);
  if (rc != 0) {
    return -1;
  }

  event_sink = exec_options->event_sink;
  event_sink_user_data = exec_options->event_sink_user_data;

  rc = turbo_agent_runtime_fork_json_value_graph_stream_controlled(
      runtime, graph, checkpoint_id, override_state, options, cancel_token, event_sink,
      event_sink_user_data, out_summary_json, out_state);

  turbo_runtime_json_destroy(override_state);
  free(checkpoint_id);
  return rc;
}
