#ifndef TURBO_AGENT_STATE_OUTPUT_INTERNAL_H
#define TURBO_AGENT_STATE_OUTPUT_INTERNAL_H

#include "turbo_agent_state_core_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API int turbo_agent_state_executor_tool_results_failed_impl(
    const json_value_t *state);
CXX_C_API const json_value_t *turbo_agent_state_latest_tool_results_event_impl(
    const json_value_t *state);
CXX_C_API const json_value_t *turbo_agent_state_tool_results_outputs_impl(
    const json_value_t *event);
CXX_C_API const char *turbo_agent_state_tool_result_child_thread_id_impl(
    const json_value_t *output_item);
CXX_C_API const char *turbo_agent_state_tool_result_child_run_id_impl(
    const json_value_t *output_item);
CXX_C_API const char *turbo_agent_state_tool_result_child_checkpoint_id_impl(
    const json_value_t *output_item);
CXX_C_API const char *turbo_agent_state_tool_result_child_status_impl(
    const json_value_t *output_item);
CXX_C_API const char *turbo_agent_state_tool_result_parent_agent_run_id_impl(
    const json_value_t *output_item);
CXX_C_API const char *turbo_agent_state_tool_result_parent_tool_call_id_impl(
    const json_value_t *output_item);
CXX_C_API const char *turbo_agent_state_tool_result_parent_tool_name_impl(
    const json_value_t *output_item);
CXX_C_API const char *turbo_agent_state_tool_result_parent_graph_run_id_impl(
    const json_value_t *output_item);
CXX_C_API const char *turbo_agent_state_tool_result_call_frame_id_impl(
    const json_value_t *output_item);
CXX_C_API const char *turbo_agent_state_final_answer_text_impl(
    const json_value_t *state);
CXX_C_API int turbo_agent_state_parse_final_output_json_impl(const json_value_t *state,
                                                             json_value_t **out_json);

#ifdef TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP
#define turbo_agent_state_latest_tool_results_event \
  turbo_agent_state_latest_tool_results_event_impl
#define turbo_agent_state_tool_results_outputs \
  turbo_agent_state_tool_results_outputs_impl
#define turbo_agent_state_tool_result_child_thread_id \
  turbo_agent_state_tool_result_child_thread_id_impl
#define turbo_agent_state_tool_result_child_run_id \
  turbo_agent_state_tool_result_child_run_id_impl
#define turbo_agent_state_tool_result_child_checkpoint_id \
  turbo_agent_state_tool_result_child_checkpoint_id_impl
#define turbo_agent_state_tool_result_child_status \
  turbo_agent_state_tool_result_child_status_impl
#define turbo_agent_state_tool_result_parent_agent_run_id \
  turbo_agent_state_tool_result_parent_agent_run_id_impl
#define turbo_agent_state_tool_result_parent_tool_call_id \
  turbo_agent_state_tool_result_parent_tool_call_id_impl
#define turbo_agent_state_tool_result_parent_tool_name \
  turbo_agent_state_tool_result_parent_tool_name_impl
#define turbo_agent_state_tool_result_parent_graph_run_id \
  turbo_agent_state_tool_result_parent_graph_run_id_impl
#define turbo_agent_state_tool_result_call_frame_id \
  turbo_agent_state_tool_result_call_frame_id_impl
#define turbo_agent_state_executor_tool_results_failed \
  turbo_agent_state_executor_tool_results_failed_impl
#define turbo_agent_state_final_answer_text turbo_agent_state_final_answer_text_impl
#define turbo_agent_state_parse_final_output_json \
  turbo_agent_state_parse_final_output_json_impl
#endif

#ifdef __cplusplus
}
#endif

#endif
