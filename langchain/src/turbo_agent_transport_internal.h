#ifndef TURBO_AGENT_TRANSPORT_INTERNAL_H
#define TURBO_AGENT_TRANSPORT_INTERNAL_H

#include "turbo_agent_core_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API int turbo_agent_http_transport(const char *request_json, char **out_response_json,
                                         void *user_data);
CXX_C_API int turbo_agent_configure_http_client_openai(const turbo_agent_t *agent,
                                                       http_client_t *http_client);
CXX_C_API int turbo_agent_configure_http_client_anthropic(const turbo_agent_t *agent,
                                                          http_client_t *http_client);

#ifdef __cplusplus
}
#endif

#endif
