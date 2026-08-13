#ifndef TURBO_AGENT_TOOL_EXECUTOR_INTERNAL_H
#define TURBO_AGENT_TOOL_EXECUTOR_INTERNAL_H

#include "turbo_agent_runtime.h"
#include "turbo_agent_tool_executor.h"
#include "turbo_runtime_control.h"
#include "turbo_tool_registry.h"

typedef struct turbo_agent_tool_executor_s turbo_agent_tool_executor_t;

typedef struct turbo_agent_tool_execution_s {
  const char *call_id;
  const char *tool_name;
  const char *arguments_json;
  turbo_tool_execution_policy_t policy;
  turbo_tool_status_t status;
  char *output;
  int replayed;
  int approval_granted;
  int arguments_owned;
  const char *turn_key;
} turbo_agent_tool_execution_t;

CXX_C_API int turbo_agent_tool_executor_create(const turbo_agent_tool_executor_config_t *config,
                                               turbo_agent_tool_executor_t **out_executor);
CXX_C_API void turbo_agent_tool_executor_destroy(turbo_agent_tool_executor_t *executor);
CXX_C_API int turbo_agent_tool_executor_execute(
    turbo_agent_tool_executor_t *executor, turbo_agent_runtime_t *runtime,
    const turbo_cancel_token_t *cancel_token, const char *thread_id, const char *run_id,
    const turbo_tool_registry_t *registry, turbo_agent_tool_execution_t *calls, size_t call_count);

#endif
