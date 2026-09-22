#ifndef TURBO_STATE_GRAPH_STORE_H
#define TURBO_STATE_GRAPH_STORE_H

#include <turbo_agent_api.h>

#include "turbo_state_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct json_value_s json_value_t;

typedef int (*turbo_state_graph_store_load_fn)(void *user_data, const char *snapshot_id,
                                               char **out_snapshot_json);
typedef int (*turbo_state_graph_store_save_fn)(void *user_data, const char *snapshot_id,
                                               const char *snapshot_json);
typedef int (*turbo_state_graph_store_list_fn)(void *user_data, char **out_snapshot_ids_json);
typedef int (*turbo_state_graph_store_delete_fn)(void *user_data, const char *snapshot_id);
typedef void (*turbo_state_graph_store_user_data_free_fn)(void *user_data);

typedef struct turbo_state_graph_store_s {
  turbo_state_graph_store_load_fn load;
  turbo_state_graph_store_save_fn save;
  turbo_state_graph_store_list_fn list;
  turbo_state_graph_store_delete_fn remove;
  void *user_data;
  turbo_state_graph_store_user_data_free_fn user_data_free;
} turbo_state_graph_store_t;

typedef struct turbo_state_graph_store_list_options_s {
  const char *graph_name;
  const char *thread_id;
  const char *run_id;
  const char *history_entry_id;
  const char *checkpoint_id;
  const char *run_status;
  size_t limit;
} turbo_state_graph_store_list_options_t;

CXX_C_API turbo_state_graph_store_t turbo_state_graph_store_memory_create(void);
CXX_C_API turbo_state_graph_store_t turbo_state_graph_store_file_create(const char *root_dir);
CXX_C_API void turbo_state_graph_store_destroy(turbo_state_graph_store_t *store);

CXX_C_API int turbo_state_graph_store_delete(const turbo_state_graph_store_t *store,
                                             const char *snapshot_id);

CXX_C_API int turbo_state_graph_store_list(const turbo_state_graph_store_t *store,
                                           json_value_t **out_snapshot_ids_json);

CXX_C_API int turbo_state_graph_store_get_snapshot_descriptor(
    const turbo_state_graph_store_t *store, const char *snapshot_id,
    json_value_t **out_descriptor_json);

CXX_C_API int turbo_state_graph_store_list_snapshot_descriptors(
    const turbo_state_graph_store_t *store, json_value_t **out_descriptors_json);

CXX_C_API int turbo_state_graph_store_list_snapshot_descriptors_filtered(
    const turbo_state_graph_store_t *store,
    const turbo_state_graph_store_list_options_t *options,
    json_value_t **out_descriptors_json);

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_descriptor(
    const turbo_state_graph_store_t *store,
    const turbo_state_graph_store_list_options_t *options,
    json_value_t **out_descriptor_json);

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_descriptor_for_thread(
    const turbo_state_graph_store_t *store, const char *thread_id,
    json_value_t **out_descriptor_json);

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_descriptor_for_run(
    const turbo_state_graph_store_t *store, const char *run_id,
    json_value_t **out_descriptor_json);

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_descriptor_for_history_entry(
    const turbo_state_graph_store_t *store, const char *history_entry_id,
    json_value_t **out_descriptor_json);

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_descriptor_for_checkpoint(
    const turbo_state_graph_store_t *store, const char *checkpoint_id,
    json_value_t **out_descriptor_json);

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_id(
    const turbo_state_graph_store_t *store,
    const turbo_state_graph_store_list_options_t *options, char **out_snapshot_id);

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_id_for_thread(
    const turbo_state_graph_store_t *store, const char *thread_id,
    char **out_snapshot_id);

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_id_for_run(
    const turbo_state_graph_store_t *store, const char *run_id, char **out_snapshot_id);

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_id_for_history_entry(
    const turbo_state_graph_store_t *store, const char *history_entry_id,
    char **out_snapshot_id);

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_id_for_checkpoint(
    const turbo_state_graph_store_t *store, const char *checkpoint_id,
    char **out_snapshot_id);

CXX_C_API turbo_state_graph_status_t turbo_state_graph_store_load_latest_snapshot(
    const turbo_state_graph_store_t *store,
    const turbo_state_graph_store_list_options_t *options,
    turbo_state_graph_t *graph);

CXX_C_API turbo_state_graph_status_t turbo_state_graph_store_load_latest_snapshot_for_thread(
    const turbo_state_graph_store_t *store, const char *thread_id,
    turbo_state_graph_t *graph);

CXX_C_API turbo_state_graph_status_t turbo_state_graph_store_load_latest_snapshot_for_run(
    const turbo_state_graph_store_t *store, const char *run_id,
    turbo_state_graph_t *graph);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_store_load_latest_snapshot_for_history_entry(
    const turbo_state_graph_store_t *store, const char *history_entry_id,
    turbo_state_graph_t *graph);

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_store_load_latest_snapshot_for_checkpoint(
    const turbo_state_graph_store_t *store, const char *checkpoint_id,
    turbo_state_graph_t *graph);

CXX_C_API turbo_state_graph_status_t turbo_state_graph_store_save_snapshot(
    const turbo_state_graph_store_t *store, const char *snapshot_id,
    const turbo_state_graph_t *graph);

CXX_C_API turbo_state_graph_status_t turbo_state_graph_store_load_snapshot(
    const turbo_state_graph_store_t *store, const char *snapshot_id,
    turbo_state_graph_t *graph);

#ifdef __cplusplus
}
#endif

#endif
