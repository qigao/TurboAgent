#ifndef TURBO_AGENT_TEST_SUPPORT_H
#define TURBO_AGENT_TEST_SUPPORT_H

#include "tinytest.h"
#include "turbo_event.h"
#include "turbo_parser.h"
#include "turbo_runtime_data_bind.h"

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct turbo_agent_s turbo_agent_t;

static void turbo_agent_test_check_checkpoint_summary_shape(
    const json_value_t *summary, const char *expected_id, const char *expected_run_id,
    const char *expected_parent_checkpoint_id);

static int turbo_agent_test_fixture_path(const char *fixture_name, char *buffer,
                                         size_t buffer_size) {
  const char *this_file = __FILE__;
  const char *slash = strrchr(this_file, '\\');
  const char *alt_slash = strrchr(this_file, '/');
  const char *separator = slash > alt_slash ? slash : alt_slash;
  size_t directory_length;
  int written;

  if (!fixture_name || !buffer || buffer_size == 0) {
    return -1;
  }
  directory_length = separator ? (size_t)(separator - this_file) : strlen(this_file);
  written = snprintf(buffer, buffer_size, "%.*s%cfixtures%cruntime_v2%c%s",
                     (int)directory_length, this_file,
#ifdef _WIN32
                     '\\', '\\', '\\',
#else
                     '/', '/', '/',
#endif
                     fixture_name);
  return written > 0 && (size_t)written < buffer_size ? 0 : -1;
}

static json_value_t *turbo_agent_test_load_fixture_json(const char *fixture_name) {
  char path[1024];
  FILE *stream;
  char *buffer = NULL;
  long length;
  size_t read_count;
  json_value_t *json = NULL;

  check_int_eq(turbo_agent_test_fixture_path(fixture_name, path, sizeof(path)), 0);
  stream = fopen(path, "rb");
  check_not_null(stream);
  check_int_eq(fseek(stream, 0, SEEK_END), 0);
  length = ftell(stream);
  check_true(length >= 0);
  check_int_eq(fseek(stream, 0, SEEK_SET), 0);
  buffer = (char *)malloc((size_t)length + 1);
  check_not_null(buffer);
  read_count = fread(buffer, 1, (size_t)length, stream);
  check_size_eq(read_count, (size_t)length);
  buffer[length] = '\0';
  check_int_eq(turbo_parse_json((const uint8_t *)buffer, (size_t)length, &json), 0);
  fclose(stream);
  free(buffer);
  return json;
}

static const json_value_t *turbo_agent_test_find_named_fixture_entry(const json_value_t *fixture,
                                                                     const char *entry_name) {
  size_t i;

  check_not_null(fixture);
  check_not_null(entry_name);
  if (turbo_json_type(fixture) == TURBO_JSON_OBJECT) {
    const json_value_t *direct = turbo_json_object_get(fixture, entry_name);
    if (direct) {
      return direct;
    }
  }
  if (turbo_json_type(fixture) != TURBO_JSON_ARRAY) {
    return NULL;
  }
  for (i = 0; i < turbo_json_array_size(fixture); ++i) {
    const json_value_t *entry = turbo_json_array_get(fixture, i);
    const char *name = entry ? turbo_json_get_string(entry, "name") : NULL;

    if (name && strcmp(name, entry_name) == 0) {
      return entry;
    }
  }
  return NULL;
}

static const json_value_t *turbo_agent_test_find_command_descriptor(const json_value_t *summary,
                                                                    const char *name) {
  const json_value_t *descriptors;
  size_t count;
  size_t i;

  check_not_null(summary);
  check_not_null(name);
  descriptors = turbo_json_object_get(summary, "available_command_descriptors");
  check_not_null(descriptors);
  count = turbo_json_array_size(descriptors);
  for (i = 0; i < count; ++i) {
    const json_value_t *descriptor = turbo_json_array_get(descriptors, i);
    const char *descriptor_name;

    if (!descriptor) {
      continue;
    }
    descriptor_name = turbo_json_get_string(descriptor, "name");
    if (descriptor_name && strcmp(descriptor_name, name) == 0) {
      return descriptor;
    }
  }
  return NULL;
}

static void turbo_agent_test_check_string_array_contains_all(const json_value_t *array,
                                                             const char *const *values,
                                                             size_t value_count) {
  size_t i;
  size_t j;

  check_not_null(array);
  check_true(turbo_json_type(array) == TURBO_JSON_ARRAY);
  for (i = 0; i < value_count; ++i) {
    int found = 0;

    for (j = 0; j < turbo_json_array_size(array); ++j) {
      const json_value_t *entry = turbo_json_array_get(array, j);
      const char *entry_text = entry ? turbo_json_string(entry) : NULL;

      if (entry_text && strcmp(entry_text, values[i]) == 0) {
        found = 1;
        break;
      }
    }
    check_true(found);
  }
}

static void turbo_agent_test_check_string_array_equals_fixture(const json_value_t *actual_array,
                                                               const json_value_t *expected_array) {
  size_t i;
  size_t expected_count;

  check_not_null(actual_array);
  check_not_null(expected_array);
  check_true(turbo_json_type(actual_array) == TURBO_JSON_ARRAY);
  check_true(turbo_json_type(expected_array) == TURBO_JSON_ARRAY);
  expected_count = turbo_json_array_size(expected_array);
  check_size_eq(turbo_json_array_size(actual_array), expected_count);
  for (i = 0; i < expected_count; ++i) {
    const json_value_t *entry = turbo_json_array_get(expected_array, i);
    const char *entry_text = entry ? turbo_json_string(entry) : NULL;

    check_not_null(entry_text);
    check_true(turbo_json_type(entry) == TURBO_JSON_STRING);
    turbo_agent_test_check_string_array_contains_all(actual_array, &entry_text, 1);
  }
}

typedef struct turbo_agent_test_replay_capture_s {
  size_t count;
  size_t node_start_count;
  size_t interrupted_count;
  size_t completed_count;
  size_t model_count;
  size_t trace_count;
  size_t tool_result_count;
} turbo_agent_test_replay_capture_t;

