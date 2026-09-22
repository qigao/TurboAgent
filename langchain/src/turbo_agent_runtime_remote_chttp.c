#include "turbo_agent_runtime_remote_chttp.h"

#include <json_parser.h>

#include <stdlib.h>
#include <string.h>

static const char turbo_agent_runtime_remote_chttp_invalid_request[] =
    "{\"jsonrpc\":\"2.0\",\"result\":null,\"error\":{\"code\":-32600,"
    "\"message\":\"Invalid request\"},\"id\":null}";
static const char turbo_agent_runtime_remote_chttp_internal_error[] =
    "{\"jsonrpc\":\"2.0\",\"result\":null,\"error\":{\"code\":-32603,"
    "\"message\":\"Internal error\"},\"id\":null}";

static int turbo_agent_runtime_remote_chttp_reply(
    chttp_server_response *response, unsigned int status, const char *body) {
  return chttp_server_reply(response, status, "application/json", body, strlen(body));
}

static int turbo_agent_runtime_remote_chttp_handler(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  turbo_agent_runtime_remote_t *remote = (turbo_agent_runtime_remote_t *)user;
  json_value_t *validated = NULL;
  char *request_text = NULL;
  char *response_text = NULL;
  int rc;

  if (!remote || !request || !response) {
    return SALTS_EINVAL;
  }
  if (request->method != CHTTP_METHOD_POST) {
    return turbo_agent_runtime_remote_chttp_reply(
        response, 405u, turbo_agent_runtime_remote_chttp_invalid_request);
  }
  if (request->body_streamed || !request->body || request->body_size == 0u ||
      memchr(request->body, '\0', request->body_size) != NULL) {
    return turbo_agent_runtime_remote_chttp_reply(
        response, 400u, turbo_agent_runtime_remote_chttp_invalid_request);
  }

  validated = json_parse((const char *)request->body, request->body_size);
  if (!validated) {
    return turbo_agent_runtime_remote_chttp_reply(
        response, 400u, turbo_agent_runtime_remote_chttp_invalid_request);
  }
  json_free(validated);

  request_text = (char *)malloc(request->body_size + 1u);
  if (!request_text) {
    return turbo_agent_runtime_remote_chttp_reply(
        response, 500u, turbo_agent_runtime_remote_chttp_internal_error);
  }
  memcpy(request_text, request->body, request->body_size);
  request_text[request->body_size] = '\0';

  rc = turbo_agent_runtime_remote_dispatch_jsonrpc_text(
      remote, request_text, &response_text);
  free(request_text);
  if (rc != 0 || !response_text) {
    turbo_json_serialize_free(response_text);
    return turbo_agent_runtime_remote_chttp_reply(
        response, 500u, turbo_agent_runtime_remote_chttp_internal_error);
  }

  rc = chttp_server_reply(response, 200u, "application/json",
                          response_text, strlen(response_text));
  turbo_json_serialize_free(response_text);
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_chttp_mount(
    turbo_agent_runtime_remote_t *remote, chttp_server *server, const char *path) {
  const char *route = (path && path[0]) ? path : "/v1/runtime/jsonrpc";
  if (!remote || !server) {
    return SALTS_EINVAL;
  }
  return chttp_server_post(server, route,
                           turbo_agent_runtime_remote_chttp_handler, remote);
}
