#include "turbo_agent_app.h"
#include "turbo_agent_memory_store.h"
#include "turbo_agent_session.h"
#include "turbo_agent_state.h"
#include "turbo_runtime_data_bind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *runtime_memory_record_text(const char *text) {
  return text ? text : "(null)";
}

static json_value_t *runtime_memory_record_create_record(const char *id, const char *namespace_name,
                                                         const char *kind, const char *key,
                                                         const char *text, const char *scope,
                                                         const char *path,
                                                         const char *created_at) {
  json_value_t *record = turbo_json_create_object();
  json_value_t *metadata = turbo_json_create_object();

  if (!record || !metadata) {
    turbo_free_json(&metadata);
    turbo_free_json(&record);
    return NULL;
  }
  turbo_json_object_set_string(record, "id", id);
  turbo_json_object_set_string(record, "namespace", namespace_name);
  turbo_json_object_set_string(record, "kind", kind);
  turbo_json_object_set_string(record, "key", key);
  turbo_json_object_set_string(record, "text", text);
  turbo_json_object_set_string(metadata, "scope", scope);
  turbo_json_object_set_string(metadata, "path", path);
  turbo_json_object_add(record, "metadata", metadata);
  if (created_at && created_at[0]) {
    turbo_json_object_set_string(record, "created_at", created_at);
  } else {
    turbo_json_object_set_null(record, "created_at");
  }
  return record;
}

static int runtime_memory_record_print_session_memory_text(
    const turbo_agent_session_t *session, const char *namespace_prefix) {
  turbo_runtime_data_bind_value_t *state = NULL;
  json_value_t *state_json = NULL;
  char *memory_text = NULL;
  int rc = -1;

  state = turbo_agent_session_create_input_state_with_memory_bind(session, "hello",
                                                                  namespace_prefix);
  if (!state) {
    goto cleanup;
  }
  state_json = turbo_runtime_data_bind_value_to_json(state);
  if (!state_json) {
    goto cleanup;
  }
  memory_text = turbo_agent_state_memory_context_text(state_json);
  if (!memory_text) {
    goto cleanup;
  }
  printf("session memory text: %s\n", runtime_memory_record_text(memory_text));
  rc = 0;

cleanup:
  free(memory_text);
  turbo_free_json(&state_json);
  turbo_runtime_data_bind_value_destroy(state);
  return rc;
}

