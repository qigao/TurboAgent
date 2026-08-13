#ifndef TURBO_EVENT_H
#define TURBO_EVENT_H

#include <platform.h>

#include "turbo_runtime_json.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*turbo_event_sink_json_value_fn)(const json_value_t *event,
                                         void *user_data);

typedef enum turbo_event_stream_mode_e {
  TURBO_EVENT_STREAM_ALL = 0,
  TURBO_EVENT_STREAM_MESSAGES = 1,
  TURBO_EVENT_STREAM_UPDATES = 2,
  TURBO_EVENT_STREAM_TOOLS = 3,
  TURBO_EVENT_STREAM_DEBUG = 4
} turbo_event_stream_mode_t;

typedef struct turbo_event_stream_filter_s {
  turbo_event_stream_mode_t mode;
  turbo_event_sink_json_value_fn sink;
  void *sink_user_data;
} turbo_event_stream_filter_t;

/**
 * @brief Return the canonical event kind string for one TurboParser JSON-native event.
 */
CXX_C_API const char *turbo_event_kind_json_value(const json_value_t *event);

/**
 * @brief Return whether one canonical event should be emitted for a stream mode.
 *
 * Modes are transport-agnostic:
 * - `ALL`: every canonical event
 * - `MESSAGES`: model/message events
 * - `UPDATES`: non-model graph/runtime updates
 * - `TOOLS`: tool-result events
 * - `DEBUG`: trace events
 */
CXX_C_API int turbo_event_stream_mode_accepts_json_value(
    turbo_event_stream_mode_t mode, const json_value_t *event);

/**
 * @brief Event-sink adapter that forwards only events accepted by `filter->mode`.
 *
 * `user_data` must point to a `turbo_event_stream_filter_t`. The wrapped sink
 * receives the original event object during the callback; it must clone if it
 * needs to retain the event after returning.
 */
CXX_C_API void turbo_event_stream_filter_sink_json_value(
    const json_value_t *event, void *user_data);

/**
 * @brief Validate one canonical TurboParser JSON-native event by dispatching on its kind.
 */
CXX_C_API int turbo_event_validate_json_value(const json_value_t *event);

/**
 * @brief Return the canonical TurboParser JSON-native schema for model events.
 */
CXX_C_API json_value_t *turbo_event_model_schema_json_value(void);

/**
 * @brief Validate one TurboParser JSON-native model event against the canonical shape.
 */
CXX_C_API int turbo_event_model_validate_json_value(const json_value_t *event);

/**
 * @brief Create one canonical TurboParser JSON-native model event.
 */
CXX_C_API json_value_t *
turbo_event_model_create_json_value(const char *response_id, const char *output_text,
                              const json_value_t *tool_calls);

/**
 * @brief Return the canonical TurboParser JSON-native schema for trace events.
 */
CXX_C_API json_value_t *turbo_event_trace_schema_json_value(void);

/**
 * @brief Validate one TurboParser JSON-native trace event.
 */
CXX_C_API int turbo_event_trace_validate_json_value(const json_value_t *event);

/**
 * @brief Create one canonical TurboParser JSON-native trace event.
 */
CXX_C_API json_value_t *turbo_event_trace_create_json_value(
    const char *name, const char *detail, const char *payload, int64_t status);

/**
 * @brief Return the canonical TurboParser JSON-native schema for tool result events.
 */
CXX_C_API json_value_t *turbo_event_tool_result_schema_json_value(void);

/**
 * @brief Validate one TurboParser JSON-native tool result event.
 */
CXX_C_API int turbo_event_tool_result_validate_json_value(const json_value_t *event);

/**
 * @brief Create one canonical TurboParser JSON-native tool result event.
 */
CXX_C_API json_value_t *turbo_event_tool_result_create_json_value(
    const char *name, const char *arguments_json, const char *output,
    const json_value_t *output_value, int64_t status);

/**
 * @brief Return the canonical TurboParser JSON-native schema for supervisor handoff events.
 */
CXX_C_API json_value_t *turbo_event_handoff_schema_json_value(void);

/**
 * @brief Validate one TurboParser JSON-native supervisor handoff event.
 */
CXX_C_API int turbo_event_handoff_validate_json_value(const json_value_t *event);

/**
 * @brief Create one canonical TurboParser JSON-native supervisor handoff event.
 */
CXX_C_API json_value_t *turbo_event_handoff_create_json_value(
    const char *phase, const char *from_agent, const char *target_agent, const char *reason,
    const char *active_agent, int64_t status);

#ifdef __cplusplus
}
#endif

#endif
