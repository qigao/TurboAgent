/* Internal header for turbo_agent_runtime_v1 sub-modules.
 * Shared declarations that allow extracted .c files to call each other
 * without polluting the public API.  Only #include from src/ files. */
#ifndef TURBO_AGENT_RUNTIME_V1_INTERNAL_H
#define TURBO_AGENT_RUNTIME_V1_INTERNAL_H

#include "turbo_agent_runtime.h"

#include "turbo_agent_state.h"
#include "turbo_parser.h"
#include <turbo_thread.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_runtime_memory_record_s {
  char *collection;
  char *id;
  char *record_json;
  struct turbo_agent_runtime_memory_record_s *next;
} turbo_agent_runtime_memory_record_t;

typedef struct {
  turbo_agent_runtime_memory_record_t *head;
} turbo_agent_runtime_memory_store_t;

typedef struct {
  char *root_dir;
} turbo_agent_runtime_file_store_t;

struct turbo_agent_runtime_s {
  turbo_agent_runtime_store_t store;
  turbo_mutex_t store_mutex;
};

/* ---- bind helpers ---- */
int turbo_agent_runtime_json_value_object_set_string(json_value_t *object, const char *key,
                                                     const char *value);
int turbo_agent_runtime_json_value_object_set_int64(json_value_t *object, const char *key,
                                                    int64_t value);
int turbo_agent_runtime_json_value_object_set_clone(json_value_t *object, const char *key,
                                                    const json_value_t *value);
int turbo_agent_runtime_json_value_object_set_optional_string(json_value_t *object, const char *key,
                                                              const json_value_t *value);

/* ---- small utilities ---- */
char *turbo_agent_runtime_strdup(const char *text);
const char *turbo_agent_runtime_status_text(turbo_graph_exec_status_t status);
int turbo_agent_runtime_result_to_json(const turbo_graph_run_result_t *result,
                                       json_value_t **out_result);

/* ---- platform IO / ID generation ---- */
int turbo_agent_runtime_make_timestamp(char *buffer, size_t buffer_size);
char *turbo_agent_runtime_make_id(const char *prefix);
int turbo_agent_runtime_write_text_file(const char *path, const char *content);
int turbo_agent_runtime_read_text_file(const char *path, char **out_content);
int turbo_agent_runtime_ensure_dir(const char *path);
char *turbo_agent_runtime_join_path(const char *left, const char *right);
char *turbo_agent_runtime_record_path(const char *root_dir, const char *collection, const char *id);

/* ---- JSON helpers ---- */
int turbo_agent_runtime_parse_json_string(const char *json_text, json_value_t **out_json);
int turbo_agent_runtime_json_matches_filter(const json_value_t *record, const char *filter_key,
                                            const char *filter_value);
json_value_t *turbo_agent_runtime_sorted_json_array_clone(const json_value_t *array_json,
                                                          int (*compare)(const void *,
                                                                         const void *));

/* ---- comparison helpers ---- */
int turbo_agent_runtime_record_string_compare(const char *left, const char *right);
int turbo_agent_runtime_run_updated_at_compare_desc(const void *left, const void *right);

/* ---- metadata helpers ---- */
void turbo_agent_runtime_interrupt_metadata(const char *status, const char *next_node,
                                            const json_value_t *state_json,
                                            const char **out_interrupt_reason,
                                            const char **out_pending_action,
                                            const char *const **out_command_names,
                                            size_t *out_command_count,
                                            char **out_owned_executor_failure_reason);

/* ---- store adapter (JSON layer over raw store) ---- */
int turbo_agent_runtime_store_put_json(turbo_agent_runtime_t *runtime, const char *collection,
                                       const char *id, const json_value_t *record_json);
int turbo_agent_runtime_store_get_json(turbo_agent_runtime_t *runtime, const char *collection,
                                       const char *id, json_value_t **out_record_json);
int turbo_agent_runtime_store_list_json(turbo_agent_runtime_t *runtime, const char *collection,
                                        const char *filter_key, const char *filter_value,
                                        json_value_t **out_records_json);

/* ---- thread / checkpoint resolution ---- */
int turbo_agent_runtime_current_checkpoint_id_for_thread(turbo_agent_runtime_t *runtime,
                                                         const char *thread_id,
                                                         char **out_checkpoint_id);
int turbo_agent_runtime_latest_run_id_for_thread(turbo_agent_runtime_t *runtime,
                                                 const char *thread_id, char **out_run_id);

/* ---- state preparation ---- */
int turbo_agent_runtime_prepare_checkpoint_command_override_json_value(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id, const json_value_t *command,
    json_value_t **out_state_override);
int turbo_agent_runtime_prepare_checkpoint_state_override_json_value(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id, const json_value_t *state_patch,
    json_value_t **out_state_override);
int turbo_agent_runtime_prepare_thread_command_override_json_value(
    turbo_agent_runtime_t *runtime, const char *thread_id, const json_value_t *command,
    json_value_t **out_state_override);
int turbo_agent_runtime_prepare_thread_state_override_json_value(turbo_agent_runtime_t *runtime,
                                                                 const char *thread_id,
                                                                 const json_value_t *state_patch,
                                                                 json_value_t **out_state_override);

/* ---- execution streams ---- */
int turbo_agent_runtime_start_json_value_graph_linked_stream(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const json_value_t *state,
    const turbo_graph_run_options_t *options, const char *thread_id,
    const turbo_agent_runtime_parent_link_t *parent_link, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data, json_value_t **out_summary_json, json_value_t **out_state);
int turbo_agent_runtime_start_json_value_graph_linked_stream_controlled(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const json_value_t *state,
    const turbo_graph_run_options_t *options, const char *thread_id,
    const turbo_agent_runtime_parent_link_t *parent_link, const turbo_cancel_token_t *cancel_token,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state);
int turbo_agent_runtime_resume_json_value_graph_stream(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *input, const turbo_graph_run_options_t *options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state);
int turbo_agent_runtime_resume_json_value_graph_stream_controlled(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_cancel_token_t *cancel_token, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data, json_value_t **out_summary_json, json_value_t **out_state);
int turbo_agent_runtime_fork_json_value_graph_stream(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *input, const turbo_graph_run_options_t *options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state);
int turbo_agent_runtime_fork_json_value_graph_stream_controlled(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_cancel_token_t *cancel_token, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data, json_value_t **out_summary_json, json_value_t **out_state);

/* ---- collection name constants ---- */
extern const char *const turbo_agent_runtime_threads_collection;
extern const char *const turbo_agent_runtime_runs_collection;
extern const char *const turbo_agent_runtime_checkpoints_collection;

#ifdef __cplusplus
}
#endif

#endif /* TURBO_AGENT_RUNTIME_V1_INTERNAL_H */