static void turbo_agent_test_capture_replayed_history_event(
    const turbo_runtime_data_bind_value_t *event, void *user_data) {
  turbo_agent_test_replay_capture_t *capture =
      (turbo_agent_test_replay_capture_t *)user_data;
  const char *type;
  const char *kind;

  check_not_null(event);
  check_not_null(capture);
  capture->count++;

  type = turbo_runtime_data_bind_value_as_string(
      turbo_runtime_data_bind_object_get(event, "type"));
  if (type) {
    if (strcmp(type, "node_start") == 0) {
      capture->node_start_count++;
    } else if (strcmp(type, "interrupted") == 0) {
      capture->interrupted_count++;
    } else if (strcmp(type, "completed") == 0) {
      capture->completed_count++;
    }
  }

  kind = turbo_runtime_data_bind_value_as_string(
      turbo_runtime_data_bind_object_get(event, "kind"));
  if (kind) {
    if (strcmp(kind, "model") == 0) {
      capture->model_count++;
    } else if (strcmp(kind, "trace") == 0) {
      capture->trace_count++;
    } else if (strcmp(kind, "tool_result") == 0) {
      capture->tool_result_count++;
    }
  }
}

typedef struct turbo_agent_test_observer_capture_s {
  size_t count;
  size_t model_delta_count;
  size_t tool_call_started_count;
  size_t tool_result_count;
  size_t state_updated_count;
  size_t interrupted_count;
  size_t completed_count;
} turbo_agent_test_observer_capture_t;

static void turbo_agent_test_capture_observer_event(
    const turbo_runtime_data_bind_value_t *event, void *user_data) {
  turbo_agent_test_observer_capture_t *capture =
      (turbo_agent_test_observer_capture_t *)user_data;
  const char *kind;
  const char *type;

  check_not_null(event);
  check_not_null(capture);
  capture->count++;

  kind = turbo_runtime_data_bind_value_as_string(
      turbo_runtime_data_bind_object_get(event, "kind"));
  type = turbo_runtime_data_bind_value_as_string(
      turbo_runtime_data_bind_object_get(event, "type"));
  check_str_eq(kind, "observer");
  check_not_null(turbo_runtime_data_bind_object_get(event, "event"));

  if (!type) {
    return;
  }
  if (strcmp(type, "model_delta") == 0) {
    capture->model_delta_count++;
  } else if (strcmp(type, "tool_call_started") == 0) {
    capture->tool_call_started_count++;
  } else if (strcmp(type, "tool_result") == 0) {
    capture->tool_result_count++;
  } else if (strcmp(type, "state_updated") == 0) {
    capture->state_updated_count++;
  } else if (strcmp(type, "interrupted") == 0) {
    capture->interrupted_count++;
  } else if (strcmp(type, "completed") == 0) {
    capture->completed_count++;
  } else {
    check_true(0);
  }
}

typedef struct turbo_agent_test_trace_capture_s {
  size_t count;
  size_t trace_count;
  size_t model_request_count;
  size_t model_response_count;
} turbo_agent_test_trace_capture_t;

static void turbo_agent_test_capture_trace_event(turbo_agent_t *agent_unused,
                                                 const turbo_runtime_data_bind_value_t *event,
                                                 void *user_data) {
  turbo_agent_test_trace_capture_t *capture =
      (turbo_agent_test_trace_capture_t *)user_data;
  const char *kind;
  const char *name;

  (void)agent_unused;
  check_not_null(event);
  check_not_null(capture);
  capture->count++;
  kind = turbo_runtime_data_bind_value_as_string(
      turbo_runtime_data_bind_object_get(event, "kind"));
  if (kind && strcmp(kind, "trace") == 0) {
    capture->trace_count++;
  }
  name = turbo_runtime_data_bind_value_as_string(
      turbo_runtime_data_bind_object_get(event, "name"));
  if (name) {
    if (strcmp(name, "model_request") == 0) {
      capture->model_request_count++;
    } else if (strcmp(name, "model_response") == 0) {
      capture->model_response_count++;
    }
  }
}

static void turbo_agent_test_check_thread_timeline_bind(
    const turbo_runtime_data_bind_value_t *timeline, const char *thread_id,
    const char *latest_run_id, const char *resolved_current_checkpoint_id,
    int pending_run_expected, size_t expected_run_count,
    size_t expected_checkpoint_count, size_t minimum_history_event_count) {
  const turbo_runtime_data_bind_value_t *thread;
  const turbo_runtime_data_bind_value_t *latest_run;
  const turbo_runtime_data_bind_value_t *pending_run;
  const turbo_runtime_data_bind_value_t *resolved_current_run;
  const turbo_runtime_data_bind_value_t *resolved_current_checkpoint_id_value;
  const turbo_runtime_data_bind_value_t *resolved_current_checkpoint;
  const turbo_runtime_data_bind_value_t *runs;
  const turbo_runtime_data_bind_value_t *current_run_checkpoints;
  const turbo_runtime_data_bind_value_t *history_events;
  json_value_t *resolved_current_checkpoint_json = NULL;

  check_not_null(timeline);
  check_not_null(thread_id);
  check_not_null(latest_run_id);
  thread = turbo_runtime_data_bind_object_get(timeline, "thread");
  latest_run = turbo_runtime_data_bind_object_get(timeline, "latest_run");
  pending_run = turbo_runtime_data_bind_object_get(timeline, "pending_run");
  resolved_current_run = turbo_runtime_data_bind_object_get(timeline, "resolved_current_run");
  resolved_current_checkpoint_id_value =
      turbo_runtime_data_bind_object_get(timeline, "resolved_current_checkpoint_id");
  resolved_current_checkpoint =
      turbo_runtime_data_bind_object_get(timeline, "resolved_current_checkpoint");
  runs = turbo_runtime_data_bind_object_get(timeline, "runs");
  current_run_checkpoints = turbo_runtime_data_bind_object_get(timeline, "current_run_checkpoints");
  history_events = turbo_runtime_data_bind_object_get(timeline, "history_events");

  check_not_null(thread);
  check_not_null(latest_run);
  check_not_null(pending_run);
  check_not_null(resolved_current_run);
  check_not_null(resolved_current_checkpoint_id_value);
  check_not_null(resolved_current_checkpoint);
  check_not_null(runs);
  check_not_null(current_run_checkpoints);
  check_not_null(history_events);
  check_true(turbo_runtime_data_bind_value_kind(thread) == TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT);
  check_true(turbo_runtime_data_bind_value_kind(latest_run) == TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT);
  check_true(turbo_runtime_data_bind_value_kind(resolved_current_run) ==
              TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT);
  check_true(turbo_runtime_data_bind_value_kind(runs) == TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY);
  check_true(turbo_runtime_data_bind_value_kind(current_run_checkpoints) ==
              TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY);
  check_true(turbo_runtime_data_bind_value_kind(history_events) ==
              TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY);
  check_str_eq(
      turbo_runtime_data_bind_value_as_string(turbo_runtime_data_bind_object_get(thread, "id")),
      thread_id);
  check_str_eq(
      turbo_runtime_data_bind_value_as_string(turbo_runtime_data_bind_object_get(latest_run, "id")),
      latest_run_id);
  check_str_eq(
      turbo_runtime_data_bind_value_as_string(
          turbo_runtime_data_bind_object_get(resolved_current_run, "id")),
      latest_run_id);
  if (resolved_current_checkpoint_id) {
    check_str_eq(turbo_runtime_data_bind_value_as_string(resolved_current_checkpoint_id_value),
                 resolved_current_checkpoint_id);
    check_true(turbo_runtime_data_bind_value_kind(resolved_current_checkpoint) ==
               TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(resolved_current_checkpoint, "id")),
                 resolved_current_checkpoint_id);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(resolved_current_checkpoint, "run_id")),
                 latest_run_id);
    resolved_current_checkpoint_json =
        turbo_runtime_data_bind_value_to_json(resolved_current_checkpoint);
    check_not_null(resolved_current_checkpoint_json);
    turbo_agent_test_check_checkpoint_summary_shape(resolved_current_checkpoint_json,
                                                    resolved_current_checkpoint_id, latest_run_id,
                                                    NULL);
  } else {
    check_true(turbo_runtime_data_bind_value_kind(resolved_current_checkpoint_id_value) ==
               TURBO_RUNTIME_DATA_BIND_VALUE_NULL);
    check_true(turbo_runtime_data_bind_value_kind(resolved_current_checkpoint) ==
               TURBO_RUNTIME_DATA_BIND_VALUE_NULL);
  }
  check_size_eq(turbo_runtime_data_bind_value_size(runs), expected_run_count);
  check_size_eq(turbo_runtime_data_bind_value_size(current_run_checkpoints),
                expected_checkpoint_count);
  check_true(turbo_runtime_data_bind_value_size(history_events) >= minimum_history_event_count);

  if (pending_run_expected) {
    check_true(turbo_runtime_data_bind_value_kind(pending_run) ==
               TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT);
    check_str_eq(
        turbo_runtime_data_bind_value_as_string(
            turbo_runtime_data_bind_object_get(pending_run, "id")),
        latest_run_id);
  } else {
    check_true(turbo_runtime_data_bind_value_kind(pending_run) == TURBO_RUNTIME_DATA_BIND_VALUE_NULL);
  }
  turbo_free_json(&resolved_current_checkpoint_json);
}

static void turbo_agent_test_check_lineage_string_field(const json_value_t *object,
                                                        const char *field,
                                                        const char *expected_value) {
  const json_value_t *value;

  check_not_null(object);
  check_not_null(field);
  value = turbo_json_object_get(object, field);
  check_not_null(value);
  if (expected_value) {
    check_str_eq(turbo_json_get_string(object, field), expected_value);
  } else {
    check_true(turbo_json_type(value) == TURBO_JSON_NULL);
  }
}

static const json_value_t *turbo_agent_test_find_lineage_branch(const json_value_t *branches,
                                                                const char *parent_run_id) {
  size_t i;

  check_not_null(branches);
  check_true(turbo_json_type(branches) == TURBO_JSON_ARRAY);
  for (i = 0; i < turbo_json_array_size(branches); ++i) {
    const json_value_t *branch = turbo_json_array_get(branches, i);
    const json_value_t *branch_parent_run_id;
    const char *actual_parent_run_id;

    if (!branch) {
      continue;
    }
    branch_parent_run_id = turbo_json_object_get(branch, "parent_run_id");
    if (!branch_parent_run_id) {
      continue;
    }
    if (parent_run_id) {
      actual_parent_run_id = turbo_json_get_string(branch, "parent_run_id");
      if (actual_parent_run_id && strcmp(actual_parent_run_id, parent_run_id) == 0) {
        return branch;
      }
    } else if (turbo_json_type(branch_parent_run_id) == TURBO_JSON_NULL) {
      return branch;
    }
  }
  return NULL;
}

static void turbo_agent_test_check_lineage_branch(const json_value_t *branch,
                                                  const char *parent_run_id,
                                                  const char *forked_from_checkpoint_id,
                                                  const char *parent_checkpoint_id,
                                                  const char *branch_root_checkpoint_id) {
  check_not_null(branch);
  check_true(turbo_json_type(branch) == TURBO_JSON_OBJECT);
  turbo_agent_test_check_lineage_string_field(branch, "parent_run_id", parent_run_id);
  turbo_agent_test_check_lineage_string_field(branch, "forked_from_checkpoint_id",
                                              forked_from_checkpoint_id);
  turbo_agent_test_check_lineage_string_field(branch, "parent_checkpoint_id",
                                              parent_checkpoint_id);
  turbo_agent_test_check_lineage_string_field(branch, "branch_root_checkpoint_id",
                                              branch_root_checkpoint_id);
}

static const char *turbo_agent_test_optional_string_field(const json_value_t *object,
                                                          const char *field) {
  const json_value_t *value;

  check_not_null(object);
  check_not_null(field);
  value = turbo_json_object_get(object, field);
  check_not_null(value);
  return turbo_json_type(value) == TURBO_JSON_NULL ? NULL : turbo_json_get_string(object, field);
}

