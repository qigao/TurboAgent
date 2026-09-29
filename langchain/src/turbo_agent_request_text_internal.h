#ifndef TURBO_AGENT_REQUEST_TEXT_INTERNAL_H
#define TURBO_AGENT_REQUEST_TEXT_INTERNAL_H

#include "turbo_agent_core_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API char *turbo_agent_request_join_text_blocks(const char *left, const char *separator,
                                                     const char *right);
CXX_C_API char *turbo_agent_build_effective_instructions(const turbo_agent_t *agent,
                                                         const json_value_t *state);

#ifdef __cplusplus
}
#endif

#endif
