#include "turbo_runtime_segment.h"
#include "turbo_runtime_head_internal.h"
#include "turbo_event_log.h"
#include <salts/clock.h>
#include <cmeta_uuid.h>
#include <fmt.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *const turbo_runtime_segment_threads_collection = "threads";
static const char *const turbo_runtime_segment_runs_collection = "runs";
static const char *const turbo_runtime_segment_checkpoints_collection = "checkpoints";
static atomic_ullong turbo_runtime_record_counter = 1;
/* These legacy record fields are serialized as JSON doubles. */
#define TURBO_RUNTIME_RECORD_MAX_EXACT_INTEGER UINT64_C(9007199254740991)

static void turbo_runtime_segment_json_free(json_value_t **value) {
  if (!value || !*value) {
    return;
  }
  json_free(*value);
  *value = NULL;
}

typedef struct turbo_runtime_segment_stream_capture_s {
  turbo_event_log_t *events;
  turbo_graph_checkpoint_t *checkpoint;
  turbo_graph_exec_status_t checkpoint_status;
  turbo_graph_checkpoint_cb user_checkpoint_cb;
  void *user_checkpoint_user_data;
  turbo_event_sink_json_value_fn user_event_sink;
  void *user_event_sink_user_data;
} turbo_runtime_segment_stream_capture_t;