static void turbo_agent_test_check_checkpoint_summary_shape(
    const json_value_t *summary, const char *expected_id, const char *expected_run_id,
    const char *expected_parent_checkpoint_id) {
  const json_value_t *run_id;
  const json_value_t *parent_checkpoint_id;
  const json_value_t *seq;
  const json_value_t *created_at;
  const json_value_t *status;
  const json_value_t *next_node;
  const json_value_t *steps;

  check_not_null(summary);
  if (!expected_id) {
    check_true(turbo_json_type(summary) == TURBO_JSON_NULL);
    return;
  }

  check_true(turbo_json_type(summary) == TURBO_JSON_OBJECT);
  turbo_agent_test_check_lineage_string_field(summary, "id", expected_id);
  run_id = turbo_json_object_get(summary, "run_id");
  check_not_null(run_id);
  if (expected_run_id) {
    turbo_agent_test_check_lineage_string_field(summary, "run_id", expected_run_id);
  } else {
    check_true(turbo_json_type(run_id) == TURBO_JSON_STRING ||
               turbo_json_type(run_id) == TURBO_JSON_NULL);
  }
  parent_checkpoint_id = turbo_json_object_get(summary, "parent_checkpoint_id");
  check_not_null(parent_checkpoint_id);
  if (expected_parent_checkpoint_id) {
    turbo_agent_test_check_lineage_string_field(summary, "parent_checkpoint_id",
                                                expected_parent_checkpoint_id);
  } else {
    check_true(turbo_json_type(parent_checkpoint_id) == TURBO_JSON_STRING ||
               turbo_json_type(parent_checkpoint_id) == TURBO_JSON_NULL);
  }
  seq = turbo_json_object_get(summary, "seq");
  created_at = turbo_json_object_get(summary, "created_at");
  status = turbo_json_object_get(summary, "status");
  next_node = turbo_json_object_get(summary, "next_node");
  steps = turbo_json_object_get(summary, "steps");
  check_not_null(seq);
  check_not_null(created_at);
  check_not_null(status);
  check_not_null(next_node);
  check_not_null(steps);
  check_true(turbo_json_type(seq) == TURBO_JSON_NUMBER || turbo_json_type(seq) == TURBO_JSON_NULL);
  check_true(turbo_json_type(created_at) == TURBO_JSON_STRING ||
             turbo_json_type(created_at) == TURBO_JSON_NULL);
  check_true(turbo_json_type(status) == TURBO_JSON_STRING ||
             turbo_json_type(status) == TURBO_JSON_NULL);
  check_true(turbo_json_type(next_node) == TURBO_JSON_STRING ||
             turbo_json_type(next_node) == TURBO_JSON_NULL);
  check_true(turbo_json_type(steps) == TURBO_JSON_NUMBER ||
             turbo_json_type(steps) == TURBO_JSON_NULL);
}

static void turbo_agent_test_check_checkpoint_context(
    const json_value_t *context, const char *thread_id, const char *run_id,
    const char *checkpoint_id, const char *parent_checkpoint_id,
    size_t minimum_history_event_count) {
  const json_value_t *thread;
  const json_value_t *run;
  const json_value_t *checkpoint_summary;
  const json_value_t *state;
  const json_value_t *ancestor_checkpoints;
  const json_value_t *history_events;

  check_not_null(context);
  check_true(turbo_json_type(context) == TURBO_JSON_OBJECT);
  thread = turbo_json_object_get(context, "thread");
  run = turbo_json_object_get(context, "run");
  checkpoint_summary = turbo_json_object_get(context, "checkpoint_summary");
  state = turbo_json_object_get(context, "state");
  ancestor_checkpoints = turbo_json_object_get(context, "ancestor_checkpoints");
  history_events = turbo_json_object_get(context, "history_events");
  check_not_null(thread);
  check_not_null(run);
  check_not_null(checkpoint_summary);
  check_not_null(state);
  check_not_null(history_events);
  check_true(turbo_json_type(thread) == TURBO_JSON_OBJECT);
  check_true(turbo_json_type(run) == TURBO_JSON_OBJECT);
  check_true(turbo_json_type(checkpoint_summary) == TURBO_JSON_OBJECT);
  check_true(turbo_json_type(state) == TURBO_JSON_OBJECT);
  check_true(turbo_json_type(history_events) == TURBO_JSON_ARRAY);
  turbo_agent_test_check_lineage_string_field(thread, "id", thread_id);
  turbo_agent_test_check_lineage_string_field(run, "id", run_id);
  turbo_agent_test_check_checkpoint_summary_shape(checkpoint_summary, checkpoint_id, run_id,
                                                  parent_checkpoint_id);
  check_true(turbo_json_object_size(state) > 0);
  check_true(!ancestor_checkpoints || turbo_json_type(ancestor_checkpoints) == TURBO_JSON_NULL);
  check_true(turbo_json_array_size(history_events) >= minimum_history_event_count);
}

static void turbo_agent_test_check_supervisor_inspect(const json_value_t *inspect,
                                                      const char *active_agent,
                                                      const char *target_agent,
                                                      const char *handoff_reason,
                                                      size_t inbox_count,
                                                      size_t handoff_count) {
  const json_value_t *supervisor;
  const json_value_t *inbox;
  const json_value_t *history;
  const json_value_t *control;
  const json_value_t *workflow;

  check_not_null(inspect);
  check_true(turbo_json_type(inspect) == TURBO_JSON_OBJECT);
  supervisor = turbo_json_object_get(inspect, "supervisor");
  inbox = turbo_json_object_get(inspect, "inbox");
  history = turbo_json_object_get(inspect, "handoff_history");
  control = turbo_json_object_get(inspect, "control_snapshot");
  workflow = turbo_json_object_get(inspect, "workflow_snapshot");
  check_not_null(supervisor);
  check_not_null(inbox);
  check_not_null(history);
  check_not_null(control);
  check_not_null(workflow);
  check_true(turbo_json_type(supervisor) == TURBO_JSON_OBJECT);
  check_true(turbo_json_type(inbox) == TURBO_JSON_ARRAY);
  check_true(turbo_json_type(history) == TURBO_JSON_ARRAY);
  check_true(turbo_json_type(control) == TURBO_JSON_OBJECT);
  check_true(turbo_json_type(workflow) == TURBO_JSON_OBJECT);
  turbo_agent_test_check_lineage_string_field(supervisor, "active_agent", active_agent);
  turbo_agent_test_check_lineage_string_field(supervisor, "target_agent", target_agent);
  turbo_agent_test_check_lineage_string_field(supervisor, "handoff_reason", handoff_reason);
  check_int_eq(turbo_json_get_int(supervisor, "inbox_count", -1), (int)inbox_count);
  check_int_eq(turbo_json_get_int(supervisor, "handoff_count", -1), (int)handoff_count);
  check_size_eq(turbo_json_array_size(inbox), inbox_count);
  check_size_eq(turbo_json_array_size(history), handoff_count);
  check_not_null(turbo_json_object_get(control, "supervisor"));
  check_not_null(turbo_json_object_get(workflow, "supervisor"));
}

