#include "turbo_agent_session.h"
#include "turbo_agent_state.h"
#include "turbo_runtime_json.h"

#include <string.h>

static int turbo_agent_session_load_memory_context_record(json_value_t *state,
                                                          const json_value_t *record) {
  const char *kind;
  const json_value_t *metadata;
  const json_value_t *path_value;
  const char *scope;
  const char *path = NULL;
  const char *text;

  if (!state || !record || json_type(record) != JSON_OBJECT) {
    return -1;
  }
  kind = json_get_string(record, "kind");
  if (!kind || strcmp(kind, "context") != 0) {
    return 0;
  }
  if (turbo_agent_memory_validate_record(record) != 0) {
    return -1;
  }
  metadata = json_object_get(record, "metadata");
  if (!metadata || json_type(metadata) != JSON_OBJECT) {
    return -1;
  }
  scope = json_get_string(metadata, "scope");
  text = json_get_string(record, "text");
  path_value = json_object_get(metadata, "path");
  if (!scope || scope[0] == '\0' || !text || text[0] == '\0' || !path_value) {
    return -1;
  }
  if (json_type(path_value) == JSON_STRING) {
    path = json_get_string(metadata, "path");
  } else if (json_type(path_value) != JSON_NULL) {
    return -1;
  }
  return turbo_agent_state_add_memory_context_layer(state, scope, path, text);
}

CXX_C_API int turbo_agent_session_memory_get(const turbo_agent_session_t *session,
                                             const char *memory_namespace, const char *key,
                                             char **out_value_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || !store->get) {
    return -1;
  }
  return turbo_agent_memory_get(store, memory_namespace, key, out_value_json);
}

CXX_C_API int turbo_agent_session_memory_put(const turbo_agent_session_t *session,
                                             const char *memory_namespace, const char *key,
                                             const char *value_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || !store->put) {
    return -1;
  }
  return turbo_agent_memory_put(store, memory_namespace, key, value_json);
}

CXX_C_API int turbo_agent_session_memory_put_context(const turbo_agent_session_t *session,
                                                     const char *memory_namespace, const char *key,
                                                     const char *scope, const char *path,
                                                     const char *text) {
  json_value_t *payload;
  char *serialized;
  int rc;

  if (!scope || scope[0] == '\0' || !text || text[0] == '\0') {
    return -1;
  }
  payload = json_create_object();
  if (!payload) {
    return -1;
  }
  if (turbo_runtime_json_object_set_string(payload, "scope", scope) != TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(payload, "path", path ? path : "") !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(payload, "text", text) != TURBO_RUNTIME_JSON_OK) {
    json_free(payload);
    return -1;
  }
  serialized = json_serialize(payload, NULL);
  json_free(payload);
  payload = NULL;
  if (!serialized) {
    return -1;
  }
  rc = turbo_agent_session_memory_put(session, memory_namespace, key, serialized);
  json_serialize_free(serialized);
  return rc;
}

CXX_C_API int turbo_agent_session_memory_delete(const turbo_agent_session_t *session,
                                                const char *memory_namespace, const char *key) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || !store->remove) {
    return -1;
  }
  return turbo_agent_memory_delete(store, memory_namespace, key);
}

CXX_C_API int turbo_agent_session_memory_list(const turbo_agent_session_t *session,
                                              const char *namespace_prefix,
                                              json_value_t **out_records_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || !store->list) {
    return -1;
  }
  return turbo_agent_memory_list(store, namespace_prefix, out_records_json);
}

CXX_C_API int turbo_agent_session_memory_list_records(const turbo_agent_session_t *session,
                                                      const char *namespace_prefix,
                                                      json_value_t **out_records_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || (!store->list && !store->query)) {
    return -1;
  }
  return turbo_agent_memory_list_records(store, namespace_prefix, out_records_json);
}

CXX_C_API int turbo_agent_session_memory_get_record(const turbo_agent_session_t *session,
                                                    const char *memory_namespace, const char *key,
                                                    json_value_t **out_record_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || !store->get) {
    return -1;
  }
  return turbo_agent_memory_get_record(store, memory_namespace, key, out_record_json);
}

CXX_C_API int turbo_agent_session_memory_put_record(const turbo_agent_session_t *session,
                                                    const json_value_t *record_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || !store->put) {
    return -1;
  }
  return turbo_agent_memory_put_record(store, record_json);
}

CXX_C_API int turbo_agent_session_memory_validate_record(const json_value_t *record_json) {
  return turbo_agent_memory_validate_record(record_json);
}

CXX_C_API int turbo_agent_session_memory_query_records(const turbo_agent_session_t *session,
                                                       const char *namespace_prefix,
                                                       const char *kind, const char *key_prefix,
                                                       const char *text_substring,
                                                       json_value_t **out_records_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || (!store->list && !store->query)) {
    return -1;
  }
  return turbo_agent_memory_query_records(store, namespace_prefix, kind, key_prefix, text_substring,
                                          out_records_json);
}

CXX_C_API int
turbo_agent_session_memory_query_records_ex(const turbo_agent_session_t *session,
                                            const turbo_agent_memory_query_options_t *options,
                                            json_value_t **out_records_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || (!store->list && !store->query)) {
    return -1;
  }
  return turbo_agent_memory_query_records_ex(store, options, out_records_json);
}

CXX_C_API int turbo_agent_session_load_memory_context(const turbo_agent_session_t *session,
                                                      json_value_t *state,
                                                      const char *namespace_prefix) {
  json_value_t *records = NULL;
  size_t i;
  int rc;

  if (!session || !state) {
    return -1;
  }
  rc = turbo_agent_session_memory_query_records(session, namespace_prefix, "context", NULL, NULL,
                                                &records);
  if (rc != 0 || !records || json_type(records) != JSON_ARRAY) {
    json_free(records);
    records = NULL;
    return -1;
  }
  for (i = 0; i < json_array_size(records); ++i) {
    if (turbo_agent_session_load_memory_context_record(state, json_array_get(records, i)) != 0) {
      json_free(records);
      records = NULL;
      return -1;
    }
  }
  json_free(records);
  records = NULL;
  return 0;
}
