#include "turbo_cli_storage.h"
#include "turbo_runtime_store_sqlite.h"
#include "turbo_runtime_segment.h"
#include "turbo_runtime_history.h"
#include <stdlib.h>
#include <string.h>

static const char collection[] = "cli_sessions", key[] = "current";

/* Persisted strings must be exact C-compatible strings, not a matching prefix
 * followed by an embedded NUL. Metadata is admission data, never executable. */
static const char *metadata_string(const json_value_t *object, const char *name) {
  const json_value_t *member = json_object_get(object, name);
  const char *value = json_string(member);
  return json_type(member) == JSON_STRING && value &&
         strlen(value) == json_string_len(member) ? value : NULL;
}

void turbo_cli_storage_close(turbo_cli_storage_t *storage) {
  if (!storage) return;
  if (storage->store.user_data_free) storage->store.user_data_free(storage->store.user_data);
  json_free(storage->metadata);
  *storage = (turbo_cli_storage_t){0};
}

int turbo_cli_storage_open(turbo_cli_storage_t *storage, const char *path,
                          const char *workspace, int resume) {
  turbo_runtime_sqlite_config_t config;
  const char *saved_workspace, *thread, *phase, *last_run;
  char *text = NULL, *id = NULL;
  int64_t turns;
  int rc;
  turbo_runtime_sqlite_config_init(&config);
  config.path = path; config.create_if_missing = !resume;
  rc = turbo_runtime_store_sqlite_open(&config, &storage->store);
  if (rc != SALTS_OK) return rc;
  rc = storage->store.get(storage->store.user_data, collection, key, &text);
  if (resume) {
    if (rc != SALTS_OK) goto failed;
    storage->metadata = json_parse(text, strlen(text));
    free(text); text = NULL;
    saved_workspace = metadata_string(storage->metadata, "workspace");
    thread = metadata_string(storage->metadata, "thread_id");
    phase = metadata_string(storage->metadata, "phase");
    last_run = metadata_string(storage->metadata, "last_run_id");
    turns = turbo_runtime_json_value_as_int64(json_object_get(storage->metadata, "turn_count"), -1);
    rc = SALTS_EPROTO;
    if (turbo_runtime_json_value_as_int64(json_object_get(storage->metadata, "format_version"), -1) != 1 ||
        !saved_workspace || strcmp(saved_workspace, workspace) || !thread || !thread[0] ||
        strlen(thread) > 1024 || !phase || !last_run || strlen(last_run) > 1024 ||
        turns < 0 || turns > TURBO_CLI_MAX_TURNS || (turns == 0) != (last_run[0] == 0)) goto failed;
    if (strcmp(phase, "ready")) { rc = SALTS_EBUSY; goto failed; }
    storage->turns = (size_t)turns;
    return SALTS_OK;
  }
  free(text); text = NULL;
  if (rc == SALTS_OK) { rc = SALTS_EALREADY; goto failed; }
  if (rc != SALTS_ENOENT) goto failed;
  storage->metadata = json_create_object();
  id = turbo_runtime_record_uuid("thread");
  rc = SALTS_ENOMEM;
  if (!storage->metadata || !id ||
      turbo_runtime_json_object_set_int64(storage->metadata, "format_version", 1) != 0 ||
      turbo_runtime_json_object_set_string(storage->metadata, "workspace", workspace) != 0 ||
      turbo_runtime_json_object_set_string(storage->metadata, "thread_id", id) != 0 ||
      turbo_runtime_json_object_set_int64(storage->metadata, "turn_count", 0) != 0 ||
      turbo_runtime_json_object_set_string(storage->metadata, "phase", "ready") != 0 ||
      turbo_runtime_json_object_set_string(storage->metadata, "last_run_id", "") != 0) goto failed;
  free(id); id = NULL;
  text = json_serialize(storage->metadata, NULL);
  if (!text) goto failed;
  rc = storage->store.put(storage->store.user_data, collection, key, text);
  json_serialize_free(text); text = NULL;
  if (rc != SALTS_OK) goto failed;
  return SALTS_OK;
failed:
  free(text); free(id);
  turbo_cli_storage_close(storage);
  return rc;
}

static int completed_run(turbo_cli_storage_t *storage, turbo_agent_session_t *session,
                         const char *id, json_value_t **out_state) {
  json_value_t *run = NULL;
  const char *thread, *status, *stored_id;
  int rc;
  if (!id || !id[0]) return SALTS_EPROTO;
  rc = turbo_agent_runtime_get_published_thread_head(turbo_agent_session_runtime(session),
          metadata_string(storage->metadata, "thread_id"), &run);
  /* Only older databases without a head may use the exact CLI-pinned run. */
  if (rc == SALTS_ENOENT) rc = turbo_agent_session_get_run(session, id, &run);
  if (rc != SALTS_OK) return rc;
  rc = SALTS_EPROTO;
  thread = metadata_string(run, "thread_id"); status = metadata_string(run, "status");
  stored_id = metadata_string(run, "id");
  if (stored_id && !strcmp(stored_id, id) &&
      thread && !strcmp(thread, metadata_string(storage->metadata, "thread_id")) &&
      status && !strcmp(status, "completed")) {
    rc = SALTS_OK;
    if (out_state) {
      const json_value_t *state = json_object_get(run, "state_snapshot");
      if (!state || json_type(state) != JSON_OBJECT) rc = SALTS_EPROTO;
      else {
        *out_state = json_clone(state);
        if (!*out_state) rc = SALTS_ENOMEM;
      }
    }
  }
  json_free(run);
  return rc;
}

int turbo_cli_storage_load(turbo_cli_storage_t *storage, turbo_agent_session_t *session,
                          json_value_t **out_state) {
  const char *id;
  *out_state = NULL;
  if (!storage->metadata || !storage->turns) return SALTS_OK;
  id = metadata_string(storage->metadata, "last_run_id");
  return completed_run(storage, session, id, out_state);
}

static int publish(turbo_cli_storage_t *storage, turbo_agent_session_t *session,
                   const char *phase, const char *last_run, size_t turns) {
  json_value_t *next = json_clone(storage->metadata);
  int rc = SALTS_ENOMEM;
  if (!next || turbo_runtime_json_object_set_string(next, "phase", phase) != 0 ||
      (last_run && turbo_runtime_json_object_set_string(next, "last_run_id", last_run) != 0) ||
      turbo_runtime_json_object_set_int64(next, "turn_count", (int64_t)turns) != 0) goto done;
  rc = turbo_agent_runtime_store_put_json(turbo_agent_session_runtime(session), collection, key, next);
  if (rc == SALTS_OK) {
    json_free(storage->metadata); storage->metadata = next; next = NULL;
    storage->turns = turns;
  }
done:
  json_free(next);
  return rc;
}

int turbo_cli_storage_begin(turbo_cli_storage_t *storage, turbo_agent_session_t *session) {
  if (!storage->metadata) return SALTS_OK;
  if (storage->turns >= TURBO_CLI_MAX_TURNS) return SALTS_ENOSPC;
  return publish(storage, session, "active", NULL, storage->turns);
}

int turbo_cli_storage_complete(turbo_cli_storage_t *storage, turbo_agent_session_t *session) {
  const char *id;
  if (!storage->metadata) return SALTS_OK;
  id = turbo_agent_session_last_run_id(session);
  if (completed_run(storage, session, id, NULL) != SALTS_OK) return SALTS_EPROTO;
  return publish(storage, session, "ready", id, storage->turns + 1);
}