static void turbo_agent_test_check_thread_lineage_bind(const json_value_t *lineage,
                                                       const char *thread_id,
                                                       const char *latest_run_id,
                                                       const char *pending_run_id,
                                                       const char *root_checkpoint_id) {
  const json_value_t *branches;

  check_not_null(lineage);
  check_str_eq(turbo_json_get_string(lineage, "thread_id"), thread_id);
  check_str_eq(turbo_json_get_string(lineage, "latest_run_id"), latest_run_id);
  turbo_agent_test_check_lineage_string_field(lineage, "pending_run_id", pending_run_id);
  turbo_agent_test_check_lineage_string_field(lineage, "root_checkpoint_id", root_checkpoint_id);
  branches = turbo_json_object_get(lineage, "branches");
  check_not_null(branches);
  check_true(turbo_json_type(branches) == TURBO_JSON_ARRAY);
}

static const json_value_t *turbo_agent_test_find_branch_tree_branch(const json_value_t *branches,
                                                                    const char *current_run_id) {
  size_t i;

  check_not_null(branches);
  check_true(turbo_json_type(branches) == TURBO_JSON_ARRAY);
  for (i = 0; i < turbo_json_array_size(branches); ++i) {
    const json_value_t *branch = turbo_json_array_get(branches, i);
    const json_value_t *current_run_value;
    const char *actual_current_run_id;
    const char *run_field;

    if (!branch) {
      continue;
    }
    current_run_value = turbo_json_object_get(branch, "current_run_id");
    run_field = current_run_value ? "current_run_id" : "run_id";
    current_run_value = turbo_json_object_get(branch, run_field);
    if (!current_run_value) {
      continue;
    }
    if (current_run_id) {
      actual_current_run_id = turbo_json_get_string(branch, run_field);
      if (actual_current_run_id && strcmp(actual_current_run_id, current_run_id) == 0) {
        return branch;
      }
    } else if (turbo_json_type(current_run_value) == TURBO_JSON_NULL) {
      return branch;
    }
  }
  return NULL;
}

static const json_value_t *turbo_agent_test_find_branch_tree_edge(const json_value_t *edges,
                                                                  const char *source_checkpoint_id,
                                                                  const char *target_run_id) {
  size_t i;

  check_not_null(edges);
  check_true(turbo_json_type(edges) == TURBO_JSON_ARRAY);
  for (i = 0; i < turbo_json_array_size(edges); ++i) {
    const json_value_t *edge = turbo_json_array_get(edges, i);
    const json_value_t *source_checkpoint_value;
    const char *actual_source_checkpoint_id;

    if (!edge) {
      continue;
    }
    source_checkpoint_value = turbo_json_object_get(edge, "source_checkpoint_id");
    if (!source_checkpoint_value) {
      continue;
    }
    if (source_checkpoint_id) {
      actual_source_checkpoint_id = turbo_json_get_string(edge, "source_checkpoint_id");
      if (!actual_source_checkpoint_id ||
          strcmp(actual_source_checkpoint_id, source_checkpoint_id) != 0) {
        continue;
      }
    } else if (turbo_json_type(source_checkpoint_value) != TURBO_JSON_NULL) {
      continue;
    }
    if (target_run_id) {
      const char *actual_target_run_id = turbo_json_get_string(edge, "target_run_id");

      if (actual_target_run_id && strcmp(actual_target_run_id, target_run_id) == 0) {
        return edge;
      }
    } else if (turbo_json_type(turbo_json_object_get(edge, "target_run_id")) ==
               TURBO_JSON_NULL) {
      return edge;
    }
  }
  return NULL;
}

static void turbo_agent_test_check_branch_tree_branch(const json_value_t *branch,
                                                      const char *current_run_id,
                                                      const char *parent_run_id,
                                                      const char *source_checkpoint_id,
                                                      const char *branch_root_checkpoint_id) {
  const json_value_t *checkpoint_summary;
  const json_value_t *source_checkpoint_summary;
  const char *actual_source_checkpoint_id;
  const char *actual_checkpoint_id;
  const char *actual_source_run_id;
  const char *run_field;

  check_not_null(branch);
  check_true(turbo_json_type(branch) == TURBO_JSON_OBJECT);
  run_field = turbo_json_object_get(branch, "current_run_id") ? "current_run_id" : "run_id";
  turbo_agent_test_check_lineage_string_field(branch, run_field, current_run_id);
  turbo_agent_test_check_lineage_string_field(branch, "parent_run_id", parent_run_id);
  actual_checkpoint_id = turbo_agent_test_optional_string_field(branch, "checkpoint_id");
  actual_source_checkpoint_id = turbo_agent_test_optional_string_field(branch,
                                                                      "source_checkpoint_id");
  checkpoint_summary = turbo_json_object_get(branch, "checkpoint_summary");
  check_not_null(checkpoint_summary);
  turbo_agent_test_check_checkpoint_summary_shape(checkpoint_summary, actual_checkpoint_id,
                                                  current_run_id, actual_source_checkpoint_id);
  source_checkpoint_summary = turbo_json_object_get(branch, "source_checkpoint_summary");
  check_not_null(source_checkpoint_summary);
  actual_source_run_id = parent_run_id;
  turbo_agent_test_check_checkpoint_summary_shape(source_checkpoint_summary,
                                                  actual_source_checkpoint_id,
                                                  actual_source_run_id, NULL);
  turbo_agent_test_check_lineage_string_field(branch, "branch_root_checkpoint_id",
                                              branch_root_checkpoint_id);
}

static void turbo_agent_test_check_branch_tree_edge(const json_value_t *edge,
                                                    const char *source_checkpoint_id,
                                                    const char *target_run_id) {
  const json_value_t *source_checkpoint_summary;
  const char *actual_source_checkpoint_id;

  check_not_null(edge);
  check_true(turbo_json_type(edge) == TURBO_JSON_OBJECT);
  turbo_agent_test_check_lineage_string_field(edge, "source_checkpoint_id",
                                              source_checkpoint_id);
  actual_source_checkpoint_id = turbo_agent_test_optional_string_field(edge,
                                                                      "source_checkpoint_id");
  source_checkpoint_summary = turbo_json_object_get(edge, "source_checkpoint_summary");
  check_not_null(source_checkpoint_summary);
  turbo_agent_test_check_checkpoint_summary_shape(source_checkpoint_summary,
                                                  actual_source_checkpoint_id, NULL, NULL);
  turbo_agent_test_check_lineage_string_field(edge, "target_run_id", target_run_id);
}

