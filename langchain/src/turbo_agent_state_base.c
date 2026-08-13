#include "turbo_agent_state.h"
#include "turbo_agent_state_core_internal.h"
#include "turbo_event.h"
#include "turbo_prompt.h"

#include <stdlib.h>

#define TURBO_AGENT_STATE_SCHEMA_VERSION_LOCAL 1

static json_value_t *
turbo_agent_state_to_json_object_local(const json_value_t *state) {
  json_value_t *json_state;

  if (!state) {
    return NULL;
  }

  json_state = turbo_json_clone(state);
  if (!json_state) {
    return NULL;
  }

  if (turbo_json_type(json_state) != TURBO_JSON_OBJECT) {
    turbo_free_json(&json_state);
    return NULL;
  }

  return json_state;
}

static json_value_t *turbo_agent_state_get_array_local(json_value_t *state, const char *key) {
  json_value_t *value;

  if (!state || !key || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  value = turbo_json_object_get(state, key);
  return value && turbo_json_type(value) == TURBO_JSON_ARRAY ? value : NULL;
}

static const json_value_t *turbo_agent_state_get_array_const_local(const json_value_t *state,
                                                                   const char *key) {
  return turbo_agent_state_get_array_local((json_value_t *)state, key);
}

static json_value_t *turbo_agent_state_get_object_local(json_value_t *state, const char *key) {
  json_value_t *value;

  if (!state || !key || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  value = turbo_json_object_get(state, key);
  return value && turbo_json_type(value) == TURBO_JSON_OBJECT ? value : NULL;
}

static const json_value_t *turbo_agent_state_get_object_const_local(const json_value_t *state,
                                                                    const char *key) {
  return turbo_agent_state_get_object_local((json_value_t *)state, key);
}

static json_value_t *turbo_agent_state_get_or_create_array_local(json_value_t *state,
                                                                 const char *key) {
  json_value_t *array;

  if (!state || !key || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  array = turbo_json_object_get(state, key);
  if (!array) {
    array = turbo_json_create_array();
    if (!array) {
      return NULL;
    }
    turbo_json_object_add(state, key, array);
  }

  return turbo_json_type(array) == TURBO_JSON_ARRAY ? array : NULL;
}

static json_value_t *turbo_agent_state_get_or_create_object_local(json_value_t *state,
                                                                  const char *key) {
  json_value_t *object;

  if (!state || !key || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  object = turbo_json_object_get(state, key);
  if (!object) {
    object = turbo_json_create_object();
    if (!object) {
      return NULL;
    }
    turbo_json_object_add(state, key, object);
  }

  return turbo_json_type(object) == TURBO_JSON_OBJECT ? object : NULL;
}

CXX_C_API json_value_t *turbo_agent_state_get_array_impl(json_value_t *state, const char *key) {
  return turbo_agent_state_get_array_local(state, key);
}

CXX_C_API json_value_t *turbo_agent_state_get_or_create_array_impl(json_value_t *state,
                                                                   const char *key) {
  return turbo_agent_state_get_or_create_array_local(state, key);
}

CXX_C_API json_value_t *turbo_agent_state_get_object_impl(json_value_t *state, const char *key) {
  return turbo_agent_state_get_object_local(state, key);
}

CXX_C_API json_value_t *turbo_agent_state_get_or_create_object_impl(json_value_t *state,
                                                                    const char *key) {
  return turbo_agent_state_get_or_create_object_local(state, key);
}

CXX_C_API const json_value_t *turbo_agent_state_get_object_const_impl(
    const json_value_t *state, const char *key) {
  return turbo_agent_state_get_object_const_local(state, key);
}

CXX_C_API const json_value_t *turbo_agent_state_get_array_const_impl(
    const json_value_t *state, const char *key) {
  return turbo_agent_state_get_array_const_local(state, key);
}

size_t turbo_agent_state_schema_version_impl(void) {
  return TURBO_AGENT_STATE_SCHEMA_VERSION_LOCAL;
}

json_value_t *turbo_agent_state_create_impl(void) {
  json_value_t *state = turbo_json_create_object();
  json_value_t *input;
  json_value_t *events;

  if (!state) {
    return NULL;
  }

  input = turbo_json_create_array();
  events = turbo_json_create_array();
  if (!input || !events) {
    turbo_free_json(&input);
    turbo_free_json(&events);
    turbo_free_json(&state);
    return NULL;
  }

  turbo_json_object_set_number(state, "state_version",
                               (double)TURBO_AGENT_STATE_SCHEMA_VERSION_LOCAL);
  turbo_json_object_add(state, "input", input);
  turbo_json_object_add(state, "events", events);
  return state;
}

json_value_t *turbo_agent_state_create_json_value_impl(void) {
  json_value_t *state = turbo_agent_state_create();
  json_value_t *bound;

  if (!state) {
    return NULL;
  }

  bound = turbo_json_clone(state);
  turbo_free_json(&state);
  return bound;
}

size_t turbo_agent_state_version_impl(const json_value_t *state) {
  if (!state || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return 0;
  }

  return (size_t)turbo_json_get_double(state, "state_version", 0);
}

int turbo_agent_state_version_supported_impl(const json_value_t *state) {
  size_t version = turbo_agent_state_version(state);
  return version == 0 || version == TURBO_AGENT_STATE_SCHEMA_VERSION_LOCAL ? 1 : 0;
}

int turbo_agent_state_add_user_message_impl(json_value_t *state, const char *text) {
  json_value_t *input;

  if (!state || !text) {
    return -1;
  }

  input = turbo_agent_state_get_array_local(state, "input");
  if (!input) {
    return -1;
  }

  return turbo_prompt_messages_append(input, "user", text) == TURBO_PROMPT_OK ? 0 : -1;
}

const json_value_t *turbo_agent_state_events_impl(const json_value_t *state) {
  return turbo_agent_state_get_array_const_local(state, "events");
}

size_t turbo_agent_state_event_count_impl(const json_value_t *state) {
  const json_value_t *events = turbo_agent_state_events(state);
  return events && turbo_json_type(events) == TURBO_JSON_ARRAY ? turbo_json_array_size(events) : 0;
}

const json_value_t *turbo_agent_state_event_at_impl(const json_value_t *state, size_t index) {
  const json_value_t *events = turbo_agent_state_events(state);

  if (!events || turbo_json_type(events) != TURBO_JSON_ARRAY ||
      index >= turbo_json_array_size(events)) {
    return NULL;
  }

  return turbo_json_array_get(events, index);
}

const json_value_t *turbo_agent_state_trace_events_impl(const json_value_t *state) {
  return turbo_agent_state_get_array_const_local(state, "trace_events");
}

size_t turbo_agent_state_trace_event_count_impl(const json_value_t *state) {
  const json_value_t *events = turbo_agent_state_trace_events(state);
  return events && turbo_json_type(events) == TURBO_JSON_ARRAY ? turbo_json_array_size(events) : 0;
}

const json_value_t *turbo_agent_state_trace_event_at_impl(const json_value_t *state,
                                                          size_t index) {
  const json_value_t *events = turbo_agent_state_trace_events(state);

  if (!events || turbo_json_type(events) != TURBO_JSON_ARRAY ||
      index >= turbo_json_array_size(events)) {
    return NULL;
  }

  return turbo_json_array_get(events, index);
}

json_value_t *
turbo_agent_state_trace_events_json_value_impl(const json_value_t *state) {
  json_value_t *json_state = turbo_agent_state_to_json_object_local(state);
  const json_value_t *events;
  json_value_t *bound = NULL;

  if (!json_state) {
    return NULL;
  }

  events = turbo_agent_state_trace_events(json_state);
  if (events) {
    bound = turbo_json_clone(events);
  }

  turbo_free_json(&json_state);
  return bound;
}

int turbo_agent_state_add_trace_event_json_value_impl(
    json_value_t *state, const json_value_t *event) {
  const json_value_t *trace_events_const;
  json_value_t *trace_events;
  json_value_t *event_copy;

  if (!state || !event ||
      turbo_json_type(state) != TURBO_JSON_OBJECT ||
      turbo_event_trace_validate_json_value(event) != 0) {
    return -1;
  }

  trace_events_const = turbo_json_object_get(state, "trace_events");
  if (!trace_events_const) {
    trace_events = turbo_json_create_array();
    if (!trace_events ||
        turbo_runtime_json_object_set(state, "trace_events", trace_events) !=
            TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_json_destroy(trace_events);
      return -1;
    }
    trace_events_const = turbo_json_object_get(state, "trace_events");
  }

  if (!trace_events_const ||
      turbo_json_type(trace_events_const) !=
          TURBO_JSON_ARRAY) {
    return -1;
  }

  event_copy = turbo_json_clone(event);
  if (!event_copy) {
    return -1;
  }

  if (turbo_runtime_json_array_append((json_value_t *)trace_events_const,
                                           event_copy) != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(event_copy);
    return -1;
  }

  return 0;
}

void turbo_agent_state_capture_trace_event_json_value_impl(
    const json_value_t *event, void *user_data) {
  json_value_t *state = (json_value_t *)user_data;

  if (!state) {
    return;
  }

  turbo_agent_state_add_trace_event_json_value(state, event);
}
