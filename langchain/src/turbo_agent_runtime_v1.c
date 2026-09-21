#include "turbo_agent_runtime.h"

#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_runtime_v1_internal.h"
#include "turbo_agent_state.h"
#include "turbo_agent_util_internal.h"
#include "turbo_event_log.h"
#include "turbo_graph_run_log.h"
#include <json_parser.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  #include <direct.h>
  #include <windows.h>
#else
  #include <dirent.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
#endif

typedef struct turbo_agent_runtime_stream_capture_s {
  turbo_event_log_t *events;
  turbo_graph_checkpoint_t *checkpoint;
  turbo_graph_exec_status_t checkpoint_status;
  turbo_graph_checkpoint_cb user_checkpoint_cb;
  void *user_checkpoint_user_data;
  turbo_event_sink_json_value_fn user_event_sink;
  void *user_event_sink_user_data;
} turbo_agent_runtime_stream_capture_t;

const char *const turbo_agent_runtime_threads_collection = "threads";
const char *const turbo_agent_runtime_runs_collection = "runs";
const char *const turbo_agent_runtime_checkpoints_collection = "checkpoints";

static atomic_ullong turbo_agent_runtime_id_counter = 1;

/* Forward declarations are in turbo_agent_runtime_v1_internal.h */
static int turbo_agent_runtime_record_checkpoint_with_event_log(
    turbo_agent_runtime_t *runtime, const char *thread_id, const char *run_id,
    const char *parent_checkpoint_id, const char *parent_agent_run_id,
    const char *parent_tool_call_id, const char *parent_tool_name, const char *parent_graph_run_id,
    const char *call_frame_id, size_t seq, const char *timestamp,
    const turbo_graph_checkpoint_t *checkpoint, const turbo_event_log_t *event_log,
    const json_value_t *state, const char **out_checkpoint_id);

int turbo_agent_runtime_json_value_object_set_string(json_value_t *object, const char *key,
                                                     const char *value) {
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

int turbo_agent_runtime_json_value_object_set_int64(json_value_t *object, const char *key,
                                                    int64_t value) {
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

int turbo_agent_runtime_json_value_object_set_clone(json_value_t *object, const char *key,
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

int turbo_agent_runtime_json_value_object_set_optional_string(json_value_t *object, const char *key,
                                                              const json_value_t *value) {
  const char *text = turbo_runtime_json_value_as_string(value);

  if (!text) {
    return 0;
  }
  return turbo_agent_runtime_json_value_object_set_string(object, key, text);
}

static const char *turbo_agent_runtime_observer_type_for_trace_name(const char *name) {
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

static int turbo_agent_runtime_observer_event_from_json_value(const json_value_t *raw_event,
                                                              json_value_t **out_observer_event) {
  const json_value_t *kind_value;
  const json_value_t *type_value;
  const char *kind;
  const char *type;
  const char *observer_type = NULL;
  json_value_t *observer_event = NULL;

  if (!raw_event || !out_observer_event) {
    return -1;
  }
  *out_observer_event = NULL;

  kind_value = turbo_json_object_get(raw_event, "kind");
  kind = turbo_runtime_json_value_as_string(kind_value);
  if (kind && strcmp(kind, "model") == 0) {
    observer_type = "model_delta";
  } else if (kind && strcmp(kind, "tool_result") == 0) {
    observer_type = "tool_result";
  } else if (kind && strcmp(kind, "trace") == 0) {
    observer_type = turbo_agent_runtime_observer_type_for_trace_name(
        turbo_runtime_json_value_as_string(turbo_json_object_get(raw_event, "name")));
  } else {
    type_value = turbo_json_object_get(raw_event, "type");
    type = turbo_runtime_json_value_as_string(type_value);
    if (type && strcmp(type, "interrupted") == 0) {
      observer_type = "interrupted";
    } else if (type && strcmp(type, "completed") == 0) {
      observer_type = "completed";
    } else if (type && type[0] != '\0') {
      observer_type = "state_updated";
    }
  }

  if (!observer_type) {
    observer_type = "state_updated";
  }

  observer_event = turbo_json_create_object();
  if (!observer_event ||
      turbo_agent_runtime_json_value_object_set_string(observer_event, "kind", "observer") != 0 ||
      turbo_agent_runtime_json_value_object_set_string(observer_event, "type", observer_type) !=
          0 ||
      turbo_agent_runtime_json_value_object_set_clone(observer_event, "event", raw_event) != 0) {
    turbo_runtime_json_destroy(observer_event);
    return -1;
  }

  if (kind && strcmp(kind, "trace") == 0) {
    const json_value_t *status_value = turbo_json_object_get(raw_event, "status");

    if (turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "name", turbo_json_object_get(raw_event, "name")) != 0 ||
        turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "detail", turbo_json_object_get(raw_event, "detail")) != 0 ||
        turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "payload", turbo_json_object_get(raw_event, "payload")) != 0) {
      turbo_runtime_json_destroy(observer_event);
      return -1;
    }
    if (status_value &&
        turbo_agent_runtime_json_value_object_set_int64(
            observer_event, "status", turbo_runtime_json_value_as_int64(status_value, 0)) != 0) {
      turbo_runtime_json_destroy(observer_event);
      return -1;
    }
  } else if (kind && strcmp(kind, "model") == 0) {
    if (turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "response_id", turbo_json_object_get(raw_event, "response_id")) != 0 ||
        turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "output_text", turbo_json_object_get(raw_event, "output_text")) != 0) {
      turbo_runtime_json_destroy(observer_event);
      return -1;
    }
    if (turbo_json_object_get(raw_event, "tool_calls") &&
        turbo_agent_runtime_json_value_object_set_clone(
            observer_event, "tool_calls", turbo_json_object_get(raw_event, "tool_calls")) != 0) {
      turbo_runtime_json_destroy(observer_event);
      return -1;
    }
  } else if (kind && strcmp(kind, "tool_result") == 0) {
    const json_value_t *status_value = turbo_json_object_get(raw_event, "status");

    if (turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "name", turbo_json_object_get(raw_event, "name")) != 0 ||
        turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "arguments_json", turbo_json_object_get(raw_event, "arguments_json")) !=
            0 ||
        turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "output", turbo_json_object_get(raw_event, "output")) != 0) {
      turbo_runtime_json_destroy(observer_event);
      return -1;
    }
    if (status_value &&
        turbo_agent_runtime_json_value_object_set_int64(
            observer_event, "status", turbo_runtime_json_value_as_int64(status_value, 0)) != 0) {
      turbo_runtime_json_destroy(observer_event);
      return -1;
    }
  } else {
    if (turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "last_node", turbo_json_object_get(raw_event, "last_node")) != 0 ||
        turbo_agent_runtime_json_value_object_set_optional_string(
            observer_event, "next_node", turbo_json_object_get(raw_event, "next_node")) != 0) {
      turbo_runtime_json_destroy(observer_event);
      return -1;
    }
    if (turbo_json_object_get(raw_event, "steps") &&
        turbo_agent_runtime_json_value_object_set_int64(
            observer_event, "steps",
            turbo_runtime_json_value_as_int64(turbo_json_object_get(raw_event, "steps"), 0)) != 0) {
      turbo_runtime_json_destroy(observer_event);
      return -1;
    }
  }

  *out_observer_event = observer_event;
  return 1;
}

static int
turbo_agent_runtime_events_json_value_has_terminal_type(const json_value_t *events_json_value,
                                                        const char *terminal_type) {
  size_t i;
  size_t count;

  if (!events_json_value || !terminal_type ||
      turbo_json_type(events_json_value) != TURBO_JSON_ARRAY) {
    return 0;
  }
  count = turbo_runtime_json_value_size(events_json_value);
  for (i = 0; i < count; ++i) {
    const json_value_t *raw_event = turbo_json_array_get(events_json_value, i);
    const char *type;

    if (!raw_event) {
      continue;
    }
    type = turbo_runtime_json_value_as_string(turbo_json_object_get(raw_event, "type"));
    if (type && strcmp(type, terminal_type) == 0) {
      return 1;
    }
  }
  return 0;
}

static int
turbo_agent_runtime_observer_terminal_event_from_record_json(const json_value_t *record_json,
                                                             json_value_t **out_observer_event) {
  const char *status;
  json_value_t *record_json_value = NULL;
  json_value_t *observer_event = NULL;

  if (!record_json || !out_observer_event) {
    return -1;
  }
  *out_observer_event = NULL;
  status = turbo_json_get_string(record_json, "status");
  if (!status || (strcmp(status, "interrupted") != 0 && strcmp(status, "completed") != 0)) {
    return 0;
  }
  record_json_value = turbo_json_clone(record_json);
  if (!record_json_value) {
    return -1;
  }
  observer_event = turbo_json_create_object();
  if (!observer_event ||
      turbo_agent_runtime_json_value_object_set_string(observer_event, "kind", "observer") != 0 ||
      turbo_agent_runtime_json_value_object_set_string(observer_event, "type", status) != 0 ||
      turbo_agent_runtime_json_value_object_set_clone(observer_event, "event", record_json_value) !=
          0) {
    turbo_runtime_json_destroy(record_json_value);
    turbo_runtime_json_destroy(observer_event);
    return -1;
  }
  turbo_runtime_json_destroy(record_json_value);
  *out_observer_event = observer_event;
  return 1;
}

static int turbo_agent_runtime_observe_terminal_record_json(
    const json_value_t *events_json_value, const json_value_t *record_json,
    const turbo_agent_observer_json_value_sink_t *sink) {
  json_value_t *observer_event = NULL;
  int rc;

  if (!record_json || !sink || !sink->callback) {
    return -1;
  }
  rc = turbo_agent_runtime_observer_terminal_event_from_record_json(record_json, &observer_event);
  if (rc <= 0) {
    turbo_runtime_json_destroy(observer_event);
    return rc;
  }
  if (!events_json_value ||
      !turbo_agent_runtime_events_json_value_has_terminal_type(
          events_json_value,
          turbo_runtime_json_value_as_string(turbo_json_object_get(observer_event, "type")))) {
    sink->callback(observer_event, sink->user_data);
  }
  turbo_runtime_json_destroy(observer_event);
  return 1;
}

char *turbo_agent_runtime_strdup(const char *text) {
  size_t len;
  char *copy;

  if (!text) {
    return NULL;
  }
  len = strlen(text);
  copy = (char *)malloc(len + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, text, len + 1);
  return copy;
}

const char *turbo_agent_runtime_status_text(turbo_graph_exec_status_t status) {
  switch (status) {
  case TURBO_GRAPH_EXEC_OK:
  case TURBO_GRAPH_EXEC_STOP:
    return "completed";
  case TURBO_GRAPH_EXEC_INTERRUPTED:
    return "interrupted";
  case TURBO_GRAPH_EXEC_CANCELLED:
    return "cancelled";
  case TURBO_GRAPH_EXEC_DEADLINE:
    return "timed_out";
  default:
    return "failed";
  }
}

int turbo_agent_runtime_result_to_json(const turbo_graph_run_result_t *result,
                                       json_value_t **out_result) {
  json_value_t *json_result;

  if (!result || !out_result) {
    return -1;
  }

  json_result = turbo_json_create_object();
  if (!json_result) {
    return -1;
  }

  turbo_json_object_set_number(json_result, "status", (double)result->status);
  if (result->last_node) {
    turbo_json_object_set_string(json_result, "last_node", result->last_node);
  } else {
    turbo_json_object_set_null(json_result, "last_node");
  }
  if (result->next_node) {
    turbo_json_object_set_string(json_result, "next_node", result->next_node);
  } else {
    turbo_json_object_set_null(json_result, "next_node");
  }
  turbo_json_object_set_number(json_result, "steps", (double)result->steps);

  *out_result = json_result;
  return 0;
}

#define TURBO_AGENT_RUNTIME_ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

typedef struct {
  const char *name;
  const char *type;
  const char *description;
  int required;
} turbo_agent_runtime_command_property_spec_t;

typedef struct {
  const char *name;
  const char *label;
  const char *description;
  const char *category;
  const char *input_mode;
  const char *placeholder;
  const char *success_state_hint;
  const char *example_payload_json;
  const char *resume_mode;
  const char *primary_key;
  const char *const *accepted_keys;
  size_t accepted_key_count;
  int requires_input;
  int supports_json_value;
  const turbo_agent_runtime_command_property_spec_t *properties;
  size_t property_count;
} turbo_agent_runtime_command_spec_t;

int turbo_agent_runtime_parse_json_string(const char *json_text, json_value_t **out_json);

static const turbo_agent_runtime_command_property_spec_t
    turbo_agent_runtime_review_approve_properties[] = {
        {"approved", "boolean", "Optional explicit approval flag. Defaults to true.", 0}};

static const turbo_agent_runtime_command_property_spec_t
    turbo_agent_runtime_review_reject_properties[] = {
        {"reason", "string", "Optional reviewer note explaining why approval was denied.", 0}};

static const turbo_agent_runtime_command_property_spec_t
    turbo_agent_runtime_request_replan_properties[] = {
        {"reason", "string", "Optional explanation passed into replan state.", 0}};

static const turbo_agent_runtime_command_property_spec_t
    turbo_agent_runtime_append_feedback_properties[] = {
        {"text", "string", "Feedback text to append before continuing.", 1}};

static const turbo_agent_runtime_command_property_spec_t
    turbo_agent_runtime_append_user_message_properties[] = {
        {"text", "string", "User message text to append.", 1}};

static const turbo_agent_runtime_command_property_spec_t
    turbo_agent_runtime_override_final_output_properties[] = {
        {"text", "string", "Replacement final output text.", 0},
        {"output_json", "object", "Structured final output that will be serialized to text.", 0}};

static const char *const turbo_agent_runtime_review_approve_keys[] = {"approved"};
static const char *const turbo_agent_runtime_review_reject_keys[] = {"reason"};
static const char *const turbo_agent_runtime_request_replan_keys[] = {"reason"};
static const char *const turbo_agent_runtime_append_feedback_keys[] = {"text"};
static const char *const turbo_agent_runtime_append_user_message_keys[] = {"text"};
static const char *const turbo_agent_runtime_override_final_output_keys[] = {"text", "output_json"};

static const turbo_agent_runtime_command_spec_t turbo_agent_runtime_command_specs[] = {
    {"approve_review", "Approve Review", "Approve pending review and continue the interrupted run.",
     "review", "none", NULL, "Marks review as approved so execution can continue.",
     "{\"kind\":\"approve_review\",\"approved\":true}", "resume_or_fork", "approved",
     turbo_agent_runtime_review_approve_keys,
     TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_review_approve_keys), 0, 0,
     turbo_agent_runtime_review_approve_properties,
     TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_review_approve_properties)},
    {"reject_review", "Reject Review", "Reject pending review and keep the review gate closed.",
     "review", "optional_text", "Explain what must change before approval.",
     "Keeps review required and records the rejection note for the next pass.",
     "{\"kind\":\"reject_review\",\"reason\":\"needs another pass\"}", "resume_or_fork", "reason",
     turbo_agent_runtime_review_reject_keys,
     TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_review_reject_keys), 0, 0,
     turbo_agent_runtime_review_reject_properties,
     TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_review_reject_properties)},
    {"request_replan", "Request Replan", "Request a replanning pass before resuming execution.",
     "planning", "optional_text", "Describe why the current plan should be rebuilt.",
     "Marks the state for replanning before the next execution pass.",
     "{\"kind\":\"request_replan\",\"reason\":\"the constraints changed\"}", "resume_or_fork",
     "reason", turbo_agent_runtime_request_replan_keys,
     TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_request_replan_keys), 0, 0,
     turbo_agent_runtime_request_replan_properties,
     TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_request_replan_properties)},
    {"append_feedback", "Append Feedback",
     "Append one host feedback message to the user-visible input.", "input", "text",
     "Add one short feedback message for the next run.",
     "Appends one user-visible feedback message to the canonical input.",
     "{\"kind\":\"append_feedback\",\"text\":\"please add tests\"}", "resume_or_fork", "text",
     turbo_agent_runtime_append_feedback_keys,
     TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_append_feedback_keys), 1, 0,
     turbo_agent_runtime_append_feedback_properties,
     TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_append_feedback_properties)},
    {"append_user_message", "Append User Message",
     "Append one new user message before resuming or forking.", "input", "text",
     "Add the next user message to include before continuing.",
     "Appends one new user-role message to the canonical input.",
     "{\"kind\":\"append_user_message\",\"text\":\"continue with the CLI path\"}", "resume_or_fork",
     "text", turbo_agent_runtime_append_user_message_keys,
     TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_append_user_message_keys), 1, 0,
     turbo_agent_runtime_append_user_message_properties,
     TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_append_user_message_properties)},
    {"override_final_output", "Override Final Output",
     "Replace the current final output with host-provided content.", "output", "text_or_json",
     "Replace the final output text or provide structured output_json.",
     "Overrides the current final answer with host-provided content.",
     "{\"kind\":\"override_final_output\",\"text\":\"Ship it.\"}", "resume_or_fork", "text",
     turbo_agent_runtime_override_final_output_keys,
     TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_override_final_output_keys), 1, 1,
     turbo_agent_runtime_override_final_output_properties,
     TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_override_final_output_properties)}};

static const char *const turbo_agent_runtime_review_commands[] = {
    "approve_review",      "reject_review",  "append_feedback",
    "append_user_message", "request_replan", "override_final_output"};

static const char *const turbo_agent_runtime_replan_commands[] = {
    "append_feedback", "append_user_message", "override_final_output"};

static const char *const turbo_agent_runtime_host_intervention_commands[] = {
    "append_feedback", "append_user_message", "request_replan", "override_final_output"};

static const char *const turbo_agent_runtime_default_interrupt_commands[] = {
    "append_feedback", "append_user_message", "override_final_output"};

static json_value_t *turbo_agent_runtime_create_args_schema(void) {
  json_value_t *schema = turbo_json_create_object();
  json_value_t *properties = NULL;
  json_value_t *required = NULL;

  if (!schema) {
    return NULL;
  }
  properties = turbo_json_create_object();
  required = turbo_json_create_array();
  if (!properties || !required) {
    turbo_free_json(&properties);
    turbo_free_json(&required);
    turbo_free_json(&schema);
    return NULL;
  }
  turbo_json_object_set_string(schema, "type", "object");
  turbo_json_object_add(schema, "properties", properties);
  turbo_json_object_add(schema, "required", required);
  return schema;
}

static int turbo_agent_runtime_args_schema_add_property(json_value_t *schema, const char *name,
                                                        const char *type, const char *description,
                                                        int required) {
  json_value_t *properties;
  json_value_t *property;
  json_value_t *required_array;

  if (!schema || !name || !type) {
    return -1;
  }
  properties = turbo_json_object_get(schema, "properties");
  required_array = turbo_json_object_get(schema, "required");
  if (!properties || !required_array) {
    return -1;
  }
  property = turbo_json_create_object();
  if (!property) {
    return -1;
  }
  turbo_json_object_set_string(property, "type", type);
  if (description && description[0] != '\0') {
    turbo_json_object_set_string(property, "description", description);
  }
  turbo_json_object_add(properties, name, property);
  if (required) {
    turbo_json_array_add(required_array, turbo_json_create_string(name));
  }
  return 0;
}

static const turbo_agent_runtime_command_spec_t *
turbo_agent_runtime_find_command_spec(const char *name) {
  size_t i;

  if (!name) {
    return NULL;
  }
  for (i = 0; i < TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_command_specs); ++i) {
    if (strcmp(turbo_agent_runtime_command_specs[i].name, name) == 0) {
      return &turbo_agent_runtime_command_specs[i];
    }
  }
  return NULL;
}

static json_value_t *turbo_agent_runtime_string_array(const char *const *values, size_t count);

static json_value_t *
turbo_agent_runtime_command_descriptor_from_spec(const turbo_agent_runtime_command_spec_t *spec) {
  json_value_t *descriptor;
  json_value_t *args_schema;
  json_value_t *accepted_keys;
  json_value_t *example_payload;
  size_t i;

  if (!spec) {
    return NULL;
  }
  descriptor = turbo_json_create_object();
  args_schema = turbo_agent_runtime_create_args_schema();
  accepted_keys = NULL;
  example_payload = NULL;
  if (!descriptor || !args_schema) {
    turbo_free_json(&descriptor);
    turbo_free_json(&args_schema);
    return NULL;
  }
  accepted_keys = turbo_agent_runtime_string_array(spec->accepted_keys, spec->accepted_key_count);
  if (!accepted_keys) {
    turbo_free_json(&descriptor);
    turbo_free_json(&args_schema);
    return NULL;
  }
  if (spec->example_payload_json && spec->example_payload_json[0] != '\0' &&
      turbo_agent_runtime_parse_json_string(spec->example_payload_json, &example_payload) != 0) {
    turbo_free_json(&accepted_keys);
    turbo_free_json(&descriptor);
    turbo_free_json(&args_schema);
    return NULL;
  }

  for (i = 0; i < spec->property_count; ++i) {
    const turbo_agent_runtime_command_property_spec_t *property = &spec->properties[i];
    if (turbo_agent_runtime_args_schema_add_property(args_schema, property->name, property->type,
                                                     property->description,
                                                     property->required) != 0) {
      goto fail;
    }
  }

  turbo_json_object_set_string(descriptor, "name", spec->name);
  turbo_json_object_set_string(descriptor, "label", spec->label);
  turbo_json_object_set_string(descriptor, "suggested_title", spec->label);
  turbo_json_object_set_string(descriptor, "description", spec->description);
  turbo_json_object_set_string(descriptor, "category", spec->category);
  turbo_json_object_set_string(descriptor, "input_mode", spec->input_mode);
  if (spec->placeholder && spec->placeholder[0] != '\0') {
    turbo_json_object_set_string(descriptor, "placeholder", spec->placeholder);
  } else {
    turbo_json_object_set_null(descriptor, "placeholder");
  }
  if (spec->success_state_hint && spec->success_state_hint[0] != '\0') {
    turbo_json_object_set_string(descriptor, "success_state_hint", spec->success_state_hint);
  } else {
    turbo_json_object_set_null(descriptor, "success_state_hint");
  }
  if (example_payload) {
    turbo_json_object_add(descriptor, "example_payload", example_payload);
    example_payload = NULL;
  } else {
    turbo_json_object_set_null(descriptor, "example_payload");
  }
  turbo_json_object_set_string(descriptor, "resume_mode", spec->resume_mode);
  turbo_json_object_set_string(descriptor, "primary_key", spec->primary_key);
  turbo_json_object_add(descriptor, "accepted_keys", accepted_keys);
  accepted_keys = NULL;
  turbo_json_object_set_bool(descriptor, "requires_input", spec->requires_input ? true : false);
  turbo_json_object_set_bool(descriptor, "supports_json_value",
                             spec->supports_json_value ? true : false);
  turbo_json_object_add(descriptor, "args_schema", args_schema);
  return descriptor;

fail:
  turbo_free_json(&example_payload);
  turbo_free_json(&accepted_keys);
  turbo_free_json(&args_schema);
  turbo_free_json(&descriptor);
  return NULL;
}

static json_value_t *turbo_agent_runtime_command_descriptor(const char *name) {
  return turbo_agent_runtime_command_descriptor_from_spec(
      turbo_agent_runtime_find_command_spec(name));
}

static json_value_t *turbo_agent_runtime_string_array(const char *const *values, size_t count) {
  json_value_t *array;
  size_t i;

  if (!values && count != 0) {
    return NULL;
  }
  array = turbo_json_create_array();
  if (!array) {
    return NULL;
  }
  for (i = 0; i < count; ++i) {
    turbo_json_array_add(array, turbo_json_create_string(values[i]));
  }
  return array;
}

static int turbo_agent_runtime_summary_add_command(json_value_t *available_commands,
                                                   json_value_t *available_command_descriptors,
                                                   const char *name) {
  json_value_t *descriptor;

  if (!available_commands || !available_command_descriptors || !name) {
    return -1;
  }
  descriptor = turbo_agent_runtime_command_descriptor(name);
  if (!descriptor) {
    return -1;
  }
  turbo_json_array_add(available_commands, turbo_json_create_string(name));
  turbo_json_array_add(available_command_descriptors, descriptor);
  return 0;
}

static int turbo_agent_runtime_summary_add_commands(json_value_t *available_commands,
                                                    json_value_t *available_command_descriptors,
                                                    const char *const *names, size_t name_count) {
  size_t i;

  if (!names && name_count != 0) {
    return -1;
  }
  for (i = 0; i < name_count; ++i) {
    if (turbo_agent_runtime_summary_add_command(available_commands, available_command_descriptors,
                                                names[i]) != 0) {
      return -1;
    }
  }
  return 0;
}

