#include "turbo_agent_session_workflow_internal.h"
#include <salts/error_codes.h>
#include <stdlib.h>
#include <string.h>

void turbo_agent_task_context_options_init(turbo_agent_task_context_options_t *options) {
  if (!options) return;
  memset(options, 0, sizeof(*options));
  options->struct_size = sizeof(*options);
  options->abi_version = TURBO_AGENT_TASK_CONTEXT_ABI_VERSION;
}

static int task_selector(const char *text) {
  return text && text[0] && strlen(text) <= TURBO_AGENT_TASK_CONTEXT_MAX_FIELD_BYTES;
}

static int task_string(const json_value_t *value, size_t limit, int empty, int nullable) {
  const char *text;
  if (!value) return 0;
  if (nullable && json_type(value) == JSON_NULL) return 1;
  if (json_type(value) != JSON_STRING) return 0;
  text = json_string(value);
  return text && (empty || text[0]) && strlen(text) == json_string_len(value) &&
      json_string_len(value) <= limit;
}

static int task_memory(turbo_agent_session_t const *session, const char *ns,
    const char *key, json_value_t **out) {
  static const char *fields[] = {"scope", "path", "text", "created_at"};
  json_value_t *value = NULL, *record = NULL;
  char *raw = NULL;
  unsigned seen = 0;
  int rc = SALTS_EIO;
  *out = NULL;
  if (turbo_agent_session_memory_get(session, ns, key, &raw) || !raw) goto done;
  /* Bound serialized input before parsing; escaped text can use six bytes per
   * source byte. This bounds our parse, not the backend's allocation/read. */
  if (strlen(raw) > 6u * (TURBO_AGENT_TASK_CONTEXT_MAX_TEXT_BYTES +
          3u * TURBO_AGENT_TASK_CONTEXT_MAX_FIELD_BYTES) + 1024u) {
    rc = SALTS_ENOBUFS; goto done;
  }
  value = json_parse(raw, strlen(raw));
  rc = SALTS_EPROTO;
  if (!value || json_type(value) != JSON_OBJECT || json_object_size(value) > 4) goto done;
  for (size_t i = 0; i < json_object_size(value); ++i) {
    const char *name = json_object_key(value, i);
    size_t j;
    if (!name || strlen(name) != json_object_key_len(value, i)) goto done;
    for (j = 0; j < 4; ++j) if (!strcmp(name, fields[j])) break;
    if (j == 4 || (seen & (1u << j))) goto done;
    seen |= 1u << j;
  }
  if ((seen & 7u) != 7u) goto done;
  for (size_t i = 0; i < 4; ++i) {
    const json_value_t *field = json_object_get(value, fields[i]);
    if (i == 3 && !(seen & 8u)) continue;
    if (!task_string(field, SIZE_MAX, i == 1, i == 1 || i == 3)) goto done;
    if (json_type(field) == JSON_STRING && json_string_len(field) >
        (i == 2 ? TURBO_AGENT_TASK_CONTEXT_MAX_TEXT_BYTES : TURBO_AGENT_TASK_CONTEXT_MAX_FIELD_BYTES)) {
      rc = SALTS_ENOBUFS; goto done;
    }
  }
  rc = SALTS_ENOMEM;
  record = json_create_object();
  if (!record || turbo_runtime_json_object_set_string(record, "namespace", ns) ||
      turbo_runtime_json_object_set_string(record, "key", key)) goto done;
  if (turbo_runtime_json_object_set(record, "value", value)) goto done;
  value = NULL;
  *out = record; record = NULL; rc = SALTS_OK;
done:
  free(raw); json_free(value); json_free(record);
  return rc;
}

static int task_knowledge(const turbo_agent_knowledge_context_config_t *config,
    size_t limit, json_value_t **out) {
  static const char *fields[] = {"document_id", "chunk_id", "uri", "kind", "title", "text"};
  json_value_t *results = NULL, *selected = NULL, *record = NULL;
  int rc = SALTS_EIO;
  *out = NULL;
  if (turbo_agent_knowledge_store_query_ex(config->store, config->query, config->kind,
          config->uri_prefix, limit, &results)) goto done;
  rc = SALTS_EPROTO;
  if (!results || json_type(results) != JSON_ARRAY || json_array_size(results) > limit) goto done;
  if (!json_array_size(results)) { rc = SALTS_ENOENT; goto done; }
  selected = json_create_array();
  if (!selected) { rc = SALTS_ENOMEM; goto done; }
  for (size_t i = 0; i < json_array_size(results); ++i) {
    const json_value_t *item = json_array_get(results, i);
    rc = SALTS_EPROTO;
    if (!item || json_type(item) != JSON_OBJECT) goto done;
    for (size_t j = 0; j < 6; ++j) {
      const json_value_t *field = json_object_get(item, fields[j]);
      if (!task_string(field, SIZE_MAX, j == 4, 0)) goto done;
      if (json_string_len(field) > (j == 5 ? TURBO_AGENT_TASK_CONTEXT_MAX_TEXT_BYTES
                                          : TURBO_AGENT_TASK_CONTEXT_MAX_FIELD_BYTES)) {
        rc = SALTS_ENOBUFS; goto done;
      }
    }
    if (strcmp(json_get_string(item, "kind"), config->kind) ||
        strncmp(json_get_string(item, "uri"), config->uri_prefix, strlen(config->uri_prefix))) {
      rc = SALTS_EPERM; goto done;
    }
    rc = SALTS_ENOMEM;
    record = json_create_object();
    if (!record) goto done;
    for (size_t j = 0; j < 6; ++j)
      if (turbo_runtime_json_object_set_clone(record, fields[j], json_object_get(item, fields[j]))) goto done;
    if (turbo_runtime_json_array_append(selected, record)) goto done;
    record = NULL;
  }
  *out = selected; selected = NULL; rc = SALTS_OK;
done:
  json_free(record); json_free(selected); json_free(results);
  return rc;
}

