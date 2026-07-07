#ifndef TURBO_AGENT_STATE_FLOW_INTERNAL_H
#define TURBO_AGENT_STATE_FLOW_INTERNAL_H

#include "turbo_agent_state.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API int turbo_agent_append_completed_step(json_value_t *state, size_t step_index,
                                                const char *step_text,
                                                const char *output_text);
CXX_C_API char *turbo_agent_build_completed_steps_message(const json_value_t *state);

#ifdef __cplusplus
}
#endif

#endif