void turbo_agent_runtime_interrupt_metadata(const char *status, const char *next_node,
                                            const json_value_t *state_json,
                                            const char **out_interrupt_reason,
                                            const char **out_pending_action,
                                            const char *const **out_command_names,
                                            size_t *out_command_count,
                                            char **out_owned_executor_failure_reason) {
  const char *interrupt_reason = NULL;
  const char *pending_action = NULL;
  const char *failure_reason = NULL;
  const char *const *command_names = NULL;
  size_t command_count = 0;

  if (out_interrupt_reason) {
    *out_interrupt_reason = NULL;
  }
  if (out_pending_action) {
    *out_pending_action = NULL;
  }
  if (out_command_names) {
    *out_command_names = NULL;
  }
  if (out_command_count) {
    *out_command_count = 0;
  }
  if (out_owned_executor_failure_reason) {
    *out_owned_executor_failure_reason = NULL;
  }

  if (!status || strcmp(status, "interrupted") != 0) {
    return;
  }

  if (next_node && strcmp(next_node, "review") == 0) {
    interrupt_reason = "review_required";
    pending_action = "review";
    command_names = turbo_agent_runtime_review_commands;
    command_count = TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_review_commands);
  } else if (state_json && turbo_agent_state_review_required(state_json) &&
             !turbo_agent_state_review_approved(state_json)) {
    interrupt_reason = "review_required";
    pending_action = "review";
    command_names = turbo_agent_runtime_review_commands;
    command_count = TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_review_commands);
  } else if (state_json && turbo_agent_state_replan_requested(state_json)) {
    interrupt_reason = "replan_requested";
    pending_action = "replan";
    command_names = turbo_agent_runtime_replan_commands;
    command_count = TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_replan_commands);
  } else if (state_json && turbo_agent_state_guardrail_rejection_reason(state_json)) {
    interrupt_reason = "guardrail_rejection";
    pending_action = "host_intervention";
    command_names = turbo_agent_runtime_host_intervention_commands;
    command_count = TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_host_intervention_commands);
  } else if (state_json && turbo_agent_state_model_error_detail(state_json)) {
    interrupt_reason = "model_error";
    pending_action = "host_intervention";
    command_names = turbo_agent_runtime_host_intervention_commands;
    command_count = TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_host_intervention_commands);
  } else if (state_json) {
    failure_reason = turbo_agent_state_failure_reason(state_json);
    if ((!failure_reason || failure_reason[0] == '\0') && out_owned_executor_failure_reason) {
      *out_owned_executor_failure_reason = turbo_agent_executor_failure_reason(state_json);
      failure_reason = *out_owned_executor_failure_reason;
    }
    if (failure_reason && failure_reason[0] != '\0') {
      interrupt_reason = "failure";
      pending_action = "host_intervention";
      command_names = turbo_agent_runtime_host_intervention_commands;
      command_count =
          TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_host_intervention_commands);
    }
  }
  if (!interrupt_reason) {
    interrupt_reason = "interrupt_before_node";
    pending_action = (next_node && next_node[0] != '\0') ? next_node : "resume";
    command_names = turbo_agent_runtime_default_interrupt_commands;
    command_count = TURBO_AGENT_RUNTIME_ARRAY_COUNT(turbo_agent_runtime_default_interrupt_commands);
  }

  if (out_interrupt_reason) {
    *out_interrupt_reason = interrupt_reason;
  }
  if (out_pending_action) {
    *out_pending_action = pending_action;
  }
  if (out_command_names) {
    *out_command_names = command_names;
  }
  if (out_command_count) {
    *out_command_count = command_count;
  }
}

static int
turbo_agent_runtime_make_summary(const char *thread_id, const char *run_id, const char *status,
                                 const char *checkpoint_id, const char *parent_agent_run_id,
                                 const char *parent_tool_call_id, const char *parent_tool_name,
                                 const char *parent_graph_run_id, const char *call_frame_id,
                                 const turbo_graph_run_result_t *result,
                                 const json_value_t *state_json, json_value_t **out_summary_json) {
  json_value_t *summary;
  json_value_t *result_json = NULL;
  json_value_t *available_commands = NULL;
  json_value_t *available_command_descriptors = NULL;
  const char *interrupt_reason = NULL;
  const char *pending_action = NULL;
  const char *active_agent = NULL;
  const char *handoff_target_agent = NULL;
  const char *handoff_reason = NULL;
  const char *const *command_names = NULL;
  size_t command_count = 0;
  char *owned_executor_failure_reason = NULL;

  if (!thread_id || !run_id || !status || !result || !out_summary_json) {
    return -1;
  }

  summary = turbo_json_create_object();
  if (!summary) {
    return -1;
  }

  if (turbo_agent_runtime_result_to_json(result, &result_json) != 0) {
    turbo_free_json(&summary);
    return -1;
  }

  turbo_json_object_set_string(summary, "thread_id", thread_id);
  turbo_json_object_set_string(summary, "run_id", run_id);
  turbo_json_object_set_string(summary, "status", status);
  if (checkpoint_id) {
    turbo_json_object_set_string(summary, "checkpoint_id", checkpoint_id);
  } else {
    turbo_json_object_set_null(summary, "checkpoint_id");
  }
  if (parent_agent_run_id) {
    turbo_json_object_set_string(summary, "parent_agent_run_id", parent_agent_run_id);
  } else {
    turbo_json_object_set_null(summary, "parent_agent_run_id");
  }
  if (parent_tool_call_id) {
    turbo_json_object_set_string(summary, "parent_tool_call_id", parent_tool_call_id);
  } else {
    turbo_json_object_set_null(summary, "parent_tool_call_id");
  }
  if (parent_tool_name) {
    turbo_json_object_set_string(summary, "parent_tool_name", parent_tool_name);
  } else {
    turbo_json_object_set_null(summary, "parent_tool_name");
  }
  if (parent_graph_run_id) {
    turbo_json_object_set_string(summary, "parent_graph_run_id", parent_graph_run_id);
  } else {
    turbo_json_object_set_null(summary, "parent_graph_run_id");
  }
  if (call_frame_id) {
    turbo_json_object_set_string(summary, "call_frame_id", call_frame_id);
  } else {
    turbo_json_object_set_null(summary, "call_frame_id");
  }
  if (state_json) {
    active_agent = turbo_agent_state_active_agent(state_json);
    handoff_target_agent = turbo_agent_state_handoff_target_agent(state_json);
    handoff_reason = turbo_agent_state_handoff_reason(state_json);
  }
  if (active_agent && active_agent[0] != '\0') {
    turbo_json_object_set_string(summary, "active_agent", active_agent);
  } else {
    turbo_json_object_set_null(summary, "active_agent");
  }
  if (handoff_target_agent && handoff_target_agent[0] != '\0') {
    turbo_json_object_set_string(summary, "handoff_target_agent", handoff_target_agent);
  } else {
    turbo_json_object_set_null(summary, "handoff_target_agent");
  }
  if (handoff_reason && handoff_reason[0] != '\0') {
    turbo_json_object_set_string(summary, "handoff_reason", handoff_reason);
  } else {
    turbo_json_object_set_null(summary, "handoff_reason");
  }
  if (strcmp(status, "interrupted") == 0 && result->next_node && result->next_node[0] != '\0') {
    turbo_json_object_set_string(summary, "pending_node", result->next_node);
  } else {
    turbo_json_object_set_null(summary, "pending_node");
  }

  available_commands = turbo_json_create_array();
  available_command_descriptors = turbo_json_create_array();
  if (!available_commands || !available_command_descriptors) {
    turbo_free_json(&result_json);
    turbo_free_json(&available_commands);
    turbo_free_json(&available_command_descriptors);
    turbo_free_json(&summary);
    return -1;
  }
  turbo_agent_runtime_interrupt_metadata(status, result->next_node, state_json, &interrupt_reason,
                                         &pending_action, &command_names, &command_count,
                                         &owned_executor_failure_reason);
  if (strcmp(status, "interrupted") == 0) {
    if (turbo_agent_runtime_summary_add_commands(available_commands, available_command_descriptors,
                                                 command_names, command_count) != 0) {
      goto fail;
    }
  }
  if (interrupt_reason) {
    turbo_json_object_set_string(summary, "interrupt_reason", interrupt_reason);
  } else {
    turbo_json_object_set_null(summary, "interrupt_reason");
  }
  if (pending_action) {
    turbo_json_object_set_string(summary, "pending_action", pending_action);
  } else {
    turbo_json_object_set_null(summary, "pending_action");
  }
  turbo_json_object_add(summary, "available_commands", available_commands);
  turbo_json_object_add(summary, "available_command_descriptors", available_command_descriptors);
  available_commands = NULL;
  available_command_descriptors = NULL;
  turbo_json_object_add(summary, "result", result_json);
  free(owned_executor_failure_reason);
  *out_summary_json = summary;
  return 0;

fail:
  free(owned_executor_failure_reason);
  turbo_free_json(&result_json);
  turbo_free_json(&available_commands);
  turbo_free_json(&available_command_descriptors);
  turbo_free_json(&summary);
  return -1;
}

int turbo_agent_runtime_make_timestamp(char *buffer, size_t buffer_size) {
  time_t now;
  struct tm utc_tm;

  if (!buffer || buffer_size < 21) {
    return -1;
  }

  now = time(NULL);
#ifdef _WIN32
  if (gmtime_s(&utc_tm, &now) != 0) {
    return -1;
  }
#else
  if (!gmtime_r(&now, &utc_tm)) {
    return -1;
  }
#endif

  if (strftime(buffer, buffer_size, "%Y-%m-%dT%H:%M:%SZ", &utc_tm) == 0) {
    return -1;
  }

  return 0;
}

char *turbo_agent_runtime_make_id(const char *prefix) {
  unsigned long long counter;
  unsigned long long tick;
  int needed;
  char *buffer;

  if (!prefix) {
    return NULL;
  }

  counter = atomic_fetch_add(&turbo_agent_runtime_id_counter, 1);
  tick = (unsigned long long)turbo_hrtime();
  needed = snprintf(NULL, 0, "%s_%llx%llx", prefix, tick, counter);
  if (needed < 0) {
    return NULL;
  }
  buffer = (char *)malloc((size_t)needed + 1);
  if (!buffer) {
    return NULL;
  }
  snprintf(buffer, (size_t)needed + 1, "%s_%llx%llx", prefix, tick, counter);
  return buffer;
}

int turbo_agent_runtime_write_text_file(const char *path, const char *content) {
  FILE *fp;

  if (!path || !content) {
    return -1;
  }

  fp = fopen(path, "wb");
  if (!fp) {
    return -1;
  }
  if (fwrite(content, 1, strlen(content), fp) != strlen(content)) {
    fclose(fp);
    return -1;
  }
  fclose(fp);
  return 0;
}

int turbo_agent_runtime_read_text_file(const char *path, char **out_content) {
  FILE *fp;
  long size;
  char *buffer;

  if (!path || !out_content) {
    return -1;
  }

  *out_content = NULL;
  fp = fopen(path, "rb");
  if (!fp) {
    return -1;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return -1;
  }
  size = ftell(fp);
  if (size < 0) {
    fclose(fp);
    return -1;
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return -1;
  }
  buffer = (char *)malloc((size_t)size + 1);
  if (!buffer) {
    fclose(fp);
    return -1;
  }
  if (size > 0 && fread(buffer, 1, (size_t)size, fp) != (size_t)size) {
    free(buffer);
    fclose(fp);
    return -1;
  }
  buffer[size] = '\0';
  fclose(fp);
  *out_content = buffer;
  return 0;
}

int turbo_agent_runtime_ensure_dir(const char *path) {
  if (!path || path[0] == '\0') {
    return -1;
  }
#ifdef _WIN32
  if (_mkdir(path) == 0 || errno == EEXIST) {
    return 0;
  }
#else
  if (mkdir(path, 0777) == 0 || errno == EEXIST) {
    return 0;
  }
#endif
  return -1;
}

char *turbo_agent_runtime_join_path(const char *left, const char *right) {
  int needed;
  char *buffer;

  if (!left || !right) {
    return NULL;
  }

  needed = snprintf(NULL, 0, "%s/%s", left, right);
  if (needed < 0) {
    return NULL;
  }
  buffer = (char *)malloc((size_t)needed + 1);
  if (!buffer) {
    return NULL;
  }
  snprintf(buffer, (size_t)needed + 1, "%s/%s", left, right);
  return buffer;
}

char *turbo_agent_runtime_record_path(const char *root_dir, const char *collection,
                                      const char *id) {
  char *directory;
  char *filename;
  char *path;
  int needed;

  if (!root_dir || !collection || !id) {
    return NULL;
  }

  directory = turbo_agent_runtime_join_path(root_dir, collection);
  if (!directory) {
    return NULL;
  }
  needed = snprintf(NULL, 0, "%s.json", id);
  if (needed < 0) {
    free(directory);
    return NULL;
  }
  filename = (char *)malloc((size_t)needed + 1);
  if (!filename) {
    free(directory);
    return NULL;
  }
  snprintf(filename, (size_t)needed + 1, "%s.json", id);
  path = turbo_agent_runtime_join_path(directory, filename);
  free(filename);
  free(directory);
  return path;
}

int turbo_agent_runtime_parse_json_string(const char *json_text, json_value_t **out_json) {
  json_value_t *json_root = NULL;

  if (!json_text || !out_json) {
    return -1;
  }

  *out_json = NULL;
  if (turbo_parse_json((const uint8_t *)json_text, strlen(json_text), &json_root) != 0 ||
      !json_root) {
    turbo_free_json(&json_root);
    return -1;
  }
  *out_json = json_root;
  return 0;
}

int turbo_agent_runtime_store_put_json(turbo_agent_runtime_t *runtime, const char *collection,
                                       const char *id, const json_value_t *record_json) {
  char *serialized;
  int rc;

  if (!runtime || !collection || !id || !record_json || !runtime->store.put) {
    return -1;
  }

  serialized = turbo_json_serialize(record_json, NULL);
  if (!serialized) {
    return -1;
  }
  turbo_mutex_lock(&runtime->store_mutex);
  rc = runtime->store.put(runtime->store.user_data, collection, id, serialized);
  turbo_mutex_unlock(&runtime->store_mutex);
  turbo_json_serialize_free(serialized);
  return rc;
}

int turbo_agent_runtime_store_get_json(turbo_agent_runtime_t *runtime, const char *collection,
                                       const char *id, json_value_t **out_record_json) {
  char *serialized = NULL;
  int rc;

  if (!runtime || !collection || !id || !out_record_json || !runtime->store.get) {
    return -1;
  }

  *out_record_json = NULL;
  turbo_mutex_lock(&runtime->store_mutex);
  rc = runtime->store.get(runtime->store.user_data, collection, id, &serialized);
  turbo_mutex_unlock(&runtime->store_mutex);
  if (rc != 0 || !serialized) {
    free(serialized);
    return -1;
  }
  rc = turbo_agent_runtime_parse_json_string(serialized, out_record_json);
  free(serialized);
  return rc;
}

int turbo_agent_runtime_store_list_json(turbo_agent_runtime_t *runtime, const char *collection,
                                        const char *filter_key, const char *filter_value,
                                        json_value_t **out_records_json) {
  char *serialized = NULL;
  int rc;

  if (!runtime || !collection || !out_records_json || !runtime->store.list) {
    return -1;
  }

  *out_records_json = NULL;
  turbo_mutex_lock(&runtime->store_mutex);
  rc = runtime->store.list(runtime->store.user_data, collection, filter_key, filter_value,
                           &serialized);
  turbo_mutex_unlock(&runtime->store_mutex);
  if (rc != 0 || !serialized) {
    free(serialized);
    return -1;
  }
  rc = turbo_agent_runtime_parse_json_string(serialized, out_records_json);
  free(serialized);
  if (rc != 0 || !*out_records_json || turbo_json_type(*out_records_json) != TURBO_JSON_ARRAY) {
    turbo_free_json(out_records_json);
    return -1;
  }
  return 0;
}

static char *turbo_agent_runtime_graph_name(turbo_graph_t *graph) {
  const char *topology_id;
  json_value_t *topology_json = NULL;
  char *name = NULL;
  const char *graph_name;

  if (!graph) {
    return NULL;
  }

  topology_id = turbo_graph_topology_id(graph);
  if (!topology_id || turbo_agent_runtime_parse_json_string(topology_id, &topology_json) != 0) {
    turbo_free_json(&topology_json);
    return NULL;
  }
  graph_name = turbo_json_get_string(topology_json, "graph_name");
  if (graph_name) {
    name = turbo_agent_runtime_strdup(graph_name);
  }
  turbo_free_json(&topology_json);
  return name;
}

static char *turbo_agent_runtime_graph_name_from_topology_id_text(const char *topology_id_text) {
  json_value_t *topology_json = NULL;
  char *name = NULL;
  const char *graph_name;

  if (!topology_id_text ||
      turbo_agent_runtime_parse_json_string(topology_id_text, &topology_json) != 0) {
    turbo_free_json(&topology_json);
    return NULL;
  }
  graph_name = turbo_json_get_string(topology_json, "graph_name");
  if (graph_name) {
    name = turbo_agent_runtime_strdup(graph_name);
  }
  turbo_free_json(&topology_json);
  return name;
}

static int turbo_agent_runtime_json_value_to_json(const json_value_t *state,
                                                  json_value_t **out_state_json) {
  if (!state || !out_state_json) {
    return -1;
  }
  *out_state_json = turbo_json_clone(state);
  return *out_state_json ? 0 : -1;
}

static int turbo_agent_runtime_json_value_array_append_json(json_value_t *array_json_value,
                                                            const json_value_t *json_value) {
  json_value_t *bound;
  turbo_runtime_json_status_t status;

  if (!array_json_value || !json_value) {
    return -1;
  }
  bound = turbo_json_clone(json_value);
  if (!bound) {
    return -1;
  }
  status = turbo_runtime_json_array_append(array_json_value, bound);
  if (status != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(bound);
    return -1;
  }
  return 0;
}

static int turbo_agent_runtime_collect_log_events(const turbo_graph_run_log_t *log,
                                                  json_value_t **out_events_json) {
  json_value_t *events_json_value;
  json_value_t *events_json;

  if (!log || !out_events_json) {
    return -1;
  }

  events_json_value = turbo_event_log_events_json_value(turbo_graph_run_log_events(log));
  if (!events_json_value) {
    return -1;
  }
  events_json = turbo_json_clone(events_json_value);
  turbo_runtime_json_destroy(events_json_value);
  if (!events_json || turbo_json_type(events_json) != TURBO_JSON_ARRAY) {
    turbo_free_json(&events_json);
    return -1;
  }
  *out_events_json = events_json;
  return 0;
}

static int turbo_agent_runtime_collect_event_log_events(const turbo_event_log_t *log,
                                                        json_value_t **out_events_json) {
  json_value_t *events_json_value;
  json_value_t *events_json;

  if (!log || !out_events_json) {
    return -1;
  }

  events_json_value = turbo_event_log_events_json_value(log);
  if (!events_json_value) {
    return -1;
  }
  events_json = turbo_json_clone(events_json_value);
  turbo_runtime_json_destroy(events_json_value);
  if (!events_json || turbo_json_type(events_json) != TURBO_JSON_ARRAY) {
    turbo_free_json(&events_json);
    return -1;
  }
  *out_events_json = events_json;
  return 0;
}

static turbo_graph_exec_status_t
turbo_agent_runtime_clone_checkpoint(const turbo_graph_checkpoint_t *checkpoint,
                                     turbo_graph_checkpoint_t **out_checkpoint) {
  return turbo_graph_checkpoint_clone(checkpoint, out_checkpoint);
}

static turbo_graph_exec_status_t
turbo_agent_runtime_replace_checkpoint(turbo_graph_checkpoint_t **target,
                                       const turbo_graph_checkpoint_t *checkpoint) {
  turbo_graph_checkpoint_t *copy = NULL;
  turbo_graph_exec_status_t status;

  if (!target || !checkpoint) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }
  status = turbo_agent_runtime_clone_checkpoint(checkpoint, &copy);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }
  turbo_graph_checkpoint_destroy(*target);
  *target = copy;
  return TURBO_GRAPH_EXEC_OK;
}

static void
turbo_agent_runtime_capture_stream_checkpoint(const turbo_graph_checkpoint_t *checkpoint,
                                              void *user_data) {
  turbo_agent_runtime_stream_capture_t *capture = (turbo_agent_runtime_stream_capture_t *)user_data;

  if (!capture || !checkpoint || capture->checkpoint_status != TURBO_GRAPH_EXEC_OK) {
    return;
  }
  capture->checkpoint_status =
      turbo_agent_runtime_replace_checkpoint(&capture->checkpoint, checkpoint);
  if (capture->user_checkpoint_cb) {
    capture->user_checkpoint_cb(checkpoint, capture->user_checkpoint_user_data);
  }
}

static void turbo_agent_runtime_capture_stream_event(const json_value_t *event, void *user_data) {
  turbo_agent_runtime_stream_capture_t *capture = (turbo_agent_runtime_stream_capture_t *)user_data;

  if (!capture || !event || !capture->events) {
    return;
  }
  turbo_event_log_capture_json_value(event, capture->events);
  if (capture->user_event_sink) {
    capture->user_event_sink(event, capture->user_event_sink_user_data);
  }
}

static turbo_graph_exec_status_t turbo_agent_runtime_run_graph_stream(
    turbo_graph_t *graph, const json_value_t *start_state,
    const turbo_graph_checkpoint_t *start_checkpoint, const turbo_graph_run_options_t *options,
    const turbo_cancel_token_t *cancel_token, turbo_event_log_t *events,
    turbo_event_sink_json_value_fn user_event_sink, void *user_event_sink_user_data,
    turbo_graph_run_result_t *out_result, json_value_t **out_state,
    turbo_graph_checkpoint_t **out_checkpoint) {
  turbo_graph_run_options_t effective_options;
  turbo_agent_runtime_stream_capture_t capture = {0};
  turbo_graph_exec_status_t status;

  *out_state = NULL;
  *out_checkpoint = NULL;
  if (options) {
    effective_options = *options;
  } else {
    memset(&effective_options, 0, sizeof(effective_options));
  }

  capture.events = events;
  capture.checkpoint = NULL;
  capture.checkpoint_status = TURBO_GRAPH_EXEC_OK;
  capture.user_checkpoint_cb = effective_options.checkpoint_cb;
  capture.user_checkpoint_user_data = effective_options.checkpoint_user_data;
  capture.user_event_sink = user_event_sink;
  capture.user_event_sink_user_data = user_event_sink_user_data;
  effective_options.checkpoint_cb = turbo_agent_runtime_capture_stream_checkpoint;
  effective_options.checkpoint_user_data = &capture;

  if (start_checkpoint) {
    status = turbo_graph_run_checkpoint_json_value_stream_controlled(
        graph, start_checkpoint, &effective_options, cancel_token,
        turbo_agent_runtime_capture_stream_event, &capture, out_result, out_state);
  } else {
    status = turbo_graph_run_json_value_stream_controlled(
        graph, start_state, &effective_options, cancel_token,
        turbo_agent_runtime_capture_stream_event, &capture, out_result, out_state);
  }

  *out_checkpoint = capture.checkpoint;
  if (capture.checkpoint_status != TURBO_GRAPH_EXEC_OK) {
    return capture.checkpoint_status;
  }
  if (turbo_event_log_status(events) != TURBO_EVENT_LOG_OK) {
    return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }
  return status;
}

static int turbo_agent_runtime_build_thread_record(const char *thread_id, const char *timestamp,
                                                   json_value_t **out_record) {
  json_value_t *record;

  if (!thread_id || !timestamp || !out_record) {
    return -1;
  }
  record = turbo_json_create_object();
  if (!record) {
    return -1;
  }
  turbo_json_object_set_string(record, "id", thread_id);
  turbo_json_object_set_string(record, "created_at", timestamp);
  turbo_json_object_set_string(record, "updated_at", timestamp);
  *out_record = record;
  return 0;
}

static int turbo_agent_runtime_build_run_record(
    const char *run_id, const char *thread_id, const char *parent_run_id,
    const char *parent_agent_run_id, const char *parent_tool_call_id, const char *parent_tool_name,
    const char *parent_graph_run_id, const char *call_frame_id,
    const char *forked_from_checkpoint_id, const char *graph_name, const char *topology_id,
    const char *status, const char *created_at, const char *updated_at,
    const char *latest_checkpoint_id, const turbo_graph_run_result_t *result,
    const json_value_t *state_snapshot, json_value_t **out_record) {
  json_value_t *record;
  json_value_t *result_json = NULL;

  if (!run_id || !thread_id || !graph_name || !topology_id || !status || !created_at ||
      !updated_at || !result || !out_record) {
    return -1;
  }

  if (turbo_agent_runtime_result_to_json(result, &result_json) != 0) {
    return -1;
  }

  record = turbo_json_create_object();
  if (!record) {
    turbo_free_json(&result_json);
    return -1;
  }

  turbo_json_object_set_string(record, "id", run_id);
  turbo_json_object_set_string(record, "thread_id", thread_id);
  if (parent_run_id) {
    turbo_json_object_set_string(record, "parent_run_id", parent_run_id);
  } else {
    turbo_json_object_set_null(record, "parent_run_id");
  }
  if (parent_agent_run_id) {
    turbo_json_object_set_string(record, "parent_agent_run_id", parent_agent_run_id);
  } else {
    turbo_json_object_set_null(record, "parent_agent_run_id");
  }
  if (parent_tool_call_id) {
    turbo_json_object_set_string(record, "parent_tool_call_id", parent_tool_call_id);
  } else {
    turbo_json_object_set_null(record, "parent_tool_call_id");
  }
  if (parent_tool_name) {
    turbo_json_object_set_string(record, "parent_tool_name", parent_tool_name);
  } else {
    turbo_json_object_set_null(record, "parent_tool_name");
  }
  if (parent_graph_run_id) {
    turbo_json_object_set_string(record, "parent_graph_run_id", parent_graph_run_id);
  } else {
    turbo_json_object_set_null(record, "parent_graph_run_id");
  }
  if (call_frame_id) {
    turbo_json_object_set_string(record, "call_frame_id", call_frame_id);
  } else {
    turbo_json_object_set_null(record, "call_frame_id");
  }
  if (forked_from_checkpoint_id) {
    turbo_json_object_set_string(record, "forked_from_checkpoint_id", forked_from_checkpoint_id);
  } else {
    turbo_json_object_set_null(record, "forked_from_checkpoint_id");
  }
  turbo_json_object_set_string(record, "graph_name", graph_name);
  turbo_json_object_set_string(record, "topology_id", topology_id);
  turbo_json_object_set_string(record, "status", status);
  turbo_json_object_set_string(record, "created_at", created_at);
  turbo_json_object_set_string(record, "updated_at", updated_at);
  if (latest_checkpoint_id) {
    turbo_json_object_set_string(record, "latest_checkpoint_id", latest_checkpoint_id);
  } else {
    turbo_json_object_set_null(record, "latest_checkpoint_id");
  }
  if (state_snapshot) {
    turbo_json_object_add(record, "state_snapshot", turbo_json_clone(state_snapshot));
  } else {
    turbo_json_object_set_null(record, "state_snapshot");
  }
  turbo_json_object_add(record, "result", result_json);
  *out_record = record;
  return 0;
}

