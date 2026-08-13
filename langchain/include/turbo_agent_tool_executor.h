#ifndef TURBO_AGENT_TOOL_EXECUTOR_H
#define TURBO_AGENT_TOOL_EXECUTOR_H

#include <platform.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_s turbo_agent_t;
typedef struct turbo_agent_session_s turbo_agent_session_t;

#define TURBO_AGENT_TOOL_EXECUTOR_CONFIG_ABI_VERSION 1u

typedef struct turbo_agent_tool_executor_config_s {
  size_t struct_size;
  unsigned int abi_version;
  size_t max_workers;
  size_t queue_capacity;
  size_t max_batch_calls;
  size_t max_arguments_bytes;
  size_t max_output_bytes;
} turbo_agent_tool_executor_config_t;

/** Fill a configuration with bounded production defaults. */
CXX_C_API void turbo_agent_tool_executor_config_init(turbo_agent_tool_executor_config_t *config);

/**
 * Replace an agent's tool executor configuration.
 *
 * This control-plane operation requires the agent to be quiescent. Tool
 * callbacks remain synchronous and are therefore only cooperatively
 * cancellable at callback boundaries.
 */
CXX_C_API int turbo_agent_tool_executor_configure(turbo_agent_t *agent,
                                                  const turbo_agent_tool_executor_config_t *config);

/** Configure the owned agent of a quiescent session. */
CXX_C_API int
turbo_agent_session_tool_executor_configure(turbo_agent_session_t *session,
                                            const turbo_agent_tool_executor_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
