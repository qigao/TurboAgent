#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_state_core_internal.h"

#include <string.h>

static const json_value_t *
turbo_agent_state_current_object_version_local(const json_value_t *state, const char *version_key) {
  const json_value_t *versions;

  if (!state || !version_key || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  versions = turbo_json_object_get(state, version_key);
  if (!versions || turbo_json_type(versions) != TURBO_JSON_ARRAY ||
      turbo_json_array_size(versions) == 0) {
    return NULL;
  }

  return turbo_json_array_get(versions, turbo_json_array_size(versions) - 1);
}

CXX_C_API const json_value_t *turbo_agent_state_get_current_object_version_const_impl(
    const json_value_t *state, const char *key) {
  return turbo_agent_state_current_object_version_local(state, key);
}

static json_value_t *turbo_agent_state_get_versions_array_local(json_value_t *state,
                                                                const char *key) {
  json_value_t *versions;

  if (!state || !key || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  versions = turbo_json_object_get(state, key);
  if (!versions) {
    versions = turbo_json_create_array();
    if (!versions) {
      return NULL;
    }
    turbo_json_object_add(state, key, versions);
  }

  return turbo_json_type(versions) == TURBO_JSON_ARRAY ? versions : NULL;
}

CXX_C_API json_value_t *turbo_agent_state_get_versions_array_impl(json_value_t *state,
                                                                  const char *key) {
  return turbo_agent_state_get_versions_array_local(state, key);
}

CXX_C_API int turbo_agent_state_append_array_version(json_value_t *state, const char *key,
                                                     json_value_t *value) {
  json_value_t *versions;

  if (!state || !key || !value || turbo_json_type(value) != TURBO_JSON_ARRAY) {
    return -1;
  }

  versions = turbo_agent_state_get_versions_array_local(state, key);
  if (!versions) {
    return -1;
  }

  turbo_json_array_add(versions, value);
  return 0;
}

CXX_C_API int turbo_agent_state_append_object_version(json_value_t *state, const char *key,
                                                      json_value_t *value) {
  json_value_t *versions;

  if (!state || !key || !value || turbo_json_type(value) != TURBO_JSON_OBJECT) {
    return -1;
  }

  versions = turbo_agent_state_get_versions_array_local(state, key);
  if (!versions) {
    return -1;
  }

  turbo_json_array_add(versions, value);
  return 0;
}

static json_value_t *turbo_agent_state_get_current_array_version_local(json_value_t *state,
                                                                       const char *key) {
  json_value_t *versions;

  versions = turbo_agent_state_get_versions_array_local(state, key);
  if (!versions || turbo_json_array_size(versions) == 0) {
    return NULL;
  }

  return turbo_json_array_get(versions, turbo_json_array_size(versions) - 1);
}

CXX_C_API json_value_t *turbo_agent_state_get_current_array_version_impl(json_value_t *state,
                                                                         const char *key) {
  return turbo_agent_state_get_current_array_version_local(state, key);
}

static json_value_t *turbo_agent_state_get_current_object_version_mutable_local(
    json_value_t *state, const char *key) {
  return turbo_agent_state_get_current_array_version_local(state, key);
}

CXX_C_API json_value_t *turbo_agent_state_get_current_object_version_impl(json_value_t *state,
                                                                          const char *key) {
  return turbo_agent_state_get_current_object_version_mutable_local(state, key);
}

static const json_value_t *turbo_agent_state_get_current_array_version_const_local(
    const json_value_t *state, const char *key) {
  const json_value_t *versions;

  if (!state || !key) {
    return NULL;
  }

  versions = turbo_json_object_get(state, key);
  return versions && turbo_json_type(versions) == TURBO_JSON_ARRAY &&
                 turbo_json_array_size(versions) > 0
             ? turbo_json_array_get(versions, turbo_json_array_size(versions) - 1)
             : NULL;
}

CXX_C_API const json_value_t *turbo_agent_state_get_current_array_version_const_impl(
    const json_value_t *state, const char *key) {
  return turbo_agent_state_get_current_array_version_const_local(state, key);
}

static const char *
turbo_agent_state_current_version_string_field_local(const json_value_t *state,
                                                     const char *version_key,
                                                     const char *field_key) {
  const json_value_t *object =
      turbo_agent_state_current_object_version_local(state, version_key);
  return object ? turbo_json_get_string(object, field_key) : NULL;
}

CXX_C_API const char *turbo_agent_state_current_version_string_field_impl(
    const json_value_t *state, const char *version_key, const char *field_key) {
  return turbo_agent_state_current_version_string_field_local(state, version_key, field_key);
}

static int turbo_agent_state_current_version_bool_field_local(const json_value_t *state,
                                                              const char *version_key,
                                                              const char *field_key,
                                                              int default_value) {
  const json_value_t *object =
      turbo_agent_state_current_object_version_local(state, version_key);
  return object ? (turbo_json_get_bool(object, field_key, default_value ? true : false) ? 1 : 0)
                : default_value;
}

CXX_C_API int turbo_agent_state_current_version_bool_field_impl(const json_value_t *state,
                                                                const char *version_key,
                                                                const char *field_key,
                                                                int default_value) {
  return turbo_agent_state_current_version_bool_field_local(state, version_key, field_key,
                                                            default_value);
}

static int turbo_agent_state_append_single_string_object_version_local(json_value_t *state,
                                                                       const char *version_key,
                                                                       const char *field_key,
                                                                       const char *field_value) {
  json_value_t *object;

  if (!state || !version_key || !field_key || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return -1;
  }

  object = turbo_json_create_object();
  if (!object) {
    return -1;
  }

  turbo_json_object_set_string(object, field_key, field_value ? field_value : "");
  return turbo_agent_state_append_object_version(state, version_key, object);
}

CXX_C_API int turbo_agent_state_append_single_string_object_version_impl(
    json_value_t *state, const char *version_key, const char *field_key,
    const char *field_value) {
  return turbo_agent_state_append_single_string_object_version_local(
      state, version_key, field_key, field_value);
}

static int turbo_agent_state_append_review_version_local(json_value_t *state, int required,
                                                         int approved, const char *note) {
  json_value_t *review_object;

  if (!state || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return -1;
  }

  review_object = turbo_json_create_object();
  if (!review_object) {
    return -1;
  }

  turbo_json_object_set_bool(review_object, "required", required ? true : false);
  turbo_json_object_set_bool(review_object, "approved", approved ? true : false);
  turbo_json_object_set_string(review_object, "note", note && note[0] != '\0' ? note : "");
  return turbo_agent_state_append_object_version(state, "review_versions", review_object);
}

CXX_C_API int turbo_agent_state_append_review_version_impl(json_value_t *state, int required,
                                                           int approved, const char *note) {
  return turbo_agent_state_append_review_version_local(state, required, approved, note);
}

CXX_C_API int turbo_agent_state_append_replan_version(json_value_t *state, int requested,
                                                      size_t count, size_t max_count,
                                                      const char *reason) {
  json_value_t *replan_object;

  if (!state || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return -1;
  }

  replan_object = turbo_json_create_object();
  if (!replan_object) {
    return -1;
  }

  turbo_json_object_set_bool(replan_object, "requested", requested ? true : false);
  turbo_json_object_set_number(replan_object, "count", (double)count);
  turbo_json_object_set_number(replan_object, "max_count", (double)max_count);
  turbo_json_object_set_string(replan_object, "reason", reason && reason[0] != '\0' ? reason : "");
  return turbo_agent_state_append_object_version(state, "replan_versions", replan_object);
}
