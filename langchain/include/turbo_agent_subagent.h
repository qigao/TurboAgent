#ifndef TURBO_AGENT_SUBAGENT_H
#define TURBO_AGENT_SUBAGENT_H

#include <turbo_agent_api.h>

#include "turbo_agent_app.h"
#include "turbo_tool_registry.h"
#include "turbo_tool_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum turbo_agent_subagent_mode_e {
  TURBO_AGENT_SUBAGENT_STATELESS = 0,
  TURBO_AGENT_SUBAGENT_SHARED_APP = 1
} turbo_agent_subagent_mode_t;

typedef enum turbo_agent_subagent_result_kind_e {
  TURBO_AGENT_SUBAGENT_RESULT_TEXT = 0,
  TURBO_AGENT_SUBAGENT_RESULT_JSON = 1
} turbo_agent_subagent_result_kind_t;

typedef struct turbo_agent_subagent_tool_config_s {
  const char *name;
  const char *description;
  const char *parameters_json;
  const json_value_t *parameters_schema;
  int strict;
  turbo_agent_subagent_mode_t mode;
  turbo_agent_subagent_result_kind_t result_kind;
  const turbo_agent_session_config_t *session_config;
} turbo_agent_subagent_tool_config_t;

/**
 * @brief Add one subagent-backed tool to a native tool runtime.
 *
 * The tool accepts one of:
 * - `{"messages":[...]}`
 * - `{"input":"..."}`
 * - any JSON value, which is serialized and forwarded as text input
 *
 * The returned tool result is a JSON object containing at least:
 * - `ok`
 * - `status`
 * - `thread_id`
 * - `run_id`
 * - `checkpoint_id`
 * - `child_thread_id`
 * - `child_run_id`
 * - `child_checkpoint_id`
 * - `child_status`
 * - nullable `parent_agent_run_id`
 * - nullable `parent_tool_call_id`
 * - nullable `parent_tool_name`
 * - nullable `parent_graph_run_id`
 * - nullable `call_frame_id`
 * - `summary`
 *
 * In `TURBO_AGENT_SUBAGENT_RESULT_TEXT` mode, the object also includes
 * `output_text`.
 *
 * In `TURBO_AGENT_SUBAGENT_RESULT_JSON` mode, the object includes both
 * `output_json` and its serialized `output_text`.
 *
 * `TURBO_AGENT_SUBAGENT_STATELESS` creates a fresh ephemeral app per invoke and
 * intentionally does not reuse runtime or memory stores from `session_config`.
 *
 * `TURBO_AGENT_SUBAGENT_SHARED_APP` creates one owned app once and reuses its
 * remembered thread/run state across tool invocations.
 */
CXX_C_API turbo_tool_status_t turbo_agent_subagent_add_tool_runtime(
    turbo_tool_runtime_t *runtime, const turbo_agent_subagent_tool_config_t *config);

/**
 * @brief Add one subagent-backed tool directly to a tool registry.
 *
 * Ownership semantics match `turbo_tool_registry_add(...)`: on success the
 * registry owns the backing subagent entry and destroys it with the registry.
 */
CXX_C_API turbo_tool_status_t turbo_agent_subagent_add_tool_registry(
    turbo_tool_registry_t *registry, const turbo_agent_subagent_tool_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
