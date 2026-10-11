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

/* Shared local/remote Session summary commit. Slots hold distinct owned tstrs.
 * Borrow summary for the call; copy every replacement before changing any slot.
 * Return 0 on success or -1 with all slots unchanged on invalid input/OOM.
 * Preserve legacy admission: thread/run must be strings (empty is accepted);
 * absent, non-string or empty checkpoint keeps the previous checkpoint.
 * This is an exclusive-owner transaction, not cross-thread synchronization. */
CXX_C_API int turbo_agent_util_capture_summary_ids(
    const json_value_t *summary, tstr *thread_id, tstr *run_id, tstr *checkpoint_id);

/* Data JsonValue Utilities */
CXX_C_API int turbo_agent_util_json_value_object_set_string(
    json_value_t *object, const char *key, const char *value);

CXX_C_API int turbo_agent_util_json_value_object_set_int64(
    json_value_t *object, const char *key, int64_t value);

CXX_C_API int turbo_agent_util_json_value_object_set_clone(
    json_value_t *object, const char *key,
    const json_value_t *value);

/* UTC timestamp used by durable Agent metadata; 0/-1, at least 21 bytes. */
int turbo_agent_runtime_make_timestamp(char *buffer, size_t buffer_size);

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