static int turbo_agent_runtime_build_checkpoint_record(
    const char *checkpoint_id, const char *thread_id, const char *run_id,
    const char *parent_checkpoint_id, const char *parent_agent_run_id,
    const char *parent_tool_call_id, const char *parent_tool_name, const char *parent_graph_run_id,
    const char *call_frame_id, size_t seq, const char *status, const char *created_at,
    const turbo_graph_checkpoint_t *checkpoint, const json_value_t *control_snapshot,
    const json_value_t *workflow_snapshot, const json_value_t *events_json,
    json_value_t **out_record) {
  json_value_t *record;
  char *checkpoint_json_text = NULL;
  json_value_t *checkpoint_json = NULL;
  char *graph_name = NULL;
  const char *topology_id_text;

  if (!checkpoint_id || !thread_id || !run_id || !status || !created_at || !checkpoint ||
      !control_snapshot || !workflow_snapshot || !events_json || !out_record) {
    return -1;
  }

  checkpoint_json_text = turbo_graph_checkpoint_serialize(checkpoint, NULL);
  if (!checkpoint_json_text ||
      turbo_agent_runtime_parse_json_string(checkpoint_json_text, &checkpoint_json) != 0) {
    turbo_json_serialize_free(checkpoint_json_text);
    turbo_free_json(&checkpoint_json);
    return -1;
  }
  turbo_json_serialize_free(checkpoint_json_text);
  topology_id_text = turbo_json_get_string(checkpoint_json, "topology_id");
  graph_name = turbo_agent_runtime_graph_name_from_topology_id_text(topology_id_text);

  record = turbo_json_create_object();
  if (!record || !topology_id_text || !graph_name) {
    free(graph_name);
    turbo_free_json(&checkpoint_json);
    turbo_free_json(&record);
    return -1;
  }

  turbo_json_object_set_string(record, "id", checkpoint_id);
  turbo_json_object_set_string(record, "thread_id", thread_id);
  turbo_json_object_set_string(record, "run_id", run_id);
  if (parent_checkpoint_id) {
    turbo_json_object_set_string(record, "parent_checkpoint_id", parent_checkpoint_id);
  } else {
    turbo_json_object_set_null(record, "parent_checkpoint_id");
  }
  if (parent_agent_run_id) {
    turbo_json_object_set_string(record, "parent_agent_run_id", parent_agent_run_id);
  } else {
    turbo_json_object_set_null(record, "parent_agent_run_id");
  }
  if (parent_tool_call_id) {
    turbo_json_object_set_string(record, "parent_tool_call_id", parent_tool_call_id);
  } else {
    turbo_json_object_set_null(record, "parent_tool_call_id");
  }
  if (parent_tool_name) {
    turbo_json_object_set_string(record, "parent_tool_name", parent_tool_name);
  } else {
    turbo_json_object_set_null(record, "parent_tool_name");
  }
  if (parent_graph_run_id) {
    turbo_json_object_set_string(record, "parent_graph_run_id", parent_graph_run_id);
  } else {
    turbo_json_object_set_null(record, "parent_graph_run_id");
  }
  if (call_frame_id) {
    turbo_json_object_set_string(record, "call_frame_id", call_frame_id);
  } else {
    turbo_json_object_set_null(record, "call_frame_id");
  }
  turbo_json_object_set_number(record, "seq", (double)seq);
  turbo_json_object_set_string(record, "status", status);
  turbo_json_object_set_string(record, "created_at", created_at);
  turbo_json_object_set_string(record, "graph_name", graph_name);
  turbo_json_object_set_string(record, "topology_id", topology_id_text);
  turbo_json_object_set_string(record, "next_node", turbo_graph_checkpoint_next_node(checkpoint));
  turbo_json_object_set_number(record, "steps", (double)turbo_graph_checkpoint_steps(checkpoint));
  turbo_json_object_add(record, "checkpoint_json", checkpoint_json);
  turbo_json_object_add(record, "control_snapshot", turbo_json_clone(control_snapshot));
  turbo_json_object_add(record, "workflow_snapshot", turbo_json_clone(workflow_snapshot));
  turbo_json_object_add(record, "events", turbo_json_clone(events_json));
  free(graph_name);
  *out_record = record;
  return 0;
}

static int turbo_agent_runtime_extract_checkpoint_record(
    const json_value_t *record, const char **out_thread_id, const char **out_run_id,
    const char **out_parent_checkpoint_id, size_t *out_seq, json_value_t **out_checkpoint_json) {
  const json_value_t *seq_value;

  if (!record || !out_thread_id || !out_run_id || !out_parent_checkpoint_id || !out_seq ||
      !out_checkpoint_json) {
    return -1;
  }
  *out_thread_id = turbo_json_get_string(record, "thread_id");
  *out_run_id = turbo_json_get_string(record, "run_id");
  *out_parent_checkpoint_id = turbo_json_get_string(record, "parent_checkpoint_id");
  seq_value = turbo_json_object_get(record, "seq");
  *out_checkpoint_json = turbo_json_object_get(record, "checkpoint_json");
  if (!*out_thread_id || !*out_run_id || !seq_value || !*out_checkpoint_json ||
      turbo_json_type(seq_value) != TURBO_JSON_NUMBER) {
    return -1;
  }
  *out_seq = (size_t)turbo_json_number(seq_value);
  return 0;
}

static int turbo_agent_runtime_checkpoint_from_json(const json_value_t *checkpoint_json,
                                                    const json_value_t *state_override,
                                                    turbo_graph_checkpoint_t **out_checkpoint) {
  char *serialized;
  json_value_t *checkpoint_copy = NULL;
  json_value_t *state_json = NULL;
  turbo_graph_checkpoint_t *checkpoint = NULL;

  if (!checkpoint_json || !out_checkpoint) {
    return -1;
  }
  *out_checkpoint = NULL;

  checkpoint_copy = turbo_json_create_object();
  if (!checkpoint_copy) {
    return -1;
  }
  turbo_json_object_set_number(checkpoint_copy, "checkpoint_version",
                               turbo_json_get_double(checkpoint_json, "checkpoint_version", 0));
  turbo_json_object_set_string(checkpoint_copy, "next_node",
                               turbo_json_get_string(checkpoint_json, "next_node"));
  turbo_json_object_set_number(checkpoint_copy, "steps",
                               turbo_json_get_double(checkpoint_json, "steps", 0));
  turbo_json_object_set_string(checkpoint_copy, "topology_id",
                               turbo_json_get_string(checkpoint_json, "topology_id"));
  if (state_override) {
    if (turbo_agent_runtime_json_value_to_json(state_override, &state_json) != 0) {
      turbo_free_json(&checkpoint_copy);
      return -1;
    }
  } else {
    state_json = turbo_json_clone(turbo_json_object_get(checkpoint_json, "state"));
    if (!state_json) {
      turbo_free_json(&checkpoint_copy);
      return -1;
    }
  }
  turbo_json_object_add(checkpoint_copy, "state", state_json);
  serialized = turbo_json_serialize(checkpoint_copy, NULL);
  turbo_free_json(&checkpoint_copy);
  if (!serialized) {
    return -1;
  }
  if (turbo_graph_checkpoint_deserialize(serialized, strlen(serialized), &checkpoint) !=
          TURBO_GRAPH_EXEC_OK ||
      !checkpoint) {
    turbo_json_serialize_free(serialized);
    return -1;
  }
  turbo_json_serialize_free(serialized);
  *out_checkpoint = checkpoint;
  return 0;
}

static int
turbo_agent_runtime_append_checkpoint_events_json_value(json_value_t *events_json_value,
                                                        const json_value_t *checkpoint_record) {
  json_value_t *events_json;
  size_t i;
  size_t count;

  if (!events_json_value || !checkpoint_record) {
    return -1;
  }
  events_json = turbo_json_object_get(checkpoint_record, "events");
  if (!events_json || turbo_json_type(events_json) != TURBO_JSON_ARRAY) {
    return -1;
  }
  count = turbo_json_array_size(events_json);
  for (i = 0; i < count; ++i) {
    if (turbo_agent_runtime_json_value_array_append_json(
            events_json_value, turbo_json_array_get(events_json, i)) != 0) {
      return -1;
    }
  }
  return 0;
}

static void turbo_agent_runtime_memory_store_destroy(void *user_data) {
  turbo_agent_runtime_memory_store_t *store = (turbo_agent_runtime_memory_store_t *)user_data;
  turbo_agent_runtime_memory_record_t *record;
  turbo_agent_runtime_memory_record_t *next;

  if (!store) {
    return;
  }
  record = store->head;
  while (record) {
    next = record->next;
    free(record->collection);
    free(record->id);
    free(record->record_json);
    free(record);
    record = next;
  }
  free(store);
}

static int turbo_agent_runtime_memory_store_put(void *user_data, const char *collection,
                                                const char *id, const char *record_json) {
  turbo_agent_runtime_memory_store_t *store = (turbo_agent_runtime_memory_store_t *)user_data;
  turbo_agent_runtime_memory_record_t *record;
  char *record_copy;

  if (!store || !collection || !id || !record_json) {
    return -1;
  }

  for (record = store->head; record; record = record->next) {
    if (strcmp(record->collection, collection) == 0 && strcmp(record->id, id) == 0) {
      record_copy = turbo_agent_runtime_strdup(record_json);
      if (!record_copy) {
        return -1;
      }
      free(record->record_json);
      record->record_json = record_copy;
      return 0;
    }
  }

  record = (turbo_agent_runtime_memory_record_t *)calloc(1, sizeof(*record));
  if (!record) {
    return -1;
  }
  record->collection = turbo_agent_runtime_strdup(collection);
  record->id = turbo_agent_runtime_strdup(id);
  record->record_json = turbo_agent_runtime_strdup(record_json);
  if (!record->collection || !record->id || !record->record_json) {
    free(record->collection);
    free(record->id);
    free(record->record_json);
    free(record);
    return -1;
  }
  record->next = store->head;
  store->head = record;
  return 0;
}

static int turbo_agent_runtime_memory_store_get(void *user_data, const char *collection,
                                                const char *id, char **out_record_json) {
  turbo_agent_runtime_memory_store_t *store = (turbo_agent_runtime_memory_store_t *)user_data;
  turbo_agent_runtime_memory_record_t *record;

  if (!store || !collection || !id || !out_record_json) {
    return -1;
  }
  *out_record_json = NULL;
  for (record = store->head; record; record = record->next) {
    if (strcmp(record->collection, collection) == 0 && strcmp(record->id, id) == 0) {
      *out_record_json = turbo_agent_runtime_strdup(record->record_json);
      return *out_record_json ? 0 : -1;
    }
  }
  return -1;
}

int turbo_agent_runtime_json_matches_filter(const json_value_t *record, const char *filter_key,
                                            const char *filter_value) {
  const char *value;

  if (!filter_key || !filter_value) {
    return 1;
  }
  value = turbo_json_get_string(record, filter_key);
  return value && strcmp(value, filter_value) == 0;
}

static int turbo_agent_runtime_memory_store_list(void *user_data, const char *collection,
                                                 const char *filter_key, const char *filter_value,
                                                 char **out_records_json) {
  turbo_agent_runtime_memory_store_t *store = (turbo_agent_runtime_memory_store_t *)user_data;
  turbo_agent_runtime_memory_record_t *record;
  json_value_t *records_json;
  json_value_t *record_json = NULL;

  if (!store || !collection || !out_records_json) {
    return -1;
  }
  *out_records_json = NULL;
  records_json = turbo_json_create_array();
  if (!records_json) {
    return -1;
  }
  for (record = store->head; record; record = record->next) {
    if (strcmp(record->collection, collection) != 0) {
      continue;
    }
    if (turbo_agent_runtime_parse_json_string(record->record_json, &record_json) != 0) {
      turbo_free_json(&records_json);
      return -1;
    }
    if (turbo_agent_runtime_json_matches_filter(record_json, filter_key, filter_value)) {
      turbo_json_array_add(records_json, record_json);
      record_json = NULL;
    }
    turbo_free_json(&record_json);
  }
  {
    char *serialized = turbo_json_serialize(records_json, NULL);
    *out_records_json = serialized ? turbo_agent_runtime_strdup(serialized) : NULL;
    turbo_json_serialize_free(serialized);
  }
  turbo_free_json(&records_json);
  return *out_records_json ? 0 : -1;
}

static void turbo_agent_runtime_file_store_destroy(void *user_data) {
  turbo_agent_runtime_file_store_t *store = (turbo_agent_runtime_file_store_t *)user_data;

  if (!store) {
    return;
  }
  free(store->root_dir);
  free(store);
}

static int turbo_agent_runtime_file_store_put(void *user_data, const char *collection,
                                              const char *id, const char *record_json) {
  turbo_agent_runtime_file_store_t *store = (turbo_agent_runtime_file_store_t *)user_data;
  char *directory;
  char *path;
  int rc;

  if (!store || !store->root_dir || !collection || !id || !record_json) {
    return -1;
  }
  directory = turbo_agent_runtime_join_path(store->root_dir, collection);
  if (!directory || turbo_agent_runtime_ensure_dir(directory) != 0) {
    free(directory);
    return -1;
  }
  free(directory);
  path = turbo_agent_runtime_record_path(store->root_dir, collection, id);
  if (!path) {
    return -1;
  }
  rc = turbo_agent_runtime_write_text_file(path, record_json);
  free(path);
  return rc;
}

static int turbo_agent_runtime_file_store_get(void *user_data, const char *collection,
                                              const char *id, char **out_record_json) {
  turbo_agent_runtime_file_store_t *store = (turbo_agent_runtime_file_store_t *)user_data;
  char *path;
  int rc;

  if (!store || !store->root_dir || !out_record_json) {
    return -1;
  }
  path = turbo_agent_runtime_record_path(store->root_dir, collection, id);
  if (!path) {
    return -1;
  }
  rc = turbo_agent_runtime_read_text_file(path, out_record_json);
  free(path);
  return rc;
}

static int turbo_agent_runtime_file_store_list_append(const char *path, const char *filter_key,
                                                      const char *filter_value,
                                                      json_value_t *records_json) {
  char *record_text = NULL;
  json_value_t *record_json = NULL;

  if (!path || !records_json) {
    return -1;
  }
  if (turbo_agent_runtime_read_text_file(path, &record_text) != 0 ||
      turbo_agent_runtime_parse_json_string(record_text, &record_json) != 0) {
    free(record_text);
    turbo_free_json(&record_json);
    return -1;
  }
  free(record_text);
  if (turbo_agent_runtime_json_matches_filter(record_json, filter_key, filter_value)) {
    turbo_json_array_add(records_json, record_json);
    record_json = NULL;
  }
  turbo_free_json(&record_json);
  return 0;
}

static int turbo_agent_runtime_file_store_list(void *user_data, const char *collection,
                                               const char *filter_key, const char *filter_value,
                                               char **out_records_json) {
  turbo_agent_runtime_file_store_t *store = (turbo_agent_runtime_file_store_t *)user_data;
  char *directory;
  json_value_t *records_json;
  int rc = 0;

  if (!store || !store->root_dir || !collection || !out_records_json) {
    return -1;
  }
  *out_records_json = NULL;
  directory = turbo_agent_runtime_join_path(store->root_dir, collection);
  if (!directory) {
    return -1;
  }
  records_json = turbo_json_create_array();
  if (!records_json) {
    free(directory);
    return -1;
  }

#ifdef _WIN32
  {
    WIN32_FIND_DATAA find_data;
    HANDLE handle;
    char *pattern = turbo_agent_runtime_join_path(directory, "*.json");

    if (!pattern) {
      turbo_free_json(&records_json);
      free(directory);
      return -1;
    }
    handle = FindFirstFileA(pattern, &find_data);
    free(pattern);
    if (handle != INVALID_HANDLE_VALUE) {
      do {
        char *path;
        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          continue;
        }
        path = turbo_agent_runtime_join_path(directory, find_data.cFileName);
        if (!path || turbo_agent_runtime_file_store_list_append(path, filter_key, filter_value,
                                                                records_json) != 0) {
          free(path);
          rc = -1;
          break;
        }
        free(path);
      } while (FindNextFileA(handle, &find_data));
      FindClose(handle);
    }
  }
#else
  {
    DIR *dir = opendir(directory);
    struct dirent *entry;

    if (!dir) {
      if (errno != ENOENT) {
        turbo_free_json(&records_json);
        free(directory);
        return -1;
      }
    } else {
      while ((entry = readdir(dir)) != NULL) {
        char *path;
        size_t len = strlen(entry->d_name);
        if (len < 6 || strcmp(entry->d_name + len - 5, ".json") != 0) {
          continue;
        }
        path = turbo_agent_runtime_join_path(directory, entry->d_name);
        if (!path || turbo_agent_runtime_file_store_list_append(path, filter_key, filter_value,
                                                                records_json) != 0) {
          free(path);
          rc = -1;
          break;
        }
        free(path);
      }
      closedir(dir);
    }
  }
#endif

  free(directory);
  if (rc != 0) {
    turbo_free_json(&records_json);
    return -1;
  }
  {
    char *serialized = turbo_json_serialize(records_json, NULL);
    *out_records_json = serialized ? turbo_agent_runtime_strdup(serialized) : NULL;
    turbo_json_serialize_free(serialized);
  }
  turbo_free_json(&records_json);
  return *out_records_json ? 0 : -1;
}

CXX_C_API turbo_agent_runtime_store_t turbo_agent_runtime_store_memory_create(void) {
  turbo_agent_runtime_store_t store = {0};
  turbo_agent_runtime_memory_store_t *user_data;

  user_data = (turbo_agent_runtime_memory_store_t *)calloc(1, sizeof(*user_data));
  if (!user_data) {
    return store;
  }
  store.put = turbo_agent_runtime_memory_store_put;
  store.get = turbo_agent_runtime_memory_store_get;
  store.list = turbo_agent_runtime_memory_store_list;
  store.user_data = user_data;
  store.user_data_free = turbo_agent_runtime_memory_store_destroy;
  return store;
}

CXX_C_API turbo_agent_runtime_store_t turbo_agent_runtime_store_file_create(const char *root_dir) {
  turbo_agent_runtime_store_t store = {0};
  turbo_agent_runtime_file_store_t *user_data;
  char *threads_dir;
  char *runs_dir;
  char *checkpoints_dir;

  if (!root_dir) {
    return store;
  }
  user_data = (turbo_agent_runtime_file_store_t *)calloc(1, sizeof(*user_data));
  if (!user_data) {
    return store;
  }
  user_data->root_dir = turbo_agent_runtime_strdup(root_dir);
  if (!user_data->root_dir) {
    free(user_data);
    return store;
  }
  threads_dir = turbo_agent_runtime_join_path(root_dir, turbo_agent_runtime_threads_collection);
  runs_dir = turbo_agent_runtime_join_path(root_dir, turbo_agent_runtime_runs_collection);
  checkpoints_dir =
      turbo_agent_runtime_join_path(root_dir, turbo_agent_runtime_checkpoints_collection);
  if (turbo_agent_runtime_ensure_dir(root_dir) != 0 || !threads_dir || !runs_dir ||
      !checkpoints_dir || turbo_agent_runtime_ensure_dir(threads_dir) != 0 ||
      turbo_agent_runtime_ensure_dir(runs_dir) != 0 ||
      turbo_agent_runtime_ensure_dir(checkpoints_dir) != 0) {
    free(threads_dir);
    free(runs_dir);
    free(checkpoints_dir);
    turbo_agent_runtime_file_store_destroy(user_data);
    return store;
  }
  free(threads_dir);
  free(runs_dir);
  free(checkpoints_dir);
  store.put = turbo_agent_runtime_file_store_put;
  store.get = turbo_agent_runtime_file_store_get;
  store.list = turbo_agent_runtime_file_store_list;
  store.user_data = user_data;
  store.user_data_free = turbo_agent_runtime_file_store_destroy;
  return store;
}

CXX_C_API turbo_agent_runtime_t *
turbo_agent_runtime_create(const turbo_agent_runtime_store_t *store) {
  turbo_agent_runtime_t *runtime;

  if (!store || !store->put || !store->get || !store->list) {
    return NULL;
  }
  runtime = (turbo_agent_runtime_t *)calloc(1, sizeof(*runtime));
  if (!runtime) {
    return NULL;
  }
  runtime->store = *store;
  turbo_mutex_init(&runtime->store_mutex);
  if (!runtime->store_mutex) {
    free(runtime);
    return NULL;
  }
  return runtime;
}

CXX_C_API void turbo_agent_runtime_destroy(turbo_agent_runtime_t *runtime) {
  if (!runtime) {
    return;
  }
  if (runtime->store.user_data_free) {
    runtime->store.user_data_free(runtime->store.user_data);
  }
  turbo_mutex_destroy(&runtime->store_mutex);
  free(runtime);
}

static int turbo_agent_runtime_upsert_thread(turbo_agent_runtime_t *runtime, const char *thread_id,
                                             const char *timestamp) {
  json_value_t *record = NULL;

  if (!runtime || !thread_id || !timestamp) {
    return -1;
  }
  if (turbo_agent_runtime_store_get_json(runtime, turbo_agent_runtime_threads_collection, thread_id,
                                         &record) == 0 &&
      record) {
    turbo_json_object_set_string(record, "updated_at", timestamp);
    if (!turbo_json_get_string(record, "created_at")) {
      turbo_json_object_set_string(record, "created_at", timestamp);
    }
  } else {
    turbo_free_json(&record);
    if (turbo_agent_runtime_build_thread_record(thread_id, timestamp, &record) != 0) {
      return -1;
    }
  }
  if (turbo_agent_runtime_store_put_json(runtime, turbo_agent_runtime_threads_collection, thread_id,
                                         record) != 0) {
    turbo_free_json(&record);
    return -1;
  }
  turbo_free_json(&record);
  return 0;
}

static int turbo_agent_runtime_capture_snapshots(const json_value_t *state,
                                                 json_value_t **out_control_snapshot,
                                                 json_value_t **out_workflow_snapshot) {
  json_value_t *control_json_value;
  json_value_t *workflow_json_value;

  if (!state || !out_control_snapshot || !out_workflow_snapshot) {
    return -1;
  }
  *out_control_snapshot = NULL;
  *out_workflow_snapshot = NULL;

  control_json_value = turbo_agent_state_control_snapshot_json_value(state);
  workflow_json_value = turbo_agent_state_workflow_snapshot_json_value(state);
  if (!control_json_value || !workflow_json_value) {
    turbo_runtime_json_destroy(control_json_value);
    turbo_runtime_json_destroy(workflow_json_value);
    return -1;
  }

  *out_control_snapshot = turbo_json_clone(control_json_value);
  *out_workflow_snapshot = turbo_json_clone(workflow_json_value);
  turbo_runtime_json_destroy(control_json_value);
  turbo_runtime_json_destroy(workflow_json_value);
  if (!*out_control_snapshot || !*out_workflow_snapshot) {
    turbo_free_json(out_control_snapshot);
    turbo_free_json(out_workflow_snapshot);
    return -1;
  }
  return 0;
}

static int turbo_agent_runtime_record_checkpoint(
    turbo_agent_runtime_t *runtime, const char *thread_id, const char *run_id,
    const char *parent_checkpoint_id, const char *parent_agent_run_id,
    const char *parent_tool_call_id, const char *parent_tool_name, const char *parent_graph_run_id,
    const char *call_frame_id, size_t seq, const char *timestamp, const turbo_graph_run_log_t *log,
    const json_value_t *state, const char **out_checkpoint_id) {
  return turbo_agent_runtime_record_checkpoint_with_event_log(
      runtime, thread_id, run_id, parent_checkpoint_id, parent_agent_run_id, parent_tool_call_id,
      parent_tool_name, parent_graph_run_id, call_frame_id, seq, timestamp,
      log ? turbo_graph_run_log_checkpoint(log) : NULL,
      log ? turbo_graph_run_log_events(log) : NULL, state, out_checkpoint_id);
}

static int turbo_agent_runtime_record_checkpoint_with_event_log(
    turbo_agent_runtime_t *runtime, const char *thread_id, const char *run_id,
    const char *parent_checkpoint_id, const char *parent_agent_run_id,
    const char *parent_tool_call_id, const char *parent_tool_name, const char *parent_graph_run_id,
    const char *call_frame_id, size_t seq, const char *timestamp,
    const turbo_graph_checkpoint_t *checkpoint, const turbo_event_log_t *event_log,
    const json_value_t *state, const char **out_checkpoint_id) {
  json_value_t *control_snapshot = NULL;
  json_value_t *workflow_snapshot = NULL;
  json_value_t *events_json = NULL;
  json_value_t *record = NULL;
  char *checkpoint_id = NULL;
  int rc = -1;

  if (!runtime || !thread_id || !run_id || !timestamp || !event_log || !state ||
      !out_checkpoint_id) {
    return -1;
  }
  *out_checkpoint_id = NULL;
  if (!checkpoint) {
    return 0;
  }

  checkpoint_id = turbo_agent_runtime_make_id("ckpt");
  if (!checkpoint_id ||
      turbo_agent_runtime_capture_snapshots(state, &control_snapshot, &workflow_snapshot) != 0 ||
      turbo_agent_runtime_collect_event_log_events(event_log, &events_json) != 0 ||
      turbo_agent_runtime_build_checkpoint_record(
          checkpoint_id, thread_id, run_id, parent_checkpoint_id, parent_agent_run_id,
          parent_tool_call_id, parent_tool_name, parent_graph_run_id, call_frame_id, seq,
          "interrupted", timestamp, checkpoint, control_snapshot, workflow_snapshot, events_json,
          &record) != 0 ||
      turbo_agent_runtime_store_put_json(runtime, turbo_agent_runtime_checkpoints_collection,
                                         checkpoint_id, record) != 0) {
    goto cleanup;
  }

  *out_checkpoint_id = checkpoint_id;
  checkpoint_id = NULL;
  rc = 0;

cleanup:
  free(checkpoint_id);
  turbo_free_json(&control_snapshot);
  turbo_free_json(&workflow_snapshot);
  turbo_free_json(&events_json);
  turbo_free_json(&record);
  return rc;
}

