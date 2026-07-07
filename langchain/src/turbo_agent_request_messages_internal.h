#ifndef TURBO_AGENT_REQUEST_MESSAGES_INTERNAL_H
#define TURBO_AGENT_REQUEST_MESSAGES_INTERNAL_H

#include "turbo_agent_core_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API const json_value_t *turbo_agent_request_responses_input_source(
    const json_value_t *state, json_value_t *request);
CXX_C_API json_value_t *turbo_agent_request_build_responses_input_messages(
    const json_value_t *input_source);
CXX_C_API json_value_t *turbo_agent_request_build_canonical_messages(
    const turbo_agent_t *agent, const json_value_t *state);
CXX_C_API json_value_t *turbo_agent_request_build_chat_messages(const turbo_agent_t *agent,
                                                                const json_value_t *state);
CXX_C_API int turbo_agent_request_build_anthropic_wire_messages(const turbo_agent_t *agent,
                                                                const json_value_t *state,
                                                                json_value_t **out_messages,
                                                                char **out_system);

#ifdef __cplusplus
}
#endif

#endif