static char *turbo_runtime_segment_strdup(const char *text) {
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

const char *turbo_runtime_execution_status_text(turbo_graph_exec_status_t status) {
  switch (status) {
  case TURBO_GRAPH_EXEC_OK:
  case TURBO_GRAPH_EXEC_STOP:
    return "completed";
  case TURBO_GRAPH_EXEC_INTERRUPTED:
    return "interrupted";
  case TURBO_GRAPH_EXEC_CANCELLED:
    return "cancelled";
  case TURBO_GRAPH_EXEC_DEADLINE:
    return "timed_out";
  default:
    return "failed";
  }
}

/* Own a freshly constructed field on both successful and rejected insertion. */
static int turbo_runtime_segment_set_new(json_value_t *object, const char *key,
                                         json_value_t *value) {
  if (!value) return -1;
  if (turbo_runtime_json_object_set(object, key, value) != TURBO_RUNTIME_JSON_OK) {
    json_free(value);
    return -1;
  }
  return 0;
}

int turbo_runtime_execution_result_json(const turbo_graph_run_result_t *result,
                                       json_value_t **out_result) {
  json_value_t *json_result;

  if (out_result) *out_result = NULL;
  if (!result || !out_result || result->steps > TURBO_RUNTIME_RECORD_MAX_EXACT_INTEGER) {
    return -1;
  }

  json_result = json_create_object();
  if (!json_result) {
    return -1;
  }

  if (turbo_runtime_segment_set_new(json_result, "status", json_create_number((double)result->status)) != 0) goto fail;
  if (result->last_node) {
    if (turbo_runtime_json_object_set_string(json_result, "last_node", result->last_node) != 0) goto fail;
  } else {
    if (turbo_runtime_segment_set_new(json_result, "last_node", json_create_null()) != 0) goto fail;
  }
  if (result->next_node) {
    if (turbo_runtime_json_object_set_string(json_result, "next_node", result->next_node) != 0) goto fail;
  } else {
    if (turbo_runtime_segment_set_new(json_result, "next_node", json_create_null()) != 0) goto fail;
  }
  if (turbo_runtime_segment_set_new(json_result, "steps", json_create_number((double)result->steps)) != 0) goto fail;

  *out_result = json_result;
  return 0;
fail:
  json_free(json_result);
  return -1;
}

char *turbo_runtime_record_uuid(const char *prefix) {
  cmeta_uuid_t uuid;
  char formatted[SALTS_UUID_STRING_SIZE];
  char *result;
  tstr text;
  if (!prefix || !prefix[0] || cmeta_uuid_v4_generate(&uuid) != 0 ||
      cmeta_uuid_format(&uuid, formatted, sizeof(formatted)) != 0) return NULL;
  text = tstr_format("{}_{}", prefix, formatted);
  if (!text) return NULL;
  result = turbo_runtime_segment_strdup(text);
  tstr_free(text);
  return result;
}

char *turbo_runtime_record_id(const char *prefix) {
  unsigned long long counter;
  unsigned long long tick;
  int needed;
  char *buffer;

  if (!prefix) {
    return NULL;
  }

  counter = atomic_fetch_add(&turbo_runtime_record_counter, 1);
  tick = (unsigned long long)cmeta_hrtime();
  needed = snprintf(NULL, 0, "%s_%llx%llx", prefix, tick, counter);
  if (needed < 0) {
    return NULL;
  }
  buffer = (char *)malloc((size_t)needed + 1);
  if (!buffer) {
    return NULL;
  }
  snprintf(buffer, (size_t)needed + 1, "%s_%llx%llx", prefix, tick, counter);
  return buffer;
}

int turbo_runtime_record_timestamp(char *buffer, size_t buffer_size) {
  time_t now;
  struct tm utc_tm;

  if (!buffer || buffer_size < 21) {
    return -1;
  }

  now = time(NULL);
#ifdef _WIN32
  if (gmtime_s(&utc_tm, &now) != 0) {
    return -1;
  }
#else
  if (!gmtime_r(&now, &utc_tm)) {
    return -1;
  }
#endif

  if (strftime(buffer, buffer_size, "%Y-%m-%dT%H:%M:%SZ", &utc_tm) == 0) {
    return -1;
  }

  return 0;
}

static int turbo_runtime_segment_parse_json_string(const char *json_text, json_value_t **out_json) {
  json_value_t *json_root = NULL;

  if (!json_text || !out_json) {
    return -1;
  }

  *out_json = NULL;
  json_root = json_parse(json_text, strlen(json_text));
  if (!json_root) {
    turbo_runtime_segment_json_free(&json_root);
    return -1;
  }
  *out_json = json_root;
  return 0;
}

static char *turbo_runtime_segment_graph_name(turbo_graph_t *graph) {
  const char *topology_id;
  json_value_t *topology_json = NULL;
  char *name = NULL;
  const char *graph_name;

  if (!graph) {
    return NULL;
  }

  topology_id = turbo_graph_topology_id(graph);
  if (!topology_id || turbo_runtime_segment_parse_json_string(topology_id, &topology_json) != 0) {
    turbo_runtime_segment_json_free(&topology_json);
    return NULL;
  }
  graph_name = json_get_string(topology_json, "graph_name");
  if (graph_name) {
    name = turbo_runtime_segment_strdup(graph_name);
  }
  turbo_runtime_segment_json_free(&topology_json);
  return name;
}

static char *turbo_runtime_segment_graph_name_from_topology_id_text(const char *topology_id_text) {
  json_value_t *topology_json = NULL;
  char *name = NULL;
  const char *graph_name;

  if (!topology_id_text ||
      turbo_runtime_segment_parse_json_string(topology_id_text, &topology_json) != 0) {
    turbo_runtime_segment_json_free(&topology_json);
    return NULL;
  }
  graph_name = json_get_string(topology_json, "graph_name");
  if (graph_name) {
    name = turbo_runtime_segment_strdup(graph_name);
  }
  turbo_runtime_segment_json_free(&topology_json);
  return name;
}

static int turbo_runtime_segment_json_value_to_json(const json_value_t *state,
                                                  json_value_t **out_state_json) {
  if (!state || !out_state_json) {
    return -1;
  }
  *out_state_json = json_clone(state);
  return *out_state_json ? 0 : -1;
}

static int turbo_runtime_segment_collect_event_log_events(const turbo_event_log_t *log,
                                                        json_value_t **out_events_json) {
  json_value_t *events_json_value;
  json_value_t *events_json;

  if (!log || !out_events_json) {
    return -1;
  }

  events_json_value = turbo_event_log_events_json_value(log);
  if (!events_json_value) {
    return -1;
  }
  events_json = json_clone(events_json_value);
  turbo_runtime_json_destroy(events_json_value);
  if (!events_json || json_type(events_json) != JSON_ARRAY) {
    turbo_runtime_segment_json_free(&events_json);
    return -1;
  }
  *out_events_json = events_json;
  return 0;
}

static turbo_graph_exec_status_t
turbo_runtime_segment_clone_checkpoint(const turbo_graph_checkpoint_t *checkpoint,
                                     turbo_graph_checkpoint_t **out_checkpoint) {
  return turbo_graph_checkpoint_clone(checkpoint, out_checkpoint);
}

static turbo_graph_exec_status_t
turbo_runtime_segment_replace_checkpoint(turbo_graph_checkpoint_t **target,
                                       const turbo_graph_checkpoint_t *checkpoint) {
  turbo_graph_checkpoint_t *copy = NULL;
  turbo_graph_exec_status_t status;

  if (!target || !checkpoint) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }
  status = turbo_runtime_segment_clone_checkpoint(checkpoint, &copy);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }
  turbo_graph_checkpoint_destroy(*target);
  *target = copy;
  return TURBO_GRAPH_EXEC_OK;
}

static void
turbo_runtime_segment_capture_stream_checkpoint(const turbo_graph_checkpoint_t *checkpoint,
                                              void *user_data) {
  turbo_runtime_segment_stream_capture_t *capture = (turbo_runtime_segment_stream_capture_t *)user_data;

  if (!capture || !checkpoint || capture->checkpoint_status != TURBO_GRAPH_EXEC_OK) {
    return;
  }
  capture->checkpoint_status =
      turbo_runtime_segment_replace_checkpoint(&capture->checkpoint, checkpoint);
  if (capture->user_checkpoint_cb) {
    capture->user_checkpoint_cb(checkpoint, capture->user_checkpoint_user_data);
  }
}

