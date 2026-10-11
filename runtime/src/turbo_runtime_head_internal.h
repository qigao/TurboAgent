#ifndef TURBO_RUNTIME_HEAD_INTERNAL_H
#define TURBO_RUNTIME_HEAD_INTERNAL_H
#include "turbo_runtime_history.h"
int turbo_runtime_head_initialize(turbo_agent_runtime_t *runtime, const char *thread_id);
int turbo_runtime_head_publish(turbo_agent_runtime_t *runtime, const char *thread_id,
                               const json_value_t *run);
#endif
