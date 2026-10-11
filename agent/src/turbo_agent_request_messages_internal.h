#ifndef TURBO_AGENT_REQUEST_MESSAGES_INTERNAL_H
#define TURBO_AGENT_REQUEST_MESSAGES_INTERNAL_H

#include "turbo_agent.h"
#include "turbo_runtime_json.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API const json_value_t *turbo_agent_request_responses_input_source(const json_value_t *state,
                                                                         const char **out_previous_response_id);
CXX_C_API json_value_t *
turbo_agent_request_build_responses_input_messages(const json_value_t *input_source);
CXX_C_API json_value_t *turbo_agent_request_build_canonical_messages(const turbo_agent_t *agent,
                                                                     const json_value_t *state);
CXX_C_API json_value_t *turbo_agent_request_build_canonical_messages_with_options(
    const turbo_agent_t *agent, const json_value_t *state, int include_instructions);
/* Complete state history only: no Agent configuration, memory instructions,
 * committed summary or compaction offset. Returns an independently owned array,
 * or NULL for malformed history/allocation failure without changing state. */
CXX_C_API json_value_t *turbo_agent_request_build_history_messages(const json_value_t *state);
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
