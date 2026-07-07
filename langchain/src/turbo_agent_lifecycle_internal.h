#ifndef TURBO_AGENT_LIFECYCLE_INTERNAL_H
#define TURBO_AGENT_LIFECYCLE_INTERNAL_H

#include "turbo_agent_core_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API int turbo_agent_attach_owned_resource(
    turbo_agent_t *agent, void *resource,
    turbo_agent_owned_resource_free_fn free_resource);
CXX_C_API void turbo_agent_clear_last_stream_sse(turbo_agent_t *agent);

#ifdef __cplusplus
}
#endif

#endif