static void turbo_agent_test_check_branch_tree(const json_value_t *tree,
                                               const char *thread_id,
                                               const char *current_run_id,
                                               const char *latest_run_id,
                                               const char *pending_run_id,
                                               const char *current_checkpoint_id,
                                               size_t minimum_branch_count,
                                               size_t minimum_edge_count) {
  const json_value_t *branches;
  const json_value_t *edges;
  const json_value_t *current_branch;
  const json_value_t *current_checkpoint_summary;
  const char *current_branch_source_checkpoint_id = NULL;

  check_not_null(tree);
  check_str_eq(turbo_json_get_string(tree, "thread_id"), thread_id);
  check_str_eq(turbo_json_get_string(tree, "current_run_id"), current_run_id);
  check_str_eq(turbo_json_get_string(tree, "latest_run_id"), latest_run_id);
  turbo_agent_test_check_lineage_string_field(tree, "pending_run_id", pending_run_id);
  turbo_agent_test_check_lineage_string_field(tree, "current_checkpoint_id",
                                              current_checkpoint_id);
  branches = turbo_json_object_get(tree, "branches");
  edges = turbo_json_object_get(tree, "edges");
  current_branch = turbo_json_object_get(tree, "current_branch");
  current_checkpoint_summary = turbo_json_object_get(tree, "current_checkpoint_summary");
  check_not_null(branches);
  check_not_null(edges);
  check_not_null(current_branch);
  check_not_null(current_checkpoint_summary);
  check_true(turbo_json_type(branches) == TURBO_JSON_ARRAY);
  check_true(turbo_json_type(edges) == TURBO_JSON_ARRAY);
  if (current_run_id) {
    check_true(turbo_json_type(current_branch) == TURBO_JSON_OBJECT);
    turbo_agent_test_check_branch_tree_branch(
        current_branch, current_run_id, turbo_json_get_string(current_branch, "parent_run_id"),
        turbo_json_get_string(current_branch, "source_checkpoint_id"),
        turbo_json_get_string(current_branch, "branch_root_checkpoint_id"));
    turbo_agent_test_check_lineage_string_field(current_branch, "checkpoint_id",
                                                current_checkpoint_id);
    current_branch_source_checkpoint_id =
        turbo_json_get_string(current_branch, "source_checkpoint_id");
  } else {
    check_true(turbo_json_type(current_branch) == TURBO_JSON_NULL);
  }
  turbo_agent_test_check_checkpoint_summary_shape(current_checkpoint_summary,
                                                  current_checkpoint_id, current_run_id,
                                                  current_run_id ? current_branch_source_checkpoint_id
                                                                : NULL);
  check_true(turbo_json_array_size(branches) >= minimum_branch_count);
  check_true(turbo_json_array_size(edges) >= minimum_edge_count);
}

static void turbo_agent_test_check_orchestration_inspect(
    const json_value_t *inspect, const char *thread_id, const char *run_id,
    const char *checkpoint_id, size_t minimum_child_run_count,
    const char *active_agent, const char *target_agent, const char *handoff_reason) {
  const json_value_t *supervisor_inspect;
  const json_value_t *thread_lineage;
  const json_value_t *branch_tree;
  const json_value_t *child_runs;
  turbo_runtime_data_bind_value_t *timeline = NULL;

  check_not_null(inspect);
  check_true(turbo_json_type(inspect) == TURBO_JSON_OBJECT);
  supervisor_inspect = turbo_json_object_get(inspect, "supervisor_inspect");
  thread_lineage = turbo_json_object_get(inspect, "thread_lineage");
  branch_tree = turbo_json_object_get(inspect, "branch_tree");
  child_runs = turbo_json_object_get(inspect, "child_runs");
  check_not_null(supervisor_inspect);
  check_not_null(thread_lineage);
  check_not_null(branch_tree);
  check_not_null(child_runs);
  turbo_agent_test_check_supervisor_inspect(supervisor_inspect, active_agent, target_agent,
                                            handoff_reason, 0, 1);
  timeline = turbo_runtime_data_bind_value_from_json(turbo_json_object_get(inspect, "thread_timeline"));
  check_not_null(timeline);
  turbo_agent_test_check_thread_timeline_bind(timeline, thread_id, run_id, checkpoint_id, 1, 1,
                                              1, 1);
  turbo_agent_test_check_thread_lineage_bind(thread_lineage, thread_id, run_id, run_id,
                                             checkpoint_id);
  turbo_agent_test_check_branch_tree(branch_tree, thread_id, run_id, run_id, run_id,
                                     checkpoint_id, 1, 0);
  check_true(turbo_json_type(child_runs) == TURBO_JSON_ARRAY);
  check_true(turbo_json_array_size(child_runs) >= minimum_child_run_count);
  turbo_runtime_data_bind_value_destroy(timeline);
}

static void turbo_agent_test_check_child_inspect(const json_value_t *inspect,
                                                 const char *thread_id, const char *run_id,
                                                 const char *checkpoint_id,
                                                 size_t minimum_history_event_count) {
  const json_value_t *run;
  const json_value_t *checkpoints;
  const json_value_t *latest_checkpoint;
  const json_value_t *checkpoint_context;
  const json_value_t *history_events;
  const json_value_t *trace_events;
  const json_value_t *branch_tree;
  turbo_runtime_data_bind_value_t *timeline = NULL;

  check_not_null(inspect);
  check_true(turbo_json_type(inspect) == TURBO_JSON_OBJECT);
  run = turbo_json_object_get(inspect, "run");
  checkpoints = turbo_json_object_get(inspect, "checkpoints");
  latest_checkpoint = turbo_json_object_get(inspect, "latest_checkpoint");
  checkpoint_context = turbo_json_object_get(inspect, "checkpoint_context");
  history_events = turbo_json_object_get(inspect, "history_events");
  trace_events = turbo_json_object_get(inspect, "trace_events");
  branch_tree = turbo_json_object_get(inspect, "branch_tree");
  check_not_null(run);
  check_not_null(checkpoints);
  check_not_null(latest_checkpoint);
  check_not_null(checkpoint_context);
  check_not_null(history_events);
  check_not_null(trace_events);
  check_not_null(branch_tree);
  turbo_agent_test_check_lineage_string_field(run, "id", run_id);
  check_true(turbo_json_type(checkpoints) == TURBO_JSON_ARRAY);
  check_true(turbo_json_array_size(checkpoints) >= 1);
  turbo_agent_test_check_lineage_string_field(latest_checkpoint, "id", checkpoint_id);
  turbo_agent_test_check_checkpoint_context(checkpoint_context, thread_id, run_id, checkpoint_id,
                                            NULL, minimum_history_event_count);
  check_true(turbo_json_type(history_events) == TURBO_JSON_ARRAY);
  check_true(turbo_json_array_size(history_events) >= minimum_history_event_count);
  check_true(turbo_json_type(trace_events) == TURBO_JSON_ARRAY);
  timeline = turbo_runtime_data_bind_value_from_json(turbo_json_object_get(inspect, "thread_timeline"));
  check_not_null(timeline);
  turbo_agent_test_check_thread_timeline_bind(timeline, thread_id, run_id, checkpoint_id, 1, 1,
                                              1, 1);
  turbo_agent_test_check_branch_tree(branch_tree, thread_id, run_id, run_id, run_id,
                                     checkpoint_id, 1, 0);
  turbo_runtime_data_bind_value_destroy(timeline);
}

