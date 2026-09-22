#ifndef TURBO_AGENT_TRANSPORT_INTERNAL_H
#define TURBO_AGENT_TRANSPORT_INTERNAL_H

#include "turbo_agent_core_internal.h"

#include <http_client/http.h>

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API int turbo_agent_http_transport(const char *request_json, char **out_response_json,
                                         void *user_data);
CXX_C_API int turbo_agent_build_http_headers_openai(const turbo_agent_t *agent,
                                                     chttp_header *headers,
                                                     size_t capacity,
                                                     size_t *out_count);
CXX_C_API int turbo_agent_build_http_headers_anthropic(const turbo_agent_t *agent,
                                                       chttp_header *headers,
                                                       size_t capacity,
                                                       size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif
