#include "turbo_runtime_head_internal.h"
#include <salts/error_codes.h>
#include <string.h>

static const char *const head_collection = "thread_heads";

static const char *head_string(const json_value_t *object, const char *key) {
  const json_value_t *value = json_object_get(object, key);
  const char *text = json_get_string(object, key);
  return text && strlen(text) == json_string_len(value) ? text : NULL;
}

/* Only pre-status custom backends need list to disambiguate their generic -1.
 * Typed failures never authorize absence or legacy history selection. */
static int head_read(turbo_agent_runtime_t *runtime, const char *thread_id,
                     json_value_t **out) {
  json_value_t *records = NULL;
  int rc = turbo_agent_runtime_store_get_json_status(runtime, head_collection, thread_id, out);
  if (rc != -1) return rc;
  rc = turbo_agent_runtime_store_list_json(runtime, head_collection, "id", thread_id, &records);
  if (rc == 0 && records && json_type(records) == JSON_ARRAY && json_array_size(records) == 0)
    rc = SALTS_ENOENT;
  else rc = SALTS_EIO;
  json_free(records);
  return rc;
}

static int head_load(turbo_agent_runtime_t *runtime, const char *thread_id,
                     json_value_t **out_run, int *out_empty) {
  json_value_t *head = NULL;
  const json_value_t *run;
  const json_value_t *state, *checkpoint;
  const char *id, *thread, *status;
  int rc;
  if (out_empty) *out_empty = 0;
  if (out_run) *out_run = NULL;
  if (!runtime || !thread_id || !thread_id[0] || !out_run) return SALTS_EINVAL;
  rc = head_read(runtime, thread_id, &head);
  if (rc != SALTS_OK) return rc;
  rc = SALTS_EPROTO;
  id = head_string(head, "id");
  thread = head_string(head, "thread_id");
  run = json_object_get(head, "run_snapshot");
  if (!id || strcmp(id, thread_id) || !thread || strcmp(thread, thread_id) ||
      turbo_runtime_json_value_as_int64(json_object_get(head, "schema_version"), -1) != 1 || !run)
    goto done;
  if (json_type(run) == JSON_NULL) {
    if (out_empty) *out_empty = 1;
    rc = SALTS_EBUSY;
    goto done;
  }
  id = head_string(run, "id");
  thread = head_string(run, "thread_id");
  status = head_string(run, "status");
  state = json_object_get(run, "state_snapshot");
  checkpoint = json_object_get(run, "latest_checkpoint_id");
  if (json_type(run) != JSON_OBJECT || !id || !id[0] || !thread || strcmp(thread, thread_id) ||
      !status || (strcmp(status, "completed") && strcmp(status, "interrupted") &&
                  strcmp(status, "cancelled") && strcmp(status, "timed_out")) ||
      !state || json_type(state) == JSON_NULL || !checkpoint ||
      (json_type(checkpoint) != JSON_NULL &&
       (!head_string(run, "latest_checkpoint_id") || !head_string(run, "latest_checkpoint_id")[0])))
    goto done;
  *out_run = json_clone(run);
  rc = *out_run ? SALTS_OK : SALTS_ENOMEM;
done:
  json_free(head);
  return rc;
}

int turbo_agent_runtime_get_published_thread_head(turbo_agent_runtime_t *runtime,
                                                 const char *thread_id, json_value_t **out_run) {
  return head_load(runtime, thread_id, out_run, NULL);
}

int turbo_runtime_head_publish(turbo_agent_runtime_t *runtime, const char *thread_id,
                               const json_value_t *run) {
  json_value_t *head = json_create_object();
  json_value_t *snapshot = run ? json_clone(run) : json_create_null();
  int rc = SALTS_ENOMEM;
  if (!head || !snapshot ||
      turbo_runtime_json_object_set_int64(head, "schema_version", 1) != 0 ||
      turbo_runtime_json_object_set_string(head, "id", thread_id) != 0 ||
      turbo_runtime_json_object_set_string(head, "thread_id", thread_id) != 0 ||
      turbo_runtime_json_object_set(head, "run_snapshot", snapshot) != 0) goto done;
  snapshot = NULL;
  rc = turbo_agent_runtime_store_put_json(runtime, head_collection, thread_id, head);
done:
  json_free(snapshot);
  json_free(head);
  return rc;
}

int turbo_runtime_head_initialize(turbo_agent_runtime_t *runtime, const char *thread_id) {
  json_value_t *run = NULL;
  int empty = 0;
  int rc = head_load(runtime, thread_id, &run, &empty);
  json_free(run);
  if (rc == SALTS_OK || empty) return SALTS_OK;
  if (rc != SALTS_ENOENT) return rc;
  /* Mark adoption before execution, so a failed first publication cannot expose
   * an unpublished run through the timestamp-based legacy fallback. */
  return turbo_runtime_head_publish(runtime, thread_id, NULL);
}
