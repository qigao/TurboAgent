#include "turbo_agent_request_common_internal.h"
#include <string.h>

void turbo_agent_request_config(const turbo_agent_t *agent, turbo_provider_request_t *config) {
  memset(config, 0, sizeof(*config));
  config->model = agent->model;
  config->stream = agent->stream_response;
  config->parallel_tool_calls = agent->parallel_tool_calls;
  config->structured_output_name = agent->structured_output_name;
  config->structured_output_schema_json = agent->structured_output_schema_json;
  config->structured_output_strict = agent->structured_output_strict;
}
