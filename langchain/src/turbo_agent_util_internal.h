#ifndef TURBO_AGENT_UTIL_INTERNAL_H
#define TURBO_AGENT_UTIL_INTERNAL_H

#include <platform.h>
#include <tstr.h>
#include "turbo_runtime_json.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* String Utilities */
CXX_C_API tstr turbo_agent_util_strdup(const char *src);

/* Data JsonValue Utilities */
CXX_C_API int turbo_agent_util_json_value_object_set_string(
    json_value_t *object, const char *key, const char *value);

CXX_C_API int turbo_agent_util_json_value_object_set_int64(
    json_value_t *object, const char *key, int64_t value);

CXX_C_API int turbo_agent_util_json_value_object_set_clone(
    json_value_t *object, const char *key,
    const json_value_t *value);

/* Observability Utilities */
CXX_C_API const char *turbo_agent_util_observer_type_for_trace_name(const char *name);

/* Existing items */
CXX_C_API void turbo_agent_util_free_user_data(void *user_data);
CXX_C_API int turbo_agent_util_append_bytes(char **buffer, size_t *length, const char *data,
                                            size_t data_len);
CXX_C_API int turbo_agent_util_append_text(char **buffer, size_t *length, const char *text);

#ifdef __cplusplus
}
#endif

#endif
