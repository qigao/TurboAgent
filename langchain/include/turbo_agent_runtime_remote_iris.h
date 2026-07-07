#ifndef TURBO_AGENT_RUNTIME_REMOTE_IRIS_H
#define TURBO_AGENT_RUNTIME_REMOTE_IRIS_H

#include <platform.h>

#include "iris_app.h"
#include "turbo_agent_runtime_remote.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_runtime_remote_iris_s turbo_agent_runtime_remote_iris_t;

typedef struct turbo_agent_runtime_remote_iris_config_s {
  turbo_agent_runtime_remote_t *remote;
  const char *path;
} turbo_agent_runtime_remote_iris_config_t;

/**
 * @brief Create one Iris bridge over an existing runtime remote dispatcher.
 *
 * The bridge does not own the runtime remote or the iris app. It only stores
 * the mount path and adapts Iris Req/Res objects into the existing remote
 * JSON-RPC contract.
 */
CXX_C_API turbo_agent_runtime_remote_iris_t *turbo_agent_runtime_remote_iris_create(
    const turbo_agent_runtime_remote_iris_config_t *config);

/**
 * @brief Destroy one Iris bridge instance.
 */
CXX_C_API void turbo_agent_runtime_remote_iris_destroy(
    turbo_agent_runtime_remote_iris_t *bridge);

/**
 * @brief Mount the runtime remote bridge on one Iris app.
 *
 * This registers the bridge on the given `path` and binds bridge state through
 * Iris's app-local RPC endpoint registry. It may coexist with Iris's own
 * `rpc_setup_endpoint(...)` on the same app as long as each endpoint path is
 * unique.
 */
CXX_C_API int turbo_agent_runtime_remote_iris_mount(turbo_agent_runtime_remote_iris_t *bridge,
                                                    iris_app_t *app);

#ifdef __cplusplus
}
#endif

#endif