static void turbo_runtime_segment_capture_stream_event(const json_value_t *event, void *user_data) {
  turbo_runtime_segment_stream_capture_t *capture = (turbo_runtime_segment_stream_capture_t *)user_data;

  if (!capture || !event || !capture->events) {
    return;
  }
  turbo_event_log_capture_json_value(event, capture->events);
  if (capture->user_event_sink) {
    capture->user_event_sink(event, capture->user_event_sink_user_data);
  }
}

static turbo_graph_exec_status_t turbo_runtime_segment_run_graph_stream(
    turbo_graph_t *graph, const json_value_t *start_state,
    const turbo_graph_checkpoint_t *start_checkpoint, const turbo_graph_run_options_t *options,
    const turbo_cancel_token_t *cancel_token, turbo_event_log_t *events,
    turbo_event_sink_json_value_fn user_event_sink, void *user_event_sink_user_data,
    turbo_graph_run_result_t *out_result, json_value_t **out_state,
    turbo_graph_checkpoint_t **out_checkpoint) {
  turbo_graph_run_options_t effective_options;
  turbo_runtime_segment_stream_capture_t capture = {0};
  turbo_graph_exec_status_t status;

  *out_state = NULL;
  *out_checkpoint = NULL;
  if (options) {
    effective_options = *options;
  } else {
    memset(&effective_options, 0, sizeof(effective_options));
  }

  capture.events = events;
  capture.checkpoint = NULL;
  capture.checkpoint_status = TURBO_GRAPH_EXEC_OK;
  capture.user_checkpoint_cb = effective_options.checkpoint_cb;
  capture.user_checkpoint_user_data = effective_options.checkpoint_user_data;
  capture.user_event_sink = user_event_sink;
  capture.user_event_sink_user_data = user_event_sink_user_data;
  effective_options.checkpoint_cb = turbo_runtime_segment_capture_stream_checkpoint;
  effective_options.checkpoint_user_data = &capture;

  if (start_checkpoint) {
    status = turbo_graph_run_checkpoint_json_value_stream_controlled(
        graph, start_checkpoint, &effective_options, cancel_token,
        turbo_runtime_segment_capture_stream_event, &capture, out_result, out_state);
  } else {
    status = turbo_graph_run_json_value_stream_controlled(
        graph, start_state, &effective_options, cancel_token,
        turbo_runtime_segment_capture_stream_event, &capture, out_result, out_state);
  }

  *out_checkpoint = capture.checkpoint;
  if (capture.checkpoint_status != TURBO_GRAPH_EXEC_OK) {
    return capture.checkpoint_status;
  }
  if (turbo_event_log_status(events) != TURBO_EVENT_LOG_OK) {
    return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }
  return status;
}

static int turbo_runtime_segment_build_thread_record(const char *thread_id, const char *timestamp,
                                                   json_value_t **out_record) {
  json_value_t *record;

  if (!thread_id || !timestamp || !out_record) {
    return -1;
  }
  record = json_create_object();
  if (!record) {
    return -1;
  }
  if (turbo_runtime_json_object_set_string(record, "id", thread_id) != 0) goto fail;
  if (turbo_runtime_json_object_set_string(record, "created_at", timestamp) != 0) goto fail;
  if (turbo_runtime_json_object_set_string(record, "updated_at", timestamp) != 0) goto fail;
  *out_record = record;
  return 0;
fail:
  json_free(record);
  return -1;
}