static int turbo_agent_runtime_record_checkpoint_from_parts(
    turbo_agent_runtime_t *runtime, const char *thread_id, const char *run_id,
    const char *parent_checkpoint_id, const char *parent_agent_run_id,
    const char *parent_tool_call_id, const char *parent_tool_name, const char *parent_graph_run_id,
    const char *call_frame_id, size_t seq, const char *timestamp,
    const turbo_graph_checkpoint_t *checkpoint, const turbo_event_log_t *events_log,
    const json_value_t *state, const char **out_checkpoint_id) {
  json_value_t *control_snapshot = NULL;
  json_value_t *workflow_snapshot = NULL;
  json_value_t *events_json = NULL;
  json_value_t *record = NULL;
  char *checkpoint_id = NULL;
  int rc = -1;

  if (!runtime || !thread_id || !run_id || !timestamp || !checkpoint || !events_log || !state ||
      !out_checkpoint_id) {
    return -1;
  }
  *out_checkpoint_id = NULL;

  checkpoint_id = turbo_agent_runtime_make_id("ckpt");
  if (!checkpoint_id ||
      turbo_agent_runtime_capture_snapshots(state, &control_snapshot, &workflow_snapshot) != 0 ||
      turbo_agent_runtime_collect_event_log_events(events_log, &events_json) != 0 ||
      turbo_agent_runtime_build_checkpoint_record(
          checkpoint_id, thread_id, run_id, parent_checkpoint_id, parent_agent_run_id,
          parent_tool_call_id, parent_tool_name, parent_graph_run_id, call_frame_id, seq,
          "interrupted", timestamp, checkpoint, control_snapshot, workflow_snapshot, events_json,
          &record) != 0 ||
      turbo_agent_runtime_store_put_json(runtime, turbo_agent_runtime_checkpoints_collection,
                                         checkpoint_id, record) != 0) {
    goto cleanup;
  }

  *out_checkpoint_id = checkpoint_id;
  checkpoint_id = NULL;
  rc = 0;

cleanup:
  free(checkpoint_id);
  turbo_free_json(&control_snapshot);
  turbo_free_json(&workflow_snapshot);
  turbo_free_json(&events_json);
  turbo_free_json(&record);
  return rc;
}

static int turbo_agent_runtime_finalize_run(
    turbo_agent_runtime_t *runtime, const char *run_id, const char *thread_id,
    const char *parent_run_id, const char *parent_agent_run_id, const char *parent_tool_call_id,
    const char *parent_tool_name, const char *parent_graph_run_id, const char *call_frame_id,
    const char *forked_from_checkpoint_id, turbo_graph_t *graph, const char *created_at,
    const char *updated_at, const char *latest_checkpoint_id,
    const turbo_graph_run_result_t *result, const json_value_t *state,
    json_value_t **out_run_record) {
  char *graph_name = NULL;
  const char *topology_id;
  json_value_t *run_record = NULL;
  json_value_t *state_json = NULL;
  int rc = -1;

  if (!runtime || !run_id || !thread_id || !graph || !created_at || !updated_at || !result ||
      !state) {
    return -1;
  }

  topology_id = turbo_graph_topology_id(graph);
  graph_name = turbo_agent_runtime_graph_name(graph);
  if (turbo_agent_runtime_json_value_to_json(state, &state_json) != 0) {
    free(graph_name);
    return -1;
  }
  if (!topology_id || !graph_name ||
      turbo_agent_runtime_build_run_record(
          run_id, thread_id, parent_run_id, parent_agent_run_id, parent_tool_call_id,
          parent_tool_name, parent_graph_run_id, call_frame_id, forked_from_checkpoint_id,
          graph_name, topology_id, turbo_agent_runtime_status_text(result->status), created_at,
          updated_at, latest_checkpoint_id, result, state_json, &run_record) != 0 ||
      turbo_agent_runtime_store_put_json(runtime, turbo_agent_runtime_runs_collection, run_id,
                                         run_record) != 0) {
    goto cleanup;
  }
  if (out_run_record) {
    *out_run_record = run_record;
    run_record = NULL;
  }
  rc = 0;

cleanup:
  free(graph_name);
  turbo_free_json(&state_json);
  turbo_free_json(&run_record);
  return rc;
}

static int turbo_agent_runtime_run_segment(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *thread_id, const char *run_id,
    const char *parent_run_id, const char *parent_agent_run_id, const char *parent_tool_call_id,
    const char *parent_tool_name, const char *parent_graph_run_id, const char *call_frame_id,
    const char *forked_from_checkpoint_id, const char *parent_checkpoint_id,
    size_t next_checkpoint_seq, const char *run_created_at, const json_value_t *start_state,
    const turbo_graph_checkpoint_t *start_checkpoint, const turbo_graph_run_options_t *options,
    const turbo_cancel_token_t *cancel_token, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data, json_value_t **out_summary_json, json_value_t **out_state) {
  turbo_agent_execution_context_t saved_context = {0};
  turbo_agent_execution_context_t current_context = {0};
  turbo_event_log_t *stream_events = NULL;
  turbo_graph_checkpoint_t *stream_checkpoint = NULL;
  turbo_graph_run_result_t result = {0};
  json_value_t *result_state = NULL;
  json_value_t *summary_json = NULL;
  json_value_t *summary_state_json = NULL;
  const char *persisted_checkpoint_id = NULL;
  char created_at[32];
  char updated_at[32];
  const char *checkpoint_id = NULL;
  turbo_graph_exec_status_t status;
  int rc = -1;

  if (!runtime || !graph || !thread_id || !run_id || !out_summary_json || !out_state) {
    return -1;
  }
  *out_summary_json = NULL;
  *out_state = NULL;
  if (run_created_at) {
    snprintf(created_at, sizeof(created_at), "%s", run_created_at);
  } else if (turbo_agent_runtime_make_timestamp(created_at, sizeof(created_at)) != 0) {
    return -1;
  }
  if (turbo_agent_runtime_make_timestamp(updated_at, sizeof(updated_at)) != 0 ||
      turbo_agent_runtime_upsert_thread(runtime, thread_id, updated_at) != 0) {
    return -1;
  }
  turbo_agent_execution_context_get(&saved_context);
  current_context = saved_context;
  current_context.thread_id = thread_id;
  current_context.run_id = run_id;
  current_context.runtime = runtime;
  current_context.cancel_token = cancel_token;
  turbo_agent_execution_context_set(&current_context);

  stream_events = turbo_event_log_create();
  if (!stream_events) {
    turbo_agent_execution_context_set(&saved_context);
    return -1;
  }

  status = turbo_agent_runtime_run_graph_stream(
      graph, start_state, start_checkpoint, options, cancel_token, stream_events, event_sink,
      event_sink_user_data, &result, &result_state, &stream_checkpoint);
  if (!result_state) {
    turbo_agent_execution_context_set(&saved_context);
    turbo_graph_checkpoint_destroy(stream_checkpoint);
    turbo_event_log_destroy(stream_events);
    return -1;
  }
  if (status != TURBO_GRAPH_EXEC_OK && status != TURBO_GRAPH_EXEC_STOP &&
      status != TURBO_GRAPH_EXEC_INTERRUPTED && status != TURBO_GRAPH_EXEC_CANCELLED &&
      status != TURBO_GRAPH_EXEC_DEADLINE) {
    if (status == TURBO_GRAPH_EXEC_ERROR) {
      *out_state = result_state;
      result_state = NULL;
    }
    goto cleanup;
  }

  if (status == TURBO_GRAPH_EXEC_INTERRUPTED || status == TURBO_GRAPH_EXEC_CANCELLED ||
      status == TURBO_GRAPH_EXEC_DEADLINE) {
    if (turbo_agent_runtime_record_checkpoint_from_parts(
            runtime, thread_id, run_id, parent_checkpoint_id, parent_agent_run_id,
            parent_tool_call_id, parent_tool_name, parent_graph_run_id, call_frame_id,
            next_checkpoint_seq, updated_at, stream_checkpoint, stream_events, result_state,
            &checkpoint_id) != 0) {
      goto cleanup;
    }
  }
  persisted_checkpoint_id = checkpoint_id ? checkpoint_id : parent_checkpoint_id;
  summary_state_json = turbo_json_clone(result_state);

  if (turbo_agent_runtime_finalize_run(
          runtime, run_id, thread_id, parent_run_id, parent_agent_run_id, parent_tool_call_id,
          parent_tool_name, parent_graph_run_id, call_frame_id, forked_from_checkpoint_id, graph,
          created_at, updated_at, persisted_checkpoint_id, &result, result_state, NULL) != 0 ||
      turbo_agent_runtime_make_summary(thread_id, run_id, turbo_agent_runtime_status_text(status),
                                       checkpoint_id, parent_agent_run_id, parent_tool_call_id,
                                       parent_tool_name, parent_graph_run_id, call_frame_id,
                                       &result, summary_state_json, &summary_json) != 0) {
    goto cleanup;
  }

  *out_summary_json = summary_json;
  *out_state = result_state;
  summary_json = NULL;
  result_state = NULL;
  rc = 0;

cleanup:
  turbo_agent_execution_context_set(&saved_context);
  free((char *)checkpoint_id);
  turbo_free_json(&summary_state_json);
  turbo_free_json(&summary_json);
  turbo_runtime_json_destroy(result_state);
  turbo_graph_checkpoint_destroy(stream_checkpoint);
  turbo_event_log_destroy(stream_events);
  return rc;
}

static int turbo_agent_runtime_checkpoint_seq_compare(const void *left, const void *right) {
  const json_value_t *const *a = (const json_value_t *const *)left;
  const json_value_t *const *b = (const json_value_t *const *)right;
  const json_value_t *a_seq = turbo_json_object_get(*a, "seq");
  const json_value_t *b_seq = turbo_json_object_get(*b, "seq");
  double av =
      (a_seq && turbo_json_type(a_seq) == TURBO_JSON_NUMBER) ? turbo_json_number(a_seq) : 0.0;
  double bv =
      (b_seq && turbo_json_type(b_seq) == TURBO_JSON_NUMBER) ? turbo_json_number(b_seq) : 0.0;
  return av < bv ? -1 : av > bv ? 1 : 0;
}

int turbo_agent_runtime_record_string_compare(const char *left, const char *right) {
  if (left && right) {
    return strcmp(left, right);
  }
  if (left) {
    return 1;
  }
  if (right) {
    return -1;
  }
  return 0;
}

static int turbo_agent_runtime_observability_summary_thread_id_compare_asc(const void *left,
                                                                           const void *right) {
  const json_value_t *a = *(const json_value_t *const *)left;
  const json_value_t *b = *(const json_value_t *const *)right;
  const json_value_t *a_thread_json = turbo_json_object_get(a, "thread");
  const json_value_t *b_thread_json = turbo_json_object_get(b, "thread");
  const char *a_thread_id = (a_thread_json && turbo_json_type(a_thread_json) == TURBO_JSON_OBJECT)
                                ? turbo_json_get_string(a_thread_json, "id")
                                : NULL;
  const char *b_thread_id = (b_thread_json && turbo_json_type(b_thread_json) == TURBO_JSON_OBJECT)
                                ? turbo_json_get_string(b_thread_json, "id")
                                : NULL;

  return turbo_agent_runtime_record_string_compare(a_thread_id, b_thread_id);
}

static int turbo_agent_runtime_observability_summary_thread_id_compare_desc(const void *left,
                                                                            const void *right) {
  return turbo_agent_runtime_observability_summary_thread_id_compare_asc(right, left);
}

static int
turbo_agent_runtime_observability_summary_latest_run_updated_at_compare_asc(const void *left,
                                                                            const void *right) {
  const json_value_t *a = *(const json_value_t *const *)left;
  const json_value_t *b = *(const json_value_t *const *)right;
  const char *a_updated_at = turbo_json_get_string(a, "latest_run_updated_at");
  const char *b_updated_at = turbo_json_get_string(b, "latest_run_updated_at");
  int rc = turbo_agent_runtime_record_string_compare(a_updated_at, b_updated_at);

  if (rc != 0) {
    return rc;
  }
  return turbo_agent_runtime_observability_summary_thread_id_compare_asc(left, right);
}

static int
turbo_agent_runtime_observability_summary_latest_run_updated_at_compare_desc(const void *left,
                                                                             const void *right) {
  return turbo_agent_runtime_observability_summary_latest_run_updated_at_compare_asc(right, left);
}

int turbo_agent_runtime_run_updated_at_compare_desc(const void *left, const void *right) {
  const json_value_t *a = *(const json_value_t *const *)left;
  const json_value_t *b = *(const json_value_t *const *)right;
  const char *a_updated_at = turbo_json_get_string(a, "updated_at");
  const char *b_updated_at = turbo_json_get_string(b, "updated_at");
  int rc = turbo_agent_runtime_record_string_compare(b_updated_at, a_updated_at);

  if (rc != 0) {
    return rc;
  }
  return turbo_agent_runtime_record_string_compare(turbo_json_get_string(a, "id"),
                                                   turbo_json_get_string(b, "id"));
}

json_value_t *turbo_agent_runtime_sorted_json_array_clone(const json_value_t *records_json,
                                                          int (*compare)(const void *,
                                                                         const void *)) {
  json_value_t *sorted_json = NULL;
  const json_value_t **items = NULL;
  size_t count;
  size_t i;

  if (!records_json || turbo_json_type(records_json) != TURBO_JSON_ARRAY || !compare) {
    return NULL;
  }
  sorted_json = turbo_json_create_array();
  if (!sorted_json) {
    return NULL;
  }
  count = turbo_json_array_size(records_json);
  if (count == 0) {
    return sorted_json;
  }
  items = (const json_value_t **)calloc(count, sizeof(*items));
  if (!items) {
    turbo_free_json(&sorted_json);
    return NULL;
  }
  for (i = 0; i < count; ++i) {
    items[i] = turbo_json_array_get(records_json, i);
  }
  qsort(items, count, sizeof(*items), compare);
  for (i = 0; i < count; ++i) {
    json_value_t *clone = turbo_json_clone(items[i]);
    if (!clone) {
      free(items);
      turbo_free_json(&sorted_json);
      return NULL;
    }
    turbo_json_array_add(sorted_json, clone);
  }
  free(items);
  return sorted_json;
}

static int turbo_agent_runtime_json_value_from_json_value(const json_value_t *value,
                                                          json_value_t **out_value) {
  if (!value || !out_value) {
    return -1;
  }
  *out_value = turbo_json_clone(value);
  return *out_value ? 0 : -1;
}

static int turbo_agent_runtime_state_from_checkpoint_record(const json_value_t *checkpoint_record,
                                                            json_value_t **out_state) {
  const json_value_t *checkpoint_json;
  const json_value_t *state_json;

  if (!checkpoint_record || !out_state) {
    return -1;
  }
  checkpoint_json = turbo_json_object_get(checkpoint_record, "checkpoint_json");
  if (!checkpoint_json || turbo_json_type(checkpoint_json) != TURBO_JSON_OBJECT) {
    return -1;
  }
  state_json = turbo_json_object_get(checkpoint_json, "state");
  if (!state_json) {
    return -1;
  }
  return turbo_agent_runtime_json_value_from_json_value(state_json, out_state);
}

static int turbo_agent_runtime_state_from_run_record(turbo_agent_runtime_t *runtime,
                                                     const json_value_t *run_record,
                                                     json_value_t **out_state) {
  const json_value_t *state_json;
  const char *latest_checkpoint_id;
  json_value_t *checkpoint_record = NULL;
  int rc;

  if (!runtime || !run_record || !out_state) {
    return -1;
  }
  state_json = turbo_json_object_get(run_record, "state_snapshot");
  if (state_json && turbo_json_type(state_json) != TURBO_JSON_NULL) {
    return turbo_agent_runtime_json_value_from_json_value(state_json, out_state);
  }
  latest_checkpoint_id = turbo_json_get_string(run_record, "latest_checkpoint_id");
  if (!latest_checkpoint_id || latest_checkpoint_id[0] == '\0') {
    return -1;
  }
  rc = turbo_agent_runtime_store_get_json(runtime, turbo_agent_runtime_checkpoints_collection,
                                          latest_checkpoint_id, &checkpoint_record);
  if (rc != 0) {
    return -1;
  }
  rc = turbo_agent_runtime_state_from_checkpoint_record(checkpoint_record, out_state);
  turbo_free_json(&checkpoint_record);
  return rc;
}

static json_value_t *
turbo_agent_runtime_checkpoint_summary_from_record(const json_value_t *checkpoint_record) {
  json_value_t *summary;
  const char *string_fields[] = {"id",     "run_id",     "parent_checkpoint_id",
                                 "status", "created_at", "next_node"};
  const char *number_fields[] = {"seq", "steps"};
  size_t i;

  if (!checkpoint_record || turbo_json_type(checkpoint_record) != TURBO_JSON_OBJECT) {
    return NULL;
  }
  summary = turbo_json_create_object();
  if (!summary) {
    return NULL;
  }

  for (i = 0; i < sizeof(string_fields) / sizeof(string_fields[0]); ++i) {
    const char *value = turbo_json_get_string(checkpoint_record, string_fields[i]);

    if (value && value[0] != '\0') {
      turbo_json_object_set_string(summary, string_fields[i], value);
    } else {
      turbo_json_object_set_null(summary, string_fields[i]);
    }
  }
  for (i = 0; i < sizeof(number_fields) / sizeof(number_fields[0]); ++i) {
    const json_value_t *value = turbo_json_object_get(checkpoint_record, number_fields[i]);

    if (value && turbo_json_type(value) == TURBO_JSON_NUMBER) {
      turbo_json_object_set_number(summary, number_fields[i],
                                   turbo_json_get_double(checkpoint_record, number_fields[i], 0));
    } else {
      turbo_json_object_set_null(summary, number_fields[i]);
    }
  }

  return summary;
}

static int turbo_agent_runtime_load_checkpoint_summary_by_id(turbo_agent_runtime_t *runtime,
                                                             const char *checkpoint_id,
                                                             json_value_t **out_summary) {
  json_value_t *checkpoint_json = NULL;
  json_value_t *summary = NULL;

  /* Host-facing inspect views reuse one stable lightweight checkpoint summary. */
  if (!runtime || !out_summary) {
    return -1;
  }
  *out_summary = NULL;
  if (!checkpoint_id || checkpoint_id[0] == '\0') {
    return 0;
  }
  if (turbo_agent_runtime_get_checkpoint(runtime, checkpoint_id, &checkpoint_json) != 0) {
    goto cleanup;
  }
  summary = turbo_agent_runtime_checkpoint_summary_from_record(checkpoint_json);
  if (!summary) {
    goto cleanup;
  }
  *out_summary = summary;
  summary = NULL;

cleanup:
  turbo_free_json(&summary);
  turbo_free_json(&checkpoint_json);
  return *out_summary || !checkpoint_id || checkpoint_id[0] == '\0' ? 0 : -1;
}

static int turbo_agent_runtime_attach_checkpoint_summary(turbo_agent_runtime_t *runtime,
                                                         json_value_t *object,
                                                         const char *field_name,
                                                         const char *checkpoint_id) {
  json_value_t *summary = NULL;

  if (!runtime || !object || !field_name) {
    return -1;
  }
  if (!checkpoint_id || checkpoint_id[0] == '\0') {
    turbo_json_object_set_null(object, field_name);
    return 0;
  }
  if (turbo_agent_runtime_load_checkpoint_summary_by_id(runtime, checkpoint_id, &summary) != 0) {
    return -1;
  }
  if (summary) {
    turbo_json_object_add(object, field_name, summary);
  } else {
    turbo_json_object_set_null(object, field_name);
  }
  return 0;
}

static int turbo_agent_runtime_select_run_id_for_thread(turbo_agent_runtime_t *runtime,
                                                        const char *thread_id,
                                                        const char *status_filter,
                                                        char **out_run_id) {
  json_value_t *runs = NULL;
  const char *best_run_id = NULL;
  const char *best_updated_at = NULL;
  char *owned_run_id = NULL;
  size_t i;
  size_t count;
  int rc = -1;

  if (!runtime || !thread_id || !out_run_id) {
    return -1;
  }
  *out_run_id = NULL;
  if (turbo_agent_runtime_store_list_json(runtime, turbo_agent_runtime_runs_collection, "thread_id",
                                          thread_id, &runs) != 0 ||
      !runs || turbo_json_type(runs) != TURBO_JSON_ARRAY) {
    goto cleanup;
  }
  count = turbo_json_array_size(runs);
  for (i = 0; i < count; ++i) {
    const json_value_t *record = turbo_json_array_get(runs, i);
    const char *status;
    const char *updated_at;
    const char *run_id;

    if (!record || turbo_json_type(record) != TURBO_JSON_OBJECT) {
      continue;
    }
    status = turbo_json_get_string(record, "status");
    run_id = turbo_json_get_string(record, "id");
    updated_at = turbo_json_get_string(record, "updated_at");
    if (status_filter && (!status || strcmp(status, status_filter) != 0)) {
      continue;
    }
    if (!run_id || run_id[0] == '\0') {
      continue;
    }
    if (!best_run_id ||
        turbo_agent_runtime_record_string_compare(updated_at, best_updated_at) > 0) {
      best_run_id = run_id;
      best_updated_at = updated_at;
    }
  }
  if (!best_run_id) {
    goto cleanup;
  }
  owned_run_id = turbo_agent_runtime_strdup(best_run_id);
  if (!owned_run_id) {
    goto cleanup;
  }
  *out_run_id = owned_run_id;
  owned_run_id = NULL;
  rc = 0;

cleanup:
  free(owned_run_id);
  turbo_free_json(&runs);
  return rc;
}

int turbo_agent_runtime_latest_run_id_for_thread(turbo_agent_runtime_t *runtime,
                                                 const char *thread_id, char **out_run_id) {
  return turbo_agent_runtime_select_run_id_for_thread(runtime, thread_id, NULL, out_run_id);
}

static int turbo_agent_runtime_pending_run_id_for_thread(turbo_agent_runtime_t *runtime,
                                                         const char *thread_id, char **out_run_id) {
  return turbo_agent_runtime_select_run_id_for_thread(runtime, thread_id, "interrupted",
                                                      out_run_id);
}

static int turbo_agent_runtime_latest_checkpoint_id_for_run(turbo_agent_runtime_t *runtime,
                                                            const char *run_id,
                                                            char **out_checkpoint_id) {
  json_value_t *run_record = NULL;
  const char *checkpoint_id;
  char *owned_checkpoint_id = NULL;
  int rc = -1;

  if (!runtime || !run_id || !out_checkpoint_id) {
    return -1;
  }
  *out_checkpoint_id = NULL;
  if (turbo_agent_runtime_store_get_json(runtime, turbo_agent_runtime_runs_collection, run_id,
                                         &run_record) != 0 ||
      !run_record) {
    goto cleanup;
  }
  checkpoint_id = turbo_json_get_string(run_record, "latest_checkpoint_id");
  if (!checkpoint_id || checkpoint_id[0] == '\0') {
    goto cleanup;
  }
  owned_checkpoint_id = turbo_agent_runtime_strdup(checkpoint_id);
  if (!owned_checkpoint_id) {
    goto cleanup;
  }
  *out_checkpoint_id = owned_checkpoint_id;
  owned_checkpoint_id = NULL;
  rc = 0;

cleanup:
  free(owned_checkpoint_id);
  turbo_free_json(&run_record);
  return rc;
}

static int turbo_agent_runtime_current_run_id_for_thread(turbo_agent_runtime_t *runtime,
                                                         const char *thread_id, char **out_run_id) {
  if (turbo_agent_runtime_pending_run_id_for_thread(runtime, thread_id, out_run_id) == 0 &&
      out_run_id && *out_run_id) {
    return 0;
  }
  free(out_run_id ? *out_run_id : NULL);
  if (out_run_id) {
    *out_run_id = NULL;
  }
  return turbo_agent_runtime_latest_run_id_for_thread(runtime, thread_id, out_run_id);
}

int turbo_agent_runtime_current_checkpoint_id_for_thread(turbo_agent_runtime_t *runtime,
                                                         const char *thread_id,
                                                         char **out_checkpoint_id) {
  char *run_id = NULL;
  int rc;

  if (!runtime || !thread_id || !out_checkpoint_id) {
    return -1;
  }
  *out_checkpoint_id = NULL;
  rc = turbo_agent_runtime_current_run_id_for_thread(runtime, thread_id, &run_id);
  if (rc != 0 || !run_id) {
    free(run_id);
    return -1;
  }
  rc = turbo_agent_runtime_latest_checkpoint_id_for_run(runtime, run_id, out_checkpoint_id);
  free(run_id);
  return rc;
}

static int turbo_agent_runtime_thread_head_run_id_for_thread(turbo_agent_runtime_t *runtime,
                                                             const char *thread_id,
                                                             char **out_run_id) {
  return turbo_agent_runtime_current_run_id_for_thread(runtime, thread_id, out_run_id);
}

static int turbo_agent_runtime_root_checkpoint_id_for_checkpoint(turbo_agent_runtime_t *runtime,
                                                                 const char *checkpoint_id,
                                                                 char **out_root_checkpoint_id) {
  char *current_id = NULL;
  int rc = -1;

  if (!runtime || !checkpoint_id || !out_root_checkpoint_id) {
    return -1;
  }
  *out_root_checkpoint_id = NULL;
  current_id = turbo_agent_runtime_strdup(checkpoint_id);
  if (!current_id) {
    return -1;
  }

  while (current_id) {
    json_value_t *checkpoint_record = NULL;
    const char *parent_checkpoint_id = NULL;
    char *next_id = NULL;

    if (turbo_agent_runtime_get_checkpoint(runtime, current_id, &checkpoint_record) != 0 ||
        !checkpoint_record) {
      turbo_free_json(&checkpoint_record);
      goto cleanup;
    }
    parent_checkpoint_id = turbo_json_get_string(checkpoint_record, "parent_checkpoint_id");
    if (!parent_checkpoint_id || parent_checkpoint_id[0] == '\0') {
      *out_root_checkpoint_id = current_id;
      current_id = NULL;
      turbo_free_json(&checkpoint_record);
      rc = 0;
      break;
    }
    next_id = turbo_agent_runtime_strdup(parent_checkpoint_id);
    turbo_free_json(&checkpoint_record);
    if (!next_id) {
      goto cleanup;
    }
    free(current_id);
    current_id = next_id;
  }

cleanup:
  free(current_id);
  return rc;
}

