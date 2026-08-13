#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_state_json_value_internal.h"

static json_value_t *turbo_agent_state_to_json_object_local(
    const json_value_t *state) {
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

static json_value_t *
turbo_agent_state_array_version_json_value_local(const json_value_t *state,
                                           const char *field_name, size_t index) {
  json_value_t *json_state = turbo_agent_state_to_json_object_local(state);
  const json_value_t *versions;
  const json_value_t *version;
  json_value_t *bound = NULL;

  if (!json_state || !field_name) {
    turbo_free_json(&json_state);
    return NULL;
  }

  versions = turbo_json_object_get(json_state, field_name);
  if (versions && turbo_json_type(versions) == TURBO_JSON_ARRAY &&
      index < turbo_json_array_size(versions)) {
    version = turbo_json_array_get(versions, index);
    if (version && turbo_json_type(version) == TURBO_JSON_ARRAY) {
      bound = turbo_json_clone(version);
    }
  }

  turbo_free_json(&json_state);
  return bound;
}

static json_value_t *
turbo_agent_state_latest_array_version_json_value_local(const json_value_t *state,
                                                  const char *field_name) {
  json_value_t *json_state = turbo_agent_state_to_json_object_local(state);
  const json_value_t *versions;
  size_t count = 0;

  if (!json_state || !field_name) {
    turbo_free_json(&json_state);
    return NULL;
  }

  versions = turbo_json_object_get(json_state, field_name);
  if (versions && turbo_json_type(versions) == TURBO_JSON_ARRAY) {
    count = turbo_json_array_size(versions);
  }
  turbo_free_json(&json_state);

  if (count == 0) {
    return NULL;
  }

  return turbo_agent_state_array_version_json_value_local(state, field_name, count - 1);
}

static json_value_t *
turbo_agent_state_array_field_json_value_local(const json_value_t *state,
                                         const char *field_name) {
  json_value_t *json_state = turbo_agent_state_to_json_object_local(state);
  const json_value_t *field;
  json_value_t *bound = NULL;

  if (!json_state || !field_name) {
    turbo_free_json(&json_state);
    return NULL;
  }

  field = turbo_json_object_get(json_state, field_name);
  if (field && turbo_json_type(field) == TURBO_JSON_ARRAY) {
    bound = turbo_json_clone(field);
  }

  turbo_free_json(&json_state);
  return bound;
}

static json_value_t *
turbo_agent_state_array_item_field_json_value_local(const json_value_t *state,
                                              const char *field_name, size_t index) {
  json_value_t *json_state = turbo_agent_state_to_json_object_local(state);
  const json_value_t *field;
  const json_value_t *item;
  json_value_t *bound = NULL;

  if (!json_state || !field_name) {
    turbo_free_json(&json_state);
    return NULL;
  }

  field = turbo_json_object_get(json_state, field_name);
  if (field && turbo_json_type(field) == TURBO_JSON_ARRAY &&
      index < turbo_json_array_size(field)) {
    item = turbo_json_array_get(field, index);
    if (item && turbo_json_type(item) == TURBO_JSON_OBJECT) {
      bound = turbo_json_clone(item);
    }
  }

  turbo_free_json(&json_state);
  return bound;
}

static json_value_t *
turbo_agent_state_latest_array_item_field_json_value_local(
    const json_value_t *state, const char *field_name) {
  json_value_t *json_state = turbo_agent_state_to_json_object_local(state);
  const json_value_t *field;
  size_t count = 0;

  if (!json_state || !field_name) {
    turbo_free_json(&json_state);
    return NULL;
  }

  field = turbo_json_object_get(json_state, field_name);
  if (field && turbo_json_type(field) == TURBO_JSON_ARRAY) {
    count = turbo_json_array_size(field);
  }
  turbo_free_json(&json_state);

  if (count == 0) {
    return NULL;
  }

  return turbo_agent_state_array_item_field_json_value_local(state, field_name, count - 1);
}

json_value_t *
turbo_agent_state_planner_event_version_json_value_impl(const json_value_t *state,
                                                  size_t index) {
  return turbo_agent_state_array_version_json_value_local(state, "planner_event_versions", index);
}

json_value_t *
turbo_agent_state_latest_planner_event_version_json_value_impl(
    const json_value_t *state) {
  return turbo_agent_state_latest_array_version_json_value_local(state, "planner_event_versions");
}

json_value_t *
turbo_agent_state_executor_event_version_json_value_impl(const json_value_t *state,
                                                   size_t index) {
  return turbo_agent_state_array_version_json_value_local(state, "executor_event_versions", index);
}

json_value_t *
turbo_agent_state_latest_executor_event_version_json_value_impl(
    const json_value_t *state) {
  return turbo_agent_state_latest_array_version_json_value_local(state, "executor_event_versions");
}

json_value_t *
turbo_agent_state_completed_steps_json_value_impl(const json_value_t *state) {
  return turbo_agent_state_array_field_json_value_local(state, "completed_steps");
}

json_value_t *
turbo_agent_state_completed_step_json_value_impl(const json_value_t *state,
                                           size_t index) {
  return turbo_agent_state_array_item_field_json_value_local(state, "completed_steps", index);
}

json_value_t *
turbo_agent_state_latest_completed_step_json_value_impl(
    const json_value_t *state) {
  return turbo_agent_state_latest_array_item_field_json_value_local(state, "completed_steps");
}