static int turbo_runtime_segment_build_run_record(
    const char *run_id, const char *thread_id, const char *parent_run_id,
    const char *parent_agent_run_id, const char *parent_tool_call_id, const char *parent_tool_name,
    const char *parent_graph_run_id, const char *call_frame_id,
    const char *forked_from_checkpoint_id, const char *graph_name, const char *topology_id,
    const char *status, const char *created_at, const char *updated_at,
    const char *latest_checkpoint_id, const turbo_graph_run_result_t *result,
    const json_value_t *state_snapshot, json_value_t **out_record) {
  json_value_t *record;
  json_value_t *result_json = NULL;

  if (!run_id || !thread_id || !graph_name || !topology_id || !status || !created_at ||
      !updated_at || !result || !out_record) {
    return -1;
  }

  if (turbo_runtime_execution_result_json(result, &result_json) != 0) {
    return -1;
  }

  record = json_create_object();
  if (!record) {
    turbo_runtime_segment_json_free(&result_json);
    return -1;
  }

  if (turbo_runtime_json_object_set_string(record, "id", run_id) != 0) goto fail;
  if (turbo_runtime_json_object_set_string(record, "thread_id", thread_id) != 0) goto fail;
  if (parent_run_id) {
    if (turbo_runtime_json_object_set_string(record, "parent_run_id", parent_run_id) != 0) goto fail;
  } else {
    if (turbo_runtime_segment_set_new(record, "parent_run_id", json_create_null()) != 0) goto fail;
  }
  if (parent_agent_run_id) {
    if (turbo_runtime_json_object_set_string(record, "parent_agent_run_id", parent_agent_run_id) != 0) goto fail;
  } else {
    if (turbo_runtime_segment_set_new(record, "parent_agent_run_id", json_create_null()) != 0) goto fail;
  }
  if (parent_tool_call_id) {
    if (turbo_runtime_json_object_set_string(record, "parent_tool_call_id", parent_tool_call_id) != 0) goto fail;
  } else {
    if (turbo_runtime_segment_set_new(record, "parent_tool_call_id", json_create_null()) != 0) goto fail;
  }
  if (parent_tool_name) {
    if (turbo_runtime_json_object_set_string(record, "parent_tool_name", parent_tool_name) != 0) goto fail;
  } else {
    if (turbo_runtime_segment_set_new(record, "parent_tool_name", json_create_null()) != 0) goto fail;
  }
  if (parent_graph_run_id) {
    if (turbo_runtime_json_object_set_string(record, "parent_graph_run_id", parent_graph_run_id) != 0) goto fail;
  } else {
    if (turbo_runtime_segment_set_new(record, "parent_graph_run_id", json_create_null()) != 0) goto fail;
  }
  if (call_frame_id) {
    if (turbo_runtime_json_object_set_string(record, "call_frame_id", call_frame_id) != 0) goto fail;
  } else {
    if (turbo_runtime_segment_set_new(record, "call_frame_id", json_create_null()) != 0) goto fail;
  }
  if (forked_from_checkpoint_id) {
    if (turbo_runtime_json_object_set_string(record, "forked_from_checkpoint_id", forked_from_checkpoint_id) != 0) goto fail;
  } else {
    if (turbo_runtime_segment_set_new(record, "forked_from_checkpoint_id", json_create_null()) != 0) goto fail;
  }
  if (turbo_runtime_json_object_set_string(record, "graph_name", graph_name) != 0) goto fail;
  if (turbo_runtime_json_object_set_string(record, "topology_id", topology_id) != 0) goto fail;
  if (turbo_runtime_json_object_set_string(record, "status", status) != 0) goto fail;
  if (turbo_runtime_json_object_set_string(record, "created_at", created_at) != 0) goto fail;
  if (turbo_runtime_json_object_set_string(record, "updated_at", updated_at) != 0) goto fail;
  if (latest_checkpoint_id) {
    if (turbo_runtime_json_object_set_string(record, "latest_checkpoint_id", latest_checkpoint_id) != 0) goto fail;
  } else {
    if (turbo_runtime_segment_set_new(record, "latest_checkpoint_id", json_create_null()) != 0) goto fail;
  }
  if (state_snapshot) {
    if (turbo_runtime_json_object_set_clone(record, "state_snapshot", state_snapshot) != 0) goto fail;
  } else {
    if (turbo_runtime_segment_set_new(record, "state_snapshot", json_create_null()) != 0) goto fail;
  }
  if (turbo_runtime_json_object_set(record, "result", result_json) != 0) goto fail;
  result_json = NULL;
  *out_record = record;
  return 0;
fail:
  json_free(result_json);
  json_free(record);
  return -1;
}

