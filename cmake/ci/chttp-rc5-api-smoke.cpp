#include <http_server/rpc_service.h>
#include <http_client/rpc_service.h>
#include <cstddef>

/* An independent C++17 compilation and import-link check of the same C ABI. */
static decltype(&chttp_server_rpc_configure) volatile configure =
    &chttp_server_rpc_configure;
static decltype(&chttp_server_rpc_mount) volatile mount =
    &chttp_server_rpc_mount;
static decltype(&chttp_client_rpc_request_reply) volatile request =
    &chttp_client_rpc_request_reply;
static decltype(&chttp_client_rpc_call) volatile typed =
    &chttp_client_rpc_call;
static decltype(&chttp_async_client_rpc_submit) volatile submit =
    &chttp_async_client_rpc_submit;
static decltype(&chttp_async_client_rpc_call) volatile typed_async =
    &chttp_async_client_rpc_call;

int main() {
    const chttp_server_rpc_config server = CHTTP_SERVER_RPC_CONFIG_INIT;
    const chttp_client_rpc_call_options call = CHTTP_CLIENT_RPC_CALL_OPTIONS_INIT;
    return server.size != sizeof(server) || call.size != sizeof(call)
        || configure == nullptr || mount == nullptr || request == nullptr
        || typed == nullptr || submit == nullptr || typed_async == nullptr;
}
