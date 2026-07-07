#include "turbo_agent_runtime.h"

#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_runtime_v1_internal.h"
#include "turbo_agent_state.h"
#include "turbo_agent_util_internal.h"

#include <stdlib.h>
#include <string.h>

static int turbo_agent_runtime_merge_state_patch_object_bind(
    turbo_runtime_data_bind_value_t *target_object,
    const turbo_runtime_data_bind_value_t *patch_object) {
  size_t i;
  size_t count;

  if (!target_object || !patch_object ||
      turbo_runtime_data_bind_value_kind(target_object) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT ||
      turbo_runtime_data_bind_value_kind(patch_object) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    return -1;
  }

  count = turbo_runtime_data_bind_value_size(patch_object);
  for (i = 0; i < count; ++i) {
    const char *key = turbo_runtime_data_bind_object_key_at(patch_object, i);
    const turbo_runtime_data_bind_value_t *patch_value;
    const turbo_runtime_data_bind_value_t *target_value;
    turbo_runtime_data_bind_value_t *copy;

    if (!key) {
      return -1;
    }
    patch_value = turbo_runtime_data_bind_object_get(patch_object, key);
    if (!patch_value) {
      return -1;
    }
    target_value = turbo_runtime_data_bind_object_get(target_object, key);
    if (target_value &&
        turbo_runtime_data_bind_value_kind(patch_value) == TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT &&
        turbo_runtime_data_bind_value_kind(target_value) == TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
      if (turbo_agent_runtime_merge_state_patch_object_bind(
              (turbo_runtime_data_bind_value_t *)target_value, patch_value) != 0) {
        return -1;
      }
      continue;
    }
    copy = turbo_runtime_data_bind_value_clone(patch_value);
    if (!copy) {
      return -1;
    }
    if (turbo_runtime_data_bind_object_set(target_object, key, copy) !=
        TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(copy);
      return -1;
    }
  }
  return 0;
}

static int turbo_agent_runtime_apply_state_patch_value_bind(
    const turbo_runtime_data_bind_value_t *state,
    const turbo_runtime_data_bind_value_t *patch,
    turbo_runtime_data_bind_value_t **out_state_override) {
  turbo_runtime_data_bind_value_t *updated_state = NULL;
  turbo_runtime_data_bind_value_kind_t state_kind;
  turbo_runtime_data_bind_value_kind_t patch_kind;
  int rc = -1;

  if (!state || !patch || !out_state_override) {
    return -1;
  }
  *out_state_override = NULL;

  state_kind = turbo_runtime_data_bind_value_kind(state);
  patch_kind = turbo_runtime_data_bind_value_kind(patch);
  if (state_kind == TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT &&
      patch_kind == TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    updated_state = turbo_runtime_data_bind_value_clone(state);
    if (!updated_state) {
      return -1;
    }
    if (turbo_agent_runtime_merge_state_patch_object_bind(updated_state, patch) != 0) {
      goto cleanup;
    }
  } else {
    updated_state = turbo_runtime_data_bind_value_clone(patch);
    if (!updated_state) {
      goto cleanup;
    }
  }

  *out_state_override = updated_state;
  updated_state = NULL;
  rc = 0;

cleanup:
  turbo_runtime_data_bind_value_destroy(updated_state);
  return rc;
}

static const char *turbo_agent_runtime_command_string(const json_value_t *command_json,
                                                      const char *primary_key) {
  const char *value;

  if (!command_json) {
    return NULL;
  }
  value = turbo_json_get_string(command_json, primary_key);
  if (value && value[0] != '\0') {
    return value;
  }
  return NULL;
}

static char *turbo_agent_runtime_command_text_owned(const json_value_t *command_json,
                                                    const char *primary_key) {
  const char *text_value;
  const json_value_t *json_value;
  char *serialized = NULL;
  char *owned_value = NULL;

  if (!command_json) {
    return NULL;
  }
  text_value = turbo_agent_runtime_command_string(command_json, primary_key);
  if (text_value && text_value[0] != '\0') {
    return turbo_agent_runtime_strdup(text_value);
  }

  json_value = primary_key ? turbo_json_object_get(command_json, primary_key) : NULL;
  if (!json_value || turbo_json_type(json_value) == TURBO_JSON_NULL) {
    return NULL;
  }
  if (turbo_json_type(json_value) == TURBO_JSON_STRING) {
    text_value = turbo_json_string(json_value);
    return text_value ? turbo_agent_runtime_strdup(text_value) : NULL;
  }
  serialized = turbo_json_serialize(json_value, NULL);
  if (!serialized) {
    return NULL;
  }
  owned_value = turbo_agent_runtime_strdup(serialized);
  turbo_json_serialize_free(serialized);
  return owned_value;
}

