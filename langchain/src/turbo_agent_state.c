#include "turbo_agent_state_core_internal.h"
#include "turbo_agent_state_flow_domain_internal.h"
#include "turbo_agent_state_bind_internal.h"
#include "turbo_agent_state_memory_internal.h"
#include "turbo_agent_state_output_internal.h"
#include "turbo_agent_state_snapshot_internal.h"

#define TURBO_AGENT_STATE_FORWARD0(ret_type, name) \
  ret_type name(void) { return name##_impl(); }

#define TURBO_AGENT_STATE_FORWARD1(ret_type, name, arg1_type, arg1_name) \
  ret_type name(arg1_type arg1_name) { return name##_impl(arg1_name); }

#define TURBO_AGENT_STATE_FORWARD2(ret_type, name, arg1_type, arg1_name, arg2_type, arg2_name) \
  ret_type name(arg1_type arg1_name, arg2_type arg2_name) { return name##_impl(arg1_name, arg2_name); }

#define TURBO_AGENT_STATE_FORWARD3(ret_type, name, arg1_type, arg1_name, arg2_type, arg2_name, \
                                   arg3_type, arg3_name) \
  ret_type name(arg1_type arg1_name, arg2_type arg2_name, arg3_type arg3_name) { \
    return name##_impl(arg1_name, arg2_name, arg3_name); \
  }

#define TURBO_AGENT_STATE_FORWARD4(ret_type, name, arg1_type, arg1_name, arg2_type, arg2_name, \
                                   arg3_type, arg3_name, arg4_type, arg4_name) \
  ret_type name(arg1_type arg1_name, arg2_type arg2_name, arg3_type arg3_name, \
                arg4_type arg4_name) { \
    return name##_impl(arg1_name, arg2_name, arg3_name, arg4_name); \
  }

TURBO_AGENT_STATE_FORWARD0(size_t, turbo_agent_state_schema_version)
TURBO_AGENT_STATE_FORWARD0(json_value_t *, turbo_agent_state_create)
TURBO_AGENT_STATE_FORWARD0(turbo_runtime_data_bind_value_t *, turbo_agent_state_create_bind)
TURBO_AGENT_STATE_FORWARD1(size_t, turbo_agent_state_version, const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(int, turbo_agent_state_version_supported, const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD2(int, turbo_agent_state_add_user_message, json_value_t *, state,
                           const char *, text)
TURBO_AGENT_STATE_FORWARD1(const json_value_t *, turbo_agent_state_events, const json_value_t *,
                           state)
TURBO_AGENT_STATE_FORWARD1(size_t, turbo_agent_state_event_count, const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD2(const json_value_t *, turbo_agent_state_event_at, const json_value_t *,
                           state, size_t, index)
TURBO_AGENT_STATE_FORWARD1(const json_value_t *, turbo_agent_state_trace_events,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(size_t, turbo_agent_state_trace_event_count, const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD2(const json_value_t *, turbo_agent_state_trace_event_at,
                           const json_value_t *, state, size_t, index)
TURBO_AGENT_STATE_FORWARD1(turbo_runtime_data_bind_value_t *, turbo_agent_state_trace_events_bind,
                           const turbo_runtime_data_bind_value_t *, state)
TURBO_AGENT_STATE_FORWARD2(int, turbo_agent_state_add_trace_event_bind,
                           turbo_runtime_data_bind_value_t *, state,
                           const turbo_runtime_data_bind_value_t *, event)
void turbo_agent_state_capture_trace_event_bind(const turbo_runtime_data_bind_value_t *event,
                                                void *user_data) {
  turbo_agent_state_capture_trace_event_bind_impl(event, user_data);
}
TURBO_AGENT_STATE_FORWARD3(int, turbo_agent_state_set_memory_json, json_value_t *, state,
                           const char *, key, const char *, value_json)
TURBO_AGENT_STATE_FORWARD2(const json_value_t *, turbo_agent_state_memory_json,
                           const json_value_t *, state, const char *, key)
TURBO_AGENT_STATE_FORWARD3(int, turbo_agent_state_load_memory, turbo_agent_t *, agent,
                           json_value_t *, state, const char *, key)
TURBO_AGENT_STATE_FORWARD3(int, turbo_agent_state_save_memory, turbo_agent_t *, agent,
                           json_value_t *, state, const char *, key)
TURBO_AGENT_STATE_FORWARD4(int, turbo_agent_state_add_memory_context_layer, json_value_t *, state,
                           const char *, scope, const char *, path, const char *, text)
TURBO_AGENT_STATE_FORWARD1(const json_value_t *, turbo_agent_state_memory_context,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(const json_value_t *, turbo_agent_state_memory_layers,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(size_t, turbo_agent_state_memory_layer_count, const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD2(const json_value_t *, turbo_agent_state_memory_layer_at,
                           const json_value_t *, state, size_t, index)
TURBO_AGENT_STATE_FORWARD1(char *, turbo_agent_state_memory_context_text, const json_value_t *,
                           state)
TURBO_AGENT_STATE_FORWARD2(int, turbo_agent_state_set_plan_from_json, json_value_t *, state,
                           const char *, plan_json)
TURBO_AGENT_STATE_FORWARD1(const json_value_t *, turbo_agent_state_plan, const json_value_t *,
                           state)
TURBO_AGENT_STATE_FORWARD1(size_t, turbo_agent_state_plan_step_count, const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(size_t, turbo_agent_state_plan_step_index, const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_current_plan_step_text,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(int, turbo_agent_state_advance_plan, json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(int, turbo_agent_state_plan_complete, const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD2(int, turbo_agent_state_request_replan, json_value_t *, state,
                           const char *, reason)
TURBO_AGENT_STATE_FORWARD2(int, turbo_agent_state_set_replan_limit, json_value_t *, state,
                           size_t, max_replans)
TURBO_AGENT_STATE_FORWARD1(int, turbo_agent_state_replan_requested, const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(size_t, turbo_agent_state_replan_count, const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(size_t, turbo_agent_state_replan_limit, const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_replan_reason, const json_value_t *,
                           state)
TURBO_AGENT_STATE_FORWARD3(int, turbo_agent_state_set_guardrail_rejection, json_value_t *, state,
                           const char *, phase, const char *, reason)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_guardrail_rejection_phase,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_guardrail_rejection_reason,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD3(int, turbo_agent_state_set_failure, json_value_t *, state,
                           const char *, kind, const char *, reason)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_failure_kind, const json_value_t *,
                           state)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_failure_reason, const json_value_t *,
                           state)
TURBO_AGENT_STATE_FORWARD1(json_value_t *, turbo_agent_state_control_snapshot,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(turbo_runtime_data_bind_value_t *, turbo_agent_state_control_snapshot_bind,
                           const turbo_runtime_data_bind_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(json_value_t *, turbo_agent_state_workflow_snapshot,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(turbo_runtime_data_bind_value_t *, turbo_agent_state_workflow_snapshot_bind,
                           const turbo_runtime_data_bind_value_t *, state)
TURBO_AGENT_STATE_FORWARD2(int, turbo_agent_state_set_final_answer, json_value_t *, state,
                           const char *, text)
TURBO_AGENT_STATE_FORWARD2(int, turbo_agent_state_request_review, json_value_t *, state,
                           const char *, note)
TURBO_AGENT_STATE_FORWARD2(int, turbo_agent_state_set_review_approved, json_value_t *, state,
                           int, approved)
TURBO_AGENT_STATE_FORWARD1(int, turbo_agent_state_clear_review, json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(int, turbo_agent_state_review_required, const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(int, turbo_agent_state_review_approved, const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_review_note, const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD2(int, turbo_agent_state_set_active_agent, json_value_t *, state,
                           const char *, active_agent)
TURBO_AGENT_STATE_FORWARD3(int, turbo_agent_state_request_handoff, json_value_t *, state,
                           const char *, target_agent, const char *, reason)
TURBO_AGENT_STATE_FORWARD1(int, turbo_agent_state_commit_handoff, json_value_t *, state)
TURBO_AGENT_STATE_FORWARD3(int, turbo_agent_state_append_supervisor_inbox_message,
                           json_value_t *, state, const char *, source_agent, const char *, text)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_active_agent, const json_value_t *,
                           state)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_handoff_target_agent,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_handoff_reason,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(const json_value_t *, turbo_agent_state_supervisor_inbox,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(size_t, turbo_agent_state_supervisor_inbox_count,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD2(const json_value_t *, turbo_agent_state_supervisor_inbox_at,
                           const json_value_t *, state, size_t, index)
TURBO_AGENT_STATE_FORWARD1(const json_value_t *, turbo_agent_state_supervisor_handoff_history,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(const json_value_t *, turbo_agent_state_latest_handoff_event,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_handoff_event_phase,
                           const json_value_t *, event)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_handoff_event_from_agent,
                           const json_value_t *, event)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_handoff_event_target_agent,
                           const json_value_t *, event)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_handoff_event_reason,
                           const json_value_t *, event)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_handoff_event_active_agent,
                           const json_value_t *, event)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_last_output_text, const json_value_t *,
                           state)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_latest_executor_output_text,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(const json_value_t *, turbo_agent_state_planner_event_versions,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(size_t, turbo_agent_state_planner_event_version_count,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD2(const json_value_t *, turbo_agent_state_planner_event_version_at,
                           const json_value_t *, state, size_t, index)
TURBO_AGENT_STATE_FORWARD2(turbo_runtime_data_bind_value_t *, turbo_agent_state_planner_event_version_bind,
                           const turbo_runtime_data_bind_value_t *, state, size_t, index)
TURBO_AGENT_STATE_FORWARD1(turbo_runtime_data_bind_value_t *,
                           turbo_agent_state_latest_planner_event_version_bind,
                           const turbo_runtime_data_bind_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(const json_value_t *, turbo_agent_state_executor_event_versions,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(size_t, turbo_agent_state_executor_event_version_count,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD2(const json_value_t *, turbo_agent_state_executor_event_version_at,
                           const json_value_t *, state, size_t, index)
TURBO_AGENT_STATE_FORWARD2(turbo_runtime_data_bind_value_t *, turbo_agent_state_executor_event_version_bind,
                           const turbo_runtime_data_bind_value_t *, state, size_t, index)
TURBO_AGENT_STATE_FORWARD1(turbo_runtime_data_bind_value_t *,
                           turbo_agent_state_latest_executor_event_version_bind,
                           const turbo_runtime_data_bind_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(const json_value_t *, turbo_agent_state_completed_steps,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(turbo_runtime_data_bind_value_t *, turbo_agent_state_completed_steps_bind,
                           const turbo_runtime_data_bind_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(size_t, turbo_agent_state_completed_step_count,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD2(const json_value_t *, turbo_agent_state_completed_step_at,
                           const json_value_t *, state, size_t, index)
TURBO_AGENT_STATE_FORWARD2(turbo_runtime_data_bind_value_t *, turbo_agent_state_completed_step_bind,
                           const turbo_runtime_data_bind_value_t *, state, size_t, index)
TURBO_AGENT_STATE_FORWARD1(turbo_runtime_data_bind_value_t *,
                           turbo_agent_state_latest_completed_step_bind,
                           const turbo_runtime_data_bind_value_t *, state)
TURBO_AGENT_STATE_FORWARD3(int, turbo_agent_state_set_model_error, json_value_t *, state,
                           const char *, phase, const char *, detail)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_model_error_phase,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_model_error_detail,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(const json_value_t *, turbo_agent_state_latest_tool_results_event,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(const json_value_t *, turbo_agent_state_tool_results_outputs,
                           const json_value_t *, event)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_tool_result_child_thread_id,
                           const json_value_t *, output_item)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_tool_result_child_run_id,
                           const json_value_t *, output_item)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_tool_result_child_checkpoint_id,
                           const json_value_t *, output_item)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_tool_result_child_status,
                           const json_value_t *, output_item)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_tool_result_parent_agent_run_id,
                           const json_value_t *, output_item)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_tool_result_parent_tool_call_id,
                           const json_value_t *, output_item)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_tool_result_parent_tool_name,
                           const json_value_t *, output_item)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_tool_result_parent_graph_run_id,
                           const json_value_t *, output_item)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_tool_result_call_frame_id,
                           const json_value_t *, output_item)
TURBO_AGENT_STATE_FORWARD1(int, turbo_agent_state_executor_tool_results_failed,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD1(const char *, turbo_agent_state_final_answer_text,
                           const json_value_t *, state)
TURBO_AGENT_STATE_FORWARD2(int, turbo_agent_state_parse_final_output_json, const json_value_t *,
                           state, json_value_t **, out_json)
TURBO_AGENT_STATE_FORWARD1(size_t, turbo_agent_state_pending_tool_calls, const json_value_t *,
                           state)

#undef TURBO_AGENT_STATE_FORWARD0
#undef TURBO_AGENT_STATE_FORWARD1
#undef TURBO_AGENT_STATE_FORWARD2
#undef TURBO_AGENT_STATE_FORWARD3
#undef TURBO_AGENT_STATE_FORWARD4
