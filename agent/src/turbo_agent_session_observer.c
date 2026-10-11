#include "turbo_agent_session.h"
#include "turbo_agent_util_internal.h"

#include <stdlib.h>

/* Agent owns successful registrations. Callback events are borrowed only for
 * synchronous delivery; the observer envelope keeps its own trace copy. */
typedef struct turbo_agent_session_observer_bridge_s {
  turbo_agent_observer_json_value_sink_t sink;
} turbo_agent_session_observer_bridge_t;

static int turbo_agent_session_observer_event_from_trace(const json_value_t *trace_event,
                                                         json_value_t **out_observer_event) {
  const char *observer_type;
  const char *name;
  json_value_t *observer_event = NULL;

  if (!trace_event || !out_observer_event) {
    return -1;
  }
  *out_observer_event = NULL;

  name = turbo_runtime_json_value_as_string(json_object_get(trace_event, "name"));
  observer_type = turbo_agent_util_observer_type_for_trace_name(name);
  if (!observer_type) {
    return 0;
  }

  observer_event = json_create_object();
  if (!observer_event ||
      turbo_agent_util_json_value_object_set_string(observer_event, "kind", "observer") != 0 ||
      turbo_agent_util_json_value_object_set_string(observer_event, "type", observer_type) != 0 ||
      turbo_agent_util_json_value_object_set_clone(observer_event, "event", trace_event) != 0 ||
      turbo_agent_util_json_value_object_set_string(observer_event, "name", name) != 0 ||
      turbo_agent_util_json_value_object_set_string(
          observer_event, "detail",
          turbo_runtime_json_value_as_string(json_object_get(trace_event, "detail"))) != 0 ||
      turbo_agent_util_json_value_object_set_string(
          observer_event, "payload",
          turbo_runtime_json_value_as_string(json_object_get(trace_event, "payload"))) != 0 ||
      turbo_agent_util_json_value_object_set_int64(
          observer_event, "status",
          turbo_runtime_json_value_as_int64(json_object_get(trace_event, "status"), 0)) != 0) {
    turbo_runtime_json_destroy(observer_event);
    return -1;
  }

  *out_observer_event = observer_event;
  return 1;
}

static void turbo_agent_session_observer_bridge_free(void *user_data) {
  turbo_agent_session_observer_bridge_t *bridge =
      (turbo_agent_session_observer_bridge_t *)user_data;

  if (!bridge) {
    return;
  }
  if (bridge->sink.user_data_free) {
    bridge->sink.user_data_free(bridge->sink.user_data);
  }
  free(bridge);
}

static void turbo_agent_session_capture_observer_event(turbo_agent_t *agent_unused,
                                                       const json_value_t *event, void *user_data) {
  turbo_agent_session_observer_bridge_t *bridge =
      (turbo_agent_session_observer_bridge_t *)user_data;
  json_value_t *observer_event = NULL;
  int rc;

  (void)agent_unused;
  if (!bridge || !bridge->sink.callback || !event) {
    return;
  }
  rc = turbo_agent_session_observer_event_from_trace(event, &observer_event);
  if (rc <= 0 || !observer_event) {
    turbo_runtime_json_destroy(observer_event);
    return;
  }
  bridge->sink.callback(observer_event, bridge->sink.user_data);
  turbo_runtime_json_destroy(observer_event);
}

CXX_C_API int
turbo_agent_session_add_trace_json_value_sink(turbo_agent_session_t *session,
                                              const turbo_agent_trace_json_value_sink_t *sink) {
  turbo_agent_t *agent = turbo_agent_session_agent(session);
  if (!agent || !sink) {
    return -1;
  }
  return turbo_agent_add_trace_json_value_sink(agent, sink);
}

CXX_C_API int turbo_agent_session_add_observer_json_value_sink(
    turbo_agent_session_t *session, const turbo_agent_observer_json_value_sink_t *sink) {
  turbo_agent_t *agent = turbo_agent_session_agent(session);
  turbo_agent_session_observer_bridge_t *bridge;
  turbo_agent_trace_json_value_sink_t trace_sink = {0};

  if (!agent || !sink || !sink->callback) {
    return -1;
  }
  bridge = (turbo_agent_session_observer_bridge_t *)calloc(1, sizeof(*bridge));
  if (!bridge) {
    return -1;
  }
  bridge->sink = *sink;
  trace_sink.callback = turbo_agent_session_capture_observer_event;
  trace_sink.user_data = bridge;
  trace_sink.user_data_free = turbo_agent_session_observer_bridge_free;
  if (turbo_agent_add_trace_json_value_sink(agent, &trace_sink) != 0) {
    /* Registration failed: the caller still owns sink.user_data. */
    free(bridge);
    return -1;
  }
  return 0;
}

CXX_C_API int turbo_agent_session_set_trace_history_enabled(turbo_agent_session_t *session,
                                                            int enabled) {
  turbo_agent_t *agent = turbo_agent_session_agent(session);
  if (!agent) {
    return -1;
  }
  return turbo_agent_set_trace_history_enabled(agent, enabled);
}
