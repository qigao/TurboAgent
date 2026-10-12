#ifndef TURBO_RUNTIME_STORE_H
#define TURBO_RUNTIME_STORE_H

#include "turbo_runtime_json.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Historical names are retained for source compatibility. Runtime owns this
 * opaque handle and the backend; it contains no Agent policy or execution state. */
typedef struct turbo_agent_runtime_s turbo_agent_runtime_t;

typedef int (*turbo_agent_runtime_store_put_fn)(void *user_data, const char *collection,
                                                const char *id, const char *record_json);
typedef int (*turbo_agent_runtime_store_get_fn)(void *user_data, const char *collection,
                                                const char *id, char **out_record_json);
typedef int (*turbo_agent_runtime_store_list_fn)(void *user_data, const char *collection,
                                                 const char *filter_key,
                                                 const char *filter_value,
                                                 char **out_records_json);
typedef void (*turbo_agent_runtime_store_user_data_free_fn)(void *user_data);

typedef struct turbo_agent_runtime_store_s {
  turbo_agent_runtime_store_put_fn put;
  turbo_agent_runtime_store_get_fn get;
  turbo_agent_runtime_store_list_fn list;
  void *user_data;
  turbo_agent_runtime_store_user_data_free_fn user_data_free;
} turbo_agent_runtime_store_t;

/**
 * Bind a store by value. All three operation callbacks are required.
 * On success, the handle owns user_data and calls user_data_free once at destroy
 * if provided. On failure (NULL), the caller retains user_data and must free it.
 * Callback code must remain loaded until destruction. Do not bind one owned
 * backend to multiple handles or access it directly while bound.
 *
 * Multiple callers may issue individual record operations: callbacks are
 * serialized per handle. Callbacks must not reenter the same handle. This is not
 * a transaction across operations, handles, or processes. Stop all callers and
 * drain Agent executions before destroying the handle; destroy does not wait.
 *
 * put borrows strings for the callback duration. get/list return NUL-terminated
 * malloc-owned JSON text (list must return an array); the adapter frees any
 * returned text even on error. Callbacks return 0 on success, nonzero on failure.
 */
CXX_C_API turbo_agent_runtime_t *
turbo_agent_runtime_create(const turbo_agent_runtime_store_t *store);

/** Release backend and handle after quiescence. NULL is accepted. */
CXX_C_API void turbo_agent_runtime_destroy(turbo_agent_runtime_t *runtime);

/**
 * Create a memory backend, or return a zeroed store on allocation failure.
 * Records are retained until backend destruction; there is no eviction/quota.
 * The caller owns user_data until a successful runtime_create transfers it.
 */
CXX_C_API turbo_agent_runtime_store_t turbo_agent_runtime_store_memory_create(void);

/**
 * Create a file backend rooted at root_dir (borrowed, copied by the backend).
 * Writes root_dir/<collection>/<id>.json; preserves the legacy threads/runs/
 * checkpoints directories. Missing root is created; its parent must exist.
 * Returns a zeroed store on failure. Ownership is as for memory_create.
 * Collection/id are trusted path components supplied by the application, not
 * untrusted input. This backend provides no cross-process locking, transaction,
 * atomic replacement, or crash-durability guarantee. List order is unspecified.
 */
CXX_C_API turbo_agent_runtime_store_t
turbo_agent_runtime_store_file_create(const char *root_dir);

/**
 * Serialize a borrowed record and put it at collection/id. Inputs must remain
 * stable for this synchronous call. Returns the backend's status, or -1 for
 * invalid arguments / serialization failure. Does not consume record_json.
 * Example: turbo_agent_runtime_store_put_json(runtime, "items", "id", record).
 */
CXX_C_API int turbo_agent_runtime_store_put_json(
    turbo_agent_runtime_t *runtime, const char *collection, const char *id,
    const json_value_t *record_json);

/**
 * Atomically claim one previously absent record within this Runtime handle.
 *
 * Under the existing RuntimeStore mutex, a single get(ENOENT) then put is
 * serialized against all other operations on this same handle. Unlike
 * separate get_json_status() + put_json(), concurrent claimers sharing the
 * handle cannot both succeed. Returns SALTS_OK for one successful claim,
 * SALTS_EALREADY if any record already exists, or a fail-closed backend
 * error (not interpreted as absence). A failed put may have committed:
 * callers MUST NOT perform external side effects unless SALTS_OK is returned.
 *
 * This is NOT an atomic cross-handle/process compare-and-swap: callers must
 * use one exclusively owned durable store/Runtime handle, e.g. the SQLite
 * backend's exclusive owner. Memory/file backends offer no crash guarantees.
 * Never reenter the same RuntimeStore from the backend callback.
 */
CXX_C_API int turbo_agent_runtime_store_put_json_if_absent(
    turbo_agent_runtime_t *runtime, const char *collection, const char *id,
    const json_value_t *record_json);

/**
 * Get a record as an independently owned tree. Return 0 on success, -1 for
 * invalid arguments, a missing/failed record, or invalid JSON. A non-NULL output
 * slot is cleared on entry; release a successful output with json_free().
 */
CXX_C_API int turbo_agent_runtime_store_get_json(
    turbo_agent_runtime_t *runtime, const char *collection, const char *id,
    json_value_t **out_record_json);

/** Status-preserving read. Ownership/output clearing match get_json.
 * Return SALTS_OK, SALTS_EINVAL for invalid arguments, SALTS_EPROTO for missing
 * success output or invalid JSON, or the backend's nonzero status unchanged.
 * Builtin get callbacks distinguish SALTS_ENOENT from I/O/allocation failures.
 * A legacy callback may return -1 for an unclassified failure; do not equate it
 * with absence. This entry performs no list query or implicit fallback.
 * Example: get_json_status(runtime, "items", "id", &value) == SALTS_ENOENT.
 * The old get_json entry continues mapping all failures to -1. */
CXX_C_API int turbo_agent_runtime_store_get_json_status(
    turbo_agent_runtime_t *runtime, const char *collection, const char *id,
    json_value_t **out_record_json);

/**
 * List records as an independently owned array; errors/output ownership match
 * get_json. Builtin backends match a string member exactly when both filter
 * arguments are non-NULL; either NULL disables filtering. No ordering is promised.
 */
CXX_C_API int turbo_agent_runtime_store_list_json(
    turbo_agent_runtime_t *runtime, const char *collection, const char *filter_key,
    const char *filter_value, json_value_t **out_records_json);

#ifdef __cplusplus
}
#endif
#endif
