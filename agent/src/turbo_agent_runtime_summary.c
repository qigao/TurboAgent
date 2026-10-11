#include "turbo_agent_runtime_v1_internal.h"

/* Summary/descriptor construction owns each temporary until a checked transfer.
 * On failure the unpublished tree and every untransferred child are released. */
static void summary_free(json_value_t **value) {
  json_free(*value);
  *value = NULL;
}

static int summary_take(json_value_t *object, const char *key, json_value_t **value) {
  if (turbo_runtime_json_object_set(object, key, *value) != 0) return -1;
  *value = NULL;
  return 0;
}

/* Fresh scalar values are consumed on both success and failure. */
static int summary_set_value(json_value_t *object, const char *key, json_value_t *value) {
  if (turbo_runtime_json_object_set(object, key, value) != 0) {
    json_free(value);
    return -1;
  }
  return 0;
}

static int summary_append_value(json_value_t *array, json_value_t *value) {
  if (turbo_runtime_json_array_append(array, value) != 0) {
    json_free(value);
    return -1;
  }
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
  json_value_t *schema = json_create_object();
  json_value_t *properties = NULL;
  json_value_t *required = NULL;

  if (!schema) {
    return NULL;
  }
  properties = json_create_object();
  required = json_create_array();
  if (!properties || !required) {
    summary_free(&properties);
    summary_free(&required);
    summary_free(&schema);
    return NULL;
  }
  if (turbo_runtime_json_object_set_string(schema, "type", "object") != 0) goto fail;
  if (summary_take(schema, "properties", &properties) != 0) goto fail;
  if (summary_take(schema, "required", &required) != 0) goto fail;
  return schema;

fail:
  json_free(properties);
  json_free(required);
  json_free(schema);
  return NULL;
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
  properties = json_object_get(schema, "properties");
  required_array = json_object_get(schema, "required");
  if (!properties || !required_array) {
    return -1;
  }
  property = json_create_object();
  if (!property) {
    return -1;
  }
  if (turbo_runtime_json_object_set_string(property, "type", type) != 0) goto fail;
  if (description && description[0] != '\0') {
    if (turbo_runtime_json_object_set_string(property, "description", description) != 0) goto fail;
  }
  if (summary_take(properties, name, &property) != 0) goto fail;
  if (required) {
    if (summary_append_value(required_array, json_create_string(name)) != 0) goto fail;
  }
  return 0;

fail:
  json_free(property);
  return -1;
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
  descriptor = json_create_object();
  args_schema = turbo_agent_runtime_create_args_schema();
  accepted_keys = NULL;
  example_payload = NULL;
  if (!descriptor || !args_schema) {
    summary_free(&descriptor);
    summary_free(&args_schema);
    return NULL;
  }
  accepted_keys = turbo_agent_runtime_string_array(spec->accepted_keys, spec->accepted_key_count);
  if (!accepted_keys) {
    summary_free(&descriptor);
    summary_free(&args_schema);
    return NULL;
  }
  if (spec->example_payload_json && spec->example_payload_json[0] != '\0' &&
      turbo_agent_runtime_parse_json_string(spec->example_payload_json, &example_payload) != 0) {
    summary_free(&accepted_keys);
    summary_free(&descriptor);
    summary_free(&args_schema);
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

  if (turbo_runtime_json_object_set_string(descriptor, "name", spec->name) != 0) goto fail;
  if (turbo_runtime_json_object_set_string(descriptor, "label", spec->label) != 0) goto fail;
  if (turbo_runtime_json_object_set_string(descriptor, "suggested_title", spec->label) != 0)
    goto fail;
  if (turbo_runtime_json_object_set_string(descriptor, "description", spec->description) != 0)
    goto fail;
  if (turbo_runtime_json_object_set_string(descriptor, "category", spec->category) != 0) goto fail;
  if (turbo_runtime_json_object_set_string(descriptor, "input_mode", spec->input_mode) != 0)
    goto fail;
  if (spec->placeholder && spec->placeholder[0] != '\0') {
    if (turbo_runtime_json_object_set_string(descriptor, "placeholder", spec->placeholder) != 0)
      goto fail;
  } else {
    if (summary_set_value(descriptor, "placeholder", json_create_null()) != 0) goto fail;
  }
  if (spec->success_state_hint && spec->success_state_hint[0] != '\0') {
    if (turbo_runtime_json_object_set_string(descriptor, "success_state_hint",
                                             spec->success_state_hint) != 0)
      goto fail;
  } else {
    if (summary_set_value(descriptor, "success_state_hint", json_create_null()) != 0) goto fail;
  }
  if (example_payload) {
    if (summary_take(descriptor, "example_payload", &example_payload) != 0) goto fail;
  } else {
    if (summary_set_value(descriptor, "example_payload", json_create_null()) != 0) goto fail;
  }
  if (turbo_runtime_json_object_set_string(descriptor, "resume_mode", spec->resume_mode) != 0)
    goto fail;
  if (turbo_runtime_json_object_set_string(descriptor, "primary_key", spec->primary_key) != 0)
    goto fail;
  if (summary_take(descriptor, "accepted_keys", &accepted_keys) != 0) goto fail;
  if (summary_set_value(descriptor, "requires_input",
                        json_create_bool(spec->requires_input ? true : false)) != 0)
    goto fail;
  if (summary_set_value(descriptor, "supports_json_value",
                        json_create_bool(spec->supports_json_value ? true : false)) != 0)
    goto fail;
  if (summary_take(descriptor, "args_schema", &args_schema) != 0) goto fail;
  return descriptor;

fail:
  summary_free(&example_payload);
  summary_free(&accepted_keys);
  summary_free(&args_schema);
  summary_free(&descriptor);
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
  array = json_create_array();
  if (!array) {
    return NULL;
  }
  for (i = 0; i < count; ++i) {
    if (summary_append_value(array, json_create_string(values[i])) != 0) goto fail;
  }
  return array;

fail:
  json_free(array);
  return NULL;
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
  if (summary_append_value(available_commands, json_create_string(name)) != 0 ||
      turbo_runtime_json_array_append(available_command_descriptors, descriptor) != 0) {
    json_free(descriptor);
    return -1;
  }
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

int turbo_agent_runtime_make_summary(const char *thread_id, const char *run_id, const char *status,
                                     const char *checkpoint_id, const char *parent_agent_run_id,
                                     const char *parent_tool_call_id, const char *parent_tool_name,
                                     const char *parent_graph_run_id, const char *call_frame_id,
                                     const turbo_graph_run_result_t *result,
                                     const json_value_t *state_json,
                                     json_value_t **out_summary_json) {
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

  if (out_summary_json) *out_summary_json = NULL;
  if (!thread_id || !run_id || !status || !result || !out_summary_json) {
    return -1;
  }

  summary = json_create_object();
  if (!summary) {
    return -1;
  }

  if (turbo_agent_runtime_result_to_json(result, &result_json) != 0) {
    summary_free(&summary);
    return -1;
  }

  if (turbo_runtime_json_object_set_string(summary, "thread_id", thread_id) != 0) goto fail;
  if (turbo_runtime_json_object_set_string(summary, "run_id", run_id) != 0) goto fail;
  if (turbo_runtime_json_object_set_string(summary, "status", status) != 0) goto fail;
  if (checkpoint_id) {
    if (turbo_runtime_json_object_set_string(summary, "checkpoint_id", checkpoint_id) != 0)
      goto fail;
  } else {
    if (summary_set_value(summary, "checkpoint_id", json_create_null()) != 0) goto fail;
  }
  if (parent_agent_run_id) {
    if (turbo_runtime_json_object_set_string(summary, "parent_agent_run_id", parent_agent_run_id) !=
        0)
      goto fail;
  } else {
    if (summary_set_value(summary, "parent_agent_run_id", json_create_null()) != 0) goto fail;
  }
  if (parent_tool_call_id) {
    if (turbo_runtime_json_object_set_string(summary, "parent_tool_call_id", parent_tool_call_id) !=
        0)
      goto fail;
  } else {
    if (summary_set_value(summary, "parent_tool_call_id", json_create_null()) != 0) goto fail;
  }
  if (parent_tool_name) {
    if (turbo_runtime_json_object_set_string(summary, "parent_tool_name", parent_tool_name) != 0)
      goto fail;
  } else {
    if (summary_set_value(summary, "parent_tool_name", json_create_null()) != 0) goto fail;
  }
  if (parent_graph_run_id) {
    if (turbo_runtime_json_object_set_string(summary, "parent_graph_run_id", parent_graph_run_id) !=
        0)
      goto fail;
  } else {
    if (summary_set_value(summary, "parent_graph_run_id", json_create_null()) != 0) goto fail;
  }
  if (call_frame_id) {
    if (turbo_runtime_json_object_set_string(summary, "call_frame_id", call_frame_id) != 0)
      goto fail;
  } else {
    if (summary_set_value(summary, "call_frame_id", json_create_null()) != 0) goto fail;
  }
  if (state_json) {
    active_agent = turbo_agent_state_active_agent(state_json);
    handoff_target_agent = turbo_agent_state_handoff_target_agent(state_json);
    handoff_reason = turbo_agent_state_handoff_reason(state_json);
  }
  if (active_agent && active_agent[0] != '\0') {
    if (turbo_runtime_json_object_set_string(summary, "active_agent", active_agent) != 0) goto fail;
  } else {
    if (summary_set_value(summary, "active_agent", json_create_null()) != 0) goto fail;
  }
  if (handoff_target_agent && handoff_target_agent[0] != '\0') {
    if (turbo_runtime_json_object_set_string(summary, "handoff_target_agent",
                                             handoff_target_agent) != 0)
      goto fail;
  } else {
    if (summary_set_value(summary, "handoff_target_agent", json_create_null()) != 0) goto fail;
  }
  if (handoff_reason && handoff_reason[0] != '\0') {
    if (turbo_runtime_json_object_set_string(summary, "handoff_reason", handoff_reason) != 0)
      goto fail;
  } else {
    if (summary_set_value(summary, "handoff_reason", json_create_null()) != 0) goto fail;
  }
  if (strcmp(status, "interrupted") == 0 && result->next_node && result->next_node[0] != '\0') {
    if (turbo_runtime_json_object_set_string(summary, "pending_node", result->next_node) != 0)
      goto fail;
  } else {
    if (summary_set_value(summary, "pending_node", json_create_null()) != 0) goto fail;
  }

  available_commands = json_create_array();
  available_command_descriptors = json_create_array();
  if (!available_commands || !available_command_descriptors) {
    summary_free(&result_json);
    summary_free(&available_commands);
    summary_free(&available_command_descriptors);
    summary_free(&summary);
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
    if (turbo_runtime_json_object_set_string(summary, "interrupt_reason", interrupt_reason) != 0)
      goto fail;
  } else {
    if (summary_set_value(summary, "interrupt_reason", json_create_null()) != 0) goto fail;
  }
  if (pending_action) {
    if (turbo_runtime_json_object_set_string(summary, "pending_action", pending_action) != 0)
      goto fail;
  } else {
    if (summary_set_value(summary, "pending_action", json_create_null()) != 0) goto fail;
  }
  if (summary_take(summary, "available_commands", &available_commands) != 0) goto fail;
  if (summary_take(summary, "available_command_descriptors", &available_command_descriptors) != 0)
    goto fail;
  if (summary_take(summary, "result", &result_json) != 0) goto fail;
  free(owned_executor_failure_reason);
  *out_summary_json = summary;
  return 0;

fail:
  free(owned_executor_failure_reason);
  summary_free(&result_json);
  summary_free(&available_commands);
  summary_free(&available_command_descriptors);
  summary_free(&summary);
  return -1;
}
