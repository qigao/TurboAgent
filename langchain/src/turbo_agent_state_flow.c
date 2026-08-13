#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_state_flow_domain_internal.h"
#include "turbo_agent_state_json_value_internal.h"
#include "turbo_agent_state_flow_internal.h"
#include "turbo_agent_event_internal.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_util_internal.h"

#include "turbo_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *turbo_agent_state_extract_first_json_value_local(const char *text) {
  size_t i;
  size_t start = 0;
  size_t len;
  char *copy;
  int seen_start = 0;
  int depth = 0;
  int in_string = 0;
  int escaped = 0;

  if (!text) {
    return NULL;
  }

  for (i = 0; text[i] != '\0'; ++i) {
    char ch = text[i];

    if (!seen_start) {
      if (ch == '{' || ch == '[') {
        seen_start = 1;
        start = i;
        depth = 1;
      }
      continue;
    }

    if (in_string) {
      if (escaped) {
        escaped = 0;
      } else if (ch == '\\') {
        escaped = 1;
      } else if (ch == '"') {
        in_string = 0;
      }
      continue;
    }

    if (ch == '"') {
      in_string = 1;
    } else if (ch == '{' || ch == '[') {
      depth++;
    } else if (ch == '}' || ch == ']') {
      depth--;
      if (depth == 0) {
        len = (i + 1) - start;
        copy = (char *)malloc(len + 1);
        if (!copy) {
          return NULL;
        }
        memcpy(copy, text + start, len);
        copy[len] = '\0';
        return copy;
      }
    }
  }

  return NULL;
}

static int turbo_agent_state_plan_normalize_steps_local(const json_value_t *input_steps,
                                                        json_value_t **out_steps) {
  json_value_t *steps = NULL;
  size_t i;

  if (!input_steps || !out_steps || turbo_json_type(input_steps) != TURBO_JSON_ARRAY) {
    return -1;
  }

  *out_steps = NULL;
  steps = turbo_json_create_array();
  if (!steps) {
    return -1;
  }

  for (i = 0; i < turbo_json_array_size(input_steps); ++i) {
    const json_value_t *step = turbo_json_array_get(input_steps, i);
    json_value_t *normalized_step = turbo_json_create_object();
    const char *text = NULL;
    char *serialized_text = NULL;

    if (!normalized_step) {
      turbo_free_json(&steps);
      return -1;
    }

    if (step && turbo_json_type(step) == TURBO_JSON_STRING) {
      text = turbo_json_string(step);
    } else if (step && turbo_json_type(step) == TURBO_JSON_OBJECT) {
      text = turbo_json_get_string(step, "text");
      if (!text) {
        text = turbo_json_get_string(step, "title");
      }
      if (!text) {
        text = turbo_json_get_string(step, "step");
      }
      if (!text) {
        serialized_text = turbo_json_serialize(step, NULL);
        text = serialized_text;
      }
    }

    if (!text || text[0] == '\0') {
      free(serialized_text);
      turbo_free_json(&normalized_step);
      turbo_free_json(&steps);
      return -1;
    }

    turbo_json_object_set_string(normalized_step, "text", text);
    turbo_json_object_set_string(normalized_step, "status", "pending");
    if (step && turbo_json_type(step) == TURBO_JSON_OBJECT) {
      json_value_t *step_data = NULL;
      if (turbo_agent_clone_json(step, &step_data) != TURBO_GRAPH_EXEC_OK) {
        free(serialized_text);
        turbo_free_json(&normalized_step);
        turbo_free_json(&steps);
        return -1;
      }
      turbo_json_object_add(normalized_step, "data", step_data);
    }
    turbo_json_array_add(steps, normalized_step);
    free(serialized_text);
  }

  *out_steps = steps;
  return 0;
}

