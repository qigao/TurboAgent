#include "turbo_agent_core_internal.h"
#include "turbo_agent_config_internal.h"
#include "turbo_agent_transport_internal.h"
#include "turbo_agent_lifecycle_internal.h"

#include "http_client.h"
#include "CoroNet/turbo_coro_context.h"
#include "turbo_action_tool.h"
#include "turbo_tool_registry.h"

#include <stdlib.h>

CXX_C_API void turbo_agent_clear_last_stream_sse(turbo_agent_t *agent) {
  if (!agent) {
    return;
  }

  free(agent->last_stream_sse);
  agent->last_stream_sse = NULL;
  agent->last_stream_sse_len = 0;
}

static void turbo_agent_finalize_transport(turbo_agent_t *agent) {
  if (agent && !agent->transport_fn) {
    agent->transport_fn = turbo_agent_http_transport;
    agent->transport_user_data = agent;
  }
}

static void turbo_agent_take_owned_tool_registry(turbo_agent_t *agent,
                                                 turbo_tool_registry_t *tool_registry) {
  if (!agent) {
    return;
  }

  agent->tool_registry = tool_registry;
  agent->owns_tool_registry = tool_registry ? 1 : 0;
}

CXX_C_API int turbo_agent_attach_owned_resource(
    turbo_agent_t *agent, void *resource,
    turbo_agent_owned_resource_free_fn free_resource) {
  if (!agent || !resource || !free_resource) {
    return -1;
  }

  if (agent->owned_resource || agent->owned_resource_free) {
    return -1;
  }

  agent->owned_resource = resource;
  agent->owned_resource_free = free_resource;
  return 0;
}

static void turbo_agent_release_owned_resources(turbo_agent_t *agent) {
  size_t i;

  if (!agent) {
    return;
  }

  if (agent->owns_http_client && agent->http_client) {
    http_client_destroy(agent->http_client);
  }
  if (agent->owns_http_context && agent->http_context) {
    coro_context_destroy(agent->http_context);
  }
  if (agent->owns_tool_registry && agent->tool_registry) {
    turbo_tool_registry_destroy(agent->tool_registry);
  }
  if (agent->owned_resource_free) {
    agent->owned_resource_free(agent->owned_resource);
  }
  for (i = 0; i < agent->middleware_count; ++i) {
    if (agent->middlewares[i].user_data_free) {
      agent->middlewares[i].user_data_free(agent->middlewares[i].user_data);
    }
  }
  for (i = 0; i < agent->guardrail_count; ++i) {
    if (agent->guardrails[i].user_data_free) {
      agent->guardrails[i].user_data_free(agent->guardrails[i].user_data);
    }
  }
  for (i = 0; i < agent->trace_sink_count; ++i) {
    if (agent->trace_sinks[i].user_data_free) {
      agent->trace_sinks[i].user_data_free(agent->trace_sinks[i].user_data);
    }
  }
  if (agent->has_store && agent->store.user_data_free) {
    agent->store.user_data_free(agent->store.user_data);
  }
  free(agent->middlewares);
  free(agent->guardrails);
  free(agent->trace_sinks);
  for (i = 0; i < agent->trace_bind_sink_count; ++i) {
    if (agent->trace_bind_sinks[i].user_data_free) {
      agent->trace_bind_sinks[i].user_data_free(agent->trace_bind_sinks[i].user_data);
    }
  }
  free(agent->trace_bind_sinks);
}

static void turbo_agent_free_strings(turbo_agent_t *agent) {
  if (!agent) {
    return;
  }

  tstr_free(agent->api_key);
  tstr_free(agent->model);
  tstr_free(agent->base_url);
  tstr_free(agent->endpoint_path);
  tstr_free(agent->instructions);
  tstr_free(agent->structured_output_name);
  tstr_free(agent->structured_output_schema_json);
  turbo_agent_clear_last_stream_sse(agent);
}

static turbo_agent_t *turbo_agent_create_with_owned_tool_registry(
    const turbo_agent_config_t *config, turbo_tool_registry_t *tool_registry) {
  turbo_agent_config_t bridged_config;
  turbo_agent_t *agent;

  if (!config || !tool_registry) {
    return NULL;
  }

  bridged_config = *config;
  bridged_config.tool_registry = tool_registry;
  agent = turbo_agent_create(&bridged_config);
  if (!agent) {
    turbo_tool_registry_destroy(tool_registry);
    return NULL;
  }

  turbo_agent_take_owned_tool_registry(agent, tool_registry);
  return agent;
}

CXX_C_API turbo_agent_t *turbo_agent_create(const turbo_agent_config_t *config) {
  turbo_agent_t *agent;
  const turbo_model_provider_t *provider;

  if (!config || !config->model) {
    return NULL;
  }

  if (!config->transport_fn && !config->http_client && !config->api_key) {
    return NULL;
  }

  agent = (turbo_agent_t *)calloc(1, sizeof(*agent));
  if (!agent) {
    return NULL;
  }

  provider = turbo_agent_resolve_provider(config->provider, config->api_mode, &agent->api_mode);
  if (turbo_agent_apply_core_config(agent, config, provider) != 0) {
    turbo_agent_destroy(agent);
    return NULL;
  }

  if (turbo_agent_attach_http_client(agent, config, provider) != 0) {
    turbo_agent_destroy(agent);
    return NULL;
  }

  turbo_agent_finalize_transport(agent);
  return agent;
}

CXX_C_API const turbo_tool_registry_t *
turbo_agent_tool_registry(const turbo_agent_t *agent) {
  return agent ? agent->tool_registry : NULL;
}

CXX_C_API size_t turbo_agent_tool_count(const turbo_agent_t *agent) {
  return turbo_tool_registry_count(turbo_agent_tool_registry(agent));
}

CXX_C_API turbo_agent_t *turbo_agent_create_with_action_tools(
    const turbo_agent_config_t *config,
    const turbo_action_tool_registry_t *action_tool_registry) {
  turbo_tool_registry_t *tool_registry;

  if (!config || !action_tool_registry) {
    return NULL;
  }

  tool_registry = turbo_action_tool_registry_build_tool_registry_bridge(action_tool_registry);
  if (!tool_registry) {
    return NULL;
  }

  return turbo_agent_create_with_owned_tool_registry(config, tool_registry);
}

CXX_C_API void turbo_agent_destroy(turbo_agent_t *agent) {
  if (!agent) {
    return;
  }

  turbo_agent_release_owned_resources(agent);
  turbo_agent_free_strings(agent);
  free(agent);
}
