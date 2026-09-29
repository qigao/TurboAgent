#ifndef TURBO_AGENT_REQUEST_COMMON_INTERNAL_H
#define TURBO_AGENT_REQUEST_COMMON_INTERNAL_H

#include "turbo_agent_core_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API int turbo_agent_request_parallel_tool_calls_enabled(const turbo_agent_t *agent);
CXX_C_API json_value_t *turbo_agent_request_create(const turbo_agent_t *agent);
CXX_C_API int turbo_agent_request_add_tools_if_any(json_value_t *request, json_value_t *tools);
CXX_C_API int turbo_agent_request_serialize_into_output(json_value_t *request,
                                                        char **out_request_json);

#ifdef __cplusplus
}
#endif

#endif
