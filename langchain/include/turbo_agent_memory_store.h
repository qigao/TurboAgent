#ifndef TURBO_AGENT_MEMORY_STORE_H
#define TURBO_AGENT_MEMORY_STORE_H

#include <turbo_agent_api.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct json_value_s json_value_t;

typedef struct turbo_agent_memory_query_options_s {
  const char *namespace_prefix;
  const char *kind;
  const char *key_prefix;
  const char *text_substring;
  const char *id_prefix;
  const char *metadata_scope;
  const char *metadata_path_prefix;
  const char *created_after;
  const char *created_before;
  const char *sort_by;
  const char *sort_order;
  size_t limit;
} turbo_agent_memory_query_options_t;

typedef int (*turbo_agent_memory_store_get_fn)(void *user_data, const char *memory_namespace,
                                               const char *key, char **out_value_json);
typedef int (*turbo_agent_memory_store_put_fn)(void *user_data, const char *memory_namespace,
                                               const char *key, const char *value_json);
typedef int (*turbo_agent_memory_store_list_fn)(void *user_data, const char *namespace_prefix,
                                                char **out_records_json);
/**
 * @brief Query one canonical memory-record array in serialized JSON form.
 *
 * Returned records must follow the stable canonical shape:
 * - `id`
 * - `namespace`
 * - `kind`
 * - `key`
 * - `text`
 * - `metadata`
 * - `created_at`
 */
typedef int (*turbo_agent_memory_store_query_fn)(
    void *user_data, const char *namespace_prefix, const char *kind, const char *key_prefix,
    const char *text_substring, char **out_records_json);
typedef int (*turbo_agent_memory_store_delete_fn)(void *user_data, const char *memory_namespace,
                                                  const char *key);
typedef void (*turbo_agent_memory_store_user_data_free_fn)(void *user_data);

typedef struct turbo_agent_memory_store_s {
  turbo_agent_memory_store_get_fn get;
  turbo_agent_memory_store_put_fn put;
  turbo_agent_memory_store_list_fn list;
  turbo_agent_memory_store_query_fn query;
  turbo_agent_memory_store_delete_fn remove;
  void *user_data;
  turbo_agent_memory_store_user_data_free_fn user_data_free;
} turbo_agent_memory_store_t;

/**
 * @brief Create one heap-backed namespaced long-term memory store.
 * @return Store callbacks with owned user_data, or a zeroed store on failure.
 */
CXX_C_API turbo_agent_memory_store_t turbo_agent_memory_store_memory_create(void);

/**
 * @brief Create one file-backed namespaced long-term memory store.
 *
 * Records are stored under:
 * - `root_dir/records/<hex(namespace)>__<hex(key)>.json`
 *
 * @param root_dir Root directory. Created on demand when missing.
 * @return Store callbacks with owned user_data, or a zeroed store on failure.
 */
CXX_C_API turbo_agent_memory_store_t
turbo_agent_memory_store_file_create(const char *root_dir);

/**
 * @brief Destroy one standalone long-term memory store.
 *
 * This is intended for standalone use. When the store has been copied into a
 * longer-lived owner, that owner remains responsible for freeing `user_data`.
 *
 * @param store Store to clear. May be NULL.
 */
CXX_C_API void turbo_agent_memory_store_destroy(turbo_agent_memory_store_t *store);

/**
 * @brief Load one JSON value by namespace/key.
 * @param store Store callbacks.
 * @param memory_namespace Namespace such as `user`, `project`, or `thread/<id>`.
 * @param key Stable key inside the namespace.
 * @param out_value_json Serialized JSON value owned by caller on success.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_memory_get(const turbo_agent_memory_store_t *store,
                                     const char *memory_namespace, const char *key,
                                     char **out_value_json);

/**
 * @brief Persist one JSON value by namespace/key.
 * @param store Store callbacks.
 * @param memory_namespace Namespace such as `user`, `project`, or `thread/<id>`.
 * @param key Stable key inside the namespace.
 * @param value_json Serialized JSON value.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_memory_put(const turbo_agent_memory_store_t *store,
                                     const char *memory_namespace, const char *key,
                                     const char *value_json);

/**
 * @brief Delete one namespaced memory record.
 * @param store Store callbacks.
 * @param memory_namespace Namespace such as `user`, `project`, or `thread/<id>`.
 * @param key Stable key inside the namespace.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_memory_delete(const turbo_agent_memory_store_t *store,
                                        const char *memory_namespace, const char *key);

/**
 * @brief List stored records by namespace prefix.
 *
 * Returned records have the stable JSON shape:
 * - `namespace`
 * - `key`
 * - `value_json`
 *
 * @param store Store callbacks.
 * @param namespace_prefix Optional prefix such as `user` or `thread/`.
 * @param out_records_json JSON array owned by caller on success.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_memory_list(const turbo_agent_memory_store_t *store,
                                      const char *namespace_prefix,
                                      json_value_t **out_records_json);

/**
 * @brief List canonical long-term memory records by namespace prefix.
 *
 * Returned records have the stable JSON shape:
 * - `id`
 * - `namespace`
 * - `kind`
 * - `key`
 * - `text`
 * - `metadata`
 * - `created_at`
 *
 * `created_at` is currently `null` for legacy records that do not persist
 * timestamps yet.
 */
CXX_C_API int turbo_agent_memory_list_records(const turbo_agent_memory_store_t *store,
                                              const char *namespace_prefix,
                                              json_value_t **out_records_json);

/**
 * @brief Load one canonical long-term memory record by namespace/key.
 *
 * Returned records follow the stable JSON shape:
 * - `id`
 * - `namespace`
 * - `kind`
 * - `key`
 * - `text`
 * - `metadata`
 * - `created_at`
 */
CXX_C_API int turbo_agent_memory_get_record(const turbo_agent_memory_store_t *store,
                                            const char *memory_namespace, const char *key,
                                            json_value_t **out_record_json);

/**
 * @brief Persist one canonical long-term memory record.
 *
 * `context` records are reconstructed from canonical fields. Other record kinds
 * currently require one optional `value_json` string field on the input record.
 */
CXX_C_API int turbo_agent_memory_put_record(const turbo_agent_memory_store_t *store,
                                            const json_value_t *record_json);

/**
 * @brief Validate one canonical long-term memory record.
 * @return 0 when the record matches the public contract, negative on error.
 */
CXX_C_API int turbo_agent_memory_validate_record(const json_value_t *record_json);

/**
 * @brief Query canonical memory records with lightweight in-process filters.
 *
 * Filters are additive:
 * - `namespace_prefix` filters `namespace` by prefix
 * - `kind` filters canonical `kind`
 * - `key_prefix` filters `key` by prefix
 * - `text_substring` filters canonical `text` by substring
 * - `id_prefix` filters canonical `id` by prefix
 * - `metadata_scope` filters `metadata.scope` by exact match
 * - `metadata_path_prefix` filters `metadata.path` by prefix
 * - `created_after` applies one inclusive lower bound to `created_at`
 * - `created_before` applies one inclusive upper bound to `created_at`
 * - `sort_by` currently supports `namespace`, `kind`, `key`, and `id`
 * - `sort_order` supports `asc` or `desc`
 * - `limit` truncates the final canonical result array after filtering/sorting
 *
 * Stores may implement one native `query` callback. When absent, this helper
 * falls back to `list + canonicalize + filter`. Extended filters, sorting, and
 * limiting are always applied to the canonical record array by the host helper.
 * Empty strings are treated the same as NULL for extended filters. The native
 * backend callback signature remains unchanged.
 */
CXX_C_API int turbo_agent_memory_query_records(const turbo_agent_memory_store_t *store,
                                               const char *namespace_prefix, const char *kind,
                                               const char *key_prefix,
                                               const char *text_substring,
                                               json_value_t **out_records_json);

/**
 * @brief Query canonical memory records with one options struct.
 */
CXX_C_API int turbo_agent_memory_query_records_ex(
    const turbo_agent_memory_store_t *store, const turbo_agent_memory_query_options_t *options,
    json_value_t **out_records_json);

#ifdef __cplusplus
}
#endif

#endif