static void turbo_agent_test_check_child_orchestration_inspect(
    const json_value_t *inspect, const char *parent_agent_run_id,
    const char *parent_tool_call_id, const char *parent_tool_name,
    const char *thread_id, const char *run_id, const char *checkpoint_id,
    size_t minimum_history_event_count) {
  const json_value_t *child_inspect;

  check_not_null(inspect);
  check_true(turbo_json_type(inspect) == TURBO_JSON_OBJECT);
  turbo_agent_test_check_lineage_string_field(inspect, "parent_agent_run_id",
                                              parent_agent_run_id);
  turbo_agent_test_check_lineage_string_field(inspect, "parent_tool_call_id",
                                              parent_tool_call_id);
  turbo_agent_test_check_lineage_string_field(inspect, "parent_tool_name", parent_tool_name);
  turbo_agent_test_check_lineage_string_field(inspect, "parent_graph_run_id",
                                              parent_agent_run_id);
  turbo_agent_test_check_lineage_string_field(inspect, "call_frame_id",
                                              parent_tool_call_id);
  child_inspect = turbo_json_object_get(inspect, "child_inspect");
  check_not_null(child_inspect);
  turbo_agent_test_check_child_inspect(child_inspect, thread_id, run_id, checkpoint_id,
                                       minimum_history_event_count);
}

static void turbo_agent_test_check_child_multi_agent_inspect(
    const json_value_t *inspect, const char *thread_id, const char *run_id,
    const char *checkpoint_id, const char *parent_agent_run_id,
    const char *parent_tool_call_id, const char *parent_tool_name) {
  const json_value_t *supervisor_inspect;
  const json_value_t *orchestration_inspect;
  const json_value_t *child_orchestration_inspect;

  check_not_null(inspect);
  check_true(turbo_json_type(inspect) == TURBO_JSON_OBJECT);
  supervisor_inspect = turbo_json_object_get(inspect, "supervisor_inspect");
  orchestration_inspect = turbo_json_object_get(inspect, "orchestration_inspect");
  child_orchestration_inspect = turbo_json_object_get(inspect, "child_orchestration_inspect");
  check_not_null(supervisor_inspect);
  check_not_null(orchestration_inspect);
  check_not_null(child_orchestration_inspect);
  turbo_agent_test_check_supervisor_inspect(supervisor_inspect, "planner", "executor",
                                            "delegate execution", 0, 1);
  turbo_agent_test_check_orchestration_inspect(orchestration_inspect, thread_id, run_id,
                                               checkpoint_id, 0, "planner", "executor",
                                               "delegate execution");
  turbo_agent_test_check_child_orchestration_inspect(
      child_orchestration_inspect, parent_agent_run_id, parent_tool_call_id, parent_tool_name,
      thread_id, run_id, checkpoint_id, 1);
}

static void turbo_agent_test_check_command_descriptor_example(
    const json_value_t *descriptor, const char *name, const char *placeholder,
    const char *success_state_hint, const char *example_kind, const char *example_text) {
  const json_value_t *example_payload;
  const json_value_t *example_text_value;
  const char *actual_example_text;

  check_not_null(descriptor);
  check_true(turbo_json_type(descriptor) == TURBO_JSON_OBJECT);
  turbo_agent_test_check_lineage_string_field(descriptor, "name", name);
  turbo_agent_test_check_lineage_string_field(descriptor, "placeholder", placeholder);
  turbo_agent_test_check_lineage_string_field(descriptor, "success_state_hint",
                                              success_state_hint);
  example_payload = turbo_json_object_get(descriptor, "example_payload");
  check_not_null(example_payload);
  if (example_kind) {
    check_true(turbo_json_type(example_payload) == TURBO_JSON_OBJECT);
    turbo_agent_test_check_lineage_string_field(example_payload, "kind", example_kind);
    if (example_text) {
      example_text_value = turbo_json_object_get(example_payload, "text");
      if (!example_text_value) {
        example_text_value = turbo_json_object_get(example_payload, "reason");
      }
      if (!example_text_value) {
        example_text_value = turbo_json_object_get(example_payload, "message");
      }
      check_not_null(example_text_value);
      actual_example_text = turbo_json_type(example_text_value) == TURBO_JSON_NULL
                                ? NULL
                                : turbo_json_string(example_text_value);
      check_str_eq(actual_example_text, example_text);
    }
  } else {
    check_true(turbo_json_type(example_payload) == TURBO_JSON_NULL);
  }
}

