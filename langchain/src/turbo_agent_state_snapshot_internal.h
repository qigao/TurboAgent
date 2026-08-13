#ifndef TURBO_AGENT_STATE_SNAPSHOT_INTERNAL_H
#define TURBO_AGENT_STATE_SNAPSHOT_INTERNAL_H

#include "turbo_agent_state.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API json_value_t *turbo_agent_state_ensure_versioned_substate(
    json_value_t *root_state, const char *state_key, const char *legacy_input_key,
    const char *event_versions_key);
CXX_C_API json_value_t *turbo_agent_state_control_snapshot_impl(const json_value_t *state);
CXX_C_API json_value_t *
turbo_agent_state_control_snapshot_json_value_impl(const json_value_t *state);
CXX_C_API json_value_t *turbo_agent_state_workflow_snapshot_impl(
    const json_value_t *state);
CXX_C_API json_value_t *
turbo_agent_state_workflow_snapshot_json_value_impl(const json_value_t *state);
CXX_C_API json_value_t *turbo_agent_substate_create(json_value_t *input, json_value_t *events);
CXX_C_API int turbo_agent_append_last_substate_event(json_value_t *root_state,
                                                     const char *event_versions_key,
                                                     const json_value_t *substate);

#ifdef TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP
#define turbo_agent_state_control_snapshot turbo_agent_state_control_snapshot_impl
#define turbo_agent_state_control_snapshot_json_value turbo_agent_state_control_snapshot_json_value_impl
#define turbo_agent_state_workflow_snapshot turbo_agent_state_workflow_snapshot_impl
#define turbo_agent_state_workflow_snapshot_json_value turbo_agent_state_workflow_snapshot_json_value_impl
#endif

#ifdef __cplusplus
}
#endif

#endif