static int turbo_runtime_segment_build_checkpoint_record(
    const char *checkpoint_id, const char *thread_id, const char *run_id,
    const char *parent_checkpoint_id, const char *parent_agent_run_id,
    const char *parent_tool_call_id, const char *parent_tool_name, const char *parent_graph_run_id,
    const char *call_frame_id, size_t seq, const char *status, const char *created_at,
    const turbo_graph_checkpoint_t *checkpoint, const json_value_t *control_snapshot,
    const json_value_t *workflow_snapshot, const json_value_t *events_json,
    json_value_t **out_record) {
  json_value_t *record;
  char *checkpoint_json_text = NULL;
  json_value_t *checkpoint_json = NULL;
  char *graph_name = NULL;
  const char *topology_id_text;

  if (!checkpoint_id || !thread_id || !run_id || !status || !created_at || !checkpoint ||
      !control_snapshot || !workflow_snapshot || !events_json || !out_record ||
      seq > TURBO_RUNTIME_RECORD_MAX_EXACT_INTEGER ||
      turbo_graph_checkpoint_steps(checkpoint) > TURBO_RUNTIME_RECORD_MAX_EXACT_INTEGER) {
    return -1;
  }

  checkpoint_json_text = turbo_graph_checkpoint_serialize(checkpoint, NULL);
  if (!checkpoint_json_text ||
      turbo_runtime_segment_parse_json_string(checkpoint_json_text, &checkpoint_json) != 0) {
    json_serialize_free(checkpoint_json_text);
    turbo_runtime_segment_json_free(&checkpoint_json);
    return -1;
  }
  json_serialize_free(checkpoint_json_text);
  topology_id_text = json_get_string(checkpoint_json, "topology_id");
  graph_name = turbo_runtime_segment_graph_name_from_topology_id_text(topology_id_text);

  record = json_create_object();
  if (!record || !topology_id_text || !graph_name) {
    free(graph_name);
    turbo_runtime_segment_json_free(&checkpoint_json);
    turbo_runtime_segment_json_free(&record);
    return -1;
  }

  if (turbo_runtime_json_object_set_string(record, "id", checkpoint_id) != 0) goto fail;
  if (turbo_runtime_json_object_set_string(record, "thread_id", thread_id) != 0) goto fail;
  if (turbo_runtime_json_object_set_string(record, "run_id", run_id) != 0) goto fail;
  if (parent_checkpoint_id) {
    if (turbo_runtime_json_object_set_string(record, "parent_checkpoint_id", parent_checkpoint_id) != 0) goto fail;
  } else {
    if (turbo_runtime_segment_set_new(record, "parent_checkpoint_id", json_create_null()) != 0) goto fail;
  }
  if (parent_agent_run_id) {
    if (turbo_runtime_json_object_set_string(record, "parent_agent_run_id", parent_agent_run_id) != 0) goto fail;
  } else {
    if (turbo_runtime_segment_set_new(record, "parent_agent_run_id", json_create_null()) != 0) goto fail;
  }
  if (parent_tool_call_id) {
    if (turbo_runtime_json_object_set_string(record, "parent_tool_call_id", parent_tool_call_id) != 0) goto fail;
  } else {
    if (turbo_runtime_segment_set_new(record, "parent_tool_call_id", json_create_null()) != 0) goto fail;
  }
  if (parent_tool_name) {
    if (turbo_runtime_json_object_set_string(record, "parent_tool_name", parent_tool_name) != 0) goto fail;
  } else {
    if (turbo_runtime_segment_set_new(record, "parent_tool_name", json_create_null()) != 0) goto fail;
  }
  if (parent_graph_run_id) {
    if (turbo_runtime_json_object_set_string(record, "parent_graph_run_id", parent_graph_run_id) != 0) goto fail;
  } else {
    if (turbo_runtime_segment_set_new(record, "parent_graph_run_id", json_create_null()) != 0) goto fail;
  }
  if (call_frame_id) {
    if (turbo_runtime_json_object_set_string(record, "call_frame_id", call_frame_id) != 0) goto fail;
  } else {
    if (turbo_runtime_segment_set_new(record, "call_frame_id", json_create_null()) != 0) goto fail;
  }
  if (turbo_runtime_segment_set_new(record, "seq", json_create_number((double)seq)) != 0) goto fail;
  if (turbo_runtime_json_object_set_string(record, "status", status) != 0) goto fail;
  if (turbo_runtime_json_object_set_string(record, "created_at", created_at) != 0) goto fail;
  if (turbo_runtime_json_object_set_string(record, "graph_name", graph_name) != 0) goto fail;
  if (turbo_runtime_json_object_set_string(record, "topology_id", topology_id_text) != 0) goto fail;
  if (turbo_runtime_json_object_set_string(record, "next_node", turbo_graph_checkpoint_next_node(checkpoint)) != 0) goto fail;
  if (turbo_runtime_segment_set_new(record, "steps", json_create_number((double)turbo_graph_checkpoint_steps(checkpoint))) != 0) goto fail;
  if (turbo_runtime_json_object_set(record, "checkpoint_json", checkpoint_json) != 0) goto fail;
  checkpoint_json = NULL;
  if (turbo_runtime_json_object_set_clone(record, "control_snapshot", control_snapshot) != 0) goto fail;
  if (turbo_runtime_json_object_set_clone(record, "workflow_snapshot", workflow_snapshot) != 0) goto fail;
  if (turbo_runtime_json_object_set_clone(record, "events", events_json) != 0) goto fail;
  free(graph_name);
  *out_record = record;
  return 0;
fail:
  free(graph_name);
  json_free(checkpoint_json);
  json_free(record);
  return -1;
}

