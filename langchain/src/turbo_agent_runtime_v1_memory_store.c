/* turbo_agent_runtime_v1_memory_store.c
 * Extracted from turbo_agent_runtime_v1.c  —  in-memory KV store backend
 * implementing turbo_agent_runtime_store_t via a singly-linked list. */
#include "turbo_agent_runtime_v1_internal.h"

/* ---- private types are in turbo_agent_runtime_v1_internal.h ---- */

/* ---- callbacks ---- */

static void turbo_agent_runtime_memory_store_destroy(void *user_data) {
  turbo_agent_runtime_memory_store_t *store = (turbo_agent_runtime_memory_store_t *)user_data;
  turbo_agent_runtime_memory_record_t *record;
  turbo_agent_runtime_memory_record_t *next;

  if (!store) {
    return;
  }
  record = store->head;
  while (record) {
    next = record->next;
    free(record->collection);
    free(record->id);
    free(record->record_json);
    free(record);
    record = next;
  }
  free(store);
}

static int turbo_agent_runtime_memory_store_put(void *user_data, const char *collection,
                                                const char *id, const char *record_json) {
  turbo_agent_runtime_memory_store_t *store = (turbo_agent_runtime_memory_store_t *)user_data;
  turbo_agent_runtime_memory_record_t *record;
  char *record_copy;

  if (!store || !collection || !id || !record_json) {
    return -1;
  }

  for (record = store->head; record; record = record->next) {
    if (strcmp(record->collection, collection) == 0 && strcmp(record->id, id) == 0) {
      record_copy = turbo_agent_runtime_strdup(record_json);
      if (!record_copy) {
        return -1;
      }
      free(record->record_json);
      record->record_json = record_copy;
      return 0;
    }
  }

  record = (turbo_agent_runtime_memory_record_t *)calloc(1, sizeof(*record));
  if (!record) {
    return -1;
  }
  record->collection = turbo_agent_runtime_strdup(collection);
  record->id = turbo_agent_runtime_strdup(id);
  record->record_json = turbo_agent_runtime_strdup(record_json);
  if (!record->collection || !record->id || !record->record_json) {
    free(record->collection);
    free(record->id);
    free(record->record_json);
    free(record);
    return -1;
  }
  record->next = store->head;
  store->head = record;
  return 0;
}

static int turbo_agent_runtime_memory_store_get(void *user_data, const char *collection,
                                                const char *id, char **out_record_json) {
  turbo_agent_runtime_memory_store_t *store = (turbo_agent_runtime_memory_store_t *)user_data;
  turbo_agent_runtime_memory_record_t *record;

  if (!store || !collection || !id || !out_record_json) {
    return -1;
  }
  *out_record_json = NULL;
  for (record = store->head; record; record = record->next) {
    if (strcmp(record->collection, collection) == 0 && strcmp(record->id, id) == 0) {
      *out_record_json = turbo_agent_runtime_strdup(record->record_json);
      return *out_record_json ? 0 : -1;
    }
  }
  return -1;
}

static int turbo_agent_runtime_memory_store_list(void *user_data, const char *collection,
                                                 const char *filter_key,
                                                 const char *filter_value,
                                                 char **out_records_json) {
  turbo_agent_runtime_memory_store_t *store = (turbo_agent_runtime_memory_store_t *)user_data;
  turbo_agent_runtime_memory_record_t *record;
  json_value_t *records_json;
  json_value_t *record_json = NULL;

  if (!store || !collection || !out_records_json) {
    return -1;
  }
  *out_records_json = NULL;
  records_json = turbo_json_create_array();
  if (!records_json) {
    return -1;
  }
  for (record = store->head; record; record = record->next) {
    if (strcmp(record->collection, collection) != 0) {
      continue;
    }
    if (turbo_agent_runtime_parse_json_string(record->record_json, &record_json) != 0) {
      turbo_free_json(&records_json);
      return -1;
    }
    if (turbo_agent_runtime_json_matches_filter(record_json, filter_key, filter_value)) {
      turbo_json_array_add(records_json, record_json);
      record_json = NULL;
    }
    turbo_free_json(&record_json);
  }
  *out_records_json = turbo_json_serialize(records_json, NULL);
  turbo_free_json(&records_json);
  return *out_records_json ? 0 : -1;
}

/* ---- factory ---- */

CXX_C_API turbo_agent_runtime_store_t turbo_agent_runtime_store_memory_create(void) {
  turbo_agent_runtime_store_t store = {0};
  turbo_agent_runtime_memory_store_t *user_data;

  user_data = (turbo_agent_runtime_memory_store_t *)calloc(1, sizeof(*user_data));
  if (!user_data) {
    return store;
  }
  store.put = turbo_agent_runtime_memory_store_put;
  store.get = turbo_agent_runtime_memory_store_get;
  store.list = turbo_agent_runtime_memory_store_list;
  store.user_data = user_data;
  store.user_data_free = turbo_agent_runtime_memory_store_destroy;
  return store;
}
