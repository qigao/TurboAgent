#ifndef TURBO_AGENT_STATE_CORE_INTERNAL_H
#define TURBO_AGENT_STATE_CORE_INTERNAL_H

#include "turbo_agent_state.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API size_t turbo_agent_state_schema_version_impl(void);
CXX_C_API json_value_t *turbo_agent_state_create_impl(void);
CXX_C_API json_value_t *turbo_agent_state_create_json_value_impl(void);
CXX_C_API size_t turbo_agent_state_version_impl(const json_value_t *state);
CXX_C_API int turbo_agent_state_version_supported_impl(const json_value_t *state);
CXX_C_API int turbo_agent_state_add_user_message_impl(json_value_t *state, const char *text);
CXX_C_API const json_value_t *turbo_agent_state_events_impl(const json_value_t *state);
CXX_C_API size_t turbo_agent_state_event_count_impl(const json_value_t *state);
CXX_C_API const json_value_t *turbo_agent_state_event_at_impl(const json_value_t *state,
                                                              size_t index);
CXX_C_API const json_value_t *turbo_agent_state_trace_events_impl(const json_value_t *state);
CXX_C_API size_t turbo_agent_state_trace_event_count_impl(const json_value_t *state);
CXX_C_API const json_value_t *turbo_agent_state_trace_event_at_impl(
    const json_value_t *state, size_t index);
CXX_C_API json_value_t *
turbo_agent_state_trace_events_json_value_impl(const json_value_t *state);
CXX_C_API int turbo_agent_state_add_trace_event_json_value_impl(
    json_value_t *state, const json_value_t *event);
CXX_C_API void turbo_agent_state_capture_trace_event_json_value_impl(
    const json_value_t *event, void *user_data);
CXX_C_API const json_value_t *turbo_agent_state_last_event_of_kind_impl(
    const json_value_t *state, const char *kind);
CXX_C_API const json_value_t *turbo_agent_state_last_event_impl(
    const json_value_t *state);
CXX_C_API const json_value_t *turbo_agent_state_latest_handoff_event_impl(
    const json_value_t *state);
CXX_C_API const char *turbo_agent_state_handoff_event_phase_impl(const json_value_t *event);
CXX_C_API const char *turbo_agent_state_handoff_event_from_agent_impl(
    const json_value_t *event);
CXX_C_API const char *turbo_agent_state_handoff_event_target_agent_impl(
    const json_value_t *event);
CXX_C_API const char *turbo_agent_state_handoff_event_reason_impl(const json_value_t *event);
CXX_C_API const char *turbo_agent_state_handoff_event_active_agent_impl(
    const json_value_t *event);
CXX_C_API json_value_t *turbo_agent_state_get_array_impl(json_value_t *state,
                                                         const char *key);
CXX_C_API json_value_t *turbo_agent_state_get_or_create_array_impl(
    json_value_t *state, const char *key);
CXX_C_API json_value_t *turbo_agent_state_get_object_impl(json_value_t *state,
                                                          const char *key);
CXX_C_API json_value_t *turbo_agent_state_get_or_create_object_impl(
    json_value_t *state, const char *key);
CXX_C_API const json_value_t *turbo_agent_state_get_object_const_impl(
    const json_value_t *state, const char *key);
CXX_C_API const json_value_t *turbo_agent_state_get_array_const_impl(
    const json_value_t *state, const char *key);
CXX_C_API json_value_t *turbo_agent_state_get_versions_array_impl(
    json_value_t *state, const char *key);
CXX_C_API json_value_t *turbo_agent_state_get_current_array_version_impl(
    json_value_t *state, const char *key);
CXX_C_API json_value_t *turbo_agent_state_get_current_object_version_impl(
    json_value_t *state, const char *key);
CXX_C_API const json_value_t *turbo_agent_state_get_current_array_version_const_impl(
    const json_value_t *state, const char *key);
CXX_C_API const json_value_t *turbo_agent_state_get_current_object_version_const_impl(
    const json_value_t *state, const char *key);
CXX_C_API int turbo_agent_state_append_review_version_impl(json_value_t *state,
                                                           int required,
                                                           int approved,
                                                           const char *note);
CXX_C_API int turbo_agent_state_append_single_string_object_version_impl(
    json_value_t *state, const char *version_key, const char *field_key,
    const char *field_value);
