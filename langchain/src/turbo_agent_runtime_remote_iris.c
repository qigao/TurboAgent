#include "turbo_agent_runtime_remote_iris.h"

#include "router.h"
#include "turbo_agent_util_internal.h"

#include <stdlib.h>
#include <string.h>

struct turbo_agent_runtime_remote_iris_s {
  turbo_agent_runtime_remote_t *remote;
  char *path;
  iris_app_t *bound_app;
};

static void turbo_agent_runtime_remote_iris_unbound(void *rpc_context, void *user_data) {
  turbo_agent_runtime_remote_iris_t *bridge =
      (turbo_agent_runtime_remote_iris_t *)rpc_context;
  iris_app_t *app = (iris_app_t *)user_data;

  if (!bridge) {
    return;
  }
  if (!app || bridge->bound_app == app) {
    bridge->bound_app = NULL;
  }
}

static http_method_t turbo_agent_runtime_remote_iris_parse_method(const char *method) {
  if (!method) {
    return -1;
  }
  if (strcmp(method, "GET") == 0) {
    return HTTP_GET;
  }
  if (strcmp(method, "POST") == 0) {
    return HTTP_POST;
  }
  if (strcmp(method, "PUT") == 0) {
    return HTTP_PUT;
  }
  if (strcmp(method, "PATCH") == 0) {
    return HTTP_PATCH;
  }
  if (strcmp(method, "DELETE") == 0) {
    return HTTP_DELETE;
  }
  if (strcmp(method, "OPTIONS") == 0) {
    return HTTP_OPTIONS;
  }
  if (strcmp(method, "HEAD") == 0) {
    return HTTP_HEAD;
  }
  return -1;
}

static void turbo_agent_runtime_remote_iris_send_http_response(Res *res,
                                                               http_response_t *response) {
  char *content_type = NULL;

  if (!res || !response) {
    http_response_free(response);
    return;
  }

  content_type = http_response_get_header(response, "Content-Type");
  if (response->body) {
    reply(res, response->status_code, content_type ? content_type : "application/json",
          response->body, response->body_len);
  } else {
    reply(res, response->status_code, content_type ? content_type : "text/plain", "", 0);
  }
  free(content_type);
  http_response_free(response);
}

static void turbo_agent_runtime_remote_iris_handler(Req *req, Res *res) {
  turbo_agent_runtime_remote_iris_t *bridge;
  http_method_t method;
  http_response_t *response;

  if (!req || !req->app || !res) {
    send_json(res, 500,
              "{\"jsonrpc\":\"2.0\",\"result\":null,\"error\":{\"code\":-32603,"
              "\"message\":\"Internal error\"},\"id\":null}");
    return;
  }

  bridge = (turbo_agent_runtime_remote_iris_t *)iris_app_lookup_rpc_context(
      req->app, req->path ? req->path : NULL);
  if (!bridge || !bridge->remote) {
    send_json(res, 500,
              "{\"jsonrpc\":\"2.0\",\"result\":null,\"error\":{\"code\":-32603,"
              "\"message\":\"Internal error\"},\"id\":null}");
    return;
  }

  method = turbo_agent_runtime_remote_iris_parse_method(req->method);
  response = turbo_agent_runtime_remote_handle_http_jsonrpc(
      bridge->remote, method, req->path ? req->path : bridge->path,
      req->body ? req->body : "");
  if (!response) {
    send_json(res, 500,
              "{\"jsonrpc\":\"2.0\",\"result\":null,\"error\":{\"code\":-32603,"
              "\"message\":\"Internal error\"},\"id\":null}");
    return;
  }

  turbo_agent_runtime_remote_iris_send_http_response(res, response);
}

CXX_C_API turbo_agent_runtime_remote_iris_t *turbo_agent_runtime_remote_iris_create(
    const turbo_agent_runtime_remote_iris_config_t *config) {
  turbo_agent_runtime_remote_iris_t *bridge;
  const char *path;

  if (!config || !config->remote) {
    return NULL;
  }

  path = (config->path && config->path[0]) ? config->path : "/v1/runtime/jsonrpc";
  bridge = (turbo_agent_runtime_remote_iris_t *)calloc(1, sizeof(*bridge));
  if (!bridge) {
    return NULL;
  }
  bridge->remote = config->remote;
  bridge->path = turbo_agent_util_strdup(path);
  if (!bridge->path) {
    free(bridge);
    return NULL;
  }
  return bridge;
}

CXX_C_API void turbo_agent_runtime_remote_iris_destroy(
    turbo_agent_runtime_remote_iris_t *bridge) {
  if (!bridge) {
    return;
  }
  if (bridge->bound_app && bridge->path) {
    iris_app_unbind_rpc_context(bridge->bound_app, bridge->path, bridge);
    bridge->bound_app = NULL;
  }
  tstr_free(bridge->path);
  free(bridge);
}

CXX_C_API int turbo_agent_runtime_remote_iris_mount(turbo_agent_runtime_remote_iris_t *bridge,
                                                    iris_app_t *app) {
  if (!bridge || !bridge->remote || !bridge->path || !app) {
    return -1;
  }
  if (iris_app_bind_rpc_context_ex(app, bridge->path, bridge,
                                   turbo_agent_runtime_remote_iris_unbound, app) != 0) {
    return -1;
  }
  bridge->bound_app = app;
  iris_app_post(app, bridge->path, turbo_agent_runtime_remote_iris_handler);
  iris_app_get(app, bridge->path, turbo_agent_runtime_remote_iris_handler);
  iris_app_put(app, bridge->path, turbo_agent_runtime_remote_iris_handler);
  iris_app_patch(app, bridge->path, turbo_agent_runtime_remote_iris_handler);
  iris_app_delete(app, bridge->path, turbo_agent_runtime_remote_iris_handler);
  return 0;
}
