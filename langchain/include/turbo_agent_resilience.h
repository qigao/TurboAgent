#ifndef TURBO_AGENT_RESILIENCE_H
#define TURBO_AGENT_RESILIENCE_H

#include <platform.h>

#include "turbo_runtime_control.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_s turbo_agent_t;

#define TURBO_AGENT_RETRY_POLICY_ABI_VERSION 1u
#define TURBO_AGENT_TRANSPORT_V2_ABI_VERSION 1u

typedef struct turbo_agent_retry_policy_s {
  size_t struct_size;
  unsigned int abi_version;
  unsigned int max_attempts;
  unsigned int base_delay_ms;
  unsigned int max_delay_ms;
  unsigned int max_elapsed_ms;
  unsigned int request_timeout_ms;
  unsigned int connect_timeout_ms;
  unsigned int jitter_percent;
  int honor_retry_after;
} turbo_agent_retry_policy_t;

typedef struct turbo_agent_transport_response_s {
  size_t struct_size;
  unsigned int abi_version;
  int transport_status;
  int http_status;
  unsigned int retry_after_ms;
  int retryable;
  const char *provider_request_id;
  /** malloc/free body transferred to the caller. */
  char *body;
} turbo_agent_transport_response_t;

typedef int (*turbo_agent_transport_v2_fn)(const char *request_json,
                                           const turbo_cancel_token_t *cancel_token,
                                           unsigned int request_timeout_ms,
                                           unsigned int connect_timeout_ms,
                                           turbo_agent_transport_response_t *out_response,
                                           void *user_data);
typedef void (*turbo_agent_transport_v2_user_data_free_fn)(void *user_data);

CXX_C_API void turbo_agent_retry_policy_init(turbo_agent_retry_policy_t *policy);

/**
 * Configure retry. max_attempts=1 preserves single-attempt behavior.
 * This control-plane operation requires the agent to be quiescent.
 */
CXX_C_API int turbo_agent_retry_configure(turbo_agent_t *agent,
                                          const turbo_agent_retry_policy_t *policy);

/**
 * Attach structured transport metadata without changing the v1 config ABI.
 * Reconfiguration requires the agent to be quiescent.
 */
CXX_C_API int
turbo_agent_transport_v2_set(turbo_agent_t *agent, turbo_agent_transport_v2_fn transport,
                             void *user_data,
                             turbo_agent_transport_v2_user_data_free_fn user_data_free);

#ifdef __cplusplus
}
#endif

#endif
