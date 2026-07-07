#ifndef TURBO_AGENT_STATE_BIND_INTERNAL_H
#define TURBO_AGENT_STATE_BIND_INTERNAL_H

#include "turbo_agent_state_core_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API const json_value_t *turbo_agent_state_planner_event_versions_impl(
    const json_value_t *state);
CXX_C_API size_t turbo_agent_state_planner_event_version_count_impl(
    const json_value_t *state);
CXX_C_API const json_value_t *turbo_agent_state_planner_event_version_at_impl(
    const json_value_t *state, size_t index);
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_agent_state_planner_event_version_bind_impl(const turbo_runtime_data_bind_value_t *state,
                                                  size_t index);
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_agent_state_latest_planner_event_version_bind_impl(
    const turbo_runtime_data_bind_value_t *state);
CXX_C_API const json_value_t *turbo_agent_state_executor_event_versions_impl(
    const json_value_t *state);
CXX_C_API size_t turbo_agent_state_executor_event_version_count_impl(
    const json_value_t *state);
CXX_C_API const json_value_t *turbo_agent_state_executor_event_version_at_impl(
    const json_value_t *state, size_t index);
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_agent_state_executor_event_version_bind_impl(const turbo_runtime_data_bind_value_t *state,
                                                   size_t index);
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_agent_state_latest_executor_event_version_bind_impl(
    const turbo_runtime_data_bind_value_t *state);
CXX_C_API const json_value_t *turbo_agent_state_completed_steps_impl(
    const json_value_t *state);
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_agent_state_completed_steps_bind_impl(const turbo_runtime_data_bind_value_t *state);
CXX_C_API size_t turbo_agent_state_completed_step_count_impl(const json_value_t *state);
CXX_C_API const json_value_t *turbo_agent_state_completed_step_at_impl(
    const json_value_t *state, size_t index);
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_agent_state_completed_step_bind_impl(const turbo_runtime_data_bind_value_t *state,
                                           size_t index);
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_agent_state_latest_completed_step_bind_impl(
    const turbo_runtime_data_bind_value_t *state);

#ifdef TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP
#define turbo_agent_state_planner_event_versions turbo_agent_state_planner_event_versions_impl
#define turbo_agent_state_planner_event_version_count \
  turbo_agent_state_planner_event_version_count_impl
#define turbo_agent_state_planner_event_version_at \
  turbo_agent_state_planner_event_version_at_impl
#define turbo_agent_state_planner_event_version_bind \
  turbo_agent_state_planner_event_version_bind_impl
#define turbo_agent_state_latest_planner_event_version_bind \
  turbo_agent_state_latest_planner_event_version_bind_impl
#define turbo_agent_state_executor_event_versions turbo_agent_state_executor_event_versions_impl
#define turbo_agent_state_executor_event_version_count \
  turbo_agent_state_executor_event_version_count_impl
#define turbo_agent_state_executor_event_version_at \
  turbo_agent_state_executor_event_version_at_impl
#define turbo_agent_state_executor_event_version_bind \
  turbo_agent_state_executor_event_version_bind_impl
#define turbo_agent_state_latest_executor_event_version_bind \
  turbo_agent_state_latest_executor_event_version_bind_impl
#define turbo_agent_state_completed_steps turbo_agent_state_completed_steps_impl
#define turbo_agent_state_completed_steps_bind turbo_agent_state_completed_steps_bind_impl
#define turbo_agent_state_completed_step_count turbo_agent_state_completed_step_count_impl
#define turbo_agent_state_completed_step_at turbo_agent_state_completed_step_at_impl
#define turbo_agent_state_completed_step_bind turbo_agent_state_completed_step_bind_impl
#define turbo_agent_state_latest_completed_step_bind \
  turbo_agent_state_latest_completed_step_bind_impl
#endif

#ifdef __cplusplus
}
#endif

#endif