static int turbo_runtime_segment_upsert_thread(turbo_agent_runtime_t *runtime, const char *thread_id,
                                             const char *timestamp) {
  json_value_t *record = NULL;

  if (!runtime || !thread_id || !timestamp) {
    return -1;
  }
  if (turbo_agent_runtime_store_get_json(runtime, turbo_runtime_segment_threads_collection, thread_id,
                                         &record) == 0 &&
      record) {
    if (turbo_runtime_json_object_set_string(record, "updated_at", timestamp) != 0) goto fail;
    if (!json_get_string(record, "created_at")) {
      if (turbo_runtime_json_object_set_string(record, "created_at", timestamp) != 0) goto fail;
    }
  } else {
    turbo_runtime_segment_json_free(&record);
    if (turbo_runtime_segment_build_thread_record(thread_id, timestamp, &record) != 0) {
      return -1;
    }
  }
  if (turbo_agent_runtime_store_put_json(runtime, turbo_runtime_segment_threads_collection, thread_id,
                                         record) != 0) {
    turbo_runtime_segment_json_free(&record);
    return -1;
  }
  turbo_runtime_segment_json_free(&record);
  return 0;
fail:
  json_free(record);
  return -1;
}

static int turbo_runtime_segment_snapshots(
    const turbo_runtime_segment_t *segment, const json_value_t *state,
    json_value_t **control, json_value_t **workflow) {
  if (segment->snapshot) {
    int rc = segment->snapshot(state, control, workflow, segment->user_data);
    return rc == 0 && *control && *workflow ? 0 : -1;
  }
  *control = json_create_object();
  *workflow = json_create_object();
  return *control && *workflow ? 0 : -1;
}

static int turbo_runtime_segment_record_checkpoint_from_parts(
    turbo_agent_runtime_t *runtime, const char *thread_id, const char *run_id,
    const char *parent_checkpoint_id, const char *parent_agent_run_id,
    const char *parent_tool_call_id, const char *parent_tool_name, const char *parent_graph_run_id,
    const char *call_frame_id, size_t seq, const char *timestamp,
    const turbo_graph_checkpoint_t *checkpoint, const turbo_event_log_t *events_log,
    const json_value_t *state, const char **out_checkpoint_id,
    const turbo_runtime_segment_t *segment) {
  json_value_t *control_snapshot = NULL;
  json_value_t *workflow_snapshot = NULL;
  json_value_t *events_json = NULL;
  json_value_t *record = NULL;
  char *checkpoint_id = NULL;
  int rc = -1;

  if (!runtime || !thread_id || !run_id || !timestamp || !checkpoint || !events_log || !state ||
      !out_checkpoint_id) {
    return -1;
  }
  *out_checkpoint_id = NULL;

  checkpoint_id = turbo_runtime_record_uuid("ckpt");
  if (!checkpoint_id ||
      turbo_runtime_segment_snapshots(segment, state, &control_snapshot, &workflow_snapshot) != 0 ||
      turbo_runtime_segment_collect_event_log_events(events_log, &events_json) != 0 ||
      turbo_runtime_segment_build_checkpoint_record(
          checkpoint_id, thread_id, run_id, parent_checkpoint_id, parent_agent_run_id,
          parent_tool_call_id, parent_tool_name, parent_graph_run_id, call_frame_id, seq,
          "interrupted", timestamp, checkpoint, control_snapshot, workflow_snapshot, events_json,
          &record) != 0 ||
      turbo_agent_runtime_store_put_json(runtime, turbo_runtime_segment_checkpoints_collection,
                                         checkpoint_id, record) != 0) {
    goto cleanup;
  }

  *out_checkpoint_id = checkpoint_id;
  checkpoint_id = NULL;
  rc = 0;

cleanup:
  free(checkpoint_id);
  turbo_runtime_segment_json_free(&control_snapshot);
  turbo_runtime_segment_json_free(&workflow_snapshot);
  turbo_runtime_segment_json_free(&events_json);
  turbo_runtime_segment_json_free(&record);
  return rc;
}

