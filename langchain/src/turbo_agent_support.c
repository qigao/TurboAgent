#include "turbo_agent_core_internal.h"
#include "turbo_agent_util_internal.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
  TURBO_AGENT_RUNNABLE_CALLBACK = 0,
  TURBO_AGENT_RUNNABLE_PIPE = 1
} turbo_agent_runnable_kind_t;

struct turbo_agent_runnable_s {
  turbo_agent_runnable_kind_t kind;
  turbo_agent_runnable_fn invoke;
  void *user_data;
  turbo_agent_runnable_user_data_free_fn user_data_free;
};

typedef struct {
  const turbo_agent_runnable_t *first;
  const turbo_agent_runnable_t *second;
} turbo_agent_runnable_pipe_t;

typedef struct {
  char *key;
  char *value_json;
} turbo_agent_slot_store_entry_t;

typedef struct {
  turbo_agent_slot_store_entry_t *entries;
  size_t count;
  size_t capacity;
} turbo_agent_slot_store_t;

static char *turbo_agent_slot_store_strdup_malloc(const char *src) {
  size_t len;
  char *copy;

  if (!src) {
    return NULL;
  }
  len = strlen(src);
  copy = (char *)malloc(len + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, src, len + 1);
  return copy;
}

CXX_C_API int turbo_agent_set_store(turbo_agent_t *agent, const turbo_agent_store_t *store) {
  if (!agent) {
    return -1;
  }

  if (agent->has_store && agent->store.user_data_free) {
    agent->store.user_data_free(agent->store.user_data);
  }

  memset(&agent->store, 0, sizeof(agent->store));
  agent->has_store = 0;
  if (!store) {
    return 0;
  }

  agent->store = *store;
  agent->has_store = 1;
  return 0;
}

CXX_C_API turbo_agent_runnable_t *
turbo_agent_runnable_create(const turbo_agent_runnable_config_t *config) {
  turbo_agent_runnable_t *runnable;

  if (!config || !config->invoke) {
    return NULL;
  }

  runnable = (turbo_agent_runnable_t *)calloc(1, sizeof(*runnable));
  if (!runnable) {
    return NULL;
  }

  runnable->kind = TURBO_AGENT_RUNNABLE_CALLBACK;
  runnable->invoke = config->invoke;
  runnable->user_data = config->user_data;
  runnable->user_data_free = config->user_data_free;
  return runnable;
}

CXX_C_API void turbo_agent_runnable_destroy(turbo_agent_runnable_t *runnable) {
  if (!runnable) {
    return;
  }

  if (runnable->user_data_free) {
    runnable->user_data_free(runnable->user_data);
  }
  free(runnable);
}

CXX_C_API int turbo_agent_runnable_invoke(const turbo_agent_runnable_t *runnable,
                                          const json_value_t *input,
                                          json_value_t **out_output) {
  if (!runnable || !runnable->invoke || !out_output) {
    return -1;
  }

  *out_output = NULL;
  return runnable->invoke(input, out_output, runnable->user_data);
}

static int turbo_agent_runnable_pipe_invoke(const json_value_t *input, json_value_t **out_output,
                                            void *user_data) {
  turbo_agent_runnable_pipe_t *pipe = (turbo_agent_runnable_pipe_t *)user_data;
  json_value_t *middle = NULL;
  int rc;

  if (!pipe || !pipe->first || !pipe->second || !out_output) {
    return -1;
  }

  rc = turbo_agent_runnable_invoke(pipe->first, input, &middle);
  if (rc != 0) {
    return rc;
  }

  rc = turbo_agent_runnable_invoke(pipe->second, middle, out_output);
  turbo_free_json(&middle);
  return rc;
}

CXX_C_API turbo_agent_runnable_t *
turbo_agent_runnable_pipe(const turbo_agent_runnable_t *first,
                          const turbo_agent_runnable_t *second) {
  turbo_agent_runnable_pipe_t *pipe;
  turbo_agent_runnable_config_t config;

  if (!first || !second) {
    return NULL;
  }

  pipe = (turbo_agent_runnable_pipe_t *)calloc(1, sizeof(*pipe));
  if (!pipe) {
    return NULL;
  }

  pipe->first = first;
  pipe->second = second;
  memset(&config, 0, sizeof(config));
  config.invoke = turbo_agent_runnable_pipe_invoke;
  config.user_data = pipe;
  config.user_data_free = turbo_agent_util_free_user_data;
  return turbo_agent_runnable_create(&config);
}

