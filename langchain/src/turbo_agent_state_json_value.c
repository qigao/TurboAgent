#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_state_json_value_internal.h"

static json_value_t *turbo_agent_state_to_json_object_local(
    const json_value_t *state) {
  json_value_t *json_state;

  if (!state) {
    return NULL;
  }

  json_state = json_clone(state);
  if (!json_state) {
    return NULL;
  }

  if (json_type(json_state) != JSON_OBJECT) {
    json_free(json_state); json_state = NULL;
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
    json_free(json_state); json_state = NULL;
    return NULL;
  }

  versions = json_object_get(json_state, field_name);
  if (versions && json_type(versions) == JSON_ARRAY &&
      index < json_array_size(versions)) {
    version = json_array_get(versions, index);
    if (version && json_type(version) == JSON_ARRAY) {
      bound = json_clone(version);
    }
  }

  json_free(json_state); json_state = NULL;
  return bound;
}

static json_value_t *
turbo_agent_state_latest_array_version_json_value_local(const json_value_t *state,
                                                  const char *field_name) {
  json_value_t *json_state = turbo_agent_state_to_json_object_local(state);
  const json_value_t *versions;
  size_t count = 0;

  if (!json_state || !field_name) {
    json_free(json_state); json_state = NULL;
    return NULL;
  }

  versions = json_object_get(json_state, field_name);
  if (versions && json_type(versions) == JSON_ARRAY) {
    count = json_array_size(versions);
  }
  json_free(json_state); json_state = NULL;

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
    json_free(json_state); json_state = NULL;
    return NULL;
  }

  field = json_object_get(json_state, field_name);
  if (field && json_type(field) == JSON_ARRAY) {
    bound = json_clone(field);
  }

  json_free(json_state); json_state = NULL;
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
    json_free(json_state); json_state = NULL;
    return NULL;
  }

  field = json_object_get(json_state, field_name);
  if (field && json_type(field) == JSON_ARRAY &&
      index < json_array_size(field)) {
    item = json_array_get(field, index);
    if (item && json_type(item) == JSON_OBJECT) {
      bound = json_clone(item);
    }
  }

  json_free(json_state); json_state = NULL;
  return bound;
}

static json_value_t *
turbo_agent_state_latest_array_item_field_json_value_local(
    const json_value_t *state, const char *field_name) {
  json_value_t *json_state = turbo_agent_state_to_json_object_local(state);
  const json_value_t *field;
  size_t count = 0;

  if (!json_state || !field_name) {
    json_free(json_state); json_state = NULL;
    return NULL;
  }

  field = json_object_get(json_state, field_name);
  if (field && json_type(field) == JSON_ARRAY) {
    count = json_array_size(field);
  }
  json_free(json_state); json_state = NULL;

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