int main(void) {
  turbo_agent_memory_store_t memory = turbo_agent_memory_store_memory_create();
  turbo_agent_session_config_t session_config = {0};
  turbo_agent_session_t *session = NULL;
  turbo_agent_session_config_t app_session_config = {0};
  turbo_agent_app_config_t app_config = {0};
  turbo_agent_app_t *app = NULL;
  turbo_agent_memory_query_options_t options = {0};
  json_value_t *record = NULL;
  json_value_t *archive_record = NULL;
  json_value_t *loaded = NULL;
  json_value_t *records = NULL;
  const json_value_t *first_record = NULL;
  const char *first_key = NULL;
  const char *first_created_at = NULL;

  record = runtime_memory_record_create_record("project/demo::context", "project/demo", "context",
                                               "context", "remember this", "project",
                                               "/tmp/notes.md", "2026-02-15T12:00:00Z");
  archive_record = runtime_memory_record_create_record(
      "project/demo::context-archive", "project/demo", "context", "context-archive",
      "remember older note", "project", "/tmp/archive.md", "2025-12-01T08:00:00Z");
  if (!record || !archive_record) {
    fprintf(stderr, "failed to create memory record fixture\n");
    return 1;
  }

  if (turbo_agent_memory_validate_record(record) != 0) {
    fprintf(stderr, "validate_record failed\n");
    return 1;
  }
  if (turbo_agent_memory_validate_record(archive_record) != 0) {
    fprintf(stderr, "validate_archive_record failed\n");
    return 1;
  }
  if (turbo_agent_memory_put_record(&memory, record) != 0 ||
      turbo_agent_memory_put_record(&memory, archive_record) != 0 ||
      turbo_agent_memory_get_record(&memory, "project/demo", "context", &loaded) != 0 ||
      !loaded) {
    fprintf(stderr, "store record helpers failed\n");
    return 1;
  }
  printf("store record id: %s\n", runtime_memory_record_text(turbo_json_get_string(loaded, "id")));
  turbo_free_json(&loaded);
  loaded = NULL;
  options.namespace_prefix = "project";
  options.kind = "context";
  options.key_prefix = "con";
  options.text_substring = "remember";
  options.created_after = "2026-01-01T00:00:00Z";
  options.created_before = "2026-12-31T23:59:59Z";
  options.sort_by = "key";
  options.sort_order = "desc";
  options.limit = 1;
  if (turbo_agent_memory_query_records_ex(&memory, &options, &records) != 0 || !records ||
      turbo_json_array_size(records) != 1) {
    fprintf(stderr, "store memory_query_records_ex failed\n");
    return 1;
  }
  first_record = turbo_json_array_get(records, 0);
  first_key = turbo_json_get_string(first_record, "key");
  first_created_at = turbo_json_get_string(first_record, "created_at");
  if (!first_key || strcmp(first_key, "context") != 0) {
    fprintf(stderr, "store time-window query returned unexpected record\n");
    return 1;
  }
  printf("store windowed query top key: %s created_at=%s\n", runtime_memory_record_text(first_key),
         runtime_memory_record_text(first_created_at));
  turbo_free_json(&records);
  records = NULL;

  session_config.runtime_store = turbo_agent_runtime_store_memory_create();
  session_config.memory_store = turbo_agent_memory_store_memory_create();
  session = turbo_agent_session_create(&session_config);
  if (!session) {
    fprintf(stderr, "failed to create session\n");
    return 1;
  }
  if (turbo_agent_session_memory_put_record(session, record) != 0 ||
      turbo_agent_session_memory_put_record(session, archive_record) != 0) {
    fprintf(stderr, "session memory_put_record failed\n");
    return 1;
  }
  if (turbo_agent_session_memory_query_records_ex(session, &options, &records) != 0 || !records ||
      turbo_json_array_size(records) != 1) {
    fprintf(stderr, "session memory_query_records_ex failed\n");
    return 1;
  }
  first_record = turbo_json_array_get(records, 0);
  first_key = turbo_json_get_string(first_record, "key");
  first_created_at = turbo_json_get_string(first_record, "created_at");
  if (!first_key || strcmp(first_key, "context") != 0) {
    fprintf(stderr, "session time-window query returned unexpected record\n");
    return 1;
  }
  printf("session windowed query top key: %s created_at=%s (count=%u)\n",
         runtime_memory_record_text(first_key), runtime_memory_record_text(first_created_at),
         (unsigned)turbo_json_array_size(records));
  turbo_free_json(&records);
  records = NULL;
  if (runtime_memory_record_print_session_memory_text(session, "project") != 0) {
    fprintf(stderr, "session memory context bootstrap failed\n");
    return 1;
  }

  app_session_config.runtime_store = turbo_agent_runtime_store_memory_create();
  app_session_config.memory_store = turbo_agent_memory_store_memory_create();
  app_config.session_config = &app_session_config;
  app = turbo_agent_app_create(&app_config);
  if (!app) {
    fprintf(stderr, "failed to create app\n");
    return 1;
  }
  if (turbo_agent_app_memory_put_record(app, record) != 0 ||
      turbo_agent_app_memory_put_record(app, archive_record) != 0 ||
      turbo_agent_app_memory_get_record(app, "project/demo", "context", &loaded) != 0 ||
      !loaded) {
    fprintf(stderr, "app memory record helpers failed\n");
    return 1;
  }
  printf("app record kind: %s\n",
         runtime_memory_record_text(turbo_json_get_string(loaded, "kind")));
  turbo_free_json(&loaded);
  loaded = NULL;
  if (turbo_agent_app_memory_query_records_ex(app, &options, &records) != 0 || !records ||
      turbo_json_array_size(records) != 1) {
    fprintf(stderr, "app memory_query_records_ex failed\n");
    return 1;
  }
  first_record = turbo_json_array_get(records, 0);
  first_key = turbo_json_get_string(first_record, "key");
  first_created_at = turbo_json_get_string(first_record, "created_at");
  if (!first_key || strcmp(first_key, "context") != 0) {
    fprintf(stderr, "app time-window query returned unexpected record\n");
    return 1;
  }
  printf("app windowed query top key: %s created_at=%s (count=%u)\n",
         runtime_memory_record_text(first_key), runtime_memory_record_text(first_created_at),
         (unsigned)turbo_json_array_size(records));

  turbo_free_json(&records);
  turbo_free_json(&archive_record);
  turbo_free_json(&record);
  turbo_agent_app_destroy(app);
  turbo_agent_session_destroy(session);
  turbo_agent_memory_store_destroy(&memory);
  return 0;
}
