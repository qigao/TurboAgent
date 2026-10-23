#ifndef TURBO_AGENT_TOOL_EXECUTOR_INTERNAL_H
#define TURBO_AGENT_TOOL_EXECUTOR_INTERNAL_H

#include <stddef.h>

#include "turbo_agent_policy.h"
#include "turbo_agent_runtime.h"
#include "turbo_agent_tool_executor.h"
#include "turbo_event.h"
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
  json_value_t *detail;
  int detail_capture_failed;
  int replayed;
  int approval_granted;
  int arguments_owned;
  const char *turn_key;
  const char *policy_reason;
  /* Nested calls only: legacy identity is checked before admitting a new key.
   * Borrowed with turn_key through synchronous dispatch and worker drain. */
  const char *legacy_turn_key;
} turbo_agent_tool_execution_t;

CXX_C_API int turbo_agent_tool_executor_create(const turbo_agent_tool_executor_config_t *config,
                                               turbo_agent_tool_executor_t **out_executor);
CXX_C_API void turbo_agent_tool_executor_destroy(turbo_agent_tool_executor_t *executor);
/* All turn_key/call_id pairs must be unique within the batch's shared Run.
 * A collision returns SALTS_EPROTO before any callback or Journal write, also
 * without a Runtime and when an entry was already denied or marked replayed.
 * One owned executor serializes separate callers through Journal lookup,
 * dispatch and commit; each caller owns its call buffers and keeps the shared
 * registry/policy immutable until all callers drain. Existing outcome replay
 * preserves durable status; successful model delivery cancellation is checked
 * by ToolNode. This does not provide concurrent Agent/Router operation or
 * cross-executor/Runtime/process atomic effect admission. */
CXX_C_API int turbo_agent_tool_executor_execute(
    turbo_agent_tool_executor_t *executor, turbo_agent_runtime_t *runtime,
    const turbo_cancel_token_t *cancel_token, const char *thread_id, const char *run_id,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    const turbo_tool_registry_t *registry, const turbo_agent_policy_t *policy,
    turbo_agent_tool_execution_t *calls, size_t call_count);

#endif
