#ifndef TURBO_AGENT_RUNTIME_REMOTE_CHTTP_H
#define TURBO_AGENT_RUNTIME_REMOTE_CHTTP_H

#include <platform.h>
#include <http_server/http.h>

#include "turbo_agent_runtime_remote.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Mount the runtime JSON-RPC endpoint on a caller-owned CHTTP server.
 *
 * The remote dispatcher and server remain caller-owned. Both must outlive the
 * registered route. A NULL or empty path selects "/v1/runtime/jsonrpc".
 */
CXX_C_API int turbo_agent_runtime_remote_chttp_mount(
    turbo_agent_runtime_remote_t *remote, chttp_server *server, const char *path);

#ifdef __cplusplus
}
#endif

#endif
