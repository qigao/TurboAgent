#ifndef TURBO_AGENT_RESILIENCE_INTERNAL_H
#define TURBO_AGENT_RESILIENCE_INTERNAL_H

#include "turbo_agent_resilience.h"
#include <json_parser.h>

int turbo_agent_resilient_transport(turbo_agent_t *agent, json_value_t *state,
                                    const char *request_json, char **out_response_json);
int turbo_agent_usage_record(turbo_agent_t *agent, json_value_t *state,
                             const json_value_t *response);

#endif
