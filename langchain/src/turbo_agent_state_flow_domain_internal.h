#ifndef TURBO_AGENT_STATE_FLOW_DOMAIN_INTERNAL_H
#define TURBO_AGENT_STATE_FLOW_DOMAIN_INTERNAL_H

#include "turbo_agent_state_core_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API int turbo_agent_state_set_plan_from_json_impl(json_value_t *state,
                                                        const char *plan_json);
CXX_C_API const json_value_t *turbo_agent_state_plan_impl(const json_value_t *state);
CXX_C_API size_t turbo_agent_state_plan_step_count_impl(const json_value_t *state);
CXX_C_API size_t turbo_agent_state_plan_step_index_impl(const json_value_t *state);
CXX_C_API const char *turbo_agent_state_current_plan_step_text_impl(
    const json_value_t *state);
CXX_C_API int turbo_agent_state_advance_plan_impl(json_value_t *state);
CXX_C_API int turbo_agent_state_plan_complete_impl(const json_value_t *state);
CXX_C_API int turbo_agent_state_request_replan_impl(json_value_t *state,
                                                    const char *reason);
CXX_C_API int turbo_agent_state_set_replan_limit_impl(json_value_t *state,
                                                      size_t max_replans);
CXX_C_API int turbo_agent_state_replan_requested_impl(const json_value_t *state);
CXX_C_API size_t turbo_agent_state_replan_count_impl(const json_value_t *state);
CXX_C_API size_t turbo_agent_state_replan_limit_impl(const json_value_t *state);
CXX_C_API const char *turbo_agent_state_replan_reason_impl(const json_value_t *state);
CXX_C_API int turbo_agent_state_set_guardrail_rejection_impl(json_value_t *state,
                                                             const char *phase,
                                                             const char *reason);
CXX_C_API const char *turbo_agent_state_guardrail_rejection_phase_impl(
    const json_value_t *state);
CXX_C_API const char *turbo_agent_state_guardrail_rejection_reason_impl(
    const json_value_t *state);
CXX_C_API int turbo_agent_state_set_failure_impl(json_value_t *state, const char *kind,
                                                 const char *reason);
CXX_C_API const char *turbo_agent_state_failure_kind_impl(const json_value_t *state);
CXX_C_API const char *turbo_agent_state_failure_reason_impl(const json_value_t *state);
CXX_C_API int turbo_agent_state_set_final_answer_impl(json_value_t *state,
                                                      const char *text);
CXX_C_API int turbo_agent_state_request_review_impl(json_value_t *state,
                                                    const char *note);
CXX_C_API int turbo_agent_state_set_review_approved_impl(json_value_t *state,
                                                         int approved);
CXX_C_API int turbo_agent_state_clear_review_impl(json_value_t *state);
CXX_C_API int turbo_agent_state_review_required_impl(const json_value_t *state);
CXX_C_API int turbo_agent_state_review_approved_impl(const json_value_t *state);
CXX_C_API const char *turbo_agent_state_review_note_impl(const json_value_t *state);
CXX_C_API int turbo_agent_state_set_active_agent_impl(json_value_t *state,
                                                      const char *active_agent);
CXX_C_API int turbo_agent_state_request_handoff_impl(json_value_t *state,
                                                     const char *target_agent,
                                                     const char *reason);
CXX_C_API int turbo_agent_state_commit_handoff_impl(json_value_t *state);
CXX_C_API int turbo_agent_state_append_supervisor_inbox_message_impl(
    json_value_t *state, const char *source_agent, const char *text);
CXX_C_API const char *turbo_agent_state_active_agent_impl(const json_value_t *state);
CXX_C_API const char *turbo_agent_state_handoff_target_agent_impl(
    const json_value_t *state);
CXX_C_API const char *turbo_agent_state_handoff_reason_impl(
    const json_value_t *state);
CXX_C_API const json_value_t *turbo_agent_state_supervisor_inbox_impl(
    const json_value_t *state);
CXX_C_API size_t turbo_agent_state_supervisor_inbox_count_impl(
    const json_value_t *state);
CXX_C_API const json_value_t *turbo_agent_state_supervisor_inbox_at_impl(
    const json_value_t *state, size_t index);
CXX_C_API const json_value_t *turbo_agent_state_supervisor_handoff_history_impl(
    const json_value_t *state);
CXX_C_API const char *turbo_agent_state_last_output_text_impl(
    const json_value_t *state);
CXX_C_API const char *turbo_agent_state_latest_executor_output_text_impl(
    const json_value_t *state);
