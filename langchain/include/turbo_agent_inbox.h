#ifndef TURBO_AGENT_INBOX_H
#define TURBO_AGENT_INBOX_H

#include <turbo_agent_api.h>

#include "turbo_runtime_json.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_INBOX_ABI_VERSION 1U
#define TURBO_AGENT_INBOX_WAIT_INFINITE UINT64_MAX

typedef struct turbo_agent_session_s turbo_agent_session_t;

typedef enum turbo_agent_inbox_kind_e {
  TURBO_AGENT_INBOX_STEER = 1,
  TURBO_AGENT_INBOX_FOLLOW_UP = 2
} turbo_agent_inbox_kind_t;

typedef enum turbo_agent_inbox_status_e {
  TURBO_AGENT_INBOX_QUEUED = 1,
  TURBO_AGENT_INBOX_CLAIMED = 2,
  TURBO_AGENT_INBOX_APPLIED = 3
} turbo_agent_inbox_status_t;

typedef struct turbo_agent_inbox_config_s {
  uint32_t struct_size;
  uint32_t abi_version;
  size_t max_items;
  size_t max_total_bytes;
  size_t max_item_bytes;
  size_t max_follow_ups_per_execution;
} turbo_agent_inbox_config_t;

/**
 * @brief Configure the session-owned durable inbox and recover persisted state.
 *
 * All limits must be non-zero, max_item_bytes must not exceed max_total_bytes,
 * and max_items/max_total_bytes include queued plus claimed records.
 * max_follow_ups_per_execution bounds automatically chained turns. The session
 * must have an explicit or previously captured thread id.
 */
CXX_C_API int turbo_agent_session_inbox_configure(turbo_agent_session_t *session,
                                                  const turbo_agent_inbox_config_t *config);

/**
 * @brief Persist and enqueue one immutable canonical user message.
 *
 * Producers are MPSC. timeout_ms bounds only capacity waiting; zero is
 * non-blocking. On success, out_inbox_id receives a malloc-owned string that
 * the caller releases with free(). The message must be an object containing
 * role="user" and non-empty string content. Queue saturation returns
 * TURBO_EBUSY.
 */
CXX_C_API int turbo_agent_session_enqueue(turbo_agent_session_t *session,
                                          turbo_agent_inbox_kind_t kind,
                                          const json_value_t *message, uint64_t timeout_ms,
                                          char **out_inbox_id);

/** @brief Return payload plus the latest durable transition for one item. */
CXX_C_API int turbo_agent_session_inbox_status(turbo_agent_session_t *session, const char *inbox_id,
                                               json_value_t **out_status);

/**
 * @brief Claim the oldest queued item of one kind at a consumer safe point.
 *
 * Exactly one claim may be outstanding per session inbox. The returned record
 * is caller-owned. A successful claim must reach exactly one of mark_applied
 * or requeue.
 */
CXX_C_API int turbo_agent_session_inbox_claim(turbo_agent_session_t *session,
                                              turbo_agent_inbox_kind_t kind, const char *run_id,
                                              json_value_t **out_record);

/**
 * @brief Commit an applied transition after the matching history/checkpoint
 * event is durable, release capacity, and wake blocked producers.
 */
CXX_C_API int turbo_agent_session_inbox_mark_applied(turbo_agent_session_t *session,
                                                     const char *inbox_id,
                                                     const char *applied_event_id);

/** @brief Return the outstanding claim to its original FIFO position. */
CXX_C_API int turbo_agent_session_inbox_requeue(turbo_agent_session_t *session,
                                                const char *inbox_id);

/**
 * @brief Close enqueue/claim operations and wake all waiters.
 *
 * Destruction requires producers and the consumer to be quiescent. Closing
 * does not discard queued durable records.
 */
CXX_C_API int turbo_agent_session_inbox_close(turbo_agent_session_t *session);

#ifdef __cplusplus
}
#endif

#endif