static int turbo_agent_runtime_branch_from_run_record(turbo_agent_runtime_t *runtime,
                                                      const json_value_t *run_record,
                                                      json_value_t **out_branch_json,
                                                      char **out_root_checkpoint_id) {
  json_value_t *branch_json = NULL;
  json_value_t *checkpoint_record = NULL;
  const char *run_id;
  const char *checkpoint_id;
  const char *parent_run_id;
  const char *forked_from_checkpoint_id;
  const char *parent_checkpoint_id = NULL;
  const char *status;
  const char *updated_at;
  const char *created_at;
  char *root_checkpoint_id = NULL;
  const char *root_source_checkpoint_id = NULL;

  if (!runtime || !run_record || !out_branch_json || !out_root_checkpoint_id) {
    return -1;
  }
  *out_branch_json = NULL;
  *out_root_checkpoint_id = NULL;

  run_id = turbo_json_get_string(run_record, "id");
  if (!run_id || run_id[0] == '\0') {
    return -1;
  }
  checkpoint_id = turbo_json_get_string(run_record, "latest_checkpoint_id");
  parent_run_id = turbo_json_get_string(run_record, "parent_run_id");
  forked_from_checkpoint_id = turbo_json_get_string(run_record, "forked_from_checkpoint_id");
  status = turbo_json_get_string(run_record, "status");
  updated_at = turbo_json_get_string(run_record, "updated_at");
  created_at = turbo_json_get_string(run_record, "created_at");

  if (checkpoint_id && checkpoint_id[0] != '\0') {
    if (turbo_agent_runtime_get_checkpoint(runtime, checkpoint_id, &checkpoint_record) != 0 ||
        !checkpoint_record) {
      goto cleanup;
    }
    root_source_checkpoint_id = checkpoint_id;
  } else if (forked_from_checkpoint_id && forked_from_checkpoint_id[0] != '\0') {
    root_source_checkpoint_id = forked_from_checkpoint_id;
  }

  if (forked_from_checkpoint_id && forked_from_checkpoint_id[0] != '\0') {
    parent_checkpoint_id = forked_from_checkpoint_id;
  }

  if (root_source_checkpoint_id &&
      turbo_agent_runtime_root_checkpoint_id_for_checkpoint(runtime, root_source_checkpoint_id,
                                                            &root_checkpoint_id) != 0) {
    goto cleanup;
  }

  branch_json = turbo_json_create_object();
  if (!branch_json) {
    goto cleanup;
  }
  turbo_json_object_set_string(branch_json, "run_id", run_id);
  if (checkpoint_id && checkpoint_id[0] != '\0') {
    turbo_json_object_set_string(branch_json, "checkpoint_id", checkpoint_id);
  } else {
    turbo_json_object_set_null(branch_json, "checkpoint_id");
  }
  if (parent_checkpoint_id && parent_checkpoint_id[0] != '\0') {
    turbo_json_object_set_string(branch_json, "parent_checkpoint_id", parent_checkpoint_id);
  } else {
    turbo_json_object_set_null(branch_json, "parent_checkpoint_id");
  }
  if (parent_run_id && parent_run_id[0] != '\0') {
    turbo_json_object_set_string(branch_json, "parent_run_id", parent_run_id);
  } else {
    turbo_json_object_set_null(branch_json, "parent_run_id");
  }
  if (forked_from_checkpoint_id && forked_from_checkpoint_id[0] != '\0') {
    turbo_json_object_set_string(branch_json, "forked_from_checkpoint_id",
                                 forked_from_checkpoint_id);
  } else {
    turbo_json_object_set_null(branch_json, "forked_from_checkpoint_id");
  }
  if (status && status[0] != '\0') {
    turbo_json_object_set_string(branch_json, "status", status);
  } else {
    turbo_json_object_set_null(branch_json, "status");
  }
  if (updated_at && updated_at[0] != '\0') {
    turbo_json_object_set_string(branch_json, "updated_at", updated_at);
  } else {
    turbo_json_object_set_null(branch_json, "updated_at");
  }
  if (root_checkpoint_id && root_checkpoint_id[0] != '\0') {
    turbo_json_object_set_string(branch_json, "branch_root_checkpoint_id", root_checkpoint_id);
  } else {
    turbo_json_object_set_null(branch_json, "branch_root_checkpoint_id");
  }
  if (created_at && created_at[0] != '\0') {
    turbo_json_object_set_string(branch_json, "created_at", created_at);
  } else {
    turbo_json_object_set_null(branch_json, "created_at");
  }

  *out_branch_json = branch_json;
  branch_json = NULL;
  *out_root_checkpoint_id = root_checkpoint_id;
  root_checkpoint_id = NULL;

cleanup:
  free(root_checkpoint_id);
  turbo_free_json(&checkpoint_record);
  turbo_free_json(&branch_json);
  return *out_branch_json ? 0 : -1;
}

