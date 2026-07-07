#ifndef TURBO_AGENT_REQUEST_STRUCTURED_OUTPUT_INTERNAL_H
#define TURBO_AGENT_REQUEST_STRUCTURED_OUTPUT_INTERNAL_H

#include "turbo_agent_core_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API int turbo_agent_request_add_structured_output(const turbo_agent_t *agent,
                                                        json_value_t *request, int chat_mode,
                                                        int compatible_mode,
                                                        int anthropic_mode);

#ifdef __cplusplus
}
#endif

#endif
