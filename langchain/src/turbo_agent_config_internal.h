#ifndef TURBO_AGENT_CONFIG_INTERNAL_H
#define TURBO_AGENT_CONFIG_INTERNAL_H

#include "turbo_agent_core_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API const turbo_model_provider_t *turbo_agent_resolve_provider(
    const turbo_model_provider_t *provider, turbo_agent_api_mode_t api_mode,
    turbo_agent_api_mode_t *out_api_mode);
CXX_C_API int turbo_agent_apply_core_config(turbo_agent_t *agent,
                                            const turbo_agent_config_t *config,
                                            const turbo_model_provider_t *provider);
CXX_C_API int turbo_agent_attach_http_client(turbo_agent_t *agent,
                                             const turbo_agent_config_t *config,
                                             const turbo_model_provider_t *provider);

#ifdef __cplusplus
}
#endif

#endif