static int turbo_agent_runtime_history_events_json_for_run(turbo_agent_runtime_t *runtime,
                                                           const char *run_id,
                                                           json_value_t **out_events_json) {
  json_value_t *events_json_value = NULL;
  json_value_t *events_json = NULL;

  if (!runtime || !run_id || !out_events_json) {
    return -1;
  }
  *out_events_json = NULL;
  if (turbo_agent_runtime_load_history_events_json_value(runtime, run_id, NULL,
                                                         &events_json_value) != 0 ||
      !events_json_value) {
    return -1;
  }
  events_json = turbo_json_clone(events_json_value);
  turbo_runtime_json_destroy(events_json_value);
  if (!events_json) {
    return -1;
  }
  *out_events_json = events_json;
  return 0;
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

static int
turbo_agent_runtime_merge_state_patch_object_json_value(json_value_t *target_object,
                                                        const json_value_t *patch_object) {
  size_t i;
  size_t count;

  if (!target_object || !patch_object || turbo_json_type(target_object) != TURBO_JSON_OBJECT ||
      turbo_json_type(patch_object) != TURBO_JSON_OBJECT) {
    return -1;
  }

  count = turbo_runtime_json_value_size(patch_object);
  for (i = 0; i < count; ++i) {
    const char *key = turbo_json_object_key(patch_object, i);
    const json_value_t *patch_value;
    const json_value_t *target_value;
    json_value_t *copy;

    if (!key) {
      return -1;
    }
    patch_value = turbo_json_object_get(patch_object, key);
    if (!patch_value) {
      return -1;
    }
    target_value = turbo_json_object_get(target_object, key);
    if (target_value && turbo_json_type(patch_value) == TURBO_JSON_OBJECT &&
        turbo_json_type(target_value) == TURBO_JSON_OBJECT) {
      if (turbo_agent_runtime_merge_state_patch_object_json_value((json_value_t *)target_value,
                                                                  patch_value) != 0) {
        return -1;
      }
      continue;
    }
    copy = turbo_json_clone(patch_value);
    if (!copy) {
      return -1;
    }
    if (turbo_runtime_json_object_set(target_object, key, copy) != TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_json_destroy(copy);
      return -1;
    }
  }
  return 0;
}

static int turbo_agent_runtime_apply_state_patch_to_json_value(const json_value_t *state,
                                                               const json_value_t *patch,
                                                               json_value_t **out_state_override) {
  json_value_t *updated_state = NULL;
  turbo_json_type_t state_kind;
  turbo_json_type_t patch_kind;
  int rc = -1;

  if (!state || !patch || !out_state_override) {
    return -1;
  }
  *out_state_override = NULL;

  state_kind = turbo_json_type(state);
  patch_kind = turbo_json_type(patch);
  if (state_kind == TURBO_JSON_OBJECT && patch_kind == TURBO_JSON_OBJECT) {
    updated_state = turbo_json_clone(state);
    if (!updated_state) {
      return -1;
    }
    if (turbo_agent_runtime_merge_state_patch_object_json_value(updated_state, patch) != 0) {
      goto cleanup;
    }
  } else {
    updated_state = turbo_json_clone(patch);
    if (!updated_state) {
      goto cleanup;
    }
  }

  *out_state_override = updated_state;
  updated_state = NULL;
  rc = 0;

cleanup:
  turbo_runtime_json_destroy(updated_state);
  return rc;
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

static int turbo_agent_runtime_run_command_json_value(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state, int fork_run) {
  json_value_t *state_override = NULL;
  int rc;

  if (!runtime || !graph || !checkpoint_id || !command || !out_summary_json || !out_state) {
    return -1;
  }
  rc = turbo_agent_runtime_prepare_checkpoint_command_override_json_value(runtime, checkpoint_id,
                                                                          command, &state_override);
  if (rc != 0) {
    return rc;
  }
  if (fork_run) {
    rc = turbo_agent_runtime_fork_json_value_graph_stream(runtime, graph, checkpoint_id,
                                                          state_override, options, NULL, NULL,
                                                          out_summary_json, out_state);
  } else {
    rc = turbo_agent_runtime_resume_json_value_graph_stream(runtime, graph, checkpoint_id,
                                                            state_override, options, NULL, NULL,
                                                            out_summary_json, out_state);
  }
  turbo_runtime_json_destroy(state_override);
  return rc;
}

int turbo_agent_runtime_start_json_value_graph_linked_stream(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const json_value_t *state,
    const turbo_graph_run_options_t *options, const char *thread_id,
    const turbo_agent_runtime_parent_link_t *parent_link, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data, json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_runtime_start_json_value_graph_linked_stream_controlled(
      runtime, graph, state, options, thread_id, parent_link, NULL, event_sink,
      event_sink_user_data, out_summary_json, out_state);
}

int turbo_agent_runtime_start_json_value_graph_linked_stream_controlled(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const json_value_t *state,
    const turbo_graph_run_options_t *options, const char *thread_id,
    const turbo_agent_runtime_parent_link_t *parent_link, const turbo_cancel_token_t *cancel_token,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  char *owned_thread_id = NULL;
  char *run_id = NULL;
  int rc;

  if (!runtime || !graph || !state || !out_summary_json || !out_state) {
    return -1;
  }
  if (thread_id && thread_id[0] != '\0') {
    owned_thread_id = turbo_agent_runtime_strdup(thread_id);
  } else {
    owned_thread_id = turbo_agent_runtime_make_id("thr");
  }
  run_id = turbo_agent_runtime_make_id("run");
  if (!owned_thread_id || !run_id) {
    free(owned_thread_id);
    free(run_id);
    return -1;
  }
  rc = turbo_agent_runtime_run_segment(runtime, graph, owned_thread_id, run_id, NULL,
                                       parent_link ? parent_link->parent_agent_run_id : NULL,
                                       parent_link ? parent_link->parent_tool_call_id : NULL,
                                       parent_link ? parent_link->parent_tool_name : NULL,
                                       parent_link ? parent_link->parent_graph_run_id : NULL,
                                       parent_link ? parent_link->call_frame_id : NULL, NULL, NULL,
                                       1, NULL, state, NULL, options, cancel_token, event_sink,
                                       event_sink_user_data, out_summary_json, out_state);
  free(owned_thread_id);
  free(run_id);
  return rc;
}

int turbo_agent_runtime_resume_json_value_graph_stream(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *state_override, const turbo_graph_run_options_t *options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_runtime_resume_json_value_graph_stream_controlled(
      runtime, graph, checkpoint_id, state_override, options, NULL, event_sink,
      event_sink_user_data, out_summary_json, out_state);
}

int turbo_agent_runtime_resume_json_value_graph_stream_controlled(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *state_override, const turbo_graph_run_options_t *options,
    const turbo_cancel_token_t *cancel_token, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data, json_value_t **out_summary_json, json_value_t **out_state) {
  json_value_t *checkpoint_record = NULL;
  json_value_t *run_record = NULL;
  const char *thread_id;
  const char *run_id;
  const char *parent_checkpoint_id;
  const char *created_at;
  size_t seq = 0;
  json_value_t *checkpoint_json;
  turbo_graph_checkpoint_t *checkpoint = NULL;
  int rc = -1;

  if (!runtime || !graph || !checkpoint_id || !out_summary_json || !out_state) {
    return -1;
  }
  if (turbo_agent_runtime_store_get_json(runtime, turbo_agent_runtime_checkpoints_collection,
                                         checkpoint_id, &checkpoint_record) != 0 ||
      turbo_agent_runtime_extract_checkpoint_record(checkpoint_record, &thread_id, &run_id,
                                                    &parent_checkpoint_id, &seq,
                                                    &checkpoint_json) != 0 ||
      turbo_agent_runtime_store_get_json(runtime, turbo_agent_runtime_runs_collection, run_id,
                                         &run_record) != 0) {
    goto cleanup;
  }
  created_at = turbo_json_get_string(run_record, "created_at");
  if (!created_at ||
      turbo_agent_runtime_checkpoint_from_json(checkpoint_json, state_override, &checkpoint) != 0) {
    goto cleanup;
  }
  rc = turbo_agent_runtime_run_segment(
      runtime, graph, thread_id, run_id, turbo_json_get_string(run_record, "parent_run_id"),
      turbo_json_get_string(run_record, "parent_agent_run_id"),
      turbo_json_get_string(run_record, "parent_tool_call_id"),
      turbo_json_get_string(run_record, "parent_tool_name"),
      turbo_json_get_string(run_record, "parent_graph_run_id"),
      turbo_json_get_string(run_record, "call_frame_id"),
      turbo_json_get_string(run_record, "forked_from_checkpoint_id"), checkpoint_id, seq + 1,
      created_at, NULL, checkpoint, options, cancel_token, event_sink, event_sink_user_data,
      out_summary_json, out_state);

cleanup:
  turbo_graph_checkpoint_destroy(checkpoint);
  turbo_free_json(&checkpoint_record);
  turbo_free_json(&run_record);
  return rc;
}

CXX_C_API int turbo_agent_runtime_fork_json_value_graph(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *state_override, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_runtime_fork_json_value_graph_stream(runtime, graph, checkpoint_id,
                                                          state_override, options, NULL, NULL,
                                                          out_summary_json, out_state);
}

int turbo_agent_runtime_fork_json_value_graph_stream(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *state_override, const turbo_graph_run_options_t *options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_runtime_fork_json_value_graph_stream_controlled(
      runtime, graph, checkpoint_id, state_override, options, NULL, event_sink,
      event_sink_user_data, out_summary_json, out_state);
}

int turbo_agent_runtime_fork_json_value_graph_stream_controlled(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *state_override, const turbo_graph_run_options_t *options,
    const turbo_cancel_token_t *cancel_token, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data, json_value_t **out_summary_json, json_value_t **out_state) {
  json_value_t *checkpoint_record = NULL;
  json_value_t *source_run_record = NULL;
  const char *thread_id;
  const char *source_run_id;
  const char *parent_checkpoint_id;
  size_t seq = 0;
  json_value_t *checkpoint_json;
  turbo_graph_checkpoint_t *checkpoint = NULL;
  char *new_run_id = NULL;
  int rc = -1;

  if (!runtime || !graph || !checkpoint_id || !out_summary_json || !out_state) {
    return -1;
  }
  if (turbo_agent_runtime_store_get_json(runtime, turbo_agent_runtime_checkpoints_collection,
                                         checkpoint_id, &checkpoint_record) != 0 ||
      turbo_agent_runtime_extract_checkpoint_record(checkpoint_record, &thread_id, &source_run_id,
                                                    &parent_checkpoint_id, &seq,
                                                    &checkpoint_json) != 0 ||
      turbo_agent_runtime_get_run(runtime, source_run_id, &source_run_record) != 0) {
    goto cleanup;
  }
  (void)parent_checkpoint_id;
  (void)seq;
  new_run_id = turbo_agent_runtime_make_id("run");
  if (!new_run_id ||
      turbo_agent_runtime_checkpoint_from_json(checkpoint_json, state_override, &checkpoint) != 0) {
    goto cleanup;
  }
  rc = turbo_agent_runtime_run_segment(
      runtime, graph, thread_id, new_run_id, source_run_id,
      turbo_json_get_string(source_run_record, "parent_agent_run_id"),
      turbo_json_get_string(source_run_record, "parent_tool_call_id"),
      turbo_json_get_string(source_run_record, "parent_tool_name"),
      turbo_json_get_string(source_run_record, "parent_graph_run_id"),
      turbo_json_get_string(source_run_record, "call_frame_id"), checkpoint_id, NULL, 1, NULL, NULL,
      checkpoint, options, cancel_token, event_sink, event_sink_user_data, out_summary_json,
      out_state);

cleanup:
  free(new_run_id);
  turbo_graph_checkpoint_destroy(checkpoint);
  turbo_free_json(&source_run_record);
  turbo_free_json(&checkpoint_record);
  return rc;
}

CXX_C_API int turbo_agent_runtime_get_thread(turbo_agent_runtime_t *runtime, const char *thread_id,
                                             json_value_t **out_thread_json) {
  return turbo_agent_runtime_store_get_json(runtime, turbo_agent_runtime_threads_collection,
                                            thread_id, out_thread_json);
}

CXX_C_API int turbo_agent_runtime_get_run(turbo_agent_runtime_t *runtime, const char *run_id,
                                          json_value_t **out_run_json) {
  return turbo_agent_runtime_store_get_json(runtime, turbo_agent_runtime_runs_collection, run_id,
                                            out_run_json);
}

CXX_C_API int turbo_agent_runtime_get_latest_run(turbo_agent_runtime_t *runtime,
                                                 const char *thread_id,
                                                 json_value_t **out_run_json) {
  char *run_id = NULL;
  int rc;

  if (!runtime || !thread_id || !out_run_json) {
    return -1;
  }
  rc = turbo_agent_runtime_latest_run_id_for_thread(runtime, thread_id, &run_id);
  if (rc != 0 || !run_id) {
    free(run_id);
    return -1;
  }
  rc = turbo_agent_runtime_get_run(runtime, run_id, out_run_json);
  free(run_id);
  return rc;
}

CXX_C_API int turbo_agent_runtime_get_pending_run(turbo_agent_runtime_t *runtime,
                                                  const char *thread_id,
                                                  json_value_t **out_run_json) {
  char *run_id = NULL;
  int rc;

  if (!runtime || !thread_id || !out_run_json) {
    return -1;
  }
  rc = turbo_agent_runtime_pending_run_id_for_thread(runtime, thread_id, &run_id);
  if (rc != 0 || !run_id) {
    free(run_id);
    return -1;
  }
  rc = turbo_agent_runtime_get_run(runtime, run_id, out_run_json);
  free(run_id);
  return rc;
}

CXX_C_API int turbo_agent_runtime_get_checkpoint(turbo_agent_runtime_t *runtime,
                                                 const char *checkpoint_id,
                                                 json_value_t **out_checkpoint_json) {
  return turbo_agent_runtime_store_get_json(runtime, turbo_agent_runtime_checkpoints_collection,
                                            checkpoint_id, out_checkpoint_json);
}

CXX_C_API int turbo_agent_runtime_get_latest_checkpoint(turbo_agent_runtime_t *runtime,
                                                        const char *run_id,
                                                        json_value_t **out_checkpoint_json) {
  char *checkpoint_id = NULL;
  int rc;

  if (!runtime || !run_id || !out_checkpoint_json) {
    return -1;
  }
  rc = turbo_agent_runtime_latest_checkpoint_id_for_run(runtime, run_id, &checkpoint_id);
  if (rc != 0 || !checkpoint_id) {
    free(checkpoint_id);
    return -1;
  }
  rc = turbo_agent_runtime_get_checkpoint(runtime, checkpoint_id, out_checkpoint_json);
  free(checkpoint_id);
  return rc;
}

CXX_C_API int turbo_agent_runtime_get_checkpoint_state_json_value(turbo_agent_runtime_t *runtime,
                                                                  const char *checkpoint_id,
                                                                  json_value_t **out_state) {
  json_value_t *checkpoint_record = NULL;
  int rc;

  if (!runtime || !checkpoint_id || !out_state) {
    return -1;
  }
  *out_state = NULL;
  rc = turbo_agent_runtime_get_checkpoint(runtime, checkpoint_id, &checkpoint_record);
  if (rc != 0) {
    return -1;
  }
  rc = turbo_agent_runtime_state_from_checkpoint_record(checkpoint_record, out_state);
  turbo_free_json(&checkpoint_record);
  return rc;
}

static int turbo_agent_runtime_trace_events_from_state_json_value(json_value_t *state,
                                                                  json_value_t **out_events) {
  json_value_t *events = NULL;

  if (!state || !out_events) {
    return -1;
  }
  *out_events = NULL;
  events = turbo_agent_state_trace_events_json_value(state);
  if (!events) {
    events = turbo_json_create_array();
  }
  if (!events) {
    return -1;
  }
  *out_events = events;
  return 0;
}

CXX_C_API int turbo_agent_runtime_get_checkpoint_trace_events_json_value(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id, json_value_t **out_events) {
  json_value_t *state = NULL;
  int rc;

  if (!runtime || !checkpoint_id || !out_events) {
    return -1;
  }
  rc = turbo_agent_runtime_get_checkpoint_state_json_value(runtime, checkpoint_id, &state);
  if (rc != 0 || !state) {
    turbo_runtime_json_destroy(state);
    return -1;
  }
  rc = turbo_agent_runtime_trace_events_from_state_json_value(state, out_events);
  turbo_runtime_json_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_runtime_get_run_state_json_value(turbo_agent_runtime_t *runtime,
                                                           const char *run_id,
                                                           json_value_t **out_state) {
  json_value_t *run_record = NULL;
  int rc;

  if (!runtime || !run_id || !out_state) {
    return -1;
  }
  *out_state = NULL;
  rc = turbo_agent_runtime_get_run(runtime, run_id, &run_record);
  if (rc != 0) {
    return -1;
  }
  rc = turbo_agent_runtime_state_from_run_record(runtime, run_record, out_state);
  turbo_free_json(&run_record);
  return rc;
}

CXX_C_API int turbo_agent_runtime_get_run_trace_events_json_value(turbo_agent_runtime_t *runtime,
                                                                  const char *run_id,
                                                                  json_value_t **out_events) {
  json_value_t *state = NULL;
  int rc;

  if (!runtime || !run_id || !out_events) {
    return -1;
  }
  rc = turbo_agent_runtime_get_run_state_json_value(runtime, run_id, &state);
  if (rc != 0 || !state) {
    turbo_runtime_json_destroy(state);
    return -1;
  }
  rc = turbo_agent_runtime_trace_events_from_state_json_value(state, out_events);
  turbo_runtime_json_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_runtime_get_thread_state_json_value(turbo_agent_runtime_t *runtime,
                                                              const char *thread_id,
                                                              json_value_t **out_state) {
  char *run_id = NULL;
  int rc;

  if (!runtime || !thread_id || !out_state) {
    return -1;
  }
  if (turbo_agent_runtime_latest_run_id_for_thread(runtime, thread_id, &run_id) != 0 || !run_id) {
    return -1;
  }
  rc = turbo_agent_runtime_get_run_state_json_value(runtime, run_id, out_state);
  free(run_id);
  return rc;
}

CXX_C_API int turbo_agent_runtime_get_thread_head_state_json_value(turbo_agent_runtime_t *runtime,
                                                                   const char *thread_id,
                                                                   json_value_t **out_state) {
  char *run_id = NULL;
  int rc;

  if (!runtime || !thread_id || !out_state) {
    return -1;
  }
  if (turbo_agent_runtime_thread_head_run_id_for_thread(runtime, thread_id, &run_id) != 0 ||
      !run_id) {
    return -1;
  }
  rc = turbo_agent_runtime_get_run_state_json_value(runtime, run_id, out_state);
  free(run_id);
  return rc;
}

CXX_C_API int turbo_agent_runtime_get_thread_trace_events_json_value(turbo_agent_runtime_t *runtime,
                                                                     const char *thread_id,
                                                                     json_value_t **out_events) {
  json_value_t *state = NULL;
  int rc;

  if (!runtime || !thread_id || !out_events) {
    return -1;
  }
  rc = turbo_agent_runtime_get_thread_state_json_value(runtime, thread_id, &state);
  if (rc != 0 || !state) {
    turbo_runtime_json_destroy(state);
    return -1;
  }
  rc = turbo_agent_runtime_trace_events_from_state_json_value(state, out_events);
  turbo_runtime_json_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_runtime_get_thread_head_trace_events_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id, json_value_t **out_events) {
  json_value_t *state = NULL;
  int rc;

  if (!runtime || !thread_id || !out_events) {
    return -1;
  }
  rc = turbo_agent_runtime_get_thread_head_state_json_value(runtime, thread_id, &state);
  if (rc != 0 || !state) {
    turbo_runtime_json_destroy(state);
    return -1;
  }
  rc = turbo_agent_runtime_trace_events_from_state_json_value(state, out_events);
  turbo_runtime_json_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_runtime_list_runs(turbo_agent_runtime_t *runtime, const char *thread_id,
                                            json_value_t **out_runs_json) {
  return turbo_agent_runtime_store_list_json(runtime, turbo_agent_runtime_runs_collection,
                                             "thread_id", thread_id, out_runs_json);
}

CXX_C_API int turbo_agent_runtime_list_child_runs(turbo_agent_runtime_t *runtime,
                                                  const char *parent_agent_run_id,
                                                  json_value_t **out_runs_json) {
  return turbo_agent_runtime_store_list_json(runtime, turbo_agent_runtime_runs_collection,
                                             "parent_agent_run_id", parent_agent_run_id,
                                             out_runs_json);
}

CXX_C_API int turbo_agent_runtime_list_checkpoints(turbo_agent_runtime_t *runtime,
                                                   const char *run_id,
                                                   json_value_t **out_checkpoints_json) {
  return turbo_agent_runtime_store_list_json(runtime, turbo_agent_runtime_checkpoints_collection,
                                             "run_id", run_id, out_checkpoints_json);
}

CXX_C_API int turbo_agent_runtime_list_thread_lineage(turbo_agent_runtime_t *runtime,
                                                      const char *thread_id,
                                                      json_value_t **out_lineage_json) {
  json_value_t *runs_json = NULL;
  json_value_t *sorted_runs_json = NULL;
  json_value_t *branches_json = NULL;
  json_value_t *lineage_json = NULL;
  char *latest_run_id = NULL;
  char *pending_run_id = NULL;
  char *root_checkpoint_id = NULL;
  size_t i;
  size_t count;

  if (!runtime || !thread_id || !out_lineage_json) {
    return -1;
  }
  *out_lineage_json = NULL;

  if (turbo_agent_runtime_list_runs(runtime, thread_id, &runs_json) != 0 || !runs_json ||
      turbo_json_type(runs_json) != TURBO_JSON_ARRAY) {
    goto cleanup;
  }
  sorted_runs_json = turbo_agent_runtime_sorted_json_array_clone(
      runs_json, turbo_agent_runtime_run_updated_at_compare_desc);
  if (!sorted_runs_json) {
    goto cleanup;
  }
  branches_json = turbo_json_create_array();
  lineage_json = turbo_json_create_object();
  if (!branches_json || !lineage_json) {
    goto cleanup;
  }

  if (turbo_agent_runtime_latest_run_id_for_thread(runtime, thread_id, &latest_run_id) != 0) {
    free(latest_run_id);
    latest_run_id = NULL;
  }
  if (turbo_agent_runtime_pending_run_id_for_thread(runtime, thread_id, &pending_run_id) != 0) {
    free(pending_run_id);
    pending_run_id = NULL;
  }

  count = turbo_json_array_size(sorted_runs_json);
  for (i = 0; i < count; ++i) {
    const json_value_t *run_record = turbo_json_array_get(sorted_runs_json, i);
    json_value_t *branch_json = NULL;
    char *branch_root_checkpoint_id = NULL;

    if (!run_record || turbo_json_type(run_record) != TURBO_JSON_OBJECT) {
      continue;
    }
    if (turbo_agent_runtime_branch_from_run_record(runtime, run_record, &branch_json,
                                                   &branch_root_checkpoint_id) != 0) {
      free(branch_root_checkpoint_id);
      goto cleanup;
    }
    if (!root_checkpoint_id && branch_root_checkpoint_id) {
      root_checkpoint_id = branch_root_checkpoint_id;
      branch_root_checkpoint_id = NULL;
    }
    free(branch_root_checkpoint_id);
    turbo_json_array_add(branches_json, branch_json);
  }

  turbo_json_object_set_string(lineage_json, "thread_id", thread_id);
  if (latest_run_id) {
    turbo_json_object_set_string(lineage_json, "latest_run_id", latest_run_id);
  } else {
    turbo_json_object_set_null(lineage_json, "latest_run_id");
  }
  if (pending_run_id) {
    turbo_json_object_set_string(lineage_json, "pending_run_id", pending_run_id);
  } else {
    turbo_json_object_set_null(lineage_json, "pending_run_id");
  }
  if (root_checkpoint_id) {
    turbo_json_object_set_string(lineage_json, "root_checkpoint_id", root_checkpoint_id);
  } else {
    turbo_json_object_set_null(lineage_json, "root_checkpoint_id");
  }
  turbo_json_object_add(lineage_json, "branches", branches_json);
  branches_json = NULL;

  *out_lineage_json = lineage_json;
  lineage_json = NULL;

cleanup:
  free(root_checkpoint_id);
  free(pending_run_id);
  free(latest_run_id);
  turbo_free_json(&lineage_json);
  turbo_free_json(&branches_json);
  turbo_free_json(&sorted_runs_json);
  turbo_free_json(&runs_json);
  return *out_lineage_json ? 0 : -1;
}

static json_value_t *
turbo_agent_runtime_branch_tree_edges_from_branches(turbo_agent_runtime_t *runtime,
                                                    const json_value_t *branches_json) {
  json_value_t *edges_json = NULL;
  size_t i;
  size_t count;

  if (!branches_json || turbo_json_type(branches_json) != TURBO_JSON_ARRAY) {
    return NULL;
  }
  edges_json = turbo_json_create_array();
  if (!edges_json) {
    return NULL;
  }

  count = turbo_json_array_size(branches_json);
  for (i = 0; i < count; ++i) {
    const json_value_t *branch = turbo_json_array_get(branches_json, i);
    const char *forked_from_checkpoint_id;
    const char *target_run_id;
    const char *parent_run_id;
    json_value_t *edge_json;

    if (!branch || turbo_json_type(branch) != TURBO_JSON_OBJECT) {
      continue;
    }
    forked_from_checkpoint_id = turbo_json_get_string(branch, "forked_from_checkpoint_id");
    if (!forked_from_checkpoint_id || forked_from_checkpoint_id[0] == '\0') {
      continue;
    }
    target_run_id = turbo_json_get_string(branch, "run_id");
    if (!target_run_id || target_run_id[0] == '\0') {
      turbo_free_json(&edges_json);
      return NULL;
    }
    parent_run_id = turbo_json_get_string(branch, "parent_run_id");
    edge_json = turbo_json_create_object();
    if (!edge_json) {
      turbo_free_json(&edges_json);
      return NULL;
    }
    turbo_json_object_set_string(edge_json, "kind", "fork");
    turbo_json_object_set_string(edge_json, "source_checkpoint_id", forked_from_checkpoint_id);
    turbo_json_object_set_string(edge_json, "target_run_id", target_run_id);
    if (parent_run_id && parent_run_id[0] != '\0') {
      turbo_json_object_set_string(edge_json, "parent_run_id", parent_run_id);
    } else {
      turbo_json_object_set_null(edge_json, "parent_run_id");
    }
    if (turbo_agent_runtime_attach_checkpoint_summary(
            runtime, edge_json, "source_checkpoint_summary", forked_from_checkpoint_id) != 0) {
      turbo_free_json(&edge_json);
      turbo_free_json(&edges_json);
      return NULL;
    }
    turbo_json_array_add(edges_json, edge_json);
  }

  return edges_json;
}

static json_value_t *
turbo_agent_runtime_branch_tree_branches_from_lineage(turbo_agent_runtime_t *runtime,
                                                      const json_value_t *branches_json) {
  json_value_t *tree_branches_json = NULL;
  size_t i;
  size_t count;

  if (!branches_json || turbo_json_type(branches_json) != TURBO_JSON_ARRAY) {
    return NULL;
  }
  tree_branches_json = turbo_json_create_array();
  if (!tree_branches_json) {
    return NULL;
  }

  count = turbo_json_array_size(branches_json);
  for (i = 0; i < count; ++i) {
    const json_value_t *branch = turbo_json_array_get(branches_json, i);
    json_value_t *branch_copy;
    const char *source_checkpoint_id;

    if (!branch || turbo_json_type(branch) != TURBO_JSON_OBJECT) {
      continue;
    }
    branch_copy = turbo_json_clone(branch);
    if (!branch_copy) {
      turbo_free_json(&tree_branches_json);
      return NULL;
    }
    source_checkpoint_id = turbo_json_get_string(branch, "forked_from_checkpoint_id");
    if (source_checkpoint_id && source_checkpoint_id[0] != '\0') {
      turbo_json_object_set_string(branch_copy, "source_checkpoint_id", source_checkpoint_id);
    } else {
      turbo_json_object_set_null(branch_copy, "source_checkpoint_id");
    }
    if (turbo_agent_runtime_attach_checkpoint_summary(
            runtime, branch_copy, "checkpoint_summary",
            turbo_json_get_string(branch_copy, "checkpoint_id")) != 0) {
      turbo_free_json(&branch_copy);
      turbo_free_json(&tree_branches_json);
      return NULL;
    }
    if (turbo_agent_runtime_attach_checkpoint_summary(
            runtime, branch_copy, "source_checkpoint_summary", source_checkpoint_id) != 0) {
      turbo_free_json(&branch_copy);
      turbo_free_json(&tree_branches_json);
      return NULL;
    }
    turbo_json_array_add(tree_branches_json, branch_copy);
  }

  return tree_branches_json;
}

static const json_value_t *
turbo_agent_runtime_branch_tree_find_branch_by_run_id(const json_value_t *branches_json,
                                                      const char *run_id) {
  size_t i;

  if (!branches_json || turbo_json_type(branches_json) != TURBO_JSON_ARRAY || !run_id ||
      run_id[0] == '\0') {
    return NULL;
  }

  for (i = 0; i < turbo_json_array_size(branches_json); ++i) {
    const json_value_t *branch = turbo_json_array_get(branches_json, i);
    const char *branch_run_id;

    if (!branch || turbo_json_type(branch) != TURBO_JSON_OBJECT) {
      continue;
    }
    branch_run_id = turbo_json_get_string(branch, "run_id");
    if (branch_run_id && strcmp(branch_run_id, run_id) == 0) {
      return branch;
    }
  }

  return NULL;
}

CXX_C_API int turbo_agent_runtime_get_branch_tree(turbo_agent_runtime_t *runtime,
                                                  const char *thread_id,
                                                  json_value_t **out_branch_tree_json) {
  json_value_t *lineage_json = NULL;
  json_value_t *branch_tree_json = NULL;
  json_value_t *tree_branches_json = NULL;
  json_value_t *edges_json = NULL;
  json_value_t *current_checkpoint_json = NULL;
  const json_value_t *branches_json;
  const json_value_t *current_branch_json = NULL;
  const char *current_checkpoint_id = NULL;
  char *latest_run_id = NULL;
  char *pending_run_id = NULL;

  if (!runtime || !thread_id || !out_branch_tree_json) {
    return -1;
  }
  *out_branch_tree_json = NULL;

  if (turbo_agent_runtime_list_thread_lineage(runtime, thread_id, &lineage_json) != 0 ||
      !lineage_json) {
    goto cleanup;
  }
  branches_json = turbo_json_object_get(lineage_json, "branches");
  if (!branches_json) {
    goto cleanup;
  }
  tree_branches_json =
      turbo_agent_runtime_branch_tree_branches_from_lineage(runtime, branches_json);
  if (!tree_branches_json) {
    goto cleanup;
  }
  edges_json = turbo_agent_runtime_branch_tree_edges_from_branches(runtime, branches_json);
  if (!edges_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_latest_run_id_for_thread(runtime, thread_id, &latest_run_id) != 0) {
    free(latest_run_id);
    latest_run_id = NULL;
  }
  if (turbo_agent_runtime_pending_run_id_for_thread(runtime, thread_id, &pending_run_id) != 0) {
    free(pending_run_id);
    pending_run_id = NULL;
  }
  if (latest_run_id) {
    current_branch_json =
        turbo_agent_runtime_branch_tree_find_branch_by_run_id(tree_branches_json, latest_run_id);
    if (current_branch_json) {
      current_checkpoint_id = turbo_json_get_string(current_branch_json, "checkpoint_id");
      if (turbo_agent_runtime_load_checkpoint_summary_by_id(runtime, current_checkpoint_id,
                                                            &current_checkpoint_json) != 0) {
        goto cleanup;
      }
    }
  }

  branch_tree_json = turbo_json_create_object();
  if (!branch_tree_json) {
    goto cleanup;
  }
  turbo_json_object_set_string(branch_tree_json, "thread_id",
                               turbo_json_get_string(lineage_json, "thread_id"));
  if (latest_run_id) {
    turbo_json_object_set_string(branch_tree_json, "current_run_id", latest_run_id);
  } else {
    turbo_json_object_set_null(branch_tree_json, "current_run_id");
  }
  if (latest_run_id) {
    turbo_json_object_set_string(branch_tree_json, "latest_run_id", latest_run_id);
  } else {
    turbo_json_object_set_null(branch_tree_json, "latest_run_id");
  }
  if (pending_run_id && latest_run_id && strcmp(pending_run_id, latest_run_id) == 0) {
    turbo_json_object_set_string(branch_tree_json, "pending_run_id", pending_run_id);
  } else {
    turbo_json_object_set_null(branch_tree_json, "pending_run_id");
  }
  if (!turbo_json_is_null(turbo_json_object_get(lineage_json, "root_checkpoint_id"))) {
    turbo_json_object_set_string(branch_tree_json, "root_checkpoint_id",
                                 turbo_json_get_string(lineage_json, "root_checkpoint_id"));
  } else {
    turbo_json_object_set_null(branch_tree_json, "root_checkpoint_id");
  }
  if (current_checkpoint_id && current_checkpoint_id[0] != '\0') {
    turbo_json_object_set_string(branch_tree_json, "current_checkpoint_id", current_checkpoint_id);
  } else {
    turbo_json_object_set_null(branch_tree_json, "current_checkpoint_id");
  }
  if (current_checkpoint_json) {
    turbo_json_object_add(branch_tree_json, "current_checkpoint_summary", current_checkpoint_json);
    current_checkpoint_json = NULL;
  } else {
    turbo_json_object_set_null(branch_tree_json, "current_checkpoint_summary");
  }
  if (current_branch_json) {
    json_value_t *current_branch_copy = turbo_json_clone(current_branch_json);

    if (!current_branch_copy) {
      goto cleanup;
    }
    turbo_json_object_add(branch_tree_json, "current_branch", current_branch_copy);
  } else {
    turbo_json_object_set_null(branch_tree_json, "current_branch");
  }
  turbo_json_object_add(branch_tree_json, "branches", tree_branches_json);
  tree_branches_json = NULL;
  turbo_json_object_add(branch_tree_json, "edges", edges_json);
  edges_json = NULL;

  *out_branch_tree_json = branch_tree_json;
  branch_tree_json = NULL;

cleanup:
  free(pending_run_id);
  free(latest_run_id);
  turbo_free_json(&edges_json);
  turbo_free_json(&tree_branches_json);
  turbo_free_json(&current_checkpoint_json);
  turbo_free_json(&branch_tree_json);
  turbo_free_json(&lineage_json);
  return *out_branch_tree_json ? 0 : -1;
}

CXX_C_API int turbo_agent_runtime_get_checkpoint_context(turbo_agent_runtime_t *runtime,
                                                         const char *checkpoint_id,
                                                         json_value_t **out_context_json) {
  json_value_t *checkpoint_json = NULL;
  json_value_t *checkpoint_summary_json = NULL;
  json_value_t *state_json = NULL;
  json_value_t *run_json = NULL;
  json_value_t *thread_json = NULL;
  json_value_t *history_events_json = NULL;
  json_value_t *context_json = NULL;
  json_value_t *state_json_value = NULL;
  json_value_t *history_events_json_value = NULL;
  const char *run_id;
  const char *thread_id;

  if (!runtime || !checkpoint_id || !checkpoint_id[0] || !out_context_json) {
    return -1;
  }
  *out_context_json = NULL;

  if (turbo_agent_runtime_get_checkpoint(runtime, checkpoint_id, &checkpoint_json) != 0 ||
      !checkpoint_json) {
    goto cleanup;
  }
  checkpoint_summary_json = turbo_agent_runtime_checkpoint_summary_from_record(checkpoint_json);
  if (!checkpoint_summary_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_get_checkpoint_state_json_value(runtime, checkpoint_id,
                                                          &state_json_value) != 0 ||
      !state_json_value) {
    goto cleanup;
  }
  state_json = turbo_json_clone(state_json_value);
  if (!state_json) {
    goto cleanup;
  }
  run_id = turbo_json_get_string(checkpoint_summary_json, "run_id");
  if (!run_id || !run_id[0] || turbo_agent_runtime_get_run(runtime, run_id, &run_json) != 0 ||
      !run_json) {
    goto cleanup;
  }
  thread_id = turbo_json_get_string(run_json, "thread_id");
  if (!thread_id || !thread_id[0] ||
      turbo_agent_runtime_get_thread(runtime, thread_id, &thread_json) != 0 || !thread_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_load_history_events_json_value(runtime, NULL, checkpoint_id,
                                                         &history_events_json_value) != 0 ||
      !history_events_json_value) {
    goto cleanup;
  }
  history_events_json = turbo_json_clone(history_events_json_value);
  if (!history_events_json) {
    goto cleanup;
  }

  context_json = turbo_json_create_object();
  if (!context_json) {
    goto cleanup;
  }
  turbo_json_object_add(context_json, "checkpoint_summary", checkpoint_summary_json);
  checkpoint_summary_json = NULL;
  turbo_json_object_add(context_json, "state", state_json);
  state_json = NULL;
  turbo_json_object_add(context_json, "run", run_json);
  run_json = NULL;
  turbo_json_object_add(context_json, "thread", thread_json);
  thread_json = NULL;
  turbo_json_object_add(context_json, "history_events", history_events_json);
  history_events_json = NULL;

  *out_context_json = context_json;
  context_json = NULL;

cleanup:
  turbo_free_json(&context_json);
  turbo_free_json(&history_events_json);
  turbo_runtime_json_destroy(history_events_json_value);
  turbo_free_json(&thread_json);
  turbo_free_json(&run_json);
  turbo_free_json(&state_json);
  turbo_runtime_json_destroy(state_json_value);
  turbo_free_json(&checkpoint_summary_json);
  turbo_free_json(&checkpoint_json);
  return *out_context_json ? 0 : -1;
}

CXX_C_API int
turbo_agent_runtime_load_history_events_json_value(turbo_agent_runtime_t *runtime,
                                                   const char *run_id, const char *checkpoint_id,
                                                   json_value_t **out_events_json_value) {
  json_value_t *records_json = NULL;
  json_value_t *events_json_value = NULL;
  size_t i;
  size_t count;

  if (!runtime || !out_events_json_value || (!!run_id == !!checkpoint_id)) {
    return -1;
  }
  *out_events_json_value = NULL;
  events_json_value = turbo_json_create_array();
  if (!events_json_value) {
    return -1;
  }

  if (run_id) {
    json_value_t **items;
    if (turbo_agent_runtime_list_checkpoints(runtime, run_id, &records_json) != 0) {
      goto cleanup;
    }
    count = turbo_json_array_size(records_json);
    items = (json_value_t **)calloc(count ? count : 1, sizeof(*items));
    if (!items) {
      goto cleanup;
    }
    for (i = 0; i < count; ++i) {
      items[i] = turbo_json_array_get(records_json, i);
    }
    qsort(items, count, sizeof(*items), turbo_agent_runtime_checkpoint_seq_compare);
    for (i = 0; i < count; ++i) {
      if (turbo_agent_runtime_append_checkpoint_events_json_value(events_json_value, items[i]) !=
          0) {
        free(items);
        goto cleanup;
      }
    }
    free(items);
  } else {
    char *current_id = turbo_agent_runtime_strdup(checkpoint_id);
    json_value_t **chain = NULL;
    size_t chain_count = 0;
    size_t loaded_count = 0;
    while (current_id) {
      json_value_t *record = NULL;
      const char *parent_id;
      json_value_t **grown;
      if (turbo_agent_runtime_get_checkpoint(runtime, current_id, &record) != 0 || !record) {
        free(current_id);
        while (loaded_count > 0) {
          turbo_free_json(&chain[loaded_count - 1]);
          --loaded_count;
        }
        free(chain);
        goto cleanup;
      }
      grown = (json_value_t **)realloc(chain, sizeof(*chain) * (chain_count + 1));
      if (!grown) {
        turbo_free_json(&record);
        free(current_id);
        while (loaded_count > 0) {
          turbo_free_json(&chain[loaded_count - 1]);
          --loaded_count;
        }
        free(chain);
        goto cleanup;
      }
      chain = grown;
      chain[chain_count++] = record;
      loaded_count = chain_count;
      parent_id = turbo_json_get_string(record, "parent_checkpoint_id");
      free(current_id);
      current_id = parent_id ? turbo_agent_runtime_strdup(parent_id) : NULL;
    }
    while (chain_count > 0) {
      if (turbo_agent_runtime_append_checkpoint_events_json_value(events_json_value,
                                                                  chain[chain_count - 1]) != 0) {
        for (i = 0; i < chain_count; ++i) {
          turbo_free_json(&chain[i]);
        }
        free(chain);
        goto cleanup;
      }
      turbo_free_json(&chain[chain_count - 1]);
      --chain_count;
    }
    free(chain);
  }

  *out_events_json_value = events_json_value;
  events_json_value = NULL;

cleanup:
  turbo_runtime_json_destroy(events_json_value);
  turbo_free_json(&records_json);
  return *out_events_json_value ? 0 : -1;
}

CXX_C_API int turbo_agent_runtime_load_thread_history_events_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id, json_value_t **out_events_json_value) {
  char *run_id = NULL;
  int rc;

  if (!runtime || !thread_id || !out_events_json_value) {
    return -1;
  }
  rc = turbo_agent_runtime_current_run_id_for_thread(runtime, thread_id, &run_id);
  if (rc != 0 || !run_id) {
    free(run_id);
    return -1;
  }
  rc = turbo_agent_runtime_load_history_events_json_value(runtime, run_id, NULL,
                                                          out_events_json_value);
  free(run_id);
  return rc;
}

static int turbo_agent_runtime_replay_events_json_value(const json_value_t *events_json_value,
                                                        turbo_event_sink_json_value_fn event_sink,
                                                        void *event_sink_user_data) {
  size_t i;
  size_t count;

  if (!events_json_value || !event_sink || turbo_json_type(events_json_value) != TURBO_JSON_ARRAY) {
    return -1;
  }
  count = turbo_runtime_json_value_size(events_json_value);
  for (i = 0; i < count; ++i) {
    const json_value_t *event = turbo_json_array_get(events_json_value, i);

    if (!event) {
      return -1;
    }
    event_sink(event, event_sink_user_data);
  }
  return 0;
}

static int
turbo_agent_runtime_observe_events_json_value(const json_value_t *events_json_value,
                                              const turbo_agent_observer_json_value_sink_t *sink) {
  size_t i;
  size_t count;

  if (!events_json_value || !sink || !sink->callback ||
      turbo_json_type(events_json_value) != TURBO_JSON_ARRAY) {
    return -1;
  }
  count = turbo_runtime_json_value_size(events_json_value);
  for (i = 0; i < count; ++i) {
    const json_value_t *raw_event = turbo_json_array_get(events_json_value, i);
    json_value_t *observer_event = NULL;
    int rc;

    if (!raw_event) {
      return -1;
    }
    rc = turbo_agent_runtime_observer_event_from_json_value(raw_event, &observer_event);
    if (rc < 0) {
      turbo_runtime_json_destroy(observer_event);
      return -1;
    }
    if (rc == 0 || !observer_event) {
      turbo_runtime_json_destroy(observer_event);
      continue;
    }
    sink->callback(observer_event, sink->user_data);
    turbo_runtime_json_destroy(observer_event);
  }
  return 0;
}

CXX_C_API int turbo_agent_runtime_replay_history_json_value(
    turbo_agent_runtime_t *runtime, const char *run_id, const char *checkpoint_id,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data) {
  json_value_t *events_json_value = NULL;
  int rc;

  if (!runtime || !event_sink || (!!run_id == !!checkpoint_id)) {
    return -1;
  }
  rc = turbo_agent_runtime_load_history_events_json_value(runtime, run_id, checkpoint_id,
                                                          &events_json_value);
  if (rc != 0 || !events_json_value) {
    turbo_runtime_json_destroy(events_json_value);
    return -1;
  }
  rc = turbo_agent_runtime_replay_events_json_value(events_json_value, event_sink,
                                                    event_sink_user_data);
  turbo_runtime_json_destroy(events_json_value);
  return rc;
}

CXX_C_API int turbo_agent_runtime_replay_thread_history_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data) {
  json_value_t *events_json_value = NULL;
  int rc;

  if (!runtime || !thread_id || !event_sink) {
    return -1;
  }
  rc = turbo_agent_runtime_load_thread_history_events_json_value(runtime, thread_id,
                                                                 &events_json_value);
  if (rc != 0 || !events_json_value) {
    turbo_runtime_json_destroy(events_json_value);
    return -1;
  }
  rc = turbo_agent_runtime_replay_events_json_value(events_json_value, event_sink,
                                                    event_sink_user_data);
  turbo_runtime_json_destroy(events_json_value);
  return rc;
}

CXX_C_API int
turbo_agent_runtime_observe_history_json_value(turbo_agent_runtime_t *runtime, const char *run_id,
                                               const char *checkpoint_id,
                                               const turbo_agent_observer_json_value_sink_t *sink) {
  json_value_t *events_json_value = NULL;
  json_value_t *record_json = NULL;
  int rc;

  if (!runtime || !sink || !sink->callback || (!!run_id == !!checkpoint_id)) {
    return -1;
  }
  rc = turbo_agent_runtime_load_history_events_json_value(runtime, run_id, checkpoint_id,
                                                          &events_json_value);
  if (rc != 0 || !events_json_value) {
    turbo_runtime_json_destroy(events_json_value);
    return -1;
  }
  rc = turbo_agent_runtime_observe_events_json_value(events_json_value, sink);
  if (rc == 0) {
    rc = checkpoint_id ? turbo_agent_runtime_get_checkpoint(runtime, checkpoint_id, &record_json)
                       : turbo_agent_runtime_get_run(runtime, run_id, &record_json);
    if (rc == 0 && record_json) {
      rc = turbo_agent_runtime_observe_terminal_record_json(events_json_value, record_json, sink);
      if (rc > 0) {
        rc = 0;
      }
    }
  }
  turbo_free_json(&record_json);
  turbo_runtime_json_destroy(events_json_value);
  return rc;
}

CXX_C_API int turbo_agent_runtime_observe_thread_history_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    const turbo_agent_observer_json_value_sink_t *sink) {
  json_value_t *events_json_value = NULL;
  json_value_t *record_json = NULL;
  char *checkpoint_id = NULL;
  char *run_id = NULL;
  int rc;

  if (!runtime || !thread_id || !sink || !sink->callback) {
    return -1;
  }
  rc = turbo_agent_runtime_load_thread_history_events_json_value(runtime, thread_id,
                                                                 &events_json_value);
  if (rc != 0 || !events_json_value) {
    turbo_runtime_json_destroy(events_json_value);
    return -1;
  }
  rc = turbo_agent_runtime_observe_events_json_value(events_json_value, sink);
  if (rc == 0 &&
      turbo_agent_runtime_current_checkpoint_id_for_thread(runtime, thread_id, &checkpoint_id) ==
          0 &&
      checkpoint_id) {
    rc = turbo_agent_runtime_get_checkpoint(runtime, checkpoint_id, &record_json);
  } else if (rc == 0 &&
             turbo_agent_runtime_current_run_id_for_thread(runtime, thread_id, &run_id) == 0 &&
             run_id) {
    rc = turbo_agent_runtime_get_run(runtime, run_id, &record_json);
  }
  if (rc == 0 && record_json) {
    rc = turbo_agent_runtime_observe_terminal_record_json(events_json_value, record_json, sink);
    if (rc > 0) {
      rc = 0;
    }
  }
  free(checkpoint_id);
  free(run_id);
  turbo_free_json(&record_json);
  turbo_runtime_json_destroy(events_json_value);
  return rc;
}

CXX_C_API int turbo_agent_runtime_get_thread_timeline_json_value(turbo_agent_runtime_t *runtime,
                                                                 const char *thread_id,
                                                                 json_value_t **out_timeline) {
  json_value_t *timeline_json = NULL;
  json_value_t *thread_json = NULL;
  json_value_t *latest_run_json = NULL;
  json_value_t *pending_run_json = NULL;
  json_value_t *resolved_current_run_json = NULL;
  json_value_t *resolved_current_checkpoint_json = NULL;
  json_value_t *runs_json = NULL;
  json_value_t *current_run_checkpoints_json = NULL;
  json_value_t *history_events_json = NULL;
  json_value_t *sorted_json = NULL;
  json_value_t *timeline_json_value = NULL;
  char *resolved_current_run_id = NULL;
  const char *resolved_current_checkpoint_id = NULL;
  const char *resolved_current_run_source = NULL;

  if (!runtime || !thread_id || !out_timeline) {
    return -1;
  }
  *out_timeline = NULL;

  if (turbo_agent_runtime_get_thread(runtime, thread_id, &thread_json) != 0 || !thread_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_get_latest_run(runtime, thread_id, &latest_run_json) != 0) {
    turbo_free_json(&latest_run_json);
  }
  if (turbo_agent_runtime_get_pending_run(runtime, thread_id, &pending_run_json) != 0) {
    turbo_free_json(&pending_run_json);
  }
  if (turbo_agent_runtime_list_runs(runtime, thread_id, &runs_json) != 0 || !runs_json) {
    goto cleanup;
  }
  sorted_json = turbo_agent_runtime_sorted_json_array_clone(
      runs_json, turbo_agent_runtime_run_updated_at_compare_desc);
  if (!sorted_json) {
    goto cleanup;
  }
  turbo_free_json(&runs_json);
  runs_json = sorted_json;
  sorted_json = NULL;

  if (pending_run_json) {
    const char *run_id = turbo_json_get_string(pending_run_json, "id");
    if (!run_id || run_id[0] == '\0') {
      goto cleanup;
    }
    resolved_current_run_id = turbo_agent_runtime_strdup(run_id);
    resolved_current_run_source = "pending_run";
    resolved_current_run_json = turbo_json_clone(pending_run_json);
  } else if (latest_run_json) {
    const char *run_id = turbo_json_get_string(latest_run_json, "id");
    if (!run_id || run_id[0] == '\0') {
      goto cleanup;
    }
    resolved_current_run_id = turbo_agent_runtime_strdup(run_id);
    resolved_current_run_source = "latest_run";
    resolved_current_run_json = turbo_json_clone(latest_run_json);
  }
  if ((resolved_current_run_id && !resolved_current_run_json) ||
      (!resolved_current_run_id && resolved_current_run_json)) {
    goto cleanup;
  }
  if (resolved_current_run_json) {
    resolved_current_checkpoint_id =
        turbo_json_get_string(resolved_current_run_json, "latest_checkpoint_id");
    if (resolved_current_checkpoint_id && resolved_current_checkpoint_id[0] == '\0') {
      resolved_current_checkpoint_id = NULL;
    }
  }

  if (resolved_current_run_id) {
    if (turbo_agent_runtime_list_checkpoints(runtime, resolved_current_run_id,
                                             &current_run_checkpoints_json) != 0 ||
        !current_run_checkpoints_json) {
      goto cleanup;
    }
    sorted_json = turbo_agent_runtime_sorted_json_array_clone(
        current_run_checkpoints_json, turbo_agent_runtime_checkpoint_seq_compare);
    if (!sorted_json) {
      goto cleanup;
    }
    turbo_free_json(&current_run_checkpoints_json);
    current_run_checkpoints_json = sorted_json;
    sorted_json = NULL;
    if (turbo_agent_runtime_history_events_json_for_run(runtime, resolved_current_run_id,
                                                        &history_events_json) != 0 ||
        !history_events_json) {
      goto cleanup;
    }
    if (turbo_agent_runtime_load_checkpoint_summary_by_id(runtime, resolved_current_checkpoint_id,
                                                          &resolved_current_checkpoint_json) != 0) {
      goto cleanup;
    }
  } else {
    current_run_checkpoints_json = turbo_json_create_array();
    history_events_json = turbo_json_create_array();
    if (!current_run_checkpoints_json || !history_events_json) {
      goto cleanup;
    }
  }

  timeline_json = turbo_json_create_object();
  if (!timeline_json) {
    goto cleanup;
  }
  turbo_json_object_add(timeline_json, "thread", thread_json);
  thread_json = NULL;
  if (resolved_current_run_id) {
    turbo_json_object_set_string(timeline_json, "resolved_current_run_id", resolved_current_run_id);
  } else {
    turbo_json_object_set_null(timeline_json, "resolved_current_run_id");
  }
  if (resolved_current_checkpoint_id) {
    turbo_json_object_set_string(timeline_json, "resolved_current_checkpoint_id",
                                 resolved_current_checkpoint_id);
  } else {
    turbo_json_object_set_null(timeline_json, "resolved_current_checkpoint_id");
  }
  if (resolved_current_run_source) {
    turbo_json_object_set_string(timeline_json, "resolved_current_run_source",
                                 resolved_current_run_source);
  } else {
    turbo_json_object_set_null(timeline_json, "resolved_current_run_source");
  }
  if (resolved_current_run_json) {
    turbo_json_object_add(timeline_json, "resolved_current_run", resolved_current_run_json);
    resolved_current_run_json = NULL;
  } else {
    turbo_json_object_set_null(timeline_json, "resolved_current_run");
  }
  if (resolved_current_checkpoint_json) {
    turbo_json_object_add(timeline_json, "resolved_current_checkpoint",
                          resolved_current_checkpoint_json);
    resolved_current_checkpoint_json = NULL;
  } else {
    turbo_json_object_set_null(timeline_json, "resolved_current_checkpoint");
  }
  if (latest_run_json) {
    turbo_json_object_add(timeline_json, "latest_run", latest_run_json);
    latest_run_json = NULL;
  } else {
    turbo_json_object_set_null(timeline_json, "latest_run");
  }
  if (pending_run_json) {
    turbo_json_object_add(timeline_json, "pending_run", pending_run_json);
    pending_run_json = NULL;
  } else {
    turbo_json_object_set_null(timeline_json, "pending_run");
  }
  turbo_json_object_add(timeline_json, "runs", runs_json);
  runs_json = NULL;
  turbo_json_object_add(timeline_json, "current_run_checkpoints", current_run_checkpoints_json);
  current_run_checkpoints_json = NULL;
  turbo_json_object_add(timeline_json, "history_events", history_events_json);
  history_events_json = NULL;

  timeline_json_value = turbo_json_clone(timeline_json);
  if (!timeline_json_value) {
    goto cleanup;
  }
  *out_timeline = timeline_json_value;
  timeline_json_value = NULL;

cleanup:
  free(resolved_current_run_id);
  turbo_runtime_json_destroy(timeline_json_value);
  turbo_free_json(&sorted_json);
  turbo_free_json(&history_events_json);
  turbo_free_json(&current_run_checkpoints_json);
  turbo_free_json(&runs_json);
  turbo_free_json(&resolved_current_run_json);
  turbo_free_json(&resolved_current_checkpoint_json);
  turbo_free_json(&pending_run_json);
  turbo_free_json(&latest_run_json);
  turbo_free_json(&thread_json);
  turbo_free_json(&timeline_json);
  return *out_timeline ? 0 : -1;
}

static void turbo_agent_runtime_observability_counts_from_runs(const json_value_t *runs_json,
                                                               size_t *out_runs,
                                                               size_t *out_interrupted_runs,
                                                               size_t *out_completed_runs) {
  size_t i;
  size_t count = 0;
  size_t interrupted = 0;
  size_t completed = 0;

  if (out_runs) {
    *out_runs = 0;
  }
  if (out_interrupted_runs) {
    *out_interrupted_runs = 0;
  }
  if (out_completed_runs) {
    *out_completed_runs = 0;
  }
  if (!runs_json || turbo_json_type(runs_json) != TURBO_JSON_ARRAY) {
    return;
  }

  count = turbo_json_array_size(runs_json);
  for (i = 0; i < count; ++i) {
    const json_value_t *run_record = turbo_json_array_get(runs_json, i);
    const char *status;

    if (!run_record || turbo_json_type(run_record) != TURBO_JSON_OBJECT) {
      continue;
    }
    status = turbo_json_get_string(run_record, "status");
    if (status && strcmp(status, "interrupted") == 0) {
      interrupted += 1;
    } else if (status && strcmp(status, "completed") == 0) {
      completed += 1;
    }
  }

  if (out_runs) {
    *out_runs = count;
  }
  if (out_interrupted_runs) {
    *out_interrupted_runs = interrupted;
  }
  if (out_completed_runs) {
    *out_completed_runs = completed;
  }
}

static json_value_t *turbo_agent_runtime_build_observability_counts_json(
    const json_value_t *timeline_json, const json_value_t *lineage_json,
    const json_value_t *branch_tree_json, const json_value_t *history_events_json,
    const json_value_t *trace_events_json) {
  const json_value_t *runs_json = NULL;
  const json_value_t *current_run_checkpoints_json = NULL;
  const json_value_t *branches_json = NULL;
  const json_value_t *edges_json = NULL;
  size_t runs = 0;
  size_t interrupted_runs = 0;
  size_t completed_runs = 0;
  json_value_t *counts_json = turbo_json_create_object();

  if (!counts_json) {
    return NULL;
  }

  if (timeline_json && turbo_json_type(timeline_json) == TURBO_JSON_OBJECT) {
    runs_json = turbo_json_object_get(timeline_json, "runs");
    current_run_checkpoints_json = turbo_json_object_get(timeline_json, "current_run_checkpoints");
  }
  if (branch_tree_json && turbo_json_type(branch_tree_json) == TURBO_JSON_OBJECT) {
    branches_json = turbo_json_object_get(branch_tree_json, "branches");
    edges_json = turbo_json_object_get(branch_tree_json, "edges");
  } else if (lineage_json && turbo_json_type(lineage_json) == TURBO_JSON_OBJECT) {
    branches_json = turbo_json_object_get(lineage_json, "branches");
  }

  turbo_agent_runtime_observability_counts_from_runs(runs_json, &runs, &interrupted_runs,
                                                     &completed_runs);
  turbo_json_object_set_number(counts_json, "runs", (double)runs);
  turbo_json_object_set_number(counts_json, "interrupted_runs", (double)interrupted_runs);
  turbo_json_object_set_number(counts_json, "completed_runs", (double)completed_runs);
  turbo_json_object_set_number(
      counts_json, "current_run_checkpoints",
      (double)((current_run_checkpoints_json &&
                turbo_json_type(current_run_checkpoints_json) == TURBO_JSON_ARRAY)
                   ? turbo_json_array_size(current_run_checkpoints_json)
                   : 0));
  turbo_json_object_set_number(
      counts_json, "branches",
      (double)((branches_json && turbo_json_type(branches_json) == TURBO_JSON_ARRAY)
                   ? turbo_json_array_size(branches_json)
                   : 0));
  turbo_json_object_set_number(
      counts_json, "edges",
      (double)((edges_json && turbo_json_type(edges_json) == TURBO_JSON_ARRAY)
                   ? turbo_json_array_size(edges_json)
                   : 0));
  turbo_json_object_set_number(
      counts_json, "history_events",
      (double)((history_events_json && turbo_json_type(history_events_json) == TURBO_JSON_ARRAY)
                   ? turbo_json_array_size(history_events_json)
                   : 0));
  turbo_json_object_set_number(
      counts_json, "trace_events",
      (double)((trace_events_json && turbo_json_type(trace_events_json) == TURBO_JSON_ARRAY)
                   ? turbo_json_array_size(trace_events_json)
                   : 0));
  return counts_json;
}

static const char *turbo_agent_runtime_observability_string_or_null(const json_value_t *object_json,
                                                                    const char *key) {
  const char *value;

  if (!object_json || turbo_json_type(object_json) != TURBO_JSON_OBJECT || !key) {
    return NULL;
  }
  value = turbo_json_get_string(object_json, key);
  return (value && value[0] != '\0') ? value : NULL;
}

static const char *
turbo_agent_runtime_observability_current_status(const json_value_t *timeline_json,
                                                 const json_value_t *pending_run_json,
                                                 const json_value_t *latest_run_json) {
  const json_value_t *resolved_current_run_json = NULL;
  const char *status = NULL;

  if (timeline_json && turbo_json_type(timeline_json) == TURBO_JSON_OBJECT) {
    resolved_current_run_json = turbo_json_object_get(timeline_json, "resolved_current_run");
  }
  status = turbo_agent_runtime_observability_string_or_null(resolved_current_run_json, "status");
  if (status) {
    return status;
  }
  status = turbo_agent_runtime_observability_string_or_null(pending_run_json, "status");
  if (status) {
    return status;
  }
  return turbo_agent_runtime_observability_string_or_null(latest_run_json, "status");
}

static int turbo_agent_runtime_observability_summary_copy_field(json_value_t *target_json,
                                                                const json_value_t *source_json,
                                                                const char *key) {
  const json_value_t *value_json;
  json_value_t *clone_json;

  if (!target_json || !source_json || !key) {
    return -1;
  }
  value_json = turbo_json_object_get(source_json, key);
  if (!value_json) {
    turbo_json_object_set_null(target_json, key);
    return 0;
  }
  clone_json = turbo_json_clone(value_json);
  if (!clone_json) {
    return -1;
  }
  turbo_json_object_add(target_json, key, clone_json);
  return 0;
}

static int turbo_agent_runtime_build_observability_index_summary(const json_value_t *index_json,
                                                                 json_value_t **out_summary_json) {
  static const char *const summary_keys[] = {"thread",
                                             "latest_run",
                                             "pending_run",
                                             "current_status",
                                             "current_interrupt_reason",
                                             "current_pending_action",
                                             "current_checkpoint_summary",
                                             "latest_run_status",
                                             "latest_run_updated_at",
                                             "pending_run_id",
                                             "pending_checkpoint_id",
                                             "has_failure",
                                             "has_model_error",
                                             "has_guardrail_rejection",
                                             "replan_requested",
                                             "current_failure_reason",
                                             "current_review_note",
                                             "has_pending_review",
                                             "has_handoff",
                                             "active_agent",
                                             "counts"};
  json_value_t *summary_json = NULL;
  size_t i;

  if (!index_json || turbo_json_type(index_json) != TURBO_JSON_OBJECT || !out_summary_json) {
    return -1;
  }
  *out_summary_json = NULL;
  summary_json = turbo_json_create_object();
  if (!summary_json) {
    return -1;
  }
  for (i = 0; i < sizeof(summary_keys) / sizeof(summary_keys[0]); ++i) {
    if (turbo_agent_runtime_observability_summary_copy_field(summary_json, index_json,
                                                             summary_keys[i]) != 0) {
      turbo_free_json(&summary_json);
      return -1;
    }
  }
  *out_summary_json = summary_json;
  return 0;
}

static int turbo_agent_runtime_observability_summary_matches_filters(
    const json_value_t *summary_json, const json_value_t *filters_json, bool *out_matches) {
  const json_value_t *value_json;
  const char *summary_text;
  const char *summary_thread_id;
  bool expected_bool;

  if (!summary_json || turbo_json_type(summary_json) != TURBO_JSON_OBJECT || !out_matches) {
    return -1;
  }
  *out_matches = true;
  if (!filters_json || turbo_json_is_null(filters_json)) {
    return 0;
  }
  if (turbo_json_type(filters_json) != TURBO_JSON_OBJECT) {
    return -1;
  }
  summary_thread_id = turbo_agent_runtime_observability_string_or_null(
      turbo_json_object_get(summary_json, "thread"), "id");

  value_json = turbo_json_object_get(filters_json, "status");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_STRING) {
      return -1;
    }
    summary_text = turbo_agent_runtime_observability_string_or_null(summary_json, "current_status");
    if (!summary_text || strcmp(summary_text, turbo_json_string(value_json)) != 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "has_pending_review");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_BOOL) {
      return -1;
    }
    expected_bool = turbo_json_bool(value_json);
    if (turbo_json_get_bool(summary_json, "has_pending_review", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "has_failure");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_BOOL) {
      return -1;
    }
    expected_bool = turbo_json_bool(value_json);
    if (turbo_json_get_bool(summary_json, "has_failure", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "has_handoff");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_BOOL) {
      return -1;
    }
    expected_bool = turbo_json_bool(value_json);
    if (turbo_json_get_bool(summary_json, "has_handoff", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "has_model_error");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_BOOL) {
      return -1;
    }
    expected_bool = turbo_json_bool(value_json);
    if (turbo_json_get_bool(summary_json, "has_model_error", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "has_guardrail_rejection");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_BOOL) {
      return -1;
    }
    expected_bool = turbo_json_bool(value_json);
    if (turbo_json_get_bool(summary_json, "has_guardrail_rejection", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "replan_requested");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_BOOL) {
      return -1;
    }
    expected_bool = turbo_json_bool(value_json);
    if (turbo_json_get_bool(summary_json, "replan_requested", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "active_agent");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_STRING) {
      return -1;
    }
    summary_text = turbo_agent_runtime_observability_string_or_null(summary_json, "active_agent");
    if (!summary_text || strcmp(summary_text, turbo_json_string(value_json)) != 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "current_interrupt_reason");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_STRING) {
      return -1;
    }
    summary_text =
        turbo_agent_runtime_observability_string_or_null(summary_json, "current_interrupt_reason");
    if (!summary_text || strcmp(summary_text, turbo_json_string(value_json)) != 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "latest_run_status");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_STRING) {
      return -1;
    }
    summary_text =
        turbo_agent_runtime_observability_string_or_null(summary_json, "latest_run_status");
    if (!summary_text || strcmp(summary_text, turbo_json_string(value_json)) != 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "latest_run_updated_after");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_STRING) {
      return -1;
    }
    summary_text =
        turbo_agent_runtime_observability_string_or_null(summary_json, "latest_run_updated_at");
    if (!summary_text || turbo_agent_runtime_record_string_compare(
                             summary_text, turbo_json_string(value_json)) <= 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "latest_run_updated_before");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_STRING) {
      return -1;
    }
    summary_text =
        turbo_agent_runtime_observability_string_or_null(summary_json, "latest_run_updated_at");
    if (!summary_text || turbo_agent_runtime_record_string_compare(
                             summary_text, turbo_json_string(value_json)) >= 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "thread_id_prefix");
  if (value_json && !turbo_json_is_null(value_json)) {
    const char *prefix;

    if (turbo_json_type(value_json) != TURBO_JSON_STRING) {
      return -1;
    }
    prefix = turbo_json_string(value_json);
    if (!summary_thread_id || strncmp(summary_thread_id, prefix, strlen(prefix)) != 0) {
      *out_matches = false;
      return 0;
    }
  }

  return 0;
}