static int turbo_agent_runtime_apply_command_json(json_value_t *state,
                                                  const json_value_t *command_json) {
  const char *kind;
  const json_value_t *approved_value;
  int approved;
  const char *text;
  char *owned_text = NULL;
  int rc = -1;

  if (!state || !command_json || turbo_json_type(command_json) != TURBO_JSON_OBJECT) {
    return -1;
  }
  kind = turbo_json_get_string(command_json, "kind");
  if (!kind || kind[0] == '\0') {
    return -1;
  }

  if (strcmp(kind, "approve_review") == 0) {
    approved_value = turbo_json_object_get(command_json, "approved");
    approved = approved_value ? turbo_json_get_bool(command_json, "approved", 1) : 1;
    return turbo_agent_state_set_review_approved(state, approved);
  }
  if (strcmp(kind, "reject_review") == 0) {
    owned_text = turbo_agent_runtime_command_text_owned(command_json, "reason");
    if (owned_text && owned_text[0] != '\0') {
      rc = turbo_agent_state_request_review(state, owned_text);
      free(owned_text);
      return rc;
    }
    free(owned_text);
    return turbo_agent_state_set_review_approved(state, 0);
  }
  if (strcmp(kind, "request_replan") == 0) {
    text = turbo_agent_runtime_command_string(command_json, "reason");
    rc = turbo_agent_state_clear_review(state);
    if (rc != 0) {
      return rc;
    }
    return turbo_agent_state_request_replan(state, text);
  }
  if (strcmp(kind, "append_feedback") == 0) {
    owned_text = turbo_agent_runtime_command_text_owned(command_json, "text");
    if (!owned_text || owned_text[0] == '\0') {
      free(owned_text);
      return -1;
    }
    rc = turbo_agent_state_add_user_message(state, owned_text);
    free(owned_text);
    return rc;
  }
  if (strcmp(kind, "append_user_message") == 0) {
    owned_text = turbo_agent_runtime_command_text_owned(command_json, "text");
    if (!owned_text || owned_text[0] == '\0') {
      free(owned_text);
      return -1;
    }
    rc = turbo_agent_state_add_user_message(state, owned_text);
    free(owned_text);
    return rc;
  }
  if (strcmp(kind, "override_final_output") == 0) {
    owned_text = turbo_agent_runtime_command_text_owned(command_json, "text");
    if (!owned_text || owned_text[0] == '\0') {
      free(owned_text);
      owned_text = turbo_agent_runtime_command_text_owned(command_json, "output_json");
    }
    if (!owned_text || owned_text[0] == '\0') {
      free(owned_text);
      return -1;
    }
    rc = turbo_agent_state_set_final_answer(state, owned_text);
    free(owned_text);
    return rc;
  }
  return -1;
}

static int turbo_agent_runtime_run_command_bind(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state, int fork_run) {
  turbo_runtime_data_bind_value_t *state_override = NULL;
  int rc;

  if (!runtime || !graph || !checkpoint_id || !command || !out_summary_json || !out_state) {
    return -1;
  }
  rc = turbo_agent_runtime_prepare_checkpoint_command_override_bind(runtime, checkpoint_id, command, &state_override);
  if (rc != 0) {
    return rc;
  }
  if (fork_run) {
    rc = turbo_agent_runtime_fork_bind_graph_stream(runtime, graph, checkpoint_id, state_override,
                                                    options, NULL, NULL, out_summary_json,
                                                    out_state);
  } else {
    rc = turbo_agent_runtime_resume_bind_graph_stream(runtime, graph, checkpoint_id, state_override,
                                                      options, NULL, NULL, out_summary_json,
                                                      out_state);
  }
  turbo_runtime_data_bind_value_destroy(state_override);
  return rc;
}


int turbo_agent_runtime_prepare_checkpoint_command_override_bind(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override) {
  turbo_runtime_data_bind_value_t *state = NULL;
  json_value_t *state_json = NULL;
  json_value_t *command_json = NULL;
  turbo_runtime_data_bind_value_t *updated_state = NULL;
  int rc = -1;

  if (!runtime || !checkpoint_id || !command || !out_state_override) {
    return -1;
  }
  *out_state_override = NULL;
  if (turbo_agent_runtime_get_checkpoint_state_bind(runtime, checkpoint_id, &state) != 0 || !state) {
    goto cleanup;
  }
  state_json = turbo_runtime_data_bind_value_to_json(state);
  command_json = turbo_runtime_data_bind_value_to_json(command);
  if (!state_json || !command_json ||
      turbo_agent_runtime_apply_command_json(state_json, command_json) != 0) {
    goto cleanup;
  }
  updated_state = turbo_runtime_data_bind_value_from_json(state_json);
  if (!updated_state) {
    goto cleanup;
  }
  *out_state_override = updated_state;
  updated_state = NULL;
  rc = 0;

cleanup:
  turbo_runtime_data_bind_value_destroy(updated_state);
  turbo_free_json(&command_json);
  turbo_free_json(&state_json);
  turbo_runtime_data_bind_value_destroy(state);
  return rc;
}

int turbo_agent_runtime_prepare_checkpoint_state_override_bind(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override) {
  turbo_runtime_data_bind_value_t *state = NULL;
  int rc;

  if (!runtime || !checkpoint_id || !checkpoint_id[0] || !state_patch || !out_state_override) {
    return -1;
  }
  *out_state_override = NULL;
  rc = turbo_agent_runtime_get_checkpoint_state_bind(runtime, checkpoint_id, &state);
  if (rc != 0 || !state) {
    turbo_runtime_data_bind_value_destroy(state);
    return -1;
  }
  rc = turbo_agent_runtime_apply_state_patch_value_bind(state, state_patch, out_state_override);
  turbo_runtime_data_bind_value_destroy(state);
  return rc;
}