CXX_C_API int turbo_agent_state_set_model_error_impl(json_value_t *state,
                                                     const char *phase,
                                                     const char *detail);
CXX_C_API const char *turbo_agent_state_model_error_phase_impl(
    const json_value_t *state);
CXX_C_API const char *turbo_agent_state_model_error_detail_impl(
    const json_value_t *state);
CXX_C_API size_t turbo_agent_state_pending_tool_calls_impl(const json_value_t *state);

#ifdef TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP
#define turbo_agent_state_set_plan_from_json turbo_agent_state_set_plan_from_json_impl
#define turbo_agent_state_plan turbo_agent_state_plan_impl
#define turbo_agent_state_plan_step_count turbo_agent_state_plan_step_count_impl
#define turbo_agent_state_plan_step_index turbo_agent_state_plan_step_index_impl
#define turbo_agent_state_current_plan_step_text turbo_agent_state_current_plan_step_text_impl
#define turbo_agent_state_advance_plan turbo_agent_state_advance_plan_impl
#define turbo_agent_state_plan_complete turbo_agent_state_plan_complete_impl
#define turbo_agent_state_request_replan turbo_agent_state_request_replan_impl
#define turbo_agent_state_set_replan_limit turbo_agent_state_set_replan_limit_impl
#define turbo_agent_state_replan_requested turbo_agent_state_replan_requested_impl
#define turbo_agent_state_replan_count turbo_agent_state_replan_count_impl
#define turbo_agent_state_replan_limit turbo_agent_state_replan_limit_impl
#define turbo_agent_state_replan_reason turbo_agent_state_replan_reason_impl
#define turbo_agent_state_set_guardrail_rejection turbo_agent_state_set_guardrail_rejection_impl
#define turbo_agent_state_guardrail_rejection_phase turbo_agent_state_guardrail_rejection_phase_impl
#define turbo_agent_state_guardrail_rejection_reason turbo_agent_state_guardrail_rejection_reason_impl
#define turbo_agent_state_set_failure turbo_agent_state_set_failure_impl
#define turbo_agent_state_failure_kind turbo_agent_state_failure_kind_impl
#define turbo_agent_state_failure_reason turbo_agent_state_failure_reason_impl
#define turbo_agent_state_set_final_answer turbo_agent_state_set_final_answer_impl
#define turbo_agent_state_request_review turbo_agent_state_request_review_impl
#define turbo_agent_state_set_review_approved turbo_agent_state_set_review_approved_impl
#define turbo_agent_state_clear_review turbo_agent_state_clear_review_impl
#define turbo_agent_state_review_required turbo_agent_state_review_required_impl
#define turbo_agent_state_review_approved turbo_agent_state_review_approved_impl
#define turbo_agent_state_review_note turbo_agent_state_review_note_impl
#define turbo_agent_state_set_active_agent turbo_agent_state_set_active_agent_impl
#define turbo_agent_state_request_handoff turbo_agent_state_request_handoff_impl
#define turbo_agent_state_commit_handoff turbo_agent_state_commit_handoff_impl
#define turbo_agent_state_append_supervisor_inbox_message \
  turbo_agent_state_append_supervisor_inbox_message_impl
#define turbo_agent_state_active_agent turbo_agent_state_active_agent_impl
#define turbo_agent_state_handoff_target_agent \
  turbo_agent_state_handoff_target_agent_impl
#define turbo_agent_state_handoff_reason turbo_agent_state_handoff_reason_impl
#define turbo_agent_state_supervisor_inbox turbo_agent_state_supervisor_inbox_impl
#define turbo_agent_state_supervisor_inbox_count \
  turbo_agent_state_supervisor_inbox_count_impl
#define turbo_agent_state_supervisor_inbox_at \
  turbo_agent_state_supervisor_inbox_at_impl
#define turbo_agent_state_supervisor_handoff_history \
  turbo_agent_state_supervisor_handoff_history_impl
#define turbo_agent_state_last_output_text turbo_agent_state_last_output_text_impl
#define turbo_agent_state_latest_executor_output_text \
  turbo_agent_state_latest_executor_output_text_impl
#define turbo_agent_state_set_model_error turbo_agent_state_set_model_error_impl
#define turbo_agent_state_model_error_phase turbo_agent_state_model_error_phase_impl
#define turbo_agent_state_model_error_detail turbo_agent_state_model_error_detail_impl
#define turbo_agent_state_pending_tool_calls turbo_agent_state_pending_tool_calls_impl
#endif

#ifdef __cplusplus
}
#endif

#endif