int turbo_agent_session_create_task_input_state_json_value(const turbo_agent_session_t *session,
    const char *goal, const turbo_agent_task_context_options_t *options, json_value_t **out_state) {
  const turbo_agent_knowledge_context_config_t *knowledge;
  json_value_t *packet = NULL, *memories = NULL, *matches = NULL, *record = NULL, *selection = NULL;
  char *serialized = NULL;
  int rc = SALTS_EINVAL;
  if (!out_state) return rc;
  *out_state = NULL;
  if (!session || !goal || !goal[0] || !options || options->struct_size < sizeof(*options) ||
      options->abi_version != TURBO_AGENT_TASK_CONTEXT_ABI_VERSION ||
      options->memory_key_count > TURBO_AGENT_TASK_CONTEXT_MAX_RECORDS ||
      options->knowledge_limit > TURBO_AGENT_TASK_CONTEXT_MAX_RECORDS) return rc;
  if (strlen(goal) > TURBO_AGENT_TASK_CONTEXT_MAX_TEXT_BYTES) return SALTS_ENOBUFS;
  if (options->memory_key_count) {
    const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);
    if (!task_selector(options->memory_namespace) || !options->memory_keys || !store || !store->get) return rc;
    for (size_t i = 0; i < options->memory_key_count; ++i) {
      if (!task_selector(options->memory_keys[i])) return rc;
      for (size_t j = 0; j < i; ++j)
        if (!strcmp(options->memory_keys[i], options->memory_keys[j])) return rc;
    }
  }
  knowledge = turbo_agent_session_knowledge_context_internal(session);
  if (options->knowledge_limit && (!knowledge || !knowledge->store ||
      !task_selector(knowledge->query) || !task_selector(knowledge->kind) ||
      !task_selector(knowledge->uri_prefix))) return rc;
  rc = SALTS_ENOMEM;
  packet = json_create_object(); memories = json_create_array();
  if (!packet || !memories) goto done;
  for (size_t i = 0; i < options->memory_key_count; ++i) {
    rc = task_memory(session, options->memory_namespace, options->memory_keys[i], &record);
    if (rc) goto done;
    rc = SALTS_ENOMEM;
    if (turbo_runtime_json_array_append(memories, record)) goto done;
    record = NULL;
  }
  if (options->knowledge_limit) {
    rc = task_knowledge(knowledge, options->knowledge_limit, &matches);
    if (rc) goto done;
    rc = SALTS_ENOMEM;
    selection = json_create_object();
    if (!selection || turbo_runtime_json_object_set_string(selection, "query", knowledge->query) ||
        turbo_runtime_json_object_set_string(selection, "kind", knowledge->kind) ||
        turbo_runtime_json_object_set_string(selection, "uri_prefix", knowledge->uri_prefix) ||
        turbo_runtime_json_object_set_int64(selection, "limit", (int64_t)options->knowledge_limit)) goto done;
  } else {
    matches = json_create_array(); selection = json_create_null();
    rc = SALTS_ENOMEM;
    if (!matches || !selection) goto done;
  }
  rc = SALTS_ENOMEM;
  if (turbo_runtime_json_object_set_int64(packet, "schema_version", 1) ||
      turbo_runtime_json_object_set_string(packet, "host_goal", goal) ||
      turbo_runtime_json_object_set_string(packet, "source_semantics", "selected_read_observation")) goto done;
  if (turbo_runtime_json_object_set(packet, "memory_records", memories)) goto done;
  memories = NULL;
  if (turbo_runtime_json_object_set(packet, "knowledge_matches", matches)) goto done;
  matches = NULL;
  if (turbo_runtime_json_object_set(packet, "knowledge_selection", selection)) goto done;
  selection = NULL;
  serialized = json_serialize(packet, NULL);
  if (!serialized) goto done;
  if (strlen(serialized) > TURBO_AGENT_TASK_CONTEXT_MAX_INPUT_BYTES) { rc = SALTS_ENOBUFS; goto done; }
  *out_state = turbo_agent_session_create_input_state_json_value(serialized);
  if (!*out_state) goto done;
  rc = SALTS_OK;
done:
  json_serialize_free(serialized); json_free(packet); json_free(memories);
  json_free(matches); json_free(record); json_free(selection);
  return rc;
}
