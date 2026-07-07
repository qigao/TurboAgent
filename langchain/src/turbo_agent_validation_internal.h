#ifndef TURBO_AGENT_VALIDATION_INTERNAL_H
#define TURBO_AGENT_VALIDATION_INTERNAL_H

#include "turbo_agent_core_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API int turbo_agent_structured_output_valid_for_state(const turbo_agent_t *agent,
                                                            const json_value_t *state,
                                                            char **out_reason);
CXX_C_API json_value_t *turbo_agent_build_structured_retry_state(const json_value_t *state,
                                                                 size_t attempt_index,
                                                                 const char *reason);

#ifdef __cplusplus
}
#endif

#endif
