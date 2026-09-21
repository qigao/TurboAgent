#ifndef TURBO_AGENT_CONTEXT_H
#define TURBO_AGENT_CONTEXT_H

#include <turbo_agent_api.h>

#include "turbo_runtime_json.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_CONTEXT_ABI_VERSION 1U

typedef struct turbo_agent_session_s turbo_agent_session_t;

/**
 * @brief Provider strategy that estimates tokens for an arbitrary JSON value.
 *
 * The value is either a complete provider request, an event segment, or a
 * structured summary. Implementations must use checked 64-bit arithmetic and
 * return zero only when out_tokens was written.
 */
typedef int (*turbo_agent_token_estimator_fn)(const json_value_t *value, uint64_t *out_tokens,
                                              void *user_data);

/**
 * @brief Summarize one ordered, immutable source object.
 *
 * source contains previous_summary (nullable) and events (array). The callback
 * returns an owned JSON object with schema_version=1. It must not retain
 * borrowed input pointers after returning.
 */
typedef int (*turbo_agent_context_summarizer_fn)(const json_value_t *source,
                                                 uint64_t max_summary_tokens,
                                                 json_value_t **out_summary, void *user_data);

/** Return 1 only for an explicit provider context-overflow response. */
typedef int (*turbo_agent_context_overflow_detector_fn)(int transport_status,
                                                        const char *response_json, void *user_data);

typedef struct turbo_agent_context_policy_s {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t context_window_tokens;
  uint64_t reserve_output_tokens;
  uint64_t compact_trigger_tokens;
  uint64_t retain_recent_tokens;
  uint64_t max_summary_input_tokens;
  uint64_t max_summary_tokens;
  uint32_t max_compactions_per_turn;
  turbo_agent_token_estimator_fn estimate_tokens;
  turbo_agent_context_summarizer_fn summarize;
  turbo_agent_context_overflow_detector_fn is_context_overflow;
  void *user_data;
} turbo_agent_context_policy_t;

/**
 * @brief Configure and recover the session context projection.
 *
 * The session must own an agent and have a stable thread id. Runtime V1 uses a
 * single-writer PREPARED plus commit-marker protocol; configuring two live
 * writers for one thread is unsupported.
 */
CXX_C_API int turbo_agent_session_context_configure(turbo_agent_session_t *session,
                                                    const turbo_agent_context_policy_t *policy);

/**
 * @brief Force one compaction at a quiescent session safe point.
 *
 * Full durable history is unchanged. Returns TURBO_ENOENT when no complete
 * atomic event segment is eligible for compaction.
 */
CXX_C_API int turbo_agent_session_context_compact(turbo_agent_session_t *session,
                                                  const json_value_t *state);

/** Return the active committed context head, or TURBO_ENOENT when absent. */
CXX_C_API int turbo_agent_session_context_status(turbo_agent_session_t *session,
                                                 json_value_t **out_status);

#ifdef __cplusplus
}
#endif

#endif