static int turbo_agent_slot_store_find_index(const turbo_agent_slot_store_t *store,
                                             const char *key) {
  size_t i;

  if (!store || !key) {
    return -1;
  }

  for (i = 0; i < store->count; ++i) {
    if (store->entries[i].key && strcmp(store->entries[i].key, key) == 0) {
      return (int)i;
    }
  }

  return -1;
}

static int turbo_agent_slot_store_reserve(turbo_agent_slot_store_t *store) {
  turbo_agent_slot_store_entry_t *resized;
  size_t new_capacity;

  if (!store) {
    return -1;
  }
  if (store->count < store->capacity) {
    return 0;
  }

  new_capacity = store->capacity == 0 ? 8 : store->capacity * 2;
  resized =
      (turbo_agent_slot_store_entry_t *)realloc(store->entries, new_capacity * sizeof(*resized));
  if (!resized) {
    return -1;
  }

  memset(resized + store->capacity, 0, (new_capacity - store->capacity) * sizeof(*resized));
  store->entries = resized;
  store->capacity = new_capacity;
  return 0;
}

static int turbo_agent_slot_store_get(void *user_data, const char *key,
                                      char **out_value_json) {
  turbo_agent_slot_store_t *store = (turbo_agent_slot_store_t *)user_data;
  int index;

  if (!store || !key || !out_value_json) {
    return -1;
  }

  *out_value_json = NULL;
  index = turbo_agent_slot_store_find_index(store, key);
  if (index < 0 || !store->entries[index].value_json) {
    return -1;
  }

  *out_value_json = turbo_agent_slot_store_strdup_malloc(store->entries[index].value_json);
  return *out_value_json ? 0 : -1;
}

static int turbo_agent_slot_store_put(void *user_data, const char *key,
                                      const char *value_json) {
  turbo_agent_slot_store_t *store = (turbo_agent_slot_store_t *)user_data;
  int index;
  char *key_copy;
  char *value_copy;

  if (!store || !key || !value_json) {
    return -1;
  }

  index = turbo_agent_slot_store_find_index(store, key);
  value_copy = turbo_agent_slot_store_strdup_malloc(value_json);
  if (!value_copy) {
    return -1;
  }

  if (index >= 0) {
    free(store->entries[index].value_json);
    store->entries[index].value_json = value_copy;
    return 0;
  }

  if (turbo_agent_slot_store_reserve(store) != 0) {
    free(value_copy);
    return -1;
  }

  key_copy = turbo_agent_slot_store_strdup_malloc(key);
  if (!key_copy) {
    free(value_copy);
    return -1;
  }

  store->entries[store->count].key = key_copy;
  store->entries[store->count].value_json = value_copy;
  store->count++;
  return 0;
}

static int turbo_agent_slot_store_delete(void *user_data, const char *key) {
  turbo_agent_slot_store_t *store = (turbo_agent_slot_store_t *)user_data;
  int index;
  size_t i;

  if (!store || !key) {
    return -1;
  }

  index = turbo_agent_slot_store_find_index(store, key);
  if (index < 0) {
    return -1;
  }

  free(store->entries[index].key);
  free(store->entries[index].value_json);
  for (i = (size_t)index + 1; i < store->count; ++i) {
    store->entries[i - 1] = store->entries[i];
  }
  store->count--;
  if (store->count < store->capacity) {
    memset(&store->entries[store->count], 0, sizeof(store->entries[store->count]));
  }
  return 0;
}

static void turbo_agent_slot_store_destroy(void *user_data) {
  turbo_agent_slot_store_t *store = (turbo_agent_slot_store_t *)user_data;
  size_t i;

  if (!store) {
    return;
  }

  for (i = 0; i < store->count; ++i) {
    free(store->entries[i].key);
    free(store->entries[i].value_json);
  }
  free(store->entries);
  free(store);
}

CXX_C_API turbo_agent_store_t turbo_agent_store_memory_create(void) {
  turbo_agent_store_t store = {0};
  turbo_agent_slot_store_t *memory_store =
      (turbo_agent_slot_store_t *)calloc(1, sizeof(*memory_store));

  if (!memory_store) {
    return store;
  }

  store.get = turbo_agent_slot_store_get;
  store.put = turbo_agent_slot_store_put;
  store.remove = turbo_agent_slot_store_delete;
  store.user_data = memory_store;
  store.user_data_free = turbo_agent_slot_store_destroy;
  return store;
}