static int turbo_agent_state_set_plan_object_local(json_value_t *state, json_value_t *plan_object) {
  json_value_t *versions;
  json_value_t *progress_versions;
  json_value_t *progress;

  if (!state || !plan_object || turbo_json_type(state) != TURBO_JSON_OBJECT ||
      turbo_json_type(plan_object) != TURBO_JSON_OBJECT) {
    return -1;
  }

  versions = turbo_json_object_get(state, "plan_versions");
  if (!versions) {
    versions = turbo_json_create_array();
    if (!versions) {
      return -1;
    }
    turbo_json_object_add(state, "plan_versions", versions);
  }

  if (turbo_json_type(versions) != TURBO_JSON_ARRAY) {
    return -1;
  }

  progress_versions = turbo_json_object_get(state, "plan_progress_versions");
  if (!progress_versions) {
    progress_versions = turbo_json_create_array();
    if (!progress_versions) {
      return -1;
    }
    turbo_json_object_add(state, "plan_progress_versions", progress_versions);
  }

  if (turbo_json_type(progress_versions) != TURBO_JSON_ARRAY) {
    return -1;
  }

  progress = turbo_json_create_array();
  if (!progress) {
    return -1;
  }

  turbo_json_array_add(versions, plan_object);
  turbo_json_array_add(progress_versions, progress);
  return 0;
}

static const json_value_t *turbo_agent_state_plan_steps_const_local(const json_value_t *state) {
  const json_value_t *plan = turbo_agent_state_plan(state);
  return plan && turbo_json_type(plan) == TURBO_JSON_OBJECT ? turbo_json_object_get(plan, "steps")
                                                            : NULL;
}

static size_t turbo_agent_state_current_version_size_field_local(const json_value_t *state,
                                                                 const char *version_key,
                                                                 const char *field_key) {
  const json_value_t *value =
      turbo_agent_state_get_current_object_version_const(state, version_key);
  return value && turbo_json_type(value) == TURBO_JSON_OBJECT
             ? (size_t)turbo_json_get_double(value, field_key, 0.0)
             : 0;
}

static int turbo_agent_state_append_two_string_object_version_local(
    json_value_t *state, const char *version_key, const char *field1_key,
    const char *field1_value, const char *field2_key, const char *field2_value) {
  json_value_t *object;

  if (!state || !version_key || !field1_key || !field2_key ||
      turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return -1;
  }

  object = turbo_json_create_object();
  if (!object) {
    return -1;
  }

  turbo_json_object_set_string(object, field1_key, field1_value ? field1_value : "");
  turbo_json_object_set_string(object, field2_key, field2_value ? field2_value : "");
  return turbo_agent_state_append_object_version(state, version_key, object);
}

static const json_value_t *turbo_agent_state_flow_array_last_const(const json_value_t *array) {
  return array && turbo_json_type(array) == TURBO_JSON_ARRAY && turbo_json_array_size(array) > 0
             ? turbo_json_array_get(array, turbo_json_array_size(array) - 1)
             : NULL;
}

static const json_value_t *turbo_agent_state_latest_executor_events_local(const json_value_t *state) {
  return turbo_agent_state_flow_array_last_const(turbo_agent_state_executor_event_versions(state));
}

static const json_value_t *turbo_agent_completed_steps_local(const json_value_t *state) {
  return state && turbo_json_type(state) == TURBO_JSON_OBJECT
             ? turbo_json_object_get(state, "completed_steps")
             : NULL;
}

int turbo_agent_state_set_plan_from_json_impl(json_value_t *state, const char *plan_json) {
  json_value_t *root = NULL;
  json_value_t *plan_object = NULL;
  json_value_t *steps = NULL;
  const json_value_t *input_steps = NULL;
  char *extracted_json = NULL;
  int rc = -1;

  if (!state || !plan_json) {
    return -1;
  }

  if (turbo_parse_json((const uint8_t *)plan_json, strlen(plan_json), &root) != 0) {
    extracted_json = turbo_agent_state_extract_first_json_value_local(plan_json);
    if (!extracted_json ||
        turbo_parse_json((const uint8_t *)extracted_json, strlen(extracted_json), &root) != 0) {
      free(extracted_json);
      return -1;
    }
  }

  if (turbo_json_type(root) == TURBO_JSON_ARRAY) {
    input_steps = root;
  } else if (turbo_json_type(root) == TURBO_JSON_OBJECT) {
    input_steps = turbo_json_object_get(root, "steps");
  }

  if (!input_steps || turbo_agent_state_plan_normalize_steps_local(input_steps, &steps) != 0) {
    turbo_free_json(&root);
    free(extracted_json);
    return -1;
  }

  plan_object = turbo_json_create_object();
  if (!plan_object) {
    turbo_free_json(&steps);
    turbo_free_json(&root);
    free(extracted_json);
    return -1;
  }

  turbo_json_object_add(plan_object, "steps", steps);
  rc = turbo_agent_state_set_plan_object_local(state, plan_object);
  if (rc != 0) {
    turbo_free_json(&plan_object);
  }
  turbo_free_json(&root);
  free(extracted_json);
  return rc;
}

