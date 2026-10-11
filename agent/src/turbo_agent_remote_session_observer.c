#include "turbo_agent_remote_session.h"
#include "turbo_agent_util_internal.h"

#include <string.h>

/* History and terminal records remain owned by this synchronous adapter.
 * Each callback borrows an independently owned envelope for its duration only.
 * Remote projection deliberately preserves its narrower legacy event shape. */
static int
turbo_agent_remote_session_observer_event_from_json_value(const json_value_t *raw_event,
                                                          json_value_t **out_observer_event) {
  const json_value_t *kind_value;
  const char *kind;
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
    observer_type = turbo_agent_util_observer_type_for_trace_name(
        turbo_runtime_json_value_as_string(json_object_get(raw_event, "name")));
  } else if (kind && strcmp(kind, "observer") == 0) {
    observer_type = turbo_runtime_json_value_as_string(json_object_get(raw_event, "type"));
  }

  if (!observer_type) {
    return 0;
  }

  observer_event = json_create_object();
  if (!observer_event ||
      turbo_agent_util_json_value_object_set_string(observer_event, "kind", "observer") != 0 ||
      turbo_agent_util_json_value_object_set_string(observer_event, "type", observer_type) != 0 ||
      turbo_agent_util_json_value_object_set_clone(observer_event, "event", raw_event) != 0) {
    turbo_runtime_json_destroy(observer_event);
    return -1;
  }
  *out_observer_event = observer_event;
  return 1;
}

static int turbo_agent_remote_session_events_json_value_has_terminal_type(
    const json_value_t *events_json_value, const char *terminal_type) {
  size_t count;
  size_t i;

  if (!events_json_value || !terminal_type || terminal_type[0] == '\0' ||
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

static int turbo_agent_remote_session_observe_events_json_value(
    const json_value_t *events_json_value, const turbo_agent_observer_json_value_sink_t *sink) {
  size_t count;
  size_t i;

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
    rc = turbo_agent_remote_session_observer_event_from_json_value(raw_event, &observer_event);
    if (rc < 0) {
      turbo_runtime_json_destroy(observer_event);
      return -1;
    }
    if (rc > 0 && observer_event) {
      sink->callback(observer_event, sink->user_data);
    }
    turbo_runtime_json_destroy(observer_event);
  }
  return 0;
}

static int
turbo_agent_remote_session_observer_terminal_event_from_json(const json_value_t *record_json,
                                                             json_value_t **out_observer_event) {
  const char *status;
  json_value_t *observer_event = NULL;

  if (!record_json || !out_observer_event) {
    return -1;
  }
  *out_observer_event = NULL;
  status = json_get_string(record_json, "status");
  if (!status || (strcmp(status, "interrupted") != 0 && strcmp(status, "completed") != 0)) {
    return 0;
  }
  observer_event = json_create_object();
  if (!observer_event ||
      turbo_agent_util_json_value_object_set_string(observer_event, "kind", "observer") != 0 ||
      turbo_agent_util_json_value_object_set_string(observer_event, "type", status) != 0 ||
      turbo_agent_util_json_value_object_set_clone(observer_event, "event", record_json) != 0) {
    turbo_runtime_json_destroy(observer_event);
    return -1;
  }
  *out_observer_event = observer_event;
  return 1;
}

static int turbo_agent_remote_session_observe_terminal_record_json(
    const json_value_t *events_json_value, const json_value_t *record_json,
    const turbo_agent_observer_json_value_sink_t *sink) {
  json_value_t *observer_event = NULL;
  const char *type;
  int rc;

  if (!record_json || !sink || !sink->callback) {
    return -1;
  }
  rc = turbo_agent_remote_session_observer_terminal_event_from_json(record_json, &observer_event);
  if (rc <= 0) {
    turbo_runtime_json_destroy(observer_event);
    return rc;
  }
  type = turbo_runtime_json_value_as_string(json_object_get(observer_event, "type"));
  if (!events_json_value || !type ||
      !turbo_agent_remote_session_events_json_value_has_terminal_type(events_json_value, type)) {
    sink->callback(observer_event, sink->user_data);
  }
  turbo_runtime_json_destroy(observer_event);
  return 1;
}

CXX_C_API int turbo_agent_remote_session_replay_thread_history_json_value(
    turbo_agent_remote_session_t *session, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data) {
  json_value_t *events = NULL;
  const json_value_t *event = NULL;
  size_t index;

  if (!session || !turbo_agent_remote_session_client(session) || !event_sink) {
    return -1;
  }
  if (turbo_agent_remote_session_load_thread_history_events_json_value(session, &events) != 0 ||
      !events || json_type(events) != JSON_ARRAY) {
    turbo_runtime_json_destroy(events);
    return -1;
  }
  for (index = 0; (event = json_array_get(events, index)) != NULL; ++index) {
    event_sink(event, event_sink_user_data);
  }
  turbo_runtime_json_destroy(events);
  return 0;
}

CXX_C_API int turbo_agent_remote_session_observe_thread_history_json_value(
    turbo_agent_remote_session_t *session, const turbo_agent_observer_json_value_sink_t *sink) {
  json_value_t *index_json = NULL;
  const json_value_t *events_json;
  const json_value_t *record_json = NULL;
  int rc;

  if (!session || !turbo_agent_remote_session_client(session) || !sink || !sink->callback) {
    return -1;
  }
  rc = turbo_agent_remote_session_get_observability_index(session, &index_json);
  if (rc != 0 || !index_json) {
    json_free(index_json);
    return -1;
  }
  events_json = json_object_get(index_json, "history_events");
  if (!events_json || json_type(events_json) != JSON_ARRAY) {
    json_free(index_json);
    return -1;
  }
  rc = turbo_agent_remote_session_observe_events_json_value(events_json, sink);
  if (rc == 0) {
    record_json = json_object_get(index_json, "pending_run");
    if (!record_json || json_type(record_json) != JSON_OBJECT) {
      record_json = json_object_get(index_json, "latest_run");
    }
    if (record_json && json_type(record_json) == JSON_OBJECT) {
      rc = turbo_agent_remote_session_observe_terminal_record_json(events_json, record_json, sink);
      if (rc > 0) {
        rc = 0;
      }
    }
  }
  json_free(index_json);
  return rc;
}
