#include <http_server/rpc_service.h>
#include <http_client/rpc_service.h>
#include <stddef.h>

/* Direct C11 ABI symbols must link from the installed Server/Client SDK,
 * with no standalone RpcService library, compatibility shim, or fallback. */
static int (*volatile server_configure)(
    chttp_server *, const chttp_server_rpc_config *) = &chttp_server_rpc_configure;
static int (*volatile server_mount)(
    chttp_server *, const chttp_server_rpc_mount_options *) = &chttp_server_rpc_mount;
static int (*volatile server_register)(
    chttp_server *, const char *, const crpc_method *,
    crpc_server_method_fn, void *) = &chttp_server_rpc_register;
static int (*volatile client_request)(
    chttp_client *, const chttp_client_rpc_limits *, const crpc_options *,
    crpc_response *, crpc_error *) = &chttp_client_rpc_request_reply;
static int (*volatile client_typed)(
    chttp_client *, const chttp_client_rpc_call_options *,
    chttp_client_rpc_outcome *, crpc_error *) = &chttp_client_rpc_call;
static int (*volatile async_request)(
    chttp_async_client *, const chttp_client_rpc_limits *, const crpc_options *,
    chttp_async_rpc_complete_fn, void *, chttp_request *) = &chttp_async_client_rpc_submit;
static int (*volatile async_typed)(
    chttp_async_client *, const chttp_client_rpc_call_options *,
    chttp_async_client_rpc_complete_fn, void *, chttp_request *) = &chttp_async_client_rpc_call;

int main(void) {
  const chttp_server_rpc_config server = CHTTP_SERVER_RPC_CONFIG_INIT;
  const chttp_server_rpc_mount_options mount = CHTTP_SERVER_RPC_MOUNT_OPTIONS_INIT;
  const chttp_client_rpc_limits limits = CHTTP_CLIENT_RPC_LIMITS_INIT;
  const chttp_client_rpc_call_options call = CHTTP_CLIENT_RPC_CALL_OPTIONS_INIT;
  return server.size != sizeof(server) || mount.size != sizeof(mount)
      || limits.size != sizeof(limits) || call.size != sizeof(call)
      || server_configure == NULL || server_mount == NULL
      || server_register == NULL || client_request == NULL
      || client_typed == NULL || async_request == NULL || async_typed == NULL;
}