const json_value_t *turbo_agent_state_plan_impl(const json_value_t *state) {
  return turbo_agent_state_get_current_object_version_const(state, "plan_versions");
}

size_t turbo_agent_state_plan_step_count_impl(const json_value_t *state) {
  const json_value_t *steps = turbo_agent_state_plan_steps_const_local(state);

  if (!steps || turbo_json_type(steps) != TURBO_JSON_ARRAY) {
    return 0;
  }

  return turbo_json_array_size(steps);
}

size_t turbo_agent_state_plan_step_index_impl(const json_value_t *state) {
  const json_value_t *progress =
      turbo_agent_state_get_current_array_version_const(state, "plan_progress_versions");

  return progress && turbo_json_type(progress) == TURBO_JSON_ARRAY ? turbo_json_array_size(progress)
                                                                   : 0;
}

const char *turbo_agent_state_current_plan_step_text_impl(const json_value_t *state) {
  const json_value_t *steps;
  const json_value_t *step;
  size_t index;

  steps = turbo_agent_state_plan_steps_const_local(state);
  if (!steps || turbo_json_type(steps) != TURBO_JSON_ARRAY) {
    return NULL;
  }

  index = turbo_agent_state_plan_step_index(state);
  if (index >= turbo_json_array_size(steps)) {
    return NULL;
  }

  step = turbo_json_array_get(steps, index);
  if (!step || turbo_json_type(step) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  return turbo_json_get_string(step, "text");
}

int turbo_agent_state_advance_plan_impl(json_value_t *state) {
  json_value_t *progress;
  const json_value_t *steps;
  size_t current_index;
  size_t total;

  if (!state) {
    return -1;
  }

  progress = turbo_agent_state_get_current_array_version(state, "plan_progress_versions");
  if (!progress || turbo_json_type(progress) != TURBO_JSON_ARRAY) {
    return -1;
  }

  steps = turbo_agent_state_plan_steps_const_local(state);
  if (!steps || turbo_json_type(steps) != TURBO_JSON_ARRAY) {
    return -1;
  }

  total = turbo_json_array_size(steps);
  current_index = turbo_json_array_size(progress);
  if (current_index >= total) {
    return 0;
  }

  turbo_json_array_add(progress, turbo_json_create_bool(true));
  return 0;
}

int turbo_agent_state_plan_complete_impl(const json_value_t *state) {
  return turbo_agent_state_plan_step_index(state) >= turbo_agent_state_plan_step_count(state) ? 1
                                                                                               : 0;
}

int turbo_agent_state_request_replan_impl(json_value_t *state, const char *reason) {
  size_t current_count = turbo_agent_state_replan_count(state);
  size_t max_count = turbo_agent_state_replan_limit(state);

  return turbo_agent_state_append_replan_version(state, 1, current_count + 1, max_count, reason);
}

int turbo_agent_state_replan_requested_impl(const json_value_t *state) {
  return turbo_agent_state_current_version_bool_field(state, "replan_versions", "requested", 0);
}

int turbo_agent_state_set_replan_limit_impl(json_value_t *state, size_t max_replans) {
  const char *reason = turbo_agent_state_replan_reason(state);
  size_t count = turbo_agent_state_replan_count(state);
  int requested = turbo_agent_state_replan_requested(state);

  return turbo_agent_state_append_replan_version(state, requested, count, max_replans, reason);
}

size_t turbo_agent_state_replan_count_impl(const json_value_t *state) {
  return turbo_agent_state_current_version_size_field_local(state, "replan_versions", "count");
}

size_t turbo_agent_state_replan_limit_impl(const json_value_t *state) {
  return turbo_agent_state_current_version_size_field_local(state, "replan_versions", "max_count");
}

const char *turbo_agent_state_replan_reason_impl(const json_value_t *state) {
  return turbo_agent_state_current_version_string_field(state, "replan_versions", "reason");
}

int turbo_agent_state_set_guardrail_rejection_impl(json_value_t *state, const char *phase,
                                                   const char *reason) {
  return turbo_agent_state_append_two_string_object_version_local(
      state, "guardrail_versions", "phase", phase, "reason", reason);
}

const char *turbo_agent_state_guardrail_rejection_phase_impl(const json_value_t *state) {
  const char *phase =
      turbo_agent_state_current_version_string_field(state, "guardrail_versions", "phase");
  return phase && phase[0] != '\0' ? phase : NULL;
}

const char *turbo_agent_state_guardrail_rejection_reason_impl(const json_value_t *state) {
  const char *reason =
      turbo_agent_state_current_version_string_field(state, "guardrail_versions", "reason");
  return reason && reason[0] != '\0' ? reason : NULL;
}

int turbo_agent_state_set_model_error_impl(json_value_t *state, const char *phase,
                                           const char *detail) {
  return turbo_agent_state_append_two_string_object_version_local(
      state, "model_error_versions", "phase", phase, "detail", detail);
}

const char *turbo_agent_state_model_error_phase_impl(const json_value_t *state) {
  const char *phase =
      turbo_agent_state_current_version_string_field(state, "model_error_versions", "phase");
  return phase && phase[0] != '\0' ? phase : NULL;
}

const char *turbo_agent_state_model_error_detail_impl(const json_value_t *state) {
  const char *detail =
      turbo_agent_state_current_version_string_field(state, "model_error_versions", "detail");
  return detail && detail[0] != '\0' ? detail : NULL;
}

int turbo_agent_state_set_failure_impl(json_value_t *state, const char *kind, const char *reason) {
  return turbo_agent_state_append_two_string_object_version_local(
      state, "failure_versions", "kind", kind, "reason", reason);
}

const char *turbo_agent_state_failure_kind_impl(const json_value_t *state) {
  return turbo_agent_state_current_version_string_field(state, "failure_versions", "kind");
}

const char *turbo_agent_state_failure_reason_impl(const json_value_t *state) {
  return turbo_agent_state_current_version_string_field(state, "failure_versions", "reason");
}

size_t turbo_agent_state_pending_tool_calls_impl(const json_value_t *state) {
  const json_value_t *tool_calls = turbo_agent_last_model_tool_calls(state);
  if (!tool_calls || turbo_json_type(tool_calls) != TURBO_JSON_ARRAY) {
    return 0;
  }

  return turbo_json_array_size(tool_calls);
}

CXX_C_API int turbo_agent_append_completed_step(json_value_t *state, size_t step_index,
                                                const char *step_text,
                                                const char *output_text) {
  json_value_t *completed_steps;
  json_value_t *entry;

  if (!state || !step_text || !output_text) {
    return -1;
  }

  completed_steps = turbo_agent_state_get_or_create_array(state, "completed_steps");
  if (!completed_steps) {
    return -1;
  }

  entry = turbo_json_create_object();
  if (!entry) {
    return -1;
  }

  turbo_json_object_set_number(entry, "step_index", (double)step_index);
  turbo_json_object_set_string(entry, "step", step_text);
  turbo_json_object_set_string(entry, "output", output_text);
  turbo_json_array_add(completed_steps, entry);
  return 0;
}

CXX_C_API char *turbo_agent_build_completed_steps_message(const json_value_t *state) {
  const json_value_t *completed_steps;
  char *buffer = NULL;
  size_t length = 0;
  size_t i;

  completed_steps = turbo_agent_completed_steps_local(state);
  if (!completed_steps || turbo_json_type(completed_steps) != TURBO_JSON_ARRAY ||
      turbo_json_array_size(completed_steps) == 0) {
    return NULL;
  }

  if (turbo_agent_util_append_text(&buffer, &length, "Completed step outputs so far:\n") != 0) {
    tstr_free(buffer);
    return NULL;
  }

  for (i = 0; i < turbo_json_array_size(completed_steps); ++i) {
    const json_value_t *entry = turbo_json_array_get(completed_steps, i);
    const char *step_text;
    const char *output_text;
    char *line;
    int needed;

    if (!entry || turbo_json_type(entry) != TURBO_JSON_OBJECT) {
      continue;
    }

    step_text = turbo_json_get_string(entry, "step");
    output_text = turbo_json_get_string(entry, "output");
    needed = snprintf(NULL, 0, "%lu. %s\nResult: %s\n", (unsigned long)(i + 1),
                      step_text ? step_text : "", output_text ? output_text : "");
    if (needed < 0) {
      tstr_free(buffer);
      return NULL;
    }

    line = (char *)malloc((size_t)needed + 1);
    if (!line) {
      tstr_free(buffer);
      return NULL;
    }

    snprintf(line, (size_t)needed + 1, "%lu. %s\nResult: %s\n", (unsigned long)(i + 1),
             step_text ? step_text : "", output_text ? output_text : "");
    if (turbo_agent_util_append_text(&buffer, &length, line) != 0) {
      free(line);
      tstr_free(buffer);
      return NULL;
    }
    free(line);
  }

  return buffer;
}

int turbo_agent_state_set_final_answer_impl(json_value_t *state, const char *text) {
  return turbo_agent_state_append_single_string_object_version(state, "final_answer_versions",
                                                               "text", text);
}

int turbo_agent_state_request_review_impl(json_value_t *state, const char *note) {
  return turbo_agent_state_append_review_version(state, 1, 0, note);
}

int turbo_agent_state_set_review_approved_impl(json_value_t *state, int approved) {
  const char *note = turbo_agent_state_review_note(state);
  return turbo_agent_state_append_review_version(state, approved ? 0 : 1, approved ? 1 : 0, note);
}

int turbo_agent_state_clear_review_impl(json_value_t *state) {
  return turbo_agent_state_append_review_version(state, 0, 0, "");
}

int turbo_agent_state_review_required_impl(const json_value_t *state) {
  return turbo_agent_state_current_version_bool_field(state, "review_versions", "required", 0);
}

int turbo_agent_state_review_approved_impl(const json_value_t *state) {
  return turbo_agent_state_current_version_bool_field(state, "review_versions", "approved", 0);
}

const char *turbo_agent_state_review_note_impl(const json_value_t *state) {
  return turbo_agent_state_current_version_string_field(state, "review_versions", "note");
}

static json_value_t *turbo_agent_state_supervisor_version_local(
    const json_value_t *state) {
  const json_value_t *current =
      turbo_agent_state_get_current_object_version_const(state, "supervisor_versions");
  json_value_t *version = NULL;
  json_value_t *inbox = NULL;
  json_value_t *history = NULL;
  const json_value_t *current_inbox = NULL;
  const json_value_t *current_history = NULL;
  const char *active_agent = NULL;
  const char *target_agent = NULL;
  const char *handoff_reason = NULL;

  version = turbo_json_create_object();
  if (!version) {
    return NULL;
  }

  if (current && turbo_json_type(current) == TURBO_JSON_OBJECT) {
    active_agent = turbo_json_get_string(current, "active_agent");
    target_agent = turbo_json_get_string(current, "target_agent");
    handoff_reason = turbo_json_get_string(current, "handoff_reason");
    current_inbox = turbo_json_object_get(current, "inbox");
    current_history = turbo_json_object_get(current, "handoff_history");
  }

  if (current_inbox && turbo_json_type(current_inbox) == TURBO_JSON_ARRAY) {
    if (turbo_agent_clone_json(current_inbox, &inbox) != TURBO_GRAPH_EXEC_OK) {
      turbo_free_json(&version);
      return NULL;
    }
  } else {
    inbox = turbo_json_create_array();
  }
  if (current_history && turbo_json_type(current_history) == TURBO_JSON_ARRAY) {
    if (turbo_agent_clone_json(current_history, &history) != TURBO_GRAPH_EXEC_OK) {
      turbo_free_json(&inbox);
      turbo_free_json(&version);
      return NULL;
    }
  } else {
    history = turbo_json_create_array();
  }
  if (!inbox || !history) {
    turbo_free_json(&history);
    turbo_free_json(&inbox);
    turbo_free_json(&version);
    return NULL;
  }

  turbo_json_object_set_string(version, "active_agent", active_agent ? active_agent : "");
  turbo_json_object_set_string(version, "target_agent", target_agent ? target_agent : "");
  turbo_json_object_set_string(version, "handoff_reason", handoff_reason ? handoff_reason : "");
  turbo_json_object_add(version, "inbox", inbox);
  turbo_json_object_add(version, "handoff_history", history);
  return version;
}

static void turbo_agent_state_handoff_event_set_optional_string_local(json_value_t *event,
                                                                      const char *key,
                                                                      const char *value) {
  if (!event || turbo_json_type(event) != TURBO_JSON_OBJECT || !key) {
    return;
  }

  if (value) {
    turbo_json_object_set_string(event, key, value);
  } else {
    turbo_json_object_set_null(event, key);
  }
}

static json_value_t *turbo_agent_state_handoff_event_local(const char *phase,
                                                           const char *from_agent,
                                                           const char *target_agent,
                                                           const char *reason,
                                                           const char *active_agent) {
  json_value_t *event = turbo_agent_event_create("handoff");

  if (!event) {
    return NULL;
  }

  turbo_json_object_set_string(event, "phase", phase ? phase : "");
  turbo_agent_state_handoff_event_set_optional_string_local(event, "from_agent", from_agent);
  turbo_agent_state_handoff_event_set_optional_string_local(event, "target_agent", target_agent);
  turbo_agent_state_handoff_event_set_optional_string_local(event, "reason", reason);
  turbo_agent_state_handoff_event_set_optional_string_local(event, "active_agent", active_agent);
  turbo_json_object_set_number(event, "status", 0);
  return event;
}

int turbo_agent_state_set_active_agent_impl(json_value_t *state, const char *active_agent) {
  json_value_t *version;

  if (!state || !active_agent) {
    return -1;
  }
  version = turbo_agent_state_supervisor_version_local(state);
  if (!version) {
    return -1;
  }
  turbo_json_object_set_string(version, "active_agent", active_agent);
  if (turbo_agent_state_append_object_version(state, "supervisor_versions", version) != 0) {
    turbo_free_json(&version);
    return -1;
  }
  return 0;
}

int turbo_agent_state_request_handoff_impl(json_value_t *state, const char *target_agent,
                                           const char *reason) {
  json_value_t *version;
  json_value_t *history;
  json_value_t *entry;
  json_value_t *event;
  json_value_t *events;
  const char *active_agent;

  if (!state || !target_agent) {
    return -1;
  }
  version = turbo_agent_state_supervisor_version_local(state);
  if (!version) {
    return -1;
  }
  history = turbo_json_object_get(version, "handoff_history");
  entry = turbo_json_create_object();
  active_agent = turbo_agent_state_active_agent(state);
  event = turbo_agent_state_handoff_event_local("requested", active_agent, target_agent, reason,
                                                active_agent);
  events = turbo_agent_state_get_array(state, "events");
  if (!history || turbo_json_type(history) != TURBO_JSON_ARRAY || !entry || !event || !events ||
      turbo_json_type(events) != TURBO_JSON_ARRAY) {
    turbo_free_json(&event);
    turbo_free_json(&entry);
    turbo_free_json(&version);
    return -1;
  }
  turbo_json_object_set_string(version, "target_agent", target_agent);
  turbo_json_object_set_string(version, "handoff_reason", reason ? reason : "");
  turbo_json_object_set_string(entry, "from_agent", active_agent ? active_agent : "");
  turbo_json_object_set_string(entry, "target_agent", target_agent);
  turbo_json_object_set_string(entry, "reason", reason ? reason : "");
  turbo_json_array_add(history, entry);
  if (turbo_agent_state_append_object_version(state, "supervisor_versions", version) != 0) {
    turbo_free_json(&event);
    turbo_free_json(&version);
    return -1;
  }
  turbo_json_array_add(events, event);
  return 0;
}

int turbo_agent_state_commit_handoff_impl(json_value_t *state) {
  json_value_t *version;
  json_value_t *event;
  json_value_t *events;
  const char *from_agent;
  const char *reason;
  const char *target_agent;

  if (!state) {
    return -1;
  }
  from_agent = turbo_agent_state_active_agent(state);
  target_agent = turbo_agent_state_handoff_target_agent(state);
  reason = turbo_agent_state_handoff_reason(state);
  if (!target_agent || target_agent[0] == '\0') {
    return -1;
  }
  version = turbo_agent_state_supervisor_version_local(state);
  event = turbo_agent_state_handoff_event_local("committed", from_agent, target_agent, reason,
                                                target_agent);
  events = turbo_agent_state_get_array(state, "events");
  if (!version || !event || !events || turbo_json_type(events) != TURBO_JSON_ARRAY) {
    turbo_free_json(&event);
    turbo_free_json(&version);
    return -1;
  }
  turbo_json_object_set_string(version, "active_agent", target_agent);
  turbo_json_object_set_string(version, "target_agent", "");
  turbo_json_object_set_string(version, "handoff_reason", "");
  if (turbo_agent_state_append_object_version(state, "supervisor_versions", version) != 0) {
    turbo_free_json(&event);
    turbo_free_json(&version);
    return -1;
  }
  turbo_json_array_add(events, event);
  return 0;
}

int turbo_agent_state_append_supervisor_inbox_message_impl(json_value_t *state,
                                                           const char *source_agent,
                                                           const char *text) {
  json_value_t *version;
  json_value_t *inbox;
  json_value_t *entry;

  if (!state || !text) {
    return -1;
  }
  version = turbo_agent_state_supervisor_version_local(state);
  if (!version) {
    return -1;
  }
  inbox = turbo_json_object_get(version, "inbox");
  entry = turbo_json_create_object();
  if (!inbox || turbo_json_type(inbox) != TURBO_JSON_ARRAY || !entry) {
    turbo_free_json(&entry);
    turbo_free_json(&version);
    return -1;
  }
  turbo_json_object_set_string(entry, "source_agent", source_agent ? source_agent : "");
  turbo_json_object_set_string(entry, "text", text);
  turbo_json_array_add(inbox, entry);
  if (turbo_agent_state_append_object_version(state, "supervisor_versions", version) != 0) {
    turbo_free_json(&version);
    return -1;
  }
  return 0;
}

const char *turbo_agent_state_active_agent_impl(const json_value_t *state) {
  const char *value = turbo_agent_state_current_version_string_field(
      state, "supervisor_versions", "active_agent");
  return value && value[0] != '\0' ? value : NULL;
}

const char *turbo_agent_state_handoff_target_agent_impl(const json_value_t *state) {
  const char *value = turbo_agent_state_current_version_string_field(
      state, "supervisor_versions", "target_agent");
  return value && value[0] != '\0' ? value : NULL;
}

const char *turbo_agent_state_handoff_reason_impl(const json_value_t *state) {
  const char *value = turbo_agent_state_current_version_string_field(
      state, "supervisor_versions", "handoff_reason");
  return value && value[0] != '\0' ? value : NULL;
}

const json_value_t *turbo_agent_state_supervisor_inbox_impl(const json_value_t *state) {
  const json_value_t *version =
      turbo_agent_state_get_current_object_version_const(state, "supervisor_versions");
  const json_value_t *inbox =
      version && turbo_json_type(version) == TURBO_JSON_OBJECT
          ? turbo_json_object_get(version, "inbox")
          : NULL;
  return inbox && turbo_json_type(inbox) == TURBO_JSON_ARRAY ? inbox : NULL;
}

size_t turbo_agent_state_supervisor_inbox_count_impl(const json_value_t *state) {
  const json_value_t *inbox = turbo_agent_state_supervisor_inbox(state);
  return inbox ? turbo_json_array_size(inbox) : 0;
}

const json_value_t *turbo_agent_state_supervisor_inbox_at_impl(const json_value_t *state,
                                                               size_t index) {
  const json_value_t *inbox = turbo_agent_state_supervisor_inbox(state);
  if (!inbox || index >= turbo_json_array_size(inbox)) {
    return NULL;
  }
  return turbo_json_array_get(inbox, index);
}

const json_value_t *turbo_agent_state_supervisor_handoff_history_impl(
    const json_value_t *state) {
  const json_value_t *version =
      turbo_agent_state_get_current_object_version_const(state, "supervisor_versions");
  const json_value_t *history =
      version && turbo_json_type(version) == TURBO_JSON_OBJECT
          ? turbo_json_object_get(version, "handoff_history")
          : NULL;
  return history && turbo_json_type(history) == TURBO_JSON_ARRAY ? history : NULL;
}

const char *turbo_agent_state_last_output_text_impl(const json_value_t *state) {
  const json_value_t *event = turbo_agent_state_last_event_of_kind(state, "model");
  return event && turbo_json_type(event) == TURBO_JSON_OBJECT
             ? turbo_json_get_string(event, "output_text")
             : NULL;
}

const char *turbo_agent_state_latest_executor_output_text_impl(const json_value_t *state) {
  const json_value_t *events = turbo_agent_state_latest_executor_events_local(state);
  const json_value_t *event = turbo_agent_events_last_of_kind(events, "model");

  return event && turbo_json_type(event) == TURBO_JSON_OBJECT
             ? turbo_json_get_string(event, "output_text")
             : NULL;
}

const json_value_t *turbo_agent_state_planner_event_versions_impl(const json_value_t *state) {
  return state && turbo_json_type(state) == TURBO_JSON_OBJECT
             ? turbo_json_object_get(state, "planner_event_versions")
             : NULL;
}

size_t turbo_agent_state_planner_event_version_count_impl(const json_value_t *state) {
  const json_value_t *versions = turbo_agent_state_planner_event_versions(state);
  return versions && turbo_json_type(versions) == TURBO_JSON_ARRAY ? turbo_json_array_size(versions)
                                                                   : 0;
}

const json_value_t *turbo_agent_state_planner_event_version_at_impl(const json_value_t *state,
                                                                    size_t index) {
  const json_value_t *versions = turbo_agent_state_planner_event_versions(state);
  if (!versions || turbo_json_type(versions) != TURBO_JSON_ARRAY ||
      index >= turbo_json_array_size(versions)) {
    return NULL;
  }
  return turbo_json_array_get(versions, index);
}

const json_value_t *turbo_agent_state_executor_event_versions_impl(const json_value_t *state) {
  return state && turbo_json_type(state) == TURBO_JSON_OBJECT
             ? turbo_json_object_get(state, "executor_event_versions")
             : NULL;
}

size_t turbo_agent_state_executor_event_version_count_impl(const json_value_t *state) {
  const json_value_t *versions = turbo_agent_state_executor_event_versions(state);
  return versions && turbo_json_type(versions) == TURBO_JSON_ARRAY ? turbo_json_array_size(versions)
                                                                   : 0;
}

const json_value_t *turbo_agent_state_executor_event_version_at_impl(const json_value_t *state,
                                                                     size_t index) {
  const json_value_t *versions = turbo_agent_state_executor_event_versions(state);
  if (!versions || turbo_json_type(versions) != TURBO_JSON_ARRAY ||
      index >= turbo_json_array_size(versions)) {
    return NULL;
  }
  return turbo_json_array_get(versions, index);
}

const json_value_t *turbo_agent_state_completed_steps_impl(const json_value_t *state) {
  return turbo_agent_completed_steps_local(state);
}

size_t turbo_agent_state_completed_step_count_impl(const json_value_t *state) {
  const json_value_t *steps = turbo_agent_state_completed_steps(state);
  return steps && turbo_json_type(steps) == TURBO_JSON_ARRAY ? turbo_json_array_size(steps) : 0;
}

const json_value_t *turbo_agent_state_completed_step_at_impl(const json_value_t *state,
                                                             size_t index) {
  const json_value_t *steps = turbo_agent_state_completed_steps(state);
  if (!steps || turbo_json_type(steps) != TURBO_JSON_ARRAY ||
      index >= turbo_json_array_size(steps)) {
    return NULL;
  }
  return turbo_json_array_get(steps, index);
}