static void turbo_agent_test_check_command_descriptor_fixture(const json_value_t *descriptor,
                                                              const char *fixture_name,
                                                              const char *fixture_entry_name) {
  json_value_t *fixture_root;
  const json_value_t *fixture;
  const json_value_t *accepted_keys;
  const json_value_t *fallback_keys;
  const json_value_t *fixture_example_payload;
  const char *fixture_example_kind;
  const char *fixture_example_text = NULL;

  check_not_null(descriptor);
  check_not_null(fixture_name);
  check_not_null(fixture_entry_name);
  fixture_root = turbo_agent_test_load_fixture_json(fixture_name);
  fixture = turbo_agent_test_find_named_fixture_entry(fixture_root, fixture_entry_name);
  check_not_null(fixture);
  check_true(turbo_json_type(fixture) == TURBO_JSON_OBJECT);

  turbo_agent_test_check_lineage_string_field(descriptor, "name",
                                              turbo_json_get_string(fixture, "name"));
  turbo_agent_test_check_lineage_string_field(descriptor, "label",
                                              turbo_json_get_string(fixture, "label"));
  turbo_agent_test_check_lineage_string_field(descriptor, "category",
                                              turbo_json_get_string(fixture, "category"));
  turbo_agent_test_check_lineage_string_field(descriptor, "input_mode",
                                              turbo_json_get_string(fixture, "input_mode"));
  turbo_agent_test_check_lineage_string_field(descriptor, "resume_mode",
                                              turbo_json_get_string(fixture, "resume_mode"));
  turbo_agent_test_check_lineage_string_field(descriptor, "primary_key",
                                              turbo_json_get_string(fixture, "primary_key"));
  check_true(turbo_json_get_bool(descriptor, "requires_input", false) ==
             turbo_json_get_bool(fixture, "requires_input", false));
  check_true(turbo_json_get_bool(descriptor, "supports_json_value", false) ==
             turbo_json_get_bool(fixture, "supports_json_value", false));

  fixture_example_payload = turbo_json_object_get(fixture, "example_payload");
  fixture_example_kind = fixture_example_payload ? turbo_json_get_string(fixture_example_payload, "kind")
                                                 : NULL;
  if (fixture_example_payload) {
    fixture_example_text = turbo_json_get_string(fixture_example_payload, "text");
    if (!fixture_example_text) {
      fixture_example_text = turbo_json_get_string(fixture_example_payload, "reason");
    }
    if (!fixture_example_text) {
      fixture_example_text = turbo_json_get_string(fixture_example_payload, "message");
    }
  }
  turbo_agent_test_check_command_descriptor_example(
      descriptor, turbo_json_get_string(fixture, "name"),
      turbo_json_get_string(fixture, "placeholder"),
      turbo_json_get_string(fixture, "success_state_hint"), fixture_example_kind,
      fixture_example_text);

  accepted_keys = turbo_json_object_get(fixture, "accepted_keys");
  if (accepted_keys) {
    turbo_agent_test_check_string_array_equals_fixture(
        turbo_json_object_get(descriptor, "accepted_keys"), accepted_keys);
  }
  fallback_keys = turbo_json_object_get(fixture, "fallback_keys");
  if (fallback_keys) {
    turbo_agent_test_check_string_array_equals_fixture(
        turbo_json_object_get(descriptor, "fallback_keys"), fallback_keys);
  }

  turbo_free_json(&fixture_root);
}

static void turbo_agent_test_check_memory_record_shape(const json_value_t *record,
                                                       const json_value_t *fixture) {
  const json_value_t *record_metadata;
  const json_value_t *fixture_metadata;

  check_not_null(record);
  check_not_null(fixture);
  check_true(turbo_json_type(record) == TURBO_JSON_OBJECT);
  check_true(turbo_json_type(fixture) == TURBO_JSON_OBJECT);

  turbo_agent_test_check_lineage_string_field(record, "id",
                                              turbo_json_get_string(fixture, "id"));
  turbo_agent_test_check_lineage_string_field(record, "namespace",
                                              turbo_json_get_string(fixture, "namespace"));
  turbo_agent_test_check_lineage_string_field(record, "kind",
                                              turbo_json_get_string(fixture, "kind"));
  turbo_agent_test_check_lineage_string_field(record, "key",
                                              turbo_json_get_string(fixture, "key"));
  turbo_agent_test_check_lineage_string_field(record, "text",
                                              turbo_json_get_string(fixture, "text"));

  check_true(turbo_json_type(turbo_json_object_get(record, "created_at")) ==
             turbo_json_type(turbo_json_object_get(fixture, "created_at")));
  if (turbo_json_type(turbo_json_object_get(fixture, "created_at")) != TURBO_JSON_NULL) {
    turbo_agent_test_check_lineage_string_field(record, "created_at",
                                                turbo_json_get_string(fixture, "created_at"));
  }

  record_metadata = turbo_json_object_get(record, "metadata");
  fixture_metadata = turbo_json_object_get(fixture, "metadata");
  check_true(turbo_json_type(record_metadata) == turbo_json_type(fixture_metadata));
  if (turbo_json_type(fixture_metadata) == TURBO_JSON_OBJECT) {
    turbo_agent_test_check_lineage_string_field(record_metadata, "scope",
                                                turbo_json_get_string(fixture_metadata, "scope"));
    if (turbo_json_type(turbo_json_object_get(fixture_metadata, "path")) == TURBO_JSON_NULL) {
      check_true(turbo_json_type(turbo_json_object_get(record_metadata, "path")) ==
                 TURBO_JSON_NULL);
    } else {
      turbo_agent_test_check_lineage_string_field(record_metadata, "path",
                                                  turbo_json_get_string(fixture_metadata, "path"));
    }
  }
}

static void turbo_agent_test_check_memory_record_fixture(const json_value_t *record,
                                                         const char *fixture_name,
                                                         const char *fixture_entry_name) {
  json_value_t *fixture_root;
  const json_value_t *fixture;

  check_not_null(record);
  check_not_null(fixture_name);
  fixture_root = turbo_agent_test_load_fixture_json(fixture_name);
  fixture = fixture_entry_name ? turbo_agent_test_find_named_fixture_entry(fixture_root,
                                                                           fixture_entry_name)
                               : fixture_root;
  check_not_null(fixture);
  turbo_agent_test_check_memory_record_shape(record, fixture);
  turbo_free_json(&fixture_root);
}

static void turbo_agent_test_check_memory_record_array_fixture(const json_value_t *records,
                                                               const char *fixture_name,
                                                               const char *fixture_entry_name) {
  json_value_t *fixture_root;
  const json_value_t *fixture;
  size_t i;

  check_not_null(records);
  check_not_null(fixture_name);
  fixture_root = turbo_agent_test_load_fixture_json(fixture_name);
  fixture = fixture_entry_name ? turbo_agent_test_find_named_fixture_entry(fixture_root,
                                                                           fixture_entry_name)
                               : fixture_root;
  check_not_null(fixture);
  check_true(turbo_json_type(records) == TURBO_JSON_ARRAY);
  check_true(turbo_json_type(fixture) == TURBO_JSON_ARRAY);
  check_size_eq(turbo_json_array_size(records), turbo_json_array_size(fixture));
  for (i = 0; i < turbo_json_array_size(fixture); ++i) {
    turbo_agent_test_check_memory_record_shape(turbo_json_array_get(records, i),
                                               turbo_json_array_get(fixture, i));
  }
  turbo_free_json(&fixture_root);
}

#endif