static int turbo_runtime_segment_finalize_run(
    turbo_agent_runtime_t *runtime, const char *run_id, const char *thread_id,
    const char *parent_run_id, const char *parent_agent_run_id, const char *parent_tool_call_id,
    const char *parent_tool_name, const char *parent_graph_run_id, const char *call_frame_id,
    const char *forked_from_checkpoint_id, turbo_graph_t *graph, const char *created_at,
    const char *updated_at, const char *latest_checkpoint_id,
    const turbo_graph_run_result_t *result, const json_value_t *state,
    json_value_t **out_run_record) {
  char *graph_name = NULL;
  const char *topology_id;
  json_value_t *run_record = NULL;
  json_value_t *state_json = NULL;
  int rc = -1;

  if (!runtime || !run_id || !thread_id || !graph || !created_at || !updated_at || !result ||
      !state) {
    return -1;
  }

  topology_id = turbo_graph_topology_id(graph);
  graph_name = turbo_runtime_segment_graph_name(graph);
  if (turbo_runtime_segment_json_value_to_json(state, &state_json) != 0) {
    free(graph_name);
    return -1;
  }
  if (!topology_id || !graph_name ||
      turbo_runtime_segment_build_run_record(
          run_id, thread_id, parent_run_id, parent_agent_run_id, parent_tool_call_id,
          parent_tool_name, parent_graph_run_id, call_frame_id, forked_from_checkpoint_id,
          graph_name, topology_id, turbo_runtime_execution_status_text(result->status), created_at,
          updated_at, latest_checkpoint_id, result, state_json, &run_record) != 0 ||
      turbo_agent_runtime_store_put_json(runtime, turbo_runtime_segment_runs_collection, run_id,
                                         run_record) != 0) {
    goto cleanup;
  }
  if (out_run_record) {
    *out_run_record = run_record;
    run_record = NULL;
  }
  rc = 0;

cleanup:
  free(graph_name);
  turbo_runtime_segment_json_free(&state_json);
  turbo_runtime_segment_json_free(&run_record);
  return rc;
}

static int turbo_runtime_segment_make_summary(
    const turbo_runtime_segment_t *segment, const char *checkpoint_id,
    const turbo_graph_run_result_t *result, const json_value_t *state,
    json_value_t **out_summary) {
  json_value_t *summary = NULL;
  json_value_t *result_json = NULL;
  int rc;
  if (segment->summary) {
    rc = segment->summary(segment, checkpoint_id, result, state, out_summary, segment->user_data);
    return rc == 0 && *out_summary && json_type(*out_summary) == JSON_OBJECT ? 0 : -1;
  }
  summary = json_create_object();
  if (!summary || turbo_runtime_execution_result_json(result, &result_json) != 0 ||
      turbo_runtime_json_object_set_string(summary, "thread_id", segment->thread_id) != 0 ||
      turbo_runtime_json_object_set_string(summary, "run_id", segment->run_id) != 0 ||
      turbo_runtime_json_object_set_string(summary, "status", turbo_runtime_execution_status_text(result->status)) != 0)
    goto fail;
  if (checkpoint_id) {
    if (turbo_runtime_json_object_set_string(summary, "checkpoint_id", checkpoint_id) != 0) goto fail;
  } else {
    json_value_t *empty = json_create_null();
    if (turbo_runtime_json_object_set(summary, "checkpoint_id", empty) != 0) {
      json_free(empty);
      goto fail;
    }
  }
  if (turbo_runtime_json_object_set(summary, "result", result_json) != 0) goto fail;
  *out_summary = summary;
  return 0;
fail:
  json_free(result_json);
  json_free(summary);
  return -1;
}

void turbo_runtime_segment_init(turbo_runtime_segment_t *segment) {
  if (!segment) return;
  memset(segment, 0, sizeof(*segment));
  segment->struct_size = sizeof(*segment);
  segment->abi_version = TURBO_RUNTIME_SEGMENT_ABI_VERSION;
  segment->next_checkpoint_seq = 1;
}

