#include "turbo_agent_util_internal.h"

#include <stdlib.h>
#include <string.h>

CXX_C_API tstr_t turbo_agent_util_strdup(const char *src) {
  return tstr_dup(src);
}

CXX_C_API int turbo_agent_util_json_value_object_set_string(
    json_value_t *object, const char *key, const char *value) {
  json_value_t *field;

  if (!object || !key || !value) {
    return -1;
  }
  field = turbo_json_create_string(value);
  if (!field) {
    return -1;
  }
  if (turbo_runtime_json_object_set(object, key, field) != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(field);
    return -1;
  }
  return 0;
}

CXX_C_API int turbo_agent_util_json_value_object_set_int64(
    json_value_t *object, const char *key, int64_t value) {
  json_value_t *field;

  if (!object || !key) {
    return -1;
  }
  field = turbo_json_create_int64(value);
  if (!field) {
    return -1;
  }
  if (turbo_runtime_json_object_set(object, key, field) != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(field);
    return -1;
  }
  return 0;
}

CXX_C_API int turbo_agent_util_json_value_object_set_clone(
    json_value_t *object, const char *key,
    const json_value_t *value) {
  json_value_t *copy;

  if (!object || !key || !value) {
    return -1;
  }
  copy = turbo_json_clone(value);
  if (!copy) {
    return -1;
  }
  if (turbo_runtime_json_object_set(object, key, copy) != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(copy);
    return -1;
  }
  return 0;
}

CXX_C_API const char *turbo_agent_util_observer_type_for_trace_name(const char *name) {
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

CXX_C_API void turbo_agent_util_free_user_data(void *user_data) { free(user_data); }

CXX_C_API int turbo_agent_util_append_bytes(char **buffer, size_t *length, const char *data,
                                            size_t data_len) {
  tstr_t next;

  if (!buffer || (!data && data_len > 0)) {
    return -1;
  }

  if (!*buffer) {
    next = tstr_new_len(data, data_len);
  } else {
    next = tstr_cat_len(*buffer, data, data_len);
  }

  if (!next) {
    return -1;
  }

  *buffer = (char *)next;
  if (length) {
    *length = tstr_len(next);
  }
  return 0;
}

CXX_C_API int turbo_agent_util_append_text(char **buffer, size_t *length, const char *text) {
  if (!buffer || !text) {
    return -1;
  }

  return turbo_agent_util_append_bytes(buffer, length, text, strlen(text));
}