static int turbo_agent_runtime_observability_sort_options(const json_value_t *filters_json,
                                                          const char **out_sort_by,
                                                          const char **out_sort_order,
                                                          int *out_limit) {
  const json_value_t *value_json;

  if (!out_sort_by || !out_sort_order || !out_limit) {
    return -1;
  }
  *out_sort_by = NULL;
  *out_sort_order = NULL;
  *out_limit = -1;
  if (!filters_json || turbo_json_is_null(filters_json)) {
    return 0;
  }
  if (turbo_json_type(filters_json) != TURBO_JSON_OBJECT) {
    return -1;
  }

  value_json = turbo_json_object_get(filters_json, "sort_by");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_STRING) {
      return -1;
    }
    *out_sort_by = turbo_json_string(value_json);
    if (strcmp(*out_sort_by, "latest_run_updated_at") != 0 &&
        strcmp(*out_sort_by, "thread_id") != 0) {
      return -1;
    }
  }

  value_json = turbo_json_object_get(filters_json, "sort_order");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_STRING) {
      return -1;
    }
    *out_sort_order = turbo_json_string(value_json);
    if (strcmp(*out_sort_order, "asc") != 0 && strcmp(*out_sort_order, "desc") != 0) {
      return -1;
    }
  }

  value_json = turbo_json_object_get(filters_json, "limit");
  if (value_json && !turbo_json_is_null(value_json)) {
    double limit_value;

    if (turbo_json_type(value_json) != TURBO_JSON_NUMBER) {
      return -1;
    }
    limit_value = turbo_json_number(value_json);
    if (limit_value < 0.0) {
      return -1;
    }
    *out_limit = (int)limit_value;
  }

  if (!*out_sort_by && *out_sort_order) {
    *out_sort_by = "latest_run_updated_at";
  }
  if (*out_sort_by && !*out_sort_order) {
    *out_sort_order = (strcmp(*out_sort_by, "thread_id") == 0) ? "asc" : "desc";
  }
  return 0;
}

static int (*turbo_agent_runtime_observability_sort_compare(const char *sort_by,
                                                            const char *sort_order))(const void *,
                                                                                     const void *) {
  if (!sort_by || !sort_order) {
    return NULL;
  }
  if (strcmp(sort_by, "thread_id") == 0) {
    return (strcmp(sort_order, "desc") == 0)
               ? turbo_agent_runtime_observability_summary_thread_id_compare_desc
               : turbo_agent_runtime_observability_summary_thread_id_compare_asc;
  }
  return (strcmp(sort_order, "asc") == 0)
             ? turbo_agent_runtime_observability_summary_latest_run_updated_at_compare_asc
             : turbo_agent_runtime_observability_summary_latest_run_updated_at_compare_desc;
}

