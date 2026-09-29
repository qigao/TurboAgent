#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include <json_parser.h>
#include "turbo_agent_state_core_internal.h"
#include "turbo_agent_state_flow_domain_internal.h"
#include "turbo_agent_state_json_value_internal.h"
#include "turbo_agent_state_memory_internal.h"
#include "turbo_agent_state_snapshot_internal.h"
#include "turbo_agent_event_internal.h"
#include "turbo_agent_runtime_internal.h"

#include <stdlib.h>

static json_value_t *turbo_agent_state_snapshot_to_json_object(
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

CXX_C_API json_value_t *turbo_agent_state_ensure_versioned_substate(
    json_value_t *root_state, const char *state_key, const char *legacy_input_key,
    const char *event_versions_key) {
  json_value_t *substate;
  json_value_t *input;
  json_value_t *events;
  json_value_t *input_copy = NULL;
  json_value_t *events_copy = NULL;
  const json_value_t *existing_events;

  if (!root_state || !state_key || !event_versions_key) {
    return NULL;
  }

  substate = turbo_agent_state_get_current_object_version(root_state, state_key);
  if (substate && json_type(substate) == JSON_OBJECT) {
    input = turbo_agent_state_get_array(substate, "input");
    events = turbo_agent_state_get_array(substate, "events");
    if (input && events && json_type(input) == JSON_ARRAY &&
        json_type(events) == JSON_ARRAY) {
      return substate;
    }
  }

  if (!legacy_input_key) {
    return NULL;
  }

  input = turbo_agent_state_get_current_array_version(root_state, legacy_input_key);
  existing_events =
      turbo_agent_state_get_current_array_version_const(root_state, event_versions_key);
  if (!input || json_type(input) != JSON_ARRAY) {
    return NULL;
  }

  if (existing_events && json_type(existing_events) == JSON_ARRAY) {
    if (turbo_agent_clone_json(existing_events, &events_copy) != TURBO_GRAPH_EXEC_OK) {
      return NULL;
    }
  } else {
    events_copy = json_create_array();
    if (!events_copy) {
      return NULL;
    }
  }

  if (turbo_agent_clone_json(input, &input_copy) != TURBO_GRAPH_EXEC_OK) {
    json_free(events_copy); events_copy = NULL;
    return NULL;
  }

  substate = turbo_agent_substate_create(input_copy, events_copy);
  if (!substate) {
    json_free(input_copy); input_copy = NULL;
    json_free(events_copy); events_copy = NULL;
    return NULL;
  }

  if (turbo_agent_state_append_object_version(root_state, state_key, substate) != 0) {
    json_free(substate); substate = NULL;
    return NULL;
  }

  return turbo_agent_state_get_current_object_version(root_state, state_key);
}

CXX_C_API json_value_t *turbo_agent_substate_create(json_value_t *input, json_value_t *events) {
  json_value_t *substate;

  if (!input || !events || json_type(input) != JSON_ARRAY ||
      json_type(events) != JSON_ARRAY) {
    return NULL;
  }

  substate = json_create_object();
  if (!substate) {
    return NULL;
  }

  json_object_add(substate, "input", input);
  json_object_add(substate, "events", events);
  return substate;
}

CXX_C_API int turbo_agent_append_last_substate_event(json_value_t *root_state,
                                                     const char *event_versions_key,
                                                     const json_value_t *substate) {
  json_value_t *version_events;
  const json_value_t *last_event;

  if (!root_state || !event_versions_key || !substate) {
    return -1;
  }

  last_event = turbo_agent_state_last_event(substate);
  if (!last_event) {
    return -1;
  }

  version_events = turbo_agent_state_get_current_array_version(root_state, event_versions_key);
  if (!version_events || json_type(version_events) != JSON_ARRAY) {
    json_value_t *seed = json_create_array();
    if (!seed) {
      return -1;
    }
    if (turbo_agent_state_append_array_version(root_state, event_versions_key, seed) != 0) {
      json_free(seed); seed = NULL;
      return -1;
    }
    version_events = turbo_agent_state_get_current_array_version(root_state, event_versions_key);
    if (!version_events || json_type(version_events) != JSON_ARRAY) {
      return -1;
    }
  }

  {
    json_value_t *clone = NULL;
    if (turbo_agent_clone_json(last_event, &clone) != TURBO_GRAPH_EXEC_OK) {
      return -1;
    }
    json_array_add(version_events, clone);
  }

  {
    json_value_t *clone = NULL;
    if (turbo_agent_clone_json(last_event, &clone) != TURBO_GRAPH_EXEC_OK) {
      return -1;
    }
    return turbo_agent_append_event(root_state, clone);
  }
}

json_value_t *turbo_agent_state_control_snapshot_impl(const json_value_t *state) {
  json_value_t *snapshot;
  json_value_t *replan;
  json_value_t *review;
  json_value_t *failure;
  json_value_t *model_error;
  json_value_t *guardrail;
  json_value_t *supervisor;
  const json_value_t *inbox;
  const json_value_t *history;

  if (!state) {
    return NULL;
  }

  snapshot = json_create_object();
  replan = json_create_object();
  review = json_create_object();
  failure = json_create_object();
  model_error = json_create_object();
  guardrail = json_create_object();
  supervisor = json_create_object();
  if (!snapshot || !replan || !review || !failure || !model_error || !guardrail || !supervisor) {
    json_free(snapshot); snapshot = NULL;
    json_free(replan); replan = NULL;
    json_free(review); review = NULL;
    json_free(failure); failure = NULL;
    json_free(model_error); model_error = NULL;
    json_free(guardrail); guardrail = NULL;
    json_free(supervisor); supervisor = NULL;
    return NULL;
  }

  json_object_set_number(snapshot, "state_version",
                               (double)turbo_agent_state_version(state));

  json_object_set_bool(replan, "requested",
                             turbo_agent_state_replan_requested(state) ? true : false);
  json_object_set_number(replan, "count",
                               (double)turbo_agent_state_replan_count(state));
  json_object_set_number(replan, "limit",
                               (double)turbo_agent_state_replan_limit(state));
  json_object_set_string(replan, "reason",
                               turbo_agent_state_replan_reason(state)
                                   ? turbo_agent_state_replan_reason(state)
                                   : "");

  json_object_set_bool(review, "required",
                             turbo_agent_state_review_required(state) ? true : false);
  json_object_set_bool(review, "approved",
                             turbo_agent_state_review_approved(state) ? true : false);
  json_object_set_string(review, "note",
                               turbo_agent_state_review_note(state)
                                   ? turbo_agent_state_review_note(state)
                                   : "");

  json_object_set_string(failure, "kind",
                               turbo_agent_state_failure_kind(state)
                                   ? turbo_agent_state_failure_kind(state)
                                   : "");
  json_object_set_string(failure, "reason",
                               turbo_agent_state_failure_reason(state)
                                   ? turbo_agent_state_failure_reason(state)
                                   : "");

  json_object_set_string(model_error, "phase",
                               turbo_agent_state_model_error_phase(state)
                                   ? turbo_agent_state_model_error_phase(state)
                                   : "");
  json_object_set_string(model_error, "detail",
                               turbo_agent_state_model_error_detail(state)
                                   ? turbo_agent_state_model_error_detail(state)
                                   : "");

  json_object_set_string(guardrail, "phase",
                               turbo_agent_state_guardrail_rejection_phase(state)
                                   ? turbo_agent_state_guardrail_rejection_phase(state)
                                   : "");
  json_object_set_string(guardrail, "reason",
                               turbo_agent_state_guardrail_rejection_reason(state)
                                   ? turbo_agent_state_guardrail_rejection_reason(state)
                                   : "");
  inbox = turbo_agent_state_supervisor_inbox(state);
  history = turbo_agent_state_supervisor_handoff_history(state);
  json_object_set_string(supervisor, "active_agent",
                               turbo_agent_state_active_agent(state)
                                   ? turbo_agent_state_active_agent(state)
                                   : "");
  json_object_set_string(supervisor, "target_agent",
                               turbo_agent_state_handoff_target_agent(state)
                                   ? turbo_agent_state_handoff_target_agent(state)
                                   : "");
  json_object_set_string(supervisor, "handoff_reason",
                               turbo_agent_state_handoff_reason(state)
                                   ? turbo_agent_state_handoff_reason(state)
                                   : "");
  json_object_set_number(
      supervisor, "inbox_count",
      (double)(inbox && json_type(inbox) == JSON_ARRAY ? json_array_size(inbox)
                                                                   : 0));
  json_object_set_number(supervisor, "handoff_count",
                               (double)(history && json_type(history) == JSON_ARRAY
                                            ? json_array_size(history)
                                            : 0));

  json_object_add(snapshot, "replan", replan);
  json_object_add(snapshot, "review", review);
  json_object_add(snapshot, "failure", failure);
  json_object_add(snapshot, "model_error", model_error);
  json_object_add(snapshot, "guardrail", guardrail);
  json_object_add(snapshot, "supervisor", supervisor);
  return snapshot;
}

json_value_t *
turbo_agent_state_control_snapshot_json_value_impl(const json_value_t *state) {
  json_value_t *json_state = turbo_agent_state_snapshot_to_json_object(state);
  json_value_t *snapshot;
  json_value_t *bound;

  if (!json_state) {
    return NULL;
  }

  snapshot = turbo_agent_state_control_snapshot(json_state);
  json_free(json_state); json_state = NULL;
  if (!snapshot) {
    return NULL;
  }

  bound = json_clone(snapshot);
  json_free(snapshot); snapshot = NULL;
  return bound;
}

json_value_t *turbo_agent_state_workflow_snapshot_impl(const json_value_t *state) {
  json_value_t *snapshot;
  json_value_t *control = NULL;
  json_value_t *plan_clone = NULL;
  json_value_t *completed_steps_clone = NULL;
  json_value_t *events_clone = NULL;
  json_value_t *trace_events_clone = NULL;
  json_value_t *planner_versions_clone = NULL;
  json_value_t *executor_versions_clone = NULL;
  json_value_t *memory_context_clone = NULL;
  json_value_t *supervisor_clone = NULL;
  char *memory_context_text = NULL;

  if (!state) {
    return NULL;
  }

  snapshot = json_create_object();
  if (!snapshot) {
    return NULL;
  }

  control = turbo_agent_state_control_snapshot(state);
  if (!control) {
    json_free(snapshot); snapshot = NULL;
    return NULL;
  }

  if (turbo_agent_state_plan(state) &&
      turbo_agent_clone_json(turbo_agent_state_plan(state), &plan_clone) !=
          TURBO_GRAPH_EXEC_OK) {
    json_free(control); control = NULL;
    json_free(snapshot); snapshot = NULL;
    return NULL;
  }
  if (turbo_agent_state_completed_steps(state) &&
      turbo_agent_clone_json(turbo_agent_state_completed_steps(state), &completed_steps_clone) !=
          TURBO_GRAPH_EXEC_OK) {
    json_free(plan_clone); plan_clone = NULL;
    json_free(control); control = NULL;
    json_free(snapshot); snapshot = NULL;
    return NULL;
  }
  if (turbo_agent_state_events(state) &&
      turbo_agent_clone_json(turbo_agent_state_events(state), &events_clone) !=
          TURBO_GRAPH_EXEC_OK) {
    json_free(completed_steps_clone); completed_steps_clone = NULL;
    json_free(plan_clone); plan_clone = NULL;
    json_free(control); control = NULL;
    json_free(snapshot); snapshot = NULL;
    return NULL;
  }
  if (turbo_agent_state_trace_events(state) &&
      turbo_agent_clone_json(turbo_agent_state_trace_events(state), &trace_events_clone) !=
          TURBO_GRAPH_EXEC_OK) {
    json_free(events_clone); events_clone = NULL;
    json_free(completed_steps_clone); completed_steps_clone = NULL;
    json_free(plan_clone); plan_clone = NULL;
    json_free(control); control = NULL;
    json_free(snapshot); snapshot = NULL;
    return NULL;
  }
  if (turbo_agent_state_planner_event_versions(state) &&
      turbo_agent_clone_json(turbo_agent_state_planner_event_versions(state),
                             &planner_versions_clone) != TURBO_GRAPH_EXEC_OK) {
    json_free(trace_events_clone); trace_events_clone = NULL;
    json_free(events_clone); events_clone = NULL;
    json_free(completed_steps_clone); completed_steps_clone = NULL;
    json_free(plan_clone); plan_clone = NULL;
    json_free(control); control = NULL;
    json_free(snapshot); snapshot = NULL;
    return NULL;
  }
  if (turbo_agent_state_executor_event_versions(state) &&
      turbo_agent_clone_json(turbo_agent_state_executor_event_versions(state),
                             &executor_versions_clone) != TURBO_GRAPH_EXEC_OK) {
    json_free(planner_versions_clone); planner_versions_clone = NULL;
    json_free(trace_events_clone); trace_events_clone = NULL;
    json_free(events_clone); events_clone = NULL;
    json_free(completed_steps_clone); completed_steps_clone = NULL;
    json_free(plan_clone); plan_clone = NULL;
    json_free(control); control = NULL;
    json_free(snapshot); snapshot = NULL;
    return NULL;
  }
  if (turbo_agent_state_memory_context(state) &&
      turbo_agent_clone_json(turbo_agent_state_memory_context(state), &memory_context_clone) !=
          TURBO_GRAPH_EXEC_OK) {
    json_free(executor_versions_clone); executor_versions_clone = NULL;
    json_free(planner_versions_clone); planner_versions_clone = NULL;
    json_free(trace_events_clone); trace_events_clone = NULL;
    json_free(events_clone); events_clone = NULL;
    json_free(completed_steps_clone); completed_steps_clone = NULL;
    json_free(plan_clone); plan_clone = NULL;
    json_free(control); control = NULL;
    json_free(snapshot); snapshot = NULL;
    return NULL;
  }
  if (turbo_agent_state_get_current_object_version_const(state, "supervisor_versions") &&
      turbo_agent_clone_json(
          turbo_agent_state_get_current_object_version_const(state, "supervisor_versions"),
          &supervisor_clone) != TURBO_GRAPH_EXEC_OK) {
    json_free(memory_context_clone); memory_context_clone = NULL;
    json_free(executor_versions_clone); executor_versions_clone = NULL;
    json_free(planner_versions_clone); planner_versions_clone = NULL;
    json_free(trace_events_clone); trace_events_clone = NULL;
    json_free(events_clone); events_clone = NULL;
    json_free(completed_steps_clone); completed_steps_clone = NULL;
    json_free(plan_clone); plan_clone = NULL;
    json_free(control); control = NULL;
    json_free(snapshot); snapshot = NULL;
    return NULL;
  }

  memory_context_text = turbo_agent_state_memory_context_text(state);
  if (turbo_agent_state_memory_layer_count(state) > 0 && !memory_context_text) {
    json_free(memory_context_clone); memory_context_clone = NULL;
    json_free(supervisor_clone); supervisor_clone = NULL;
    json_free(executor_versions_clone); executor_versions_clone = NULL;
    json_free(planner_versions_clone); planner_versions_clone = NULL;
    json_free(trace_events_clone); trace_events_clone = NULL;
    json_free(events_clone); events_clone = NULL;
    json_free(completed_steps_clone); completed_steps_clone = NULL;
    json_free(plan_clone); plan_clone = NULL;
    json_free(control); control = NULL;
    json_free(snapshot); snapshot = NULL;
    return NULL;
  }
  if (memory_context_clone && memory_context_text) {
    json_object_set_string(memory_context_clone, "merged_text", memory_context_text);
  }

  json_object_set_number(snapshot, "state_version",
                               (double)turbo_agent_state_version(state));
  json_object_set_bool(snapshot, "state_version_supported",
                             turbo_agent_state_version_supported(state) ? true : false);
  json_object_add(snapshot, "control", control);
  if (plan_clone) {
    json_object_add(snapshot, "plan", plan_clone);
  }
  if (completed_steps_clone) {
    json_object_add(snapshot, "completed_steps", completed_steps_clone);
  }
  if (events_clone) {
    json_object_add(snapshot, "events", events_clone);
  }
  if (trace_events_clone) {
    json_object_add(snapshot, "trace_events", trace_events_clone);
  }
  if (planner_versions_clone) {
    json_object_add(snapshot, "planner_event_versions", planner_versions_clone);
  }
  if (executor_versions_clone) {
    json_object_add(snapshot, "executor_event_versions", executor_versions_clone);
  }
  if (memory_context_clone) {
    json_object_add(snapshot, "memory_context", memory_context_clone);
  }
  if (supervisor_clone) {
    json_object_add(snapshot, "supervisor", supervisor_clone);
  }
  if (memory_context_text) {
    json_object_set_string(snapshot, "memory_context_text", memory_context_text);
  }
  free(memory_context_text);
  return snapshot;
}

json_value_t *
turbo_agent_state_workflow_snapshot_json_value_impl(const json_value_t *state) {
  json_value_t *json_state = turbo_agent_state_snapshot_to_json_object(state);
  json_value_t *snapshot;
  json_value_t *bound;

  if (!json_state) {
    return NULL;
  }

  snapshot = turbo_agent_state_workflow_snapshot(json_state);
  json_free(json_state); json_state = NULL;
  if (!snapshot) {
    return NULL;
  }

  bound = json_clone(snapshot);
  json_free(snapshot); snapshot = NULL;
  return bound;
}
