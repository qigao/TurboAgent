#ifndef TURBO_EVENT_LOG_H
#define TURBO_EVENT_LOG_H

#include <stddef.h>
#include <platform.h>

#include "turbo_event.h"
#include "turbo_runtime_data_bind.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_event_log_s turbo_event_log_t;

typedef enum {
  TURBO_EVENT_LOG_OK = 0,
  TURBO_EVENT_LOG_INVALID_ARGUMENT = -1,
  TURBO_EVENT_LOG_OUT_OF_MEMORY = -2,
  TURBO_EVENT_LOG_INVALID_EVENT = -3
} turbo_event_log_status_t;

/**
 * @brief Create one canonical event log.
 */
CXX_C_API turbo_event_log_t *turbo_event_log_create(void);

/**
 * @brief Destroy one canonical event log.
 */
CXX_C_API void turbo_event_log_destroy(turbo_event_log_t *log);

/**
 * @brief Append one cloned canonical event into the log.
 */
CXX_C_API turbo_event_log_status_t
turbo_event_log_append_bind(turbo_event_log_t *log, const turbo_runtime_data_bind_value_t *event);

/**
 * @brief Append one bind-native array of canonical events into the log.
 */
CXX_C_API turbo_event_log_status_t turbo_event_log_append_events_bind(
    turbo_event_log_t *log, const turbo_runtime_data_bind_value_t *events);

/**
 * @brief Capture one canonical event through the standard sink callback.
 */
CXX_C_API void turbo_event_log_capture_bind(const turbo_runtime_data_bind_value_t *event,
                                            void *user_data);

/**
 * @brief Return the last append status observed by the log sink.
 */
CXX_C_API turbo_event_log_status_t turbo_event_log_status(const turbo_event_log_t *log);

/**
 * @brief Reset one canonical event log to empty state.
 */
CXX_C_API turbo_event_log_status_t turbo_event_log_reset(turbo_event_log_t *log);

/**
 * @brief Replace one event log with a bind-native array of canonical events.
 */
CXX_C_API turbo_event_log_status_t turbo_event_log_load_events_bind(
    turbo_event_log_t *log, const turbo_runtime_data_bind_value_t *events);

/**
 * @brief Return the number of captured canonical events.
 */
CXX_C_API size_t turbo_event_log_size(const turbo_event_log_t *log);

/**
 * @brief Borrow one captured canonical event by index.
 */
CXX_C_API const turbo_runtime_data_bind_value_t *turbo_event_log_get(const turbo_event_log_t *log,
                                                                     size_t index);

/**
 * @brief Export all captured canonical events as one cloned bind-native array.
 */
CXX_C_API turbo_runtime_data_bind_value_t *turbo_event_log_events_bind(const turbo_event_log_t *log);

/**
 * @brief Replay captured canonical events into another sink.
 */
CXX_C_API turbo_event_log_status_t turbo_event_log_replay_bind(
    const turbo_event_log_t *log, turbo_event_sink_bind_fn event_sink, void *event_sink_user_data);

#ifdef __cplusplus
}
#endif

#endif
