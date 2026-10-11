#ifndef TURBO_AGENT_CONTEXT_INTERNAL_H
#define TURBO_AGENT_CONTEXT_INTERNAL_H

#include "turbo_agent_context.h"
#include "turbo_agent_core_internal.h"
#include "turbo_agent_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_context_s turbo_agent_context_t;

int turbo_agent_context_create(turbo_agent_runtime_t *runtime, turbo_agent_t *agent,
                               const char *thread_id, const turbo_agent_context_policy_t *policy,
                               turbo_agent_context_t **out_context);
void turbo_agent_context_destroy(turbo_agent_context_t *context);
int turbo_agent_context_prepare(turbo_agent_context_t *context, json_value_t *state,
                                int force_compaction);
int turbo_agent_context_handle_overflow(turbo_agent_context_t *context, json_value_t *state,
                                        int transport_status, const char *response_json);
int turbo_agent_context_status(turbo_agent_context_t *context, json_value_t **out_status);

#ifdef __cplusplus
}
#endif

#endif