CXX_C_API int turbo_agent_runtime_get_thread_observability_index(turbo_agent_runtime_t *runtime,
                                                                 const char *thread_id,
                                                                 json_value_t **out_index_json) {
  json_value_t *index_json = NULL;
  json_value_t *thread_json = NULL;
  json_value_t *latest_run_json = NULL;
  json_value_t *pending_run_json = NULL;
  json_value_t *thread_timeline_json = NULL;
  json_value_t *thread_lineage_json = NULL;
  json_value_t *branch_tree_json = NULL;
  json_value_t *history_events_json = NULL;
  json_value_t *trace_events_json = NULL;
  json_value_t *counts_json = NULL;
  json_value_t *control_snapshot_json = NULL;
  json_value_t *current_checkpoint_summary_json = NULL;
  json_value_t *thread_state_json = NULL;
  json_value_t *thread_timeline_json_value = NULL;
  json_value_t *history_events_json_value = NULL;
  json_value_t *trace_events_json_value = NULL;
  json_value_t *thread_state_json_value = NULL;
  json_value_t *control_snapshot_json_value = NULL;
  const json_value_t *review_json = NULL;
  const json_value_t *replan_json = NULL;
  const json_value_t *failure_json = NULL;
  const json_value_t *model_error_json = NULL;
  const json_value_t *guardrail_json = NULL;
  const json_value_t *supervisor_json = NULL;
  const char *current_status = NULL;
  const char *current_interrupt_reason = NULL;
  const char *current_pending_action = NULL;
  const char *active_agent = NULL;
  const char *latest_run_status = NULL;
  const char *latest_run_updated_at = NULL;
  const char *pending_run_id = NULL;
  const char *pending_checkpoint_id = NULL;
  char *owned_executor_failure_reason = NULL;
  const char *handoff_target_agent = NULL;
  const char *handoff_reason = NULL;
  const char *current_failure_reason = NULL;
  const char *current_review_note = NULL;
  bool has_pending_review = false;
  bool has_handoff = false;
  bool has_failure = false;
  bool has_model_error = false;
  bool has_guardrail_rejection = false;
  bool replan_requested = false;

  if (!runtime || !thread_id || !out_index_json) {
    return -1;
  }
  *out_index_json = NULL;

  if (turbo_agent_runtime_get_thread(runtime, thread_id, &thread_json) != 0 || !thread_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_get_latest_run(runtime, thread_id, &latest_run_json) != 0) {
    turbo_free_json(&latest_run_json);
  }
  if (turbo_agent_runtime_get_pending_run(runtime, thread_id, &pending_run_json) != 0) {
    turbo_free_json(&pending_run_json);
  }
  if (turbo_agent_runtime_get_thread_timeline_json_value(runtime, thread_id,
                                                         &thread_timeline_json_value) != 0 ||
      !thread_timeline_json_value) {
    goto cleanup;
  }
  thread_timeline_json = turbo_json_clone(thread_timeline_json_value);
  if (!thread_timeline_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_list_thread_lineage(runtime, thread_id, &thread_lineage_json) != 0 ||
      !thread_lineage_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_get_branch_tree(runtime, thread_id, &branch_tree_json) != 0 ||
      !branch_tree_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_load_thread_history_events_json_value(runtime, thread_id,
                                                                &history_events_json_value) != 0 ||
      !history_events_json_value) {
    goto cleanup;
  }
  history_events_json = turbo_json_clone(history_events_json_value);
  if (!history_events_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_get_thread_trace_events_json_value(runtime, thread_id,
                                                             &trace_events_json_value) != 0 ||
      !trace_events_json_value) {
    goto cleanup;
  }
  trace_events_json = turbo_json_clone(trace_events_json_value);
  if (!trace_events_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_get_thread_state_json_value(runtime, thread_id,
                                                      &thread_state_json_value) == 0 &&
      thread_state_json_value) {
    thread_state_json = turbo_json_clone(thread_state_json_value);
    if (!thread_state_json) {
      goto cleanup;
    }
    control_snapshot_json_value =
        turbo_agent_state_control_snapshot_json_value(thread_state_json_value);
    if (control_snapshot_json_value) {
      control_snapshot_json = turbo_json_clone(control_snapshot_json_value);
      if (!control_snapshot_json) {
        goto cleanup;
      }
    }
  }

  counts_json = turbo_agent_runtime_build_observability_counts_json(
      thread_timeline_json, thread_lineage_json, branch_tree_json, history_events_json,
      trace_events_json);
  if (!counts_json) {
    goto cleanup;
  }
  current_status = turbo_agent_runtime_observability_current_status(
      thread_timeline_json, pending_run_json, latest_run_json);
  latest_run_status = turbo_agent_runtime_observability_string_or_null(latest_run_json, "status");
  latest_run_updated_at =
      turbo_agent_runtime_observability_string_or_null(latest_run_json, "updated_at");
  pending_run_id = turbo_agent_runtime_observability_string_or_null(pending_run_json, "id");
  pending_checkpoint_id =
      turbo_agent_runtime_observability_string_or_null(pending_run_json, "latest_checkpoint_id");
  if (thread_timeline_json && turbo_json_type(thread_timeline_json) == TURBO_JSON_OBJECT) {
    const json_value_t *resolved_current_checkpoint_json =
        turbo_json_object_get(thread_timeline_json, "resolved_current_checkpoint");
    const char *next_node = turbo_agent_runtime_observability_string_or_null(
        resolved_current_checkpoint_json, "next_node");
    if (resolved_current_checkpoint_json &&
        turbo_json_type(resolved_current_checkpoint_json) == TURBO_JSON_OBJECT) {
      current_checkpoint_summary_json = turbo_json_clone(resolved_current_checkpoint_json);
      if (!current_checkpoint_summary_json) {
        goto cleanup;
      }
    }
    turbo_agent_runtime_interrupt_metadata(current_status, next_node, thread_state_json,
                                           &current_interrupt_reason, &current_pending_action, NULL,
                                           NULL, &owned_executor_failure_reason);
  }
  if (control_snapshot_json && turbo_json_type(control_snapshot_json) == TURBO_JSON_OBJECT) {
    review_json = turbo_json_object_get(control_snapshot_json, "review");
    replan_json = turbo_json_object_get(control_snapshot_json, "replan");
    failure_json = turbo_json_object_get(control_snapshot_json, "failure");
    model_error_json = turbo_json_object_get(control_snapshot_json, "model_error");
    guardrail_json = turbo_json_object_get(control_snapshot_json, "guardrail");
    supervisor_json = turbo_json_object_get(control_snapshot_json, "supervisor");
  }
  if (review_json && turbo_json_type(review_json) == TURBO_JSON_OBJECT) {
    has_pending_review = turbo_json_get_bool(review_json, "required", false) &&
                         !turbo_json_get_bool(review_json, "approved", false);
    current_review_note = turbo_agent_runtime_observability_string_or_null(review_json, "note");
  }
  if (replan_json && turbo_json_type(replan_json) == TURBO_JSON_OBJECT) {
    replan_requested = turbo_json_get_bool(replan_json, "requested", false);
  }
  if (failure_json && turbo_json_type(failure_json) == TURBO_JSON_OBJECT) {
    current_failure_reason =
        turbo_agent_runtime_observability_string_or_null(failure_json, "reason");
    has_failure = current_failure_reason && current_failure_reason[0] != '\0';
  }
  if (model_error_json && turbo_json_type(model_error_json) == TURBO_JSON_OBJECT) {
    has_model_error =
        turbo_agent_runtime_observability_string_or_null(model_error_json, "detail") != NULL;
  }
  if (guardrail_json && turbo_json_type(guardrail_json) == TURBO_JSON_OBJECT) {
    has_guardrail_rejection =
        turbo_agent_runtime_observability_string_or_null(guardrail_json, "reason") != NULL;
  }
  active_agent = turbo_agent_runtime_observability_string_or_null(supervisor_json, "active_agent");
  handoff_target_agent =
      turbo_agent_runtime_observability_string_or_null(supervisor_json, "target_agent");
  handoff_reason =
      turbo_agent_runtime_observability_string_or_null(supervisor_json, "handoff_reason");
  has_handoff = (handoff_target_agent && handoff_target_agent[0] != '\0') ||
                (handoff_reason && handoff_reason[0] != '\0');

  index_json = turbo_json_create_object();
  if (!index_json) {
    goto cleanup;
  }
  turbo_json_object_add(index_json, "thread", thread_json);
  thread_json = NULL;
  if (latest_run_json) {
    turbo_json_object_add(index_json, "latest_run", latest_run_json);
    latest_run_json = NULL;
  } else {
    turbo_json_object_set_null(index_json, "latest_run");
  }
  if (pending_run_json) {
    turbo_json_object_add(index_json, "pending_run", pending_run_json);
    pending_run_json = NULL;
  } else {
    turbo_json_object_set_null(index_json, "pending_run");
  }
  turbo_json_object_add(index_json, "thread_timeline", thread_timeline_json);
  thread_timeline_json = NULL;
  turbo_json_object_add(index_json, "thread_lineage", thread_lineage_json);
  thread_lineage_json = NULL;
  turbo_json_object_add(index_json, "branch_tree", branch_tree_json);
  branch_tree_json = NULL;
  if (current_status) {
    turbo_json_object_set_string(index_json, "current_status", current_status);
  } else {
    turbo_json_object_set_null(index_json, "current_status");
  }
  if (current_interrupt_reason) {
    turbo_json_object_set_string(index_json, "current_interrupt_reason", current_interrupt_reason);
  } else {
    turbo_json_object_set_null(index_json, "current_interrupt_reason");
  }
  if (current_pending_action) {
    turbo_json_object_set_string(index_json, "current_pending_action", current_pending_action);
  } else {
    turbo_json_object_set_null(index_json, "current_pending_action");
  }
  if (current_checkpoint_summary_json) {
    turbo_json_object_add(index_json, "current_checkpoint_summary",
                          current_checkpoint_summary_json);
    current_checkpoint_summary_json = NULL;
  } else {
    turbo_json_object_set_null(index_json, "current_checkpoint_summary");
  }
  if (latest_run_status) {
    turbo_json_object_set_string(index_json, "latest_run_status", latest_run_status);
  } else {
    turbo_json_object_set_null(index_json, "latest_run_status");
  }
  if (latest_run_updated_at) {
    turbo_json_object_set_string(index_json, "latest_run_updated_at", latest_run_updated_at);
  } else {
    turbo_json_object_set_null(index_json, "latest_run_updated_at");
  }
  if (pending_run_id) {
    turbo_json_object_set_string(index_json, "pending_run_id", pending_run_id);
  } else {
    turbo_json_object_set_null(index_json, "pending_run_id");
  }
  if (pending_checkpoint_id) {
    turbo_json_object_set_string(index_json, "pending_checkpoint_id", pending_checkpoint_id);
  } else {
    turbo_json_object_set_null(index_json, "pending_checkpoint_id");
  }
  turbo_json_object_set_bool(index_json, "has_failure", has_failure ? true : false);
  turbo_json_object_set_bool(index_json, "has_model_error", has_model_error ? true : false);
  turbo_json_object_set_bool(index_json, "has_guardrail_rejection",
                             has_guardrail_rejection ? true : false);
  turbo_json_object_set_bool(index_json, "replan_requested", replan_requested ? true : false);
  if (current_failure_reason) {
    turbo_json_object_set_string(index_json, "current_failure_reason", current_failure_reason);
  } else {
    turbo_json_object_set_null(index_json, "current_failure_reason");
  }
  if (current_review_note) {
    turbo_json_object_set_string(index_json, "current_review_note", current_review_note);
  } else {
    turbo_json_object_set_null(index_json, "current_review_note");
  }
  turbo_json_object_set_bool(index_json, "has_pending_review", has_pending_review ? true : false);
  turbo_json_object_set_bool(index_json, "has_handoff", has_handoff ? true : false);
  if (active_agent) {
    turbo_json_object_set_string(index_json, "active_agent", active_agent);
  } else {
    turbo_json_object_set_null(index_json, "active_agent");
  }
  turbo_json_object_add(index_json, "history_events", history_events_json);
  history_events_json = NULL;
  turbo_json_object_add(index_json, "trace_events", trace_events_json);
  trace_events_json = NULL;
  turbo_json_object_add(index_json, "counts", counts_json);
  counts_json = NULL;

  *out_index_json = index_json;
  index_json = NULL;
  free(owned_executor_failure_reason);
  turbo_runtime_json_destroy(control_snapshot_json_value);
  turbo_runtime_json_destroy(thread_state_json_value);
  turbo_runtime_json_destroy(trace_events_json_value);
  turbo_runtime_json_destroy(history_events_json_value);
  turbo_runtime_json_destroy(thread_timeline_json_value);
  turbo_free_json(&thread_state_json);
  turbo_free_json(&control_snapshot_json);
  return 0;

cleanup:
  free(owned_executor_failure_reason);
  turbo_runtime_json_destroy(control_snapshot_json_value);
  turbo_runtime_json_destroy(thread_state_json_value);
  turbo_runtime_json_destroy(trace_events_json_value);
  turbo_runtime_json_destroy(history_events_json_value);
  turbo_runtime_json_destroy(thread_timeline_json_value);
  turbo_free_json(&thread_state_json);
  turbo_free_json(&current_checkpoint_summary_json);
  turbo_free_json(&control_snapshot_json);
  turbo_free_json(&counts_json);
  turbo_free_json(&trace_events_json);
  turbo_free_json(&history_events_json);
  turbo_free_json(&branch_tree_json);
  turbo_free_json(&thread_lineage_json);
  turbo_free_json(&thread_timeline_json);
  turbo_free_json(&pending_run_json);
  turbo_free_json(&latest_run_json);
  turbo_free_json(&thread_json);
  turbo_free_json(&index_json);
  return -1;
}

CXX_C_API int
turbo_agent_runtime_list_observability_indexes_filtered(turbo_agent_runtime_t *runtime,
                                                        const json_value_t *filters_json,
                                                        json_value_t **out_indexes_json) {
  json_value_t *threads_json = NULL;
  json_value_t *sorted_threads_json = NULL;
  json_value_t *indexes_json = NULL;
  json_value_t *sorted_indexes_json = NULL;
  json_value_t *limited_indexes_json = NULL;
  const char *sort_by = NULL;
  const char *sort_order = NULL;
  int limit = -1;
  int (*compare)(const void *, const void *) = NULL;
  size_t count;
  size_t i;

  if (!runtime || !out_indexes_json) {
    return -1;
  }
  *out_indexes_json = NULL;
  if (turbo_agent_runtime_observability_sort_options(filters_json, &sort_by, &sort_order, &limit) !=
      0) {
    return -1;
  }
  compare = turbo_agent_runtime_observability_sort_compare(sort_by, sort_order);
  if (turbo_agent_runtime_store_list_json(runtime, turbo_agent_runtime_threads_collection, NULL,
                                          NULL, &threads_json) != 0 ||
      !threads_json) {
    goto cleanup;
  }
  sorted_threads_json = turbo_agent_runtime_sorted_json_array_clone(
      threads_json, turbo_agent_runtime_run_updated_at_compare_desc);
  if (!sorted_threads_json) {
    goto cleanup;
  }
  indexes_json = turbo_json_create_array();
  if (!indexes_json) {
    goto cleanup;
  }

  count = turbo_json_array_size(sorted_threads_json);
  for (i = 0; i < count; ++i) {
    const json_value_t *thread_json = turbo_json_array_get(sorted_threads_json, i);
    const char *thread_id;
    json_value_t *index_json = NULL;
    json_value_t *summary_json = NULL;
    bool matches_filters = false;

    if (!thread_json || turbo_json_type(thread_json) != TURBO_JSON_OBJECT) {
      goto cleanup;
    }
    thread_id = turbo_json_get_string(thread_json, "id");
    if (!thread_id || !thread_id[0]) {
      goto cleanup;
    }
    if (turbo_agent_runtime_get_thread_observability_index(runtime, thread_id, &index_json) != 0 ||
        !index_json ||
        turbo_agent_runtime_build_observability_index_summary(index_json, &summary_json) != 0 ||
        !summary_json) {
      turbo_free_json(&index_json);
      turbo_free_json(&summary_json);
      goto cleanup;
    }
    if (turbo_agent_runtime_observability_summary_matches_filters(summary_json, filters_json,
                                                                  &matches_filters) != 0) {
      turbo_free_json(&index_json);
      turbo_free_json(&summary_json);
      goto cleanup;
    }
    turbo_free_json(&index_json);
    if (!matches_filters) {
      turbo_free_json(&summary_json);
      continue;
    }
    turbo_json_array_add(indexes_json, summary_json);
    summary_json = NULL;
  }

  if (compare) {
    sorted_indexes_json = turbo_agent_runtime_sorted_json_array_clone(indexes_json, compare);
    if (!sorted_indexes_json) {
      goto cleanup;
    }
    turbo_free_json(&indexes_json);
    indexes_json = sorted_indexes_json;
    sorted_indexes_json = NULL;
  }

  if (limit >= 0) {
    limited_indexes_json = turbo_json_create_array();
    if (!limited_indexes_json) {
      goto cleanup;
    }
    count = turbo_json_array_size(indexes_json);
    if ((size_t)limit < count) {
      count = (size_t)limit;
    }
    for (i = 0; i < count; ++i) {
      json_value_t *clone_json = turbo_json_clone(turbo_json_array_get(indexes_json, i));

      if (!clone_json) {
        goto cleanup;
      }
      turbo_json_array_add(limited_indexes_json, clone_json);
    }
    turbo_free_json(&indexes_json);
    indexes_json = limited_indexes_json;
    limited_indexes_json = NULL;
  }

  *out_indexes_json = indexes_json;
  indexes_json = NULL;
  turbo_free_json(&threads_json);
  turbo_free_json(&sorted_threads_json);
  return 0;

cleanup:
  turbo_free_json(&threads_json);
  turbo_free_json(&sorted_threads_json);
  turbo_free_json(&sorted_indexes_json);
  turbo_free_json(&limited_indexes_json);
  turbo_free_json(&indexes_json);
  return -1;
}

CXX_C_API int turbo_agent_runtime_list_observability_indexes(turbo_agent_runtime_t *runtime,
                                                             json_value_t **out_indexes_json) {
  return turbo_agent_runtime_list_observability_indexes_filtered(runtime, NULL, out_indexes_json);
}

CXX_C_API int turbo_agent_runtime_apply_command_json_value(turbo_agent_runtime_t *runtime,
                                                           const char *checkpoint_id,
                                                           const json_value_t *command,
                                                           json_value_t **out_state_override) {
  return turbo_agent_runtime_prepare_checkpoint_command_override_json_value(
      runtime, checkpoint_id, command, out_state_override);
}

CXX_C_API int turbo_agent_runtime_prepare_checkpoint_command_override_json_value(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id, const json_value_t *command,
    json_value_t **out_state_override) {
  json_value_t *state = NULL;
  json_value_t *state_json = NULL;
  json_value_t *command_json = NULL;
  json_value_t *updated_state = NULL;
  int rc = -1;

  if (!runtime || !checkpoint_id || !command || !out_state_override) {
    return -1;
  }
  *out_state_override = NULL;
  if (turbo_agent_runtime_get_checkpoint_state_json_value(runtime, checkpoint_id, &state) != 0 ||
      !state) {
    goto cleanup;
  }
  state_json = turbo_json_clone(state);
  command_json = turbo_json_clone(command);
  if (!state_json || !command_json ||
      turbo_agent_runtime_apply_command_json(state_json, command_json) != 0) {
    goto cleanup;
  }
  updated_state = turbo_json_clone(state_json);
  if (!updated_state) {
    goto cleanup;
  }
  *out_state_override = updated_state;
  updated_state = NULL;
  rc = 0;

cleanup:
  turbo_runtime_json_destroy(updated_state);
  turbo_free_json(&command_json);
  turbo_free_json(&state_json);
  turbo_runtime_json_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_runtime_prepare_thread_command_override_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id, const json_value_t *command,
    json_value_t **out_state_override) {
  char *checkpoint_id = NULL;
  int rc;

  if (!runtime || !thread_id || !thread_id[0] || !command || !out_state_override) {
    return -1;
  }
  rc = turbo_agent_runtime_current_checkpoint_id_for_thread(runtime, thread_id, &checkpoint_id);
  if (rc != 0 || !checkpoint_id) {
    free(checkpoint_id);
    return -1;
  }
  rc = turbo_agent_runtime_prepare_checkpoint_command_override_json_value(
      runtime, checkpoint_id, command, out_state_override);
  free(checkpoint_id);
  return rc;
}

CXX_C_API int turbo_agent_runtime_apply_state_patch_json_value(turbo_agent_runtime_t *runtime,
                                                               const char *checkpoint_id,
                                                               const json_value_t *state_patch,
                                                               json_value_t **out_state_override) {
  return turbo_agent_runtime_prepare_checkpoint_state_override_json_value(
      runtime, checkpoint_id, state_patch, out_state_override);
}

CXX_C_API int turbo_agent_runtime_prepare_checkpoint_state_override_json_value(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id, const json_value_t *state_patch,
    json_value_t **out_state_override) {
  json_value_t *state = NULL;
  int rc;

  if (!runtime || !checkpoint_id || !checkpoint_id[0] || !state_patch || !out_state_override) {
    return -1;
  }
  *out_state_override = NULL;
  rc = turbo_agent_runtime_get_checkpoint_state_json_value(runtime, checkpoint_id, &state);
  if (rc != 0 || !state) {
    turbo_runtime_json_destroy(state);
    return -1;
  }
  rc = turbo_agent_runtime_apply_state_patch_to_json_value(state, state_patch, out_state_override);
  turbo_runtime_json_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_runtime_prepare_thread_state_override_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id, const json_value_t *state_patch,
    json_value_t **out_state_override) {
  char *checkpoint_id = NULL;
  int rc;

  if (!runtime || !thread_id || !thread_id[0] || !state_patch || !out_state_override) {
    return -1;
  }
  rc = turbo_agent_runtime_current_checkpoint_id_for_thread(runtime, thread_id, &checkpoint_id);
  if (rc != 0 || !checkpoint_id) {
    free(checkpoint_id);
    return -1;
  }
  rc = turbo_agent_runtime_prepare_checkpoint_state_override_json_value(
      runtime, checkpoint_id, state_patch, out_state_override);
  free(checkpoint_id);
  return rc;
}

/* ── Unified API Implementations ───────────────────────────────────────────── */

static int turbo_agent_runtime_exec_resolve_override(
    turbo_agent_runtime_t *runtime, const turbo_agent_runtime_exec_options_t *exec_options,
    const json_value_t *input, char **out_checkpoint_id, json_value_t **out_override) {
  char *checkpoint_id = NULL;
  int rc;

  if (!exec_options) return -1;

  if (exec_options->scope == TURBO_RUNTIME_SCOPE_THREAD) {
    if (!exec_options->thread_id) return -1;
    rc = turbo_agent_runtime_current_checkpoint_id_for_thread(runtime, exec_options->thread_id,
                                                              &checkpoint_id);
    if (rc != 0 || !checkpoint_id) return -1;
  } else {
    if (!exec_options->checkpoint_id) return -1;
    checkpoint_id = turbo_agent_runtime_strdup(exec_options->checkpoint_id);
    if (!checkpoint_id) return -1;
  }

  if (exec_options->input_kind == TURBO_RUNTIME_INPUT_OVERRIDE) {
    *out_override = NULL;
    if (input) {
      *out_override = turbo_json_clone(input);
      if (!*out_override) {
        free(checkpoint_id);
        return -1;
      }
    }
  } else if (exec_options->input_kind == TURBO_RUNTIME_INPUT_PATCH) {
    rc = turbo_agent_runtime_prepare_checkpoint_state_override_json_value(runtime, checkpoint_id,
                                                                          input, out_override);
    if (rc != 0) {
      free(checkpoint_id);
      return -1;
    }
  } else if (exec_options->input_kind == TURBO_RUNTIME_INPUT_COMMAND) {
    rc = turbo_agent_runtime_prepare_checkpoint_command_override_json_value(runtime, checkpoint_id,
                                                                            input, out_override);
    if (rc != 0) {
      free(checkpoint_id);
      return -1;
    }
  } else {
    free(checkpoint_id);
    return -1;
  }

  *out_checkpoint_id = checkpoint_id;
  return 0;
}

CXX_C_API int turbo_agent_runtime_exec_start(turbo_agent_runtime_t *runtime, turbo_graph_t *graph,
                                             const json_value_t *state,
                                             const turbo_graph_run_options_t *options,
                                             const turbo_agent_runtime_exec_options_t *exec_options,
                                             json_value_t **out_summary_json,
                                             json_value_t **out_state) {
  return turbo_agent_runtime_exec_start_controlled(runtime, graph, state, options, exec_options,
                                                   NULL, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_runtime_exec_start_controlled(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const json_value_t *state,
    const turbo_graph_run_options_t *options,
    const turbo_agent_runtime_exec_options_t *exec_options,
    const turbo_cancel_token_t *cancel_token, json_value_t **out_summary_json,
    json_value_t **out_state) {
  const char *thread_id = NULL;
  turbo_event_sink_json_value_fn event_sink = NULL;
  void *event_sink_user_data = NULL;
  const turbo_agent_runtime_parent_link_t *parent_link = NULL;

  if (exec_options) {
    thread_id = exec_options->thread_id;
    event_sink = exec_options->event_sink;
    event_sink_user_data = exec_options->event_sink_user_data;
    parent_link = exec_options->parent_link;
  }

  return turbo_agent_runtime_start_json_value_graph_linked_stream_controlled(
      runtime, graph, state, options, thread_id, parent_link, cancel_token, event_sink,
      event_sink_user_data, out_summary_json, out_state);
}

CXX_C_API int
turbo_agent_runtime_exec_resume(turbo_agent_runtime_t *runtime, turbo_graph_t *graph,
                                const json_value_t *input, const turbo_graph_run_options_t *options,
                                const turbo_agent_runtime_exec_options_t *exec_options,
                                json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_runtime_exec_resume_controlled(runtime, graph, input, options, exec_options,
                                                    NULL, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_runtime_exec_resume_controlled(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const json_value_t *input,
    const turbo_graph_run_options_t *options,
    const turbo_agent_runtime_exec_options_t *exec_options,
    const turbo_cancel_token_t *cancel_token, json_value_t **out_summary_json,
    json_value_t **out_state) {
  char *checkpoint_id = NULL;
  json_value_t *override_state = NULL;
  turbo_event_sink_json_value_fn event_sink = NULL;
  void *event_sink_user_data = NULL;
  int rc;

  if (!runtime || !graph || !exec_options) {
    return -1;
  }

  rc = turbo_agent_runtime_exec_resolve_override(runtime, exec_options, input, &checkpoint_id,
                                                 &override_state);
  if (rc != 0) {
    return -1;
  }

  event_sink = exec_options->event_sink;
  event_sink_user_data = exec_options->event_sink_user_data;

  rc = turbo_agent_runtime_resume_json_value_graph_stream_controlled(
      runtime, graph, checkpoint_id, override_state, options, cancel_token, event_sink,
      event_sink_user_data, out_summary_json, out_state);

  turbo_runtime_json_destroy(override_state);
  free(checkpoint_id);
  return rc;
}

CXX_C_API int turbo_agent_runtime_exec_fork(turbo_agent_runtime_t *runtime, turbo_graph_t *graph,
                                            const json_value_t *input,
                                            const turbo_graph_run_options_t *options,
                                            const turbo_agent_runtime_exec_options_t *exec_options,
                                            json_value_t **out_summary_json,
                                            json_value_t **out_state) {
  return turbo_agent_runtime_exec_fork_controlled(runtime, graph, input, options, exec_options,
                                                  NULL, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_runtime_exec_fork_controlled(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const json_value_t *input,
    const turbo_graph_run_options_t *options,
    const turbo_agent_runtime_exec_options_t *exec_options,
    const turbo_cancel_token_t *cancel_token, json_value_t **out_summary_json,
    json_value_t **out_state) {
  char *checkpoint_id = NULL;
  json_value_t *override_state = NULL;
  turbo_event_sink_json_value_fn event_sink = NULL;
  void *event_sink_user_data = NULL;
  int rc;

  if (!runtime || !graph || !exec_options) {
    return -1;
  }

  rc = turbo_agent_runtime_exec_resolve_override(runtime, exec_options, input, &checkpoint_id,
                                                 &override_state);
  if (rc != 0) {
    return -1;
  }

  event_sink = exec_options->event_sink;
  event_sink_user_data = exec_options->event_sink_user_data;

  rc = turbo_agent_runtime_fork_json_value_graph_stream_controlled(
      runtime, graph, checkpoint_id, override_state, options, cancel_token, event_sink,
      event_sink_user_data, out_summary_json, out_state);

  turbo_runtime_json_destroy(override_state);
  free(checkpoint_id);
  return rc;
}