CXX_C_API const char *turbo_agent_state_current_version_string_field_impl(
    const json_value_t *state, const char *version_key, const char *field_key);
CXX_C_API int turbo_agent_state_current_version_bool_field_impl(
    const json_value_t *state, const char *version_key, const char *field_key,
    int default_value);
CXX_C_API int turbo_agent_state_append_array_version(json_value_t *state, const char *key,
                                                     json_value_t *array);
CXX_C_API int turbo_agent_state_append_object_version(json_value_t *state, const char *key,
                                                      json_value_t *object);
CXX_C_API int turbo_agent_state_append_replan_version(json_value_t *state, int requested,
                                                      size_t count, size_t max_count,
                                                      const char *reason);

#ifdef TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP
#define turbo_agent_state_schema_version turbo_agent_state_schema_version_impl
#define turbo_agent_state_create turbo_agent_state_create_impl
#define turbo_agent_state_create_json_value turbo_agent_state_create_json_value_impl
#define turbo_agent_state_version turbo_agent_state_version_impl
#define turbo_agent_state_version_supported turbo_agent_state_version_supported_impl
#define turbo_agent_state_add_user_message turbo_agent_state_add_user_message_impl
#define turbo_agent_state_events turbo_agent_state_events_impl
#define turbo_agent_state_event_count turbo_agent_state_event_count_impl
#define turbo_agent_state_event_at turbo_agent_state_event_at_impl
#define turbo_agent_state_trace_events turbo_agent_state_trace_events_impl
#define turbo_agent_state_trace_event_count turbo_agent_state_trace_event_count_impl
#define turbo_agent_state_trace_event_at turbo_agent_state_trace_event_at_impl
#define turbo_agent_state_trace_events_json_value turbo_agent_state_trace_events_json_value_impl
#define turbo_agent_state_add_trace_event_json_value turbo_agent_state_add_trace_event_json_value_impl
#define turbo_agent_state_capture_trace_event_json_value turbo_agent_state_capture_trace_event_json_value_impl
#define turbo_agent_state_last_event_of_kind turbo_agent_state_last_event_of_kind_impl
#define turbo_agent_state_last_event turbo_agent_state_last_event_impl
#define turbo_agent_state_latest_handoff_event turbo_agent_state_latest_handoff_event_impl
#define turbo_agent_state_handoff_event_phase turbo_agent_state_handoff_event_phase_impl
#define turbo_agent_state_handoff_event_from_agent \
  turbo_agent_state_handoff_event_from_agent_impl
#define turbo_agent_state_handoff_event_target_agent \
  turbo_agent_state_handoff_event_target_agent_impl
#define turbo_agent_state_handoff_event_reason turbo_agent_state_handoff_event_reason_impl
#define turbo_agent_state_handoff_event_active_agent \
  turbo_agent_state_handoff_event_active_agent_impl
#define turbo_agent_state_get_array turbo_agent_state_get_array_impl
#define turbo_agent_state_get_or_create_array turbo_agent_state_get_or_create_array_impl
#define turbo_agent_state_get_object turbo_agent_state_get_object_impl
#define turbo_agent_state_get_or_create_object turbo_agent_state_get_or_create_object_impl
#define turbo_agent_state_get_object_const turbo_agent_state_get_object_const_impl
#define turbo_agent_state_get_array_const turbo_agent_state_get_array_const_impl
#define turbo_agent_state_get_versions_array turbo_agent_state_get_versions_array_impl
#define turbo_agent_state_get_current_array_version \
  turbo_agent_state_get_current_array_version_impl
#define turbo_agent_state_get_current_object_version \
  turbo_agent_state_get_current_object_version_impl
#define turbo_agent_state_get_current_array_version_const \
  turbo_agent_state_get_current_array_version_const_impl
#define turbo_agent_state_get_current_object_version_const \
  turbo_agent_state_get_current_object_version_const_impl
#define turbo_agent_state_append_review_version turbo_agent_state_append_review_version_impl
#define turbo_agent_state_append_single_string_object_version \
  turbo_agent_state_append_single_string_object_version_impl
#define turbo_agent_state_current_version_string_field \
  turbo_agent_state_current_version_string_field_impl
#define turbo_agent_state_current_version_bool_field \
  turbo_agent_state_current_version_bool_field_impl
#endif

#ifdef __cplusplus
}
#endif

#endif
