#include "turbo_agent_request_common_internal.h"

CXX_C_API int turbo_agent_request_parallel_tool_calls_enabled(const turbo_agent_t *agent) {
  if (!agent || !agent->parallel_tool_calls) {
    return 0;
  }

  return 1;
}

CXX_C_API json_value_t *turbo_agent_request_create(const turbo_agent_t *agent) {
  json_value_t *request;

  if (!agent || !agent->model) {
    return NULL;
  }

  request = turbo_json_create_object();
  if (!request) {
    return NULL;
  }

  turbo_json_object_set_string(request, "model", agent->model);
  if (agent->stream_response) {
    turbo_json_object_set_bool(request, "stream", true);
  }
  return request;
}

CXX_C_API int turbo_agent_request_add_tools_if_any(json_value_t *request, json_value_t *tools) {
  if (!request || !tools || turbo_json_type(tools) != TURBO_JSON_ARRAY) {
    turbo_free_json(&tools);
    return -1;
  }

  if (turbo_json_array_size(tools) > 0) {
    turbo_json_object_add(request, "tools", tools);
  } else {
    turbo_free_json(&tools);
  }

  return 0;
}

CXX_C_API int turbo_agent_request_serialize_into_output(json_value_t *request,
                                                        char **out_request_json) {
  char *serialized;

  if (!request || !out_request_json) {
    return -1;
  }

  serialized = turbo_json_serialize(request, NULL);
  turbo_free_json(&request);
  if (!serialized) {
    return -1;
  }

  *out_request_json = serialized;
  return 0;
}