int turbo_runtime_segment_execute(const turbo_runtime_segment_t *segment,
                                  json_value_t **out_summary_json, json_value_t **out_state) {
  if (out_summary_json) *out_summary_json = NULL;
  if (out_state) *out_state = NULL;
  if (!segment || segment->struct_size < sizeof(*segment) ||
      segment->abi_version != TURBO_RUNTIME_SEGMENT_ABI_VERSION ||
      !segment->runtime || !segment->graph || !segment->thread_id || !segment->run_id ||
      !out_summary_json || !out_state || out_summary_json == out_state ||
      (!segment->start_state == !segment->start_checkpoint) ||
      (!segment->enter != !segment->leave) || segment->next_checkpoint_seq == 0 ||
      segment->next_checkpoint_seq > TURBO_RUNTIME_RECORD_MAX_EXACT_INTEGER)
    return -1;
  turbo_agent_runtime_t *runtime = segment->runtime;
  turbo_graph_t *graph = segment->graph;
  const char *thread_id = segment->thread_id;
  const char *run_id = segment->run_id;
  const char *parent_run_id = segment->parent_run_id;
  const char *parent_agent_run_id = segment->parent_agent_run_id;
  const char *parent_tool_call_id = segment->parent_tool_call_id;
  const char *parent_tool_name = segment->parent_tool_name;
  const char *parent_graph_run_id = segment->parent_graph_run_id;
  const char *call_frame_id = segment->call_frame_id;
  const char *forked_from_checkpoint_id = segment->forked_from_checkpoint_id;
  const char *parent_checkpoint_id = segment->parent_checkpoint_id;
  const char *run_created_at = segment->run_created_at;
  size_t next_checkpoint_seq = segment->next_checkpoint_seq;
  const json_value_t *start_state = segment->start_state;
  const turbo_graph_checkpoint_t *start_checkpoint = segment->start_checkpoint;
  const turbo_graph_run_options_t *options = segment->options;
  const turbo_cancel_token_t *cancel_token = segment->cancel_token;
  turbo_event_sink_json_value_fn event_sink = segment->event_sink;
  void *event_sink_user_data = segment->event_sink_user_data;
  turbo_event_log_t *stream_events = NULL;
  turbo_graph_checkpoint_t *stream_checkpoint = NULL;
  turbo_graph_run_result_t result = {0};
  json_value_t *result_state = NULL;
  json_value_t *summary_json = NULL;
  json_value_t *summary_state_json = NULL;
  json_value_t *published_run = NULL;
  const char *persisted_checkpoint_id = NULL;
  char created_at[32];
  char updated_at[32];
  const char *checkpoint_id = NULL;
  turbo_graph_exec_status_t status;
  int rc = -1;

  if (run_created_at) {
    if (strlen(run_created_at) >= sizeof(created_at)) return -1;
    snprintf(created_at, sizeof(created_at), "%s", run_created_at);
  } else if (turbo_runtime_record_timestamp(created_at, sizeof(created_at)) != 0) {
    return -1;
  }
  if (turbo_runtime_record_timestamp(updated_at, sizeof(updated_at)) != 0 ||
      turbo_runtime_segment_upsert_thread(runtime, thread_id, updated_at) != 0 ||
      turbo_runtime_head_initialize(runtime, thread_id) != 0) {
    return -1;
  }
  if (segment->enter) segment->enter(segment, segment->user_data);
  stream_events = turbo_event_log_create();
  if (!stream_events) {
    if (segment->leave) segment->leave(segment, segment->user_data);
    return -1;
  }

  status = turbo_runtime_segment_run_graph_stream(
      graph, start_state, start_checkpoint, options, cancel_token, stream_events, event_sink,
      event_sink_user_data, &result, &result_state, &stream_checkpoint);
  if (!result_state) {
    if (segment->leave) segment->leave(segment, segment->user_data);
    turbo_graph_checkpoint_destroy(stream_checkpoint);
    turbo_event_log_destroy(stream_events);
    return -1;
  }
  if (status != TURBO_GRAPH_EXEC_OK && status != TURBO_GRAPH_EXEC_STOP &&
      status != TURBO_GRAPH_EXEC_INTERRUPTED && status != TURBO_GRAPH_EXEC_CANCELLED &&
      status != TURBO_GRAPH_EXEC_DEADLINE) {
    if (status == TURBO_GRAPH_EXEC_ERROR) {
      *out_state = result_state;
      result_state = NULL;
    }
    goto cleanup;
  }

  if (status == TURBO_GRAPH_EXEC_INTERRUPTED || status == TURBO_GRAPH_EXEC_CANCELLED ||
      status == TURBO_GRAPH_EXEC_DEADLINE) {
    if (turbo_runtime_segment_record_checkpoint_from_parts(
            runtime, thread_id, run_id, parent_checkpoint_id, parent_agent_run_id,
            parent_tool_call_id, parent_tool_name, parent_graph_run_id, call_frame_id,
            next_checkpoint_seq, updated_at, stream_checkpoint, stream_events, result_state,
            &checkpoint_id, segment) != 0) {
      goto cleanup;
    }
  }
  persisted_checkpoint_id = checkpoint_id ? checkpoint_id : parent_checkpoint_id;
  summary_state_json = json_clone(result_state);
  if (!summary_state_json) goto cleanup;

  if (turbo_runtime_segment_finalize_run(
          runtime, run_id, thread_id, parent_run_id, parent_agent_run_id, parent_tool_call_id,
          parent_tool_name, parent_graph_run_id, call_frame_id, forked_from_checkpoint_id, graph,
          created_at, updated_at, persisted_checkpoint_id, &result, result_state, &published_run) != 0 ||
      turbo_runtime_segment_make_summary(segment, checkpoint_id, &result,
                                         summary_state_json, &summary_json) != 0 ||
      turbo_runtime_head_publish(runtime, thread_id, published_run) != 0) {
    goto cleanup;
  }

  *out_summary_json = summary_json;
  *out_state = result_state;
  summary_json = NULL;
  result_state = NULL;
  rc = 0;

cleanup:
  if (segment->leave) segment->leave(segment, segment->user_data);
  free((char *)checkpoint_id);
  turbo_runtime_segment_json_free(&summary_state_json);
  turbo_runtime_segment_json_free(&published_run);
  turbo_runtime_segment_json_free(&summary_json);
  turbo_runtime_json_destroy(result_state);
  turbo_graph_checkpoint_destroy(stream_checkpoint);
  turbo_event_log_destroy(stream_events);
  return rc;
}
