#include "tinytest.h"
#include "turbo_agent_state.h"
#include "turbo_event_log.h"

#include <stdlib.h>

spec("turbo agent state api") {

  it("should expose state helpers through the state-specific header") {
    void *schema_version = (void *)turbo_agent_state_schema_version;
    void *create_state = (void *)turbo_agent_state_create;
    void *create_state_json_value = (void *)turbo_agent_state_create_json_value;
    void *events = (void *)turbo_agent_state_events;
    void *plan = (void *)turbo_agent_state_plan;
    void *control_snapshot = (void *)turbo_agent_state_control_snapshot;
    void *control_snapshot_json_value = (void *)turbo_agent_state_control_snapshot_json_value;
    void *trace_events_json_value = (void *)turbo_agent_state_trace_events_json_value;
    void *workflow_snapshot_json_value = (void *)turbo_agent_state_workflow_snapshot_json_value;
    void *final_answer = (void *)turbo_agent_state_final_answer_text;
    void *set_active_agent = (void *)turbo_agent_state_set_active_agent;
    void *request_handoff = (void *)turbo_agent_state_request_handoff;
    void *commit_handoff = (void *)turbo_agent_state_commit_handoff;
    void *append_inbox = (void *)turbo_agent_state_append_supervisor_inbox_message;
    void *active_agent = (void *)turbo_agent_state_active_agent;
    void *handoff_target = (void *)turbo_agent_state_handoff_target_agent;
    void *handoff_reason = (void *)turbo_agent_state_handoff_reason;
    void *supervisor_inbox = (void *)turbo_agent_state_supervisor_inbox;
    void *supervisor_inbox_count = (void *)turbo_agent_state_supervisor_inbox_count;
    void *supervisor_inbox_at = (void *)turbo_agent_state_supervisor_inbox_at;
    void *supervisor_handoff_history = (void *)turbo_agent_state_supervisor_handoff_history;
    void *latest_handoff_event = (void *)turbo_agent_state_latest_handoff_event;
    void *handoff_event_phase = (void *)turbo_agent_state_handoff_event_phase;
    void *handoff_event_from_agent = (void *)turbo_agent_state_handoff_event_from_agent;
    void *handoff_event_target_agent = (void *)turbo_agent_state_handoff_event_target_agent;
    void *handoff_event_reason = (void *)turbo_agent_state_handoff_event_reason;
    void *handoff_event_active_agent = (void *)turbo_agent_state_handoff_event_active_agent;

    check_not_null(schema_version);
    check_not_null(create_state);
    check_not_null(create_state_json_value);
    check_not_null(events);
    check_not_null(plan);
    check_not_null(control_snapshot);
    check_not_null(control_snapshot_json_value);
    check_not_null(trace_events_json_value);
    check_not_null(workflow_snapshot_json_value);
    check_not_null(final_answer);
    check_not_null(set_active_agent);
    check_not_null(request_handoff);
    check_not_null(commit_handoff);
    check_not_null(append_inbox);
    check_not_null(active_agent);
    check_not_null(handoff_target);
    check_not_null(handoff_reason);
    check_not_null(supervisor_inbox);
    check_not_null(supervisor_inbox_count);
    check_not_null(supervisor_inbox_at);
    check_not_null(supervisor_handoff_history);
    check_not_null(latest_handoff_event);
    check_not_null(handoff_event_phase);
    check_not_null(handoff_event_from_agent);
    check_not_null(handoff_event_target_agent);
    check_not_null(handoff_event_reason);
    check_not_null(handoff_event_active_agent);
  }

  it("should build state and snapshots through TurboParser JSON") {
    json_value_t *state = turbo_agent_state_create_json_value();
    json_value_t *control = NULL;
    json_value_t *workflow = NULL;

    check_not_null(state);
    check_true(turbo_runtime_json_value_as_int64(
                   turbo_json_object_get(state, "state_version"), 0) > 0);

    control = turbo_agent_state_control_snapshot_json_value(state);
    workflow = turbo_agent_state_workflow_snapshot_json_value(state);

    check_not_null(control);
    check_not_null(workflow);
    check_true(turbo_runtime_json_value_as_int64(
                   turbo_json_object_get(control, "state_version"), 0) > 0);
    check_not_null(turbo_json_object_get(workflow, "control"));
    check_not_null(turbo_json_object_get(workflow, "events"));

    turbo_runtime_json_destroy(workflow);
    turbo_runtime_json_destroy(control);
    turbo_runtime_json_destroy(state);
  }

  it("should round-trip bind trace events from state into an event log") {
    json_value_t *state = turbo_agent_state_create_json_value();
    json_value_t *trace_events = turbo_json_create_array();
    json_value_t *bound_events = NULL;
    turbo_event_log_t *log = turbo_event_log_create();

    check_not_null(state);
    check_not_null(trace_events);
    check_not_null(log);
    check_int_eq(turbo_runtime_json_array_append(
                     trace_events, turbo_event_trace_create_json_value("agent.trace", "start", "a", 0)),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_array_append(
                     trace_events, turbo_event_trace_create_json_value("agent.trace", "finish", "a", 0)),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_object_set(state, "trace_events", trace_events),
                 TURBO_RUNTIME_JSON_OK);

    bound_events = turbo_agent_state_trace_events_json_value(state);
    check_not_null(bound_events);
    check_int_eq(turbo_event_log_load_events_json_value(log, bound_events), TURBO_EVENT_LOG_OK);
    check_size_eq(turbo_event_log_size(log), 2);
    check_str_eq(turbo_runtime_json_value_as_string(
                     turbo_json_object_get(turbo_event_log_get(log, 1), "detail")),
                 "finish");

    turbo_event_log_destroy(log);
    turbo_runtime_json_destroy(bound_events);
    turbo_runtime_json_destroy(state);
  }

  it("should append canonical trace events directly into TurboParser JSON-native state") {
    json_value_t *state = turbo_agent_state_create_json_value();
    json_value_t *event =
        turbo_event_trace_create_json_value("agent.trace", "finish", "done", 0);
    json_value_t *bound_events = NULL;

    check_not_null(state);
    check_not_null(event);
    check_int_eq(turbo_agent_state_add_trace_event_json_value(state, event), 0);

    bound_events = turbo_agent_state_trace_events_json_value(state);
    check_not_null(bound_events);
    check_size_eq(turbo_runtime_json_value_size(bound_events), 1);
    check_str_eq(turbo_runtime_json_value_as_string(turbo_json_object_get(
                     turbo_json_array_get(bound_events, 0), "detail")),
                 "finish");

    turbo_runtime_json_destroy(bound_events);
    turbo_runtime_json_destroy(event);
    turbo_runtime_json_destroy(state);
  }

  it("should capture canonical trace events through the bind sink helper") {
    json_value_t *state = turbo_agent_state_create_json_value();
    json_value_t *event =
        turbo_event_trace_create_json_value("agent.trace", "start", "run", 0);
    json_value_t *bound_events = NULL;

    check_not_null(state);
    check_not_null(event);
    turbo_agent_state_capture_trace_event_json_value(event, state);

    bound_events = turbo_agent_state_trace_events_json_value(state);
    check_not_null(bound_events);
    check_size_eq(turbo_runtime_json_value_size(bound_events), 1);
    check_str_eq(turbo_runtime_json_value_as_string(turbo_json_object_get(
                     turbo_json_array_get(bound_events, 0), "detail")),
                 "start");

    turbo_runtime_json_destroy(bound_events);
    turbo_runtime_json_destroy(event);
    turbo_runtime_json_destroy(state);
  }

  it("should export planner and executor event history versions as TurboParser JSON-native arrays") {
    json_value_t *state = turbo_agent_state_create_json_value();
    json_value_t *planner_versions =
        turbo_json_create_array();
    json_value_t *executor_versions =
        turbo_json_create_array();
    json_value_t *planner_events = turbo_json_create_array();
    json_value_t *executor_events = turbo_json_create_array();
    json_value_t *planner_json_value = NULL;
    json_value_t *executor_json_value = NULL;
    turbo_event_log_t *planner_log = turbo_event_log_create();
    turbo_event_log_t *executor_log = turbo_event_log_create();

    check_not_null(state);
    check_not_null(planner_versions);
    check_not_null(executor_versions);
    check_not_null(planner_events);
    check_not_null(executor_events);
    check_not_null(planner_log);
    check_not_null(executor_log);

    check_int_eq(
        turbo_runtime_json_array_append(
            planner_events, turbo_event_trace_create_json_value("planner.trace", "start", "p", 0)),
        TURBO_RUNTIME_JSON_OK);
    check_int_eq(
        turbo_runtime_json_array_append(
            executor_events, turbo_event_trace_create_json_value("executor.trace", "finish", "e", 0)),
        TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_array_append(planner_versions, planner_events),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_array_append(executor_versions, executor_events),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(
        turbo_runtime_json_object_set(state, "planner_event_versions", planner_versions),
        TURBO_RUNTIME_JSON_OK);
    check_int_eq(
        turbo_runtime_json_object_set(state, "executor_event_versions", executor_versions),
        TURBO_RUNTIME_JSON_OK);

    planner_json_value = turbo_agent_state_planner_event_version_json_value(state, 0);
    executor_json_value = turbo_agent_state_executor_event_version_json_value(state, 0);
    check_not_null(planner_json_value);
    check_not_null(executor_json_value);
    check_int_eq(turbo_event_log_load_events_json_value(planner_log, planner_json_value), TURBO_EVENT_LOG_OK);
    check_int_eq(turbo_event_log_load_events_json_value(executor_log, executor_json_value), TURBO_EVENT_LOG_OK);
    check_size_eq(turbo_event_log_size(planner_log), 1);
    check_size_eq(turbo_event_log_size(executor_log), 1);
    check_str_eq(turbo_runtime_json_value_as_string(turbo_json_object_get(
                     turbo_event_log_get(planner_log, 0), "name")),
                 "planner.trace");
    check_str_eq(turbo_runtime_json_value_as_string(turbo_json_object_get(
                     turbo_event_log_get(executor_log, 0), "name")),
                 "executor.trace");

    turbo_event_log_destroy(executor_log);
    turbo_event_log_destroy(planner_log);
    turbo_runtime_json_destroy(executor_json_value);
    turbo_runtime_json_destroy(planner_json_value);
    turbo_runtime_json_destroy(state);
  }

  it("should export the latest planner and executor event history versions directly") {
    json_value_t *state = turbo_agent_state_create_json_value();
    json_value_t *planner_versions =
        turbo_json_create_array();
    json_value_t *executor_versions =
        turbo_json_create_array();
    json_value_t *planner_first = turbo_json_create_array();
    json_value_t *planner_last = turbo_json_create_array();
    json_value_t *executor_first = turbo_json_create_array();
    json_value_t *executor_last = turbo_json_create_array();
    json_value_t *planner_json_value = NULL;
    json_value_t *executor_json_value = NULL;

    check_not_null(state);
    check_not_null(planner_versions);
    check_not_null(executor_versions);
    check_not_null(planner_first);
    check_not_null(planner_last);
    check_not_null(executor_first);
    check_not_null(executor_last);

    check_int_eq(
        turbo_runtime_json_array_append(
            planner_first, turbo_event_trace_create_json_value("planner.trace", "start", "p1", 0)),
        TURBO_RUNTIME_JSON_OK);
    check_int_eq(
        turbo_runtime_json_array_append(
            planner_last, turbo_event_trace_create_json_value("planner.trace", "finish", "p2", 0)),
        TURBO_RUNTIME_JSON_OK);
    check_int_eq(
        turbo_runtime_json_array_append(
            executor_first, turbo_event_trace_create_json_value("executor.trace", "start", "e1", 0)),
        TURBO_RUNTIME_JSON_OK);
    check_int_eq(
        turbo_runtime_json_array_append(
            executor_last, turbo_event_trace_create_json_value("executor.trace", "finish", "e2", 0)),
        TURBO_RUNTIME_JSON_OK);

    check_int_eq(turbo_runtime_json_array_append(planner_versions, planner_first),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_array_append(planner_versions, planner_last),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_array_append(executor_versions, executor_first),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_array_append(executor_versions, executor_last),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(
        turbo_runtime_json_object_set(state, "planner_event_versions", planner_versions),
        TURBO_RUNTIME_JSON_OK);
    check_int_eq(
        turbo_runtime_json_object_set(state, "executor_event_versions", executor_versions),
        TURBO_RUNTIME_JSON_OK);

    planner_json_value = turbo_agent_state_latest_planner_event_version_json_value(state);
    executor_json_value = turbo_agent_state_latest_executor_event_version_json_value(state);
    check_not_null(planner_json_value);
    check_not_null(executor_json_value);
    check_str_eq(turbo_runtime_json_value_as_string(turbo_json_object_get(
                     turbo_json_array_get(planner_json_value, 0), "detail")),
                 "finish");
    check_str_eq(turbo_runtime_json_value_as_string(turbo_json_object_get(
                     turbo_json_array_get(executor_json_value, 0), "detail")),
                 "finish");

    turbo_runtime_json_destroy(executor_json_value);
    turbo_runtime_json_destroy(planner_json_value);
    turbo_runtime_json_destroy(state);
  }

  it("should export latest planner and executor event versions from workflow snapshots") {
    json_value_t *state = turbo_agent_state_create_json_value();
    json_value_t *planner_versions =
        turbo_json_create_array();
    json_value_t *executor_versions =
        turbo_json_create_array();
    json_value_t *planner_events = turbo_json_create_array();
    json_value_t *executor_events = turbo_json_create_array();
    json_value_t *workflow = NULL;
    json_value_t *planner_json_value = NULL;
    json_value_t *executor_json_value = NULL;

    check_not_null(state);
    check_not_null(planner_versions);
    check_not_null(executor_versions);
    check_not_null(planner_events);
    check_not_null(executor_events);

    check_int_eq(
        turbo_runtime_json_array_append(
            planner_events, turbo_event_trace_create_json_value("planner.trace", "finish", "wf-p", 0)),
        TURBO_RUNTIME_JSON_OK);
    check_int_eq(
        turbo_runtime_json_array_append(
            executor_events, turbo_event_trace_create_json_value("executor.trace", "finish", "wf-e", 0)),
        TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_array_append(planner_versions, planner_events),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_array_append(executor_versions, executor_events),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(
        turbo_runtime_json_object_set(state, "planner_event_versions", planner_versions),
        TURBO_RUNTIME_JSON_OK);
    check_int_eq(
        turbo_runtime_json_object_set(state, "executor_event_versions", executor_versions),
        TURBO_RUNTIME_JSON_OK);

    workflow = turbo_agent_state_workflow_snapshot_json_value(state);
    check_not_null(workflow);

    planner_json_value = turbo_agent_state_latest_planner_event_version_json_value(workflow);
    executor_json_value = turbo_agent_state_latest_executor_event_version_json_value(workflow);
    check_not_null(planner_json_value);
    check_not_null(executor_json_value);
    check_str_eq(turbo_runtime_json_value_as_string(turbo_json_object_get(
                     turbo_json_array_get(planner_json_value, 0), "name")),
                 "planner.trace");
    check_str_eq(turbo_runtime_json_value_as_string(turbo_json_object_get(
                     turbo_json_array_get(executor_json_value, 0), "name")),
                 "executor.trace");

    turbo_runtime_json_destroy(executor_json_value);
    turbo_runtime_json_destroy(planner_json_value);
    turbo_runtime_json_destroy(workflow);
    turbo_runtime_json_destroy(state);
  }

  it("should export completed steps through TurboParser JSON-native accessors") {
    json_value_t *state = turbo_agent_state_create_json_value();
    json_value_t *completed_steps = turbo_json_create_array();
    json_value_t *first = turbo_json_create_object();
    json_value_t *last = turbo_json_create_object();
    json_value_t *steps_json_value = NULL;
    json_value_t *step_json_value = NULL;
    json_value_t *latest_json_value = NULL;

    check_not_null(state);
    check_not_null(completed_steps);
    check_not_null(first);
    check_not_null(last);

    check_int_eq(turbo_runtime_json_object_set(first, "step_index",
                                                    turbo_json_create_int64(0)),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_object_set(
                     first, "step", turbo_json_create_string("plan")),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_object_set(
                     first, "output", turbo_json_create_string("draft")),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_object_set(last, "step_index",
                                                    turbo_json_create_int64(1)),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_object_set(
                     last, "step", turbo_json_create_string("ship")),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_object_set(
                     last, "output", turbo_json_create_string("done")),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_array_append(completed_steps, first),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_array_append(completed_steps, last),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_object_set(state, "completed_steps", completed_steps),
                 TURBO_RUNTIME_JSON_OK);

    steps_json_value = turbo_agent_state_completed_steps_json_value(state);
    step_json_value = turbo_agent_state_completed_step_json_value(state, 0);
    latest_json_value = turbo_agent_state_latest_completed_step_json_value(state);

    check_not_null(steps_json_value);
    check_not_null(step_json_value);
    check_not_null(latest_json_value);
    check_size_eq(turbo_runtime_json_value_size(steps_json_value), 2);
    check_str_eq(turbo_runtime_json_value_as_string(
                     turbo_json_object_get(step_json_value, "step")),
                 "plan");
    check_str_eq(turbo_runtime_json_value_as_string(
                     turbo_json_object_get(latest_json_value, "output")),
                 "done");

    turbo_runtime_json_destroy(latest_json_value);
    turbo_runtime_json_destroy(step_json_value);
    turbo_runtime_json_destroy(steps_json_value);
    turbo_runtime_json_destroy(state);
  }

  it("should export completed steps from workflow snapshots through TurboParser JSON-native accessors") {
    json_value_t *state = turbo_agent_state_create_json_value();
    json_value_t *completed_steps = turbo_json_create_array();
    json_value_t *entry = turbo_json_create_object();
    json_value_t *workflow = NULL;
    json_value_t *steps_json_value = NULL;
    json_value_t *latest_json_value = NULL;

    check_not_null(state);
    check_not_null(completed_steps);
    check_not_null(entry);

    check_int_eq(turbo_runtime_json_object_set(entry, "step_index",
                                                    turbo_json_create_int64(2)),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_object_set(
                     entry, "step", turbo_json_create_string("verify")),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_object_set(
                     entry, "output", turbo_json_create_string("passed")),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_array_append(completed_steps, entry),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_object_set(state, "completed_steps", completed_steps),
                 TURBO_RUNTIME_JSON_OK);

    workflow = turbo_agent_state_workflow_snapshot_json_value(state);
    check_not_null(workflow);

    steps_json_value = turbo_agent_state_completed_steps_json_value(workflow);
    latest_json_value = turbo_agent_state_latest_completed_step_json_value(workflow);
    check_not_null(steps_json_value);
    check_not_null(latest_json_value);
    check_size_eq(turbo_runtime_json_value_size(steps_json_value), 1);
    check_str_eq(turbo_runtime_json_value_as_string(
                     turbo_json_object_get(latest_json_value, "step")),
                 "verify");

    turbo_runtime_json_destroy(latest_json_value);
    turbo_runtime_json_destroy(steps_json_value);
    turbo_runtime_json_destroy(workflow);
    turbo_runtime_json_destroy(state);
  }

  it("should capture control snapshot smoke state for review replan and failures") {
    json_value_t *state = turbo_agent_state_create();
    json_value_t *control = NULL;
    const json_value_t *replan;
    const json_value_t *review;
    const json_value_t *failure;
    const json_value_t *model_error;
    const json_value_t *guardrail;

    check_not_null(state);
    check_int_eq(turbo_agent_state_request_review(state, "needs approval"), 0);
    check_int_eq(turbo_agent_state_set_review_approved(state, 0), 0);
    check_int_eq(turbo_agent_state_request_replan(state, "tool failed"), 0);
    check_int_eq(turbo_agent_state_set_failure(state, "tool_result", "bad output"), 0);
    check_int_eq(turbo_agent_state_set_model_error(state, "transport", "timeout"), 0);
    check_int_eq(turbo_agent_state_set_guardrail_rejection(state, "after_tool", "unsafe output"),
                 0);

    control = turbo_agent_state_control_snapshot(state);
    check_not_null(control);

    replan = turbo_json_object_get(control, "replan");
    review = turbo_json_object_get(control, "review");
    failure = turbo_json_object_get(control, "failure");
    model_error = turbo_json_object_get(control, "model_error");
    guardrail = turbo_json_object_get(control, "guardrail");
    check_not_null(replan);
    check_not_null(review);
    check_not_null(failure);
    check_not_null(model_error);
    check_not_null(guardrail);
    check_true(turbo_json_get_bool(replan, "requested", false));
    check_str_eq(turbo_json_get_string(replan, "reason"), "tool failed");
    check_true(turbo_json_get_bool(review, "required", false));
    check_false(turbo_json_get_bool(review, "approved", true));
    check_str_eq(turbo_json_get_string(review, "note"), "needs approval");
    check_str_eq(turbo_json_get_string(failure, "kind"), "tool_result");
    check_str_eq(turbo_json_get_string(failure, "reason"), "bad output");
    check_str_eq(turbo_json_get_string(model_error, "phase"), "transport");
    check_str_eq(turbo_json_get_string(model_error, "detail"), "timeout");
    check_str_eq(turbo_json_get_string(guardrail, "phase"), "after_tool");
    check_str_eq(turbo_json_get_string(guardrail, "reason"), "unsafe output");

    turbo_free_json(&control);
    turbo_free_json(&state);
  }

  it("should capture workflow snapshot smoke state with memory and version history") {
    json_value_t *state = turbo_agent_state_create();
    json_value_t *workflow = NULL;
    json_value_t *planner_versions;
    json_value_t *executor_versions;
    json_value_t *planner_events;
    json_value_t *executor_events;
    json_value_t *completed_steps;
    json_value_t *step;
    const json_value_t *control;
    const json_value_t *memory_context;

    check_not_null(state);
    check_int_eq(
        turbo_agent_state_add_memory_context_layer(state, "project", "/tmp/notes.md", "remember"),
        0);
    check_int_eq(turbo_agent_state_set_plan_from_json(state, "[\"draft\",\"ship\"]"), 0);

    planner_versions = turbo_json_create_array();
    executor_versions = turbo_json_create_array();
    planner_events = turbo_json_create_array();
    executor_events = turbo_json_create_array();
    completed_steps = turbo_json_create_array();
    step = turbo_json_create_object();
    check_not_null(planner_versions);
    check_not_null(executor_versions);
    check_not_null(planner_events);
    check_not_null(executor_events);
    check_not_null(completed_steps);
    check_not_null(step);

    {
      json_value_t *planner_event = turbo_json_create_object();
      json_value_t *executor_event = turbo_json_create_object();
      check_not_null(planner_event);
      check_not_null(executor_event);
      turbo_json_object_set_string(planner_event, "kind", "trace");
      turbo_json_object_set_string(planner_event, "name", "planner.trace");
      turbo_json_object_set_string(executor_event, "kind", "trace");
      turbo_json_object_set_string(executor_event, "name", "executor.trace");
      turbo_json_array_add(planner_events, planner_event);
      turbo_json_array_add(executor_events, executor_event);
    }
    turbo_json_array_add(planner_versions, planner_events);
    turbo_json_array_add(executor_versions, executor_events);
    turbo_json_object_add(state, "planner_event_versions", planner_versions);
    turbo_json_object_add(state, "executor_event_versions", executor_versions);

    turbo_json_object_set_number(step, "step_index", 0);
    turbo_json_object_set_string(step, "step", "draft");
    turbo_json_object_set_string(step, "output", "done");
    turbo_json_array_add(completed_steps, step);
    turbo_json_object_add(state, "completed_steps", completed_steps);

    workflow = turbo_agent_state_workflow_snapshot(state);
    check_not_null(workflow);
    control = turbo_json_object_get(workflow, "control");
    memory_context = turbo_json_object_get(workflow, "memory_context");
    check_not_null(control);
    check_not_null(memory_context);
    check_not_null(turbo_json_object_get(workflow, "plan"));
    check_not_null(turbo_json_object_get(workflow, "completed_steps"));
    check_not_null(turbo_json_object_get(workflow, "planner_event_versions"));
    check_not_null(turbo_json_object_get(workflow, "executor_event_versions"));
    check_str_eq(turbo_json_get_string(workflow, "memory_context_text"),
                 "Persistent memory:\n[project] /tmp/notes.md\nremember\n\n");

    turbo_free_json(&workflow);
    turbo_free_json(&state);
  }

  it("should persist supervisor state through control and workflow snapshots") {
    json_value_t *state = turbo_agent_state_create();
    json_value_t *control = NULL;
    json_value_t *workflow = NULL;
    const json_value_t *supervisor = NULL;
    const json_value_t *inbox = NULL;
    const json_value_t *history = NULL;

    check_not_null(state);
    check_int_eq(turbo_agent_state_set_active_agent(state, "planner"), 0);
    check_int_eq(
        turbo_agent_state_append_supervisor_inbox_message(state, "supervisor", "review this"),
        0);
    check_int_eq(turbo_agent_state_request_handoff(state, "executor", "delegate execution"), 0);
    check_str_eq(turbo_agent_state_active_agent(state), "planner");
    check_str_eq(turbo_agent_state_handoff_target_agent(state), "executor");
    check_str_eq(turbo_agent_state_handoff_reason(state), "delegate execution");
    check_size_eq(turbo_agent_state_supervisor_inbox_count(state), 1);
    check_not_null(turbo_agent_state_supervisor_inbox_at(state, 0));
    check_not_null(turbo_agent_state_supervisor_handoff_history(state));

    control = turbo_agent_state_control_snapshot(state);
    workflow = turbo_agent_state_workflow_snapshot(state);
    check_not_null(control);
    check_not_null(workflow);

    supervisor = turbo_json_object_get(control, "supervisor");
    check_not_null(supervisor);
    check_str_eq(turbo_json_get_string(supervisor, "active_agent"), "planner");
    check_str_eq(turbo_json_get_string(supervisor, "target_agent"), "executor");
    check_str_eq(turbo_json_get_string(supervisor, "handoff_reason"), "delegate execution");
    check_int_eq(turbo_json_get_int(supervisor, "inbox_count", 0), 1);
    check_int_eq(turbo_json_get_int(supervisor, "handoff_count", 0), 1);

    supervisor = turbo_json_object_get(workflow, "supervisor");
    check_not_null(supervisor);
    inbox = turbo_json_object_get(supervisor, "inbox");
    history = turbo_json_object_get(supervisor, "handoff_history");
    check_not_null(inbox);
    check_not_null(history);
    check_size_eq(turbo_json_array_size(inbox), 1);
    check_size_eq(turbo_json_array_size(history), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(inbox, 0), "source_agent"),
                 "supervisor");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(inbox, 0), "text"),
                 "review this");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(history, 0), "from_agent"),
                 "planner");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(history, 0), "target_agent"),
                 "executor");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(history, 0), "reason"),
                 "delegate execution");

    check_int_eq(turbo_agent_state_commit_handoff(state), 0);
    check_str_eq(turbo_agent_state_active_agent(state), "executor");
    check_null(turbo_agent_state_handoff_target_agent(state));
    check_null(turbo_agent_state_handoff_reason(state));

    turbo_free_json(&workflow);
    turbo_free_json(&control);
    turbo_free_json(&state);
  }

  it("should append canonical handoff state events for request and commit") {
    json_value_t *state = turbo_agent_state_create();
    const json_value_t *events = NULL;
    const json_value_t *latest_event = NULL;

    check_not_null(state);
    check_int_eq(turbo_agent_state_set_active_agent(state, "planner"), 0);
    check_size_eq(turbo_agent_state_event_count(state), 0);

    check_int_eq(turbo_agent_state_request_handoff(state, "executor", "delegate execution"), 0);
    check_size_eq(turbo_agent_state_event_count(state), 1);

    events = turbo_agent_state_events(state);
    latest_event = turbo_agent_state_latest_handoff_event(state);
    check_not_null(events);
    check_not_null(latest_event);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(events, 0), "kind"), "handoff");
    check_str_eq(turbo_agent_state_handoff_event_phase(latest_event), "requested");
    check_str_eq(turbo_agent_state_handoff_event_from_agent(latest_event), "planner");
    check_str_eq(turbo_agent_state_handoff_event_target_agent(latest_event), "executor");
    check_str_eq(turbo_agent_state_handoff_event_reason(latest_event), "delegate execution");
    check_str_eq(turbo_agent_state_handoff_event_active_agent(latest_event), "planner");

    check_int_eq(turbo_agent_state_commit_handoff(state), 0);
    check_size_eq(turbo_agent_state_event_count(state), 2);

    events = turbo_agent_state_events(state);
    latest_event = turbo_agent_state_latest_handoff_event(state);
    check_not_null(events);
    check_not_null(latest_event);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(events, 1), "kind"), "handoff");
    check_str_eq(turbo_agent_state_handoff_event_phase(latest_event), "committed");
    check_str_eq(turbo_agent_state_handoff_event_from_agent(latest_event), "planner");
    check_str_eq(turbo_agent_state_handoff_event_target_agent(latest_event), "executor");
    check_str_eq(turbo_agent_state_handoff_event_reason(latest_event), "delegate execution");
    check_str_eq(turbo_agent_state_handoff_event_active_agent(latest_event), "executor");

    turbo_free_json(&state);
  }

  it("should preserve final output json and surface malformed tool result failures") {
    json_value_t *state = turbo_agent_state_create();
    json_value_t *parsed = NULL;
    json_value_t *executor_versions;
    json_value_t *executor_events;
    json_value_t *tool_results;
    json_value_t *outputs;
    json_value_t *output_item;
    char *failure_reason = NULL;

    check_not_null(state);
    check_int_eq(turbo_agent_state_set_final_answer(state, "{\"ok\":true,\"value\":7}"), 0);
    check_str_eq(turbo_agent_state_final_answer_text(state), "{\"ok\":true,\"value\":7}");
    check_int_eq(turbo_agent_state_parse_final_output_json(state, &parsed), 0);
    check_not_null(parsed);
    check_true(turbo_json_get_bool(parsed, "ok", false));
    check_int_eq(turbo_json_get_int(parsed, "value", 0), 7);
    turbo_free_json(&parsed);

    executor_versions = turbo_json_create_array();
    executor_events = turbo_json_create_array();
    tool_results = turbo_json_create_object();
    outputs = turbo_json_create_array();
    output_item = turbo_json_create_object();
    check_not_null(executor_versions);
    check_not_null(executor_events);
    check_not_null(tool_results);
    check_not_null(outputs);
    check_not_null(output_item);

    turbo_json_object_set_string(tool_results, "kind", "tool_results");
    turbo_json_object_set_string(output_item, "call_id", "call_1");
    turbo_json_object_set_string(output_item, "output", "not-json");
    turbo_json_array_add(outputs, output_item);
    turbo_json_object_add(tool_results, "outputs", outputs);
    turbo_json_array_add(executor_events, tool_results);
    turbo_json_array_add(executor_versions, executor_events);
    turbo_json_object_add(state, "executor_event_versions", executor_versions);

    check_true(turbo_agent_state_executor_tool_results_failed(state));
    failure_reason = turbo_agent_executor_failure_reason(state);
    check_not_null(failure_reason);
    check_str_eq(failure_reason, "tool output was not valid JSON");
    free(failure_reason);

    turbo_free_json(&state);
  }

  it("should expose latest tool-results child lineage through state accessors") {
    json_value_t *state = turbo_agent_state_create();
    json_value_t *executor_versions;
    json_value_t *executor_events;
    json_value_t *tool_results;
    json_value_t *outputs;
    json_value_t *output_item;
    const json_value_t *latest_event;
    const json_value_t *latest_outputs;

    check_not_null(state);

    executor_versions = turbo_json_create_array();
    executor_events = turbo_json_create_array();
    tool_results = turbo_json_create_object();
    outputs = turbo_json_create_array();
    output_item = turbo_json_create_object();
    check_not_null(executor_versions);
    check_not_null(executor_events);
    check_not_null(tool_results);
    check_not_null(outputs);
    check_not_null(output_item);

    turbo_json_object_set_string(tool_results, "kind", "tool_results");
    turbo_json_object_set_string(output_item, "call_id", "call_1");
    turbo_json_object_set_string(output_item, "output",
                                 "{\"ok\":true,\"summary\":\"done\",\"stdout\":\"\","
                                 "\"stderr\":\"\",\"child_thread_id\":\"thr_child\","
                                 "\"child_run_id\":\"run_child\","
                                 "\"child_checkpoint_id\":\"ckpt_child\","
                                 "\"child_status\":\"completed\"}");
    turbo_json_object_set_string(output_item, "child_thread_id", "thr_child");
    turbo_json_object_set_string(output_item, "child_run_id", "run_child");
    turbo_json_object_set_string(output_item, "child_checkpoint_id", "ckpt_child");
    turbo_json_object_set_string(output_item, "child_status", "completed");
    turbo_json_array_add(outputs, output_item);
    turbo_json_object_add(tool_results, "outputs", outputs);
    turbo_json_array_add(executor_events, tool_results);
    turbo_json_array_add(executor_versions, executor_events);
    turbo_json_object_add(state, "executor_event_versions", executor_versions);

    latest_event = turbo_agent_state_latest_tool_results_event(state);
    latest_outputs = turbo_agent_state_tool_results_outputs(latest_event);
    check_not_null(latest_event);
    check_not_null(latest_outputs);
    check_size_eq(turbo_json_array_size(latest_outputs), 1);
    check_str_eq(turbo_agent_state_tool_result_child_thread_id(
                     turbo_json_array_get(latest_outputs, 0)),
                 "thr_child");
    check_str_eq(
        turbo_agent_state_tool_result_child_run_id(turbo_json_array_get(latest_outputs, 0)),
        "run_child");
    check_str_eq(turbo_agent_state_tool_result_child_checkpoint_id(
                     turbo_json_array_get(latest_outputs, 0)),
                 "ckpt_child");
    check_str_eq(
        turbo_agent_state_tool_result_child_status(turbo_json_array_get(latest_outputs, 0)),
        "completed");
    check_null(turbo_agent_state_tool_result_parent_agent_run_id(
        turbo_json_array_get(latest_outputs, 0)));
    check_null(turbo_agent_state_tool_result_parent_tool_call_id(
        turbo_json_array_get(latest_outputs, 0)));
    check_null(turbo_agent_state_tool_result_parent_tool_name(
        turbo_json_array_get(latest_outputs, 0)));
    check_null(turbo_agent_state_tool_result_parent_graph_run_id(
        turbo_json_array_get(latest_outputs, 0)));
    check_null(turbo_agent_state_tool_result_call_frame_id(
        turbo_json_array_get(latest_outputs, 0)));

    turbo_free_json(&state);
  }

  it("should expose parent tool lineage through tool-result output accessors") {
    json_value_t *state = turbo_agent_state_create();
    json_value_t *executor_versions;
    json_value_t *executor_events;
    json_value_t *tool_results;
    json_value_t *outputs;
    json_value_t *output_item;
    const json_value_t *latest_event;
    const json_value_t *latest_outputs;

    check_not_null(state);

    executor_versions = turbo_json_create_array();
    executor_events = turbo_json_create_array();
    tool_results = turbo_json_create_object();
    outputs = turbo_json_create_array();
    output_item = turbo_agent_tool_result_output_item_create(
        "call_1",
        "{\"ok\":true,\"summary\":\"done\",\"stdout\":\"\",\"stderr\":\"\","
        "\"parent_agent_run_id\":\"run_parent\","
        "\"parent_tool_call_id\":\"call_parent\","
        "\"parent_tool_name\":\"delegate\","
        "\"parent_graph_run_id\":\"run_graph_parent\","
        "\"call_frame_id\":\"frame_parent\","
        "\"active_agent\":\"planner\","
        "\"handoff_target_agent\":\"executor\","
        "\"handoff_reason\":\"delegate execution\"}");
    check_not_null(executor_versions);
    check_not_null(executor_events);
    check_not_null(tool_results);
    check_not_null(outputs);
    check_not_null(output_item);

    turbo_json_object_set_string(tool_results, "kind", "tool_results");
    turbo_json_array_add(outputs, output_item);
    turbo_json_object_add(tool_results, "outputs", outputs);
    turbo_json_array_add(executor_events, tool_results);
    turbo_json_array_add(executor_versions, executor_events);
    turbo_json_object_add(state, "executor_event_versions", executor_versions);

    latest_event = turbo_agent_state_latest_tool_results_event(state);
    latest_outputs = turbo_agent_state_tool_results_outputs(latest_event);
    check_not_null(latest_event);
    check_not_null(latest_outputs);
    check_size_eq(turbo_json_array_size(latest_outputs), 1);
    check_str_eq(turbo_agent_state_tool_result_parent_agent_run_id(
                     turbo_json_array_get(latest_outputs, 0)),
                 "run_parent");
    check_str_eq(turbo_agent_state_tool_result_parent_tool_call_id(
                     turbo_json_array_get(latest_outputs, 0)),
                 "call_parent");
    check_str_eq(turbo_agent_state_tool_result_parent_tool_name(
                     turbo_json_array_get(latest_outputs, 0)),
                 "delegate");
    check_str_eq(turbo_agent_state_tool_result_parent_graph_run_id(
                     turbo_json_array_get(latest_outputs, 0)),
                 "run_graph_parent");
    check_str_eq(turbo_agent_state_tool_result_call_frame_id(
                     turbo_json_array_get(latest_outputs, 0)),
                 "frame_parent");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(latest_outputs, 0), "active_agent"),
                 "planner");
    check_str_eq(
        turbo_json_get_string(turbo_json_array_get(latest_outputs, 0), "handoff_target_agent"),
        "executor");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(latest_outputs, 0), "handoff_reason"),
                 "delegate execution");

    turbo_free_json(&state);
  }

  it("should surface canonical tool result envelope mismatches as executor failures") {
    json_value_t *state = turbo_agent_state_create();
    json_value_t *executor_versions;
    json_value_t *executor_events;
    json_value_t *tool_results;
    json_value_t *outputs;
    json_value_t *output_item;
    char *failure_reason = NULL;

    check_not_null(state);

    executor_versions = turbo_json_create_array();
    executor_events = turbo_json_create_array();
    tool_results = turbo_json_create_object();
    outputs = turbo_json_create_array();
    output_item = turbo_json_create_object();
    check_not_null(executor_versions);
    check_not_null(executor_events);
    check_not_null(tool_results);
    check_not_null(outputs);
    check_not_null(output_item);

    turbo_json_object_set_string(tool_results, "kind", "tool_results");
    turbo_json_object_set_string(output_item, "call_id", "call_1");
    turbo_json_object_set_string(output_item, "output", "{\"ok\":true}");
    turbo_json_array_add(outputs, output_item);
    turbo_json_object_add(tool_results, "outputs", outputs);
    turbo_json_array_add(executor_events, tool_results);
    turbo_json_array_add(executor_versions, executor_events);
    turbo_json_object_add(state, "executor_event_versions", executor_versions);

    check_true(turbo_agent_state_executor_tool_results_failed(state));
    failure_reason = turbo_agent_executor_failure_reason(state);
    check_not_null(failure_reason);
    check_str_eq(failure_reason, "tool output did not match canonical result envelope");
    free(failure_reason);

    turbo_free_json(&state);
  }

  it("should surface malformed tool result payloads as executor failures") {
    json_value_t *state = turbo_agent_state_create();
    json_value_t *executor_versions;
    json_value_t *executor_events;
    json_value_t *tool_results;
    json_value_t *outputs;
    json_value_t *output_item;
    char *failure_reason = NULL;

    check_not_null(state);

    executor_versions = turbo_json_create_array();
    executor_events = turbo_json_create_array();
    tool_results = turbo_json_create_object();
    outputs = turbo_json_create_array();
    output_item = turbo_json_create_object();
    check_not_null(executor_versions);
    check_not_null(executor_events);
    check_not_null(tool_results);
    check_not_null(outputs);
    check_not_null(output_item);

    turbo_json_object_set_string(tool_results, "kind", "tool_results");
    turbo_json_object_set_string(output_item, "call_id", "call_1");
    turbo_json_array_add(outputs, output_item);
    turbo_json_object_add(tool_results, "outputs", outputs);
    turbo_json_array_add(executor_events, tool_results);
    turbo_json_array_add(executor_versions, executor_events);
    turbo_json_object_add(state, "executor_event_versions", executor_versions);

    check_true(turbo_agent_state_executor_tool_results_failed(state));
    failure_reason = turbo_agent_executor_failure_reason(state);
    check_not_null(failure_reason);
    check_str_eq(failure_reason, "tool result payload was malformed");
    free(failure_reason);

    turbo_free_json(&state);
  }

  it("should surface empty tool outputs as executor failures") {
    json_value_t *state = turbo_agent_state_create();
    json_value_t *executor_versions;
    json_value_t *executor_events;
    json_value_t *tool_results;
    json_value_t *outputs;
    json_value_t *output_item;
    char *failure_reason = NULL;

    check_not_null(state);

    executor_versions = turbo_json_create_array();
    executor_events = turbo_json_create_array();
    tool_results = turbo_json_create_object();
    outputs = turbo_json_create_array();
    output_item = turbo_json_create_object();
    check_not_null(executor_versions);
    check_not_null(executor_events);
    check_not_null(tool_results);
    check_not_null(outputs);
    check_not_null(output_item);

    turbo_json_object_set_string(tool_results, "kind", "tool_results");
    turbo_json_object_set_string(output_item, "call_id", "call_1");
    turbo_json_object_set_string(output_item, "output", "");
    turbo_json_array_add(outputs, output_item);
    turbo_json_object_add(tool_results, "outputs", outputs);
    turbo_json_array_add(executor_events, tool_results);
    turbo_json_array_add(executor_versions, executor_events);
    turbo_json_object_add(state, "executor_event_versions", executor_versions);

    check_true(turbo_agent_state_executor_tool_results_failed(state));
    failure_reason = turbo_agent_executor_failure_reason(state);
    check_not_null(failure_reason);
    check_str_eq(failure_reason, "tool output was empty");
    free(failure_reason);

    turbo_free_json(&state);
  }
}
