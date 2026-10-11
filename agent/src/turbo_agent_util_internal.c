#include "turbo_agent_util_internal.h"

#include <json_parser.h>

#include <stdlib.h>
#include <string.h>
#include "turbo_runtime_segment.h"

CXX_C_API tstr turbo_agent_util_strdup(const char *src) {
  return tstr_dup(src);
}

CXX_C_API int turbo_agent_util_capture_summary_ids(
    const json_value_t *summary, tstr *thread_id, tstr *run_id, tstr *checkpoint_id) {
  const char *next_thread;
  const char *next_run;
  const char *next_checkpoint;
  tstr thread_copy = NULL;
  tstr run_copy = NULL;
  tstr checkpoint_copy = NULL;

  if (!summary || !thread_id || !run_id || !checkpoint_id ||
      thread_id == run_id || thread_id == checkpoint_id || run_id == checkpoint_id) return -1;
  next_thread = json_get_string(summary, "thread_id");
  next_run = json_get_string(summary, "run_id");
  next_checkpoint = json_get_string(summary, "checkpoint_id");
  if (!next_thread || !next_run) return -1;

  thread_copy = tstr_dup(next_thread);
  if (!thread_copy) goto failure;
  run_copy = tstr_dup(next_run);
  if (!run_copy) goto failure;
  if (next_checkpoint && next_checkpoint[0]) {
    checkpoint_copy = tstr_dup(next_checkpoint);
    if (!checkpoint_copy) goto failure;
  }

  /* No fallible work remains. Old ids survive every preparation failure. */
  tstr_free(*thread_id);
  tstr_free(*run_id);
  *thread_id = thread_copy;
  *run_id = run_copy;
  if (checkpoint_copy) {
    tstr_free(*checkpoint_id);
    *checkpoint_id = checkpoint_copy;
  }
  return 0;

failure:
  tstr_free(thread_copy);
  tstr_free(run_copy);
  tstr_free(checkpoint_copy);
  return -1;
}

CXX_C_API int turbo_agent_util_json_value_object_set_string(
    json_value_t *object, const char *key, const char *value) {
  return turbo_runtime_json_object_set_string(object, key, value) ==
                 TURBO_RUNTIME_JSON_OK ? 0 : -1;
}

CXX_C_API int turbo_agent_util_json_value_object_set_int64(
    json_value_t *object, const char *key, int64_t value) {
  return turbo_runtime_json_object_set_int64(object, key, value) ==
                 TURBO_RUNTIME_JSON_OK ? 0 : -1;
}

CXX_C_API int turbo_agent_util_json_value_object_set_clone(
    json_value_t *object, const char *key, const json_value_t *value) {
  return turbo_runtime_json_object_set_clone(object, key, value) ==
                 TURBO_RUNTIME_JSON_OK ? 0 : -1;
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
  tstr next;

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

int turbo_agent_runtime_make_timestamp(char *buffer, size_t buffer_size) {
  return turbo_runtime_record_timestamp(buffer, buffer_size);
}
