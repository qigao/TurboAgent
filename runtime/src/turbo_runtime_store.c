#include "turbo_runtime_store.h"

#include <salts/thread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
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
  cmeta_mutex_t store_mutex;
};

static void turbo_runtime_store_json_free(json_value_t **value) {
  if (!value || !*value) {
    return;
  }
  json_free(*value);
  *value = NULL;
}
static char *turbo_runtime_store_strdup(const char *text) {
  size_t len;
  char *copy;

  if (!text) {
    return NULL;
  }
  len = strlen(text);
  copy = (char *)malloc(len + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, text, len + 1);
  return copy;
}

static int turbo_runtime_store_write_text_file(const char *path, const char *content) {
  FILE *fp;

  if (!path || !content) {
    return -1;
  }

  fp = fopen(path, "wb");
  if (!fp) {
    return -1;
  }
  if (fwrite(content, 1, strlen(content), fp) != strlen(content)) {
    fclose(fp);
    return -1;
  }
  fclose(fp);
  return 0;
}

static int turbo_runtime_store_read_text_file(const char *path, char **out_content) {
  FILE *fp;
  long size;
  char *buffer;

  if (!path || !out_content) {
    return -1;
  }

  *out_content = NULL;
  fp = fopen(path, "rb");
  if (!fp) {
    return errno == ENOENT ? SALTS_ENOENT : SALTS_EIO;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return SALTS_EIO;
  }
  size = ftell(fp);
  if (size < 0) {
    fclose(fp);
    return SALTS_EIO;
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return SALTS_EIO;
  }
  buffer = (char *)malloc((size_t)size + 1);
  if (!buffer) {
    fclose(fp);
    return SALTS_ENOMEM;
  }
  if (size > 0 && fread(buffer, 1, (size_t)size, fp) != (size_t)size) {
    free(buffer);
    fclose(fp);
    return SALTS_EIO;
  }
  buffer[size] = '\0';
  if (fclose(fp) != 0) { free(buffer); return SALTS_EIO; }
  *out_content = buffer;
  return 0;
}

static int turbo_runtime_store_ensure_dir(const char *path) {
  if (!path || path[0] == '\0') {
    return -1;
  }
#ifdef _WIN32
  if (_mkdir(path) == 0 || errno == EEXIST) {
    return 0;
  }
#else
  if (mkdir(path, 0777) == 0 || errno == EEXIST) {
    return 0;
  }
#endif
  return -1;
}

static char *turbo_runtime_store_join_path(const char *left, const char *right) {
  int needed;
  char *buffer;

  if (!left || !right) {
    return NULL;
  }

  needed = snprintf(NULL, 0, "%s/%s", left, right);
  if (needed < 0) {
    return NULL;
  }
  buffer = (char *)malloc((size_t)needed + 1);
  if (!buffer) {
    return NULL;
  }
  snprintf(buffer, (size_t)needed + 1, "%s/%s", left, right);
  return buffer;
}

static char *turbo_runtime_store_record_path(const char *root_dir, const char *collection,
                                      const char *id) {
  char *directory;
  char *filename;
  char *path;
  int needed;

  if (!root_dir || !collection || !id) {
    return NULL;
  }

  directory = turbo_runtime_store_join_path(root_dir, collection);
  if (!directory) {
    return NULL;
  }
  needed = snprintf(NULL, 0, "%s.json", id);
  if (needed < 0) {
    free(directory);
    return NULL;
  }
  filename = (char *)malloc((size_t)needed + 1);
  if (!filename) {
    free(directory);
    return NULL;
  }
  snprintf(filename, (size_t)needed + 1, "%s.json", id);
  path = turbo_runtime_store_join_path(directory, filename);
  free(filename);
  free(directory);
  return path;
}

static int turbo_runtime_store_parse_json_string(const char *json_text, json_value_t **out_json) {
  json_value_t *json_root = NULL;

  if (!json_text || !out_json) {
    return -1;
  }

  *out_json = NULL;
  json_root = json_parse(json_text, strlen(json_text));
  if (!json_root) {
    turbo_runtime_store_json_free(&json_root);
    return -1;
  }
  *out_json = json_root;
  return 0;
}

int turbo_agent_runtime_store_put_json(turbo_agent_runtime_t *runtime, const char *collection,
                                       const char *id, const json_value_t *record_json) {
  char *serialized;
  int rc;

  if (!runtime || !collection || !id || !record_json || !runtime->store.put) {
    return -1;
  }

  serialized = json_serialize(record_json, NULL);
  if (!serialized) {
    return -1;
  }
  cmeta_mutex_lock(&runtime->store_mutex);
  rc = runtime->store.put(runtime->store.user_data, collection, id, serialized);
  cmeta_mutex_unlock(&runtime->store_mutex);
  json_serialize_free(serialized);
  return rc;
}

int turbo_agent_runtime_store_put_json_if_absent(
    turbo_agent_runtime_t *runtime, const char *collection, const char *id,
    const json_value_t *record_json) {
  char *payload = NULL;
  char *existing = NULL;
  int rc;
  if (!runtime || !collection || !id || !record_json ||
      !runtime->store.get || !runtime->store.put)
    return SALTS_EINVAL;
  payload = json_serialize(record_json, NULL);
  if (!payload) return SALTS_ENOMEM;

  cmeta_mutex_lock(&runtime->store_mutex);
  rc = runtime->store.get(runtime->store.user_data, collection, id, &existing);
  if (rc == SALTS_OK) {
    rc = existing ? SALTS_EALREADY : SALTS_EPROTO;
  } else if (rc == SALTS_ENOENT) {
    rc = runtime->store.put(runtime->store.user_data, collection, id, payload);
  }
  cmeta_mutex_unlock(&runtime->store_mutex);

  free(existing);
  json_serialize_free(payload);
  return rc;
}

int turbo_agent_runtime_store_get_json_status(turbo_agent_runtime_t *runtime, const char *collection,
                                       const char *id, json_value_t **out_record_json) {
  char *serialized = NULL;
  int rc;

  if (out_record_json) {
    *out_record_json = NULL;
  }
  if (!runtime || !collection || !id || !out_record_json || !runtime->store.get) {
    return SALTS_EINVAL;
  }

  cmeta_mutex_lock(&runtime->store_mutex);
  rc = runtime->store.get(runtime->store.user_data, collection, id, &serialized);
  cmeta_mutex_unlock(&runtime->store_mutex);
  if (rc != 0 || !serialized) {
    free(serialized);
    return rc != 0 ? rc : SALTS_EPROTO;
  }
  rc = turbo_runtime_store_parse_json_string(serialized, out_record_json);
  free(serialized);
  return rc == 0 ? SALTS_OK : SALTS_EPROTO;
}

int turbo_agent_runtime_store_get_json(turbo_agent_runtime_t *runtime, const char *collection,
                                       const char *id, json_value_t **out_record_json) {
  return turbo_agent_runtime_store_get_json_status(runtime, collection, id, out_record_json) == 0
             ? 0 : -1;
}

int turbo_agent_runtime_store_list_json(turbo_agent_runtime_t *runtime, const char *collection,
                                        const char *filter_key, const char *filter_value,
                                        json_value_t **out_records_json) {
  char *serialized = NULL;
  int rc;

  if (out_records_json) {
    *out_records_json = NULL;
  }
  if (!runtime || !collection || !out_records_json || !runtime->store.list) {
    return -1;
  }

  cmeta_mutex_lock(&runtime->store_mutex);
  rc = runtime->store.list(runtime->store.user_data, collection, filter_key, filter_value,
                           &serialized);
  cmeta_mutex_unlock(&runtime->store_mutex);
  if (rc != 0 || !serialized) {
    free(serialized);
    return -1;
  }
  rc = turbo_runtime_store_parse_json_string(serialized, out_records_json);
  free(serialized);
  if (rc != 0 || !*out_records_json || json_type(*out_records_json) != JSON_ARRAY) {
    turbo_runtime_store_json_free(out_records_json);
    return -1;
  }
  return 0;
}

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
      record_copy = turbo_runtime_store_strdup(record_json);
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
  record->collection = turbo_runtime_store_strdup(collection);
  record->id = turbo_runtime_store_strdup(id);
  record->record_json = turbo_runtime_store_strdup(record_json);
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
    return SALTS_EINVAL;
  }
  *out_record_json = NULL;
  for (record = store->head; record; record = record->next) {
    if (strcmp(record->collection, collection) == 0 && strcmp(record->id, id) == 0) {
      *out_record_json = turbo_runtime_store_strdup(record->record_json);
      return *out_record_json ? SALTS_OK : SALTS_ENOMEM;
    }
  }
  return SALTS_ENOENT;
}

static int turbo_runtime_store_json_matches_filter(const json_value_t *record, const char *filter_key,
                                            const char *filter_value) {
  const char *value;

  if (!filter_key || !filter_value) {
    return 1;
  }
  value = json_get_string(record, filter_key);
  return value && strcmp(value, filter_value) == 0;
}

static int turbo_agent_runtime_memory_store_list(void *user_data, const char *collection,
                                                 const char *filter_key, const char *filter_value,
                                                 char **out_records_json) {
  turbo_agent_runtime_memory_store_t *store = (turbo_agent_runtime_memory_store_t *)user_data;
  turbo_agent_runtime_memory_record_t *record;
  json_value_t *records_json;
  json_value_t *record_json = NULL;

  if (!store || !collection || !out_records_json) {
    return -1;
  }
  *out_records_json = NULL;
  records_json = json_create_array();
  if (!records_json) {
    return -1;
  }
  for (record = store->head; record; record = record->next) {
    if (strcmp(record->collection, collection) != 0) {
      continue;
    }
    if (turbo_runtime_store_parse_json_string(record->record_json, &record_json) != 0) {
      turbo_runtime_store_json_free(&records_json);
      return -1;
    }
    if (turbo_runtime_store_json_matches_filter(record_json, filter_key, filter_value)) {
      if (turbo_runtime_json_array_append(records_json, record_json) != TURBO_RUNTIME_JSON_OK) {
        turbo_runtime_store_json_free(&record_json);
        turbo_runtime_store_json_free(&records_json);
        return -1;
      }
      record_json = NULL;
    }
    turbo_runtime_store_json_free(&record_json);
  }
  {
    char *serialized = json_serialize(records_json, NULL);
    *out_records_json = serialized ? turbo_runtime_store_strdup(serialized) : NULL;
    json_serialize_free(serialized);
  }
  turbo_runtime_store_json_free(&records_json);
  return *out_records_json ? 0 : -1;
}

static void turbo_agent_runtime_file_store_destroy(void *user_data) {
  turbo_agent_runtime_file_store_t *store = (turbo_agent_runtime_file_store_t *)user_data;

  if (!store) {
    return;
  }
  free(store->root_dir);
  free(store);
}

static int turbo_agent_runtime_file_store_put(void *user_data, const char *collection,
                                              const char *id, const char *record_json) {
  turbo_agent_runtime_file_store_t *store = (turbo_agent_runtime_file_store_t *)user_data;
  char *directory;
  char *path;
  int rc;

  if (!store || !store->root_dir || !collection || !id || !record_json) {
    return -1;
  }
  directory = turbo_runtime_store_join_path(store->root_dir, collection);
  if (!directory || turbo_runtime_store_ensure_dir(directory) != 0) {
    free(directory);
    return -1;
  }
  free(directory);
  path = turbo_runtime_store_record_path(store->root_dir, collection, id);
  if (!path) {
    return -1;
  }
  rc = turbo_runtime_store_write_text_file(path, record_json);
  free(path);
  return rc;
}

static int turbo_agent_runtime_file_store_get(void *user_data, const char *collection,
                                              const char *id, char **out_record_json) {
  turbo_agent_runtime_file_store_t *store = (turbo_agent_runtime_file_store_t *)user_data;
  char *path;
  int rc;

  if (out_record_json) *out_record_json = NULL;
  if (!store || !store->root_dir || !collection || !id || !out_record_json) {
    return SALTS_EINVAL;
  }
  path = turbo_runtime_store_record_path(store->root_dir, collection, id);
  if (!path) {
    return SALTS_ENOMEM;
  }
  rc = turbo_runtime_store_read_text_file(path, out_record_json);
  free(path);
  return rc;
}

static int turbo_agent_runtime_file_store_list_append(const char *path, const char *filter_key,
                                                      const char *filter_value,
                                                      json_value_t *records_json) {
  char *record_text = NULL;
  json_value_t *record_json = NULL;

  if (!path || !records_json) {
    return -1;
  }
  if (turbo_runtime_store_read_text_file(path, &record_text) != 0 ||
      turbo_runtime_store_parse_json_string(record_text, &record_json) != 0) {
    free(record_text);
    turbo_runtime_store_json_free(&record_json);
    return -1;
  }
  free(record_text);
  if (turbo_runtime_store_json_matches_filter(record_json, filter_key, filter_value)) {
    if (turbo_runtime_json_array_append(records_json, record_json) != TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_store_json_free(&record_json);
      return -1;
    }
    record_json = NULL;
  }
  turbo_runtime_store_json_free(&record_json);
  return 0;
}

static int turbo_agent_runtime_file_store_list(void *user_data, const char *collection,
                                               const char *filter_key, const char *filter_value,
                                               char **out_records_json) {
  turbo_agent_runtime_file_store_t *store = (turbo_agent_runtime_file_store_t *)user_data;
  char *directory;
  json_value_t *records_json;
  int rc = 0;

  if (!store || !store->root_dir || !collection || !out_records_json) {
    return -1;
  }
  *out_records_json = NULL;
  directory = turbo_runtime_store_join_path(store->root_dir, collection);
  if (!directory) {
    return -1;
  }
  records_json = json_create_array();
  if (!records_json) {
    free(directory);
    return -1;
  }

#ifdef _WIN32
  {
    WIN32_FIND_DATAA find_data;
    HANDLE handle;
    char *pattern = turbo_runtime_store_join_path(directory, "*.json");

    if (!pattern) {
      turbo_runtime_store_json_free(&records_json);
      free(directory);
      return -1;
    }
    handle = FindFirstFileA(pattern, &find_data);
    DWORD find_error = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    free(pattern);
    if (handle != INVALID_HANDLE_VALUE) {
      do {
        char *path;
        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          continue;
        }
        path = turbo_runtime_store_join_path(directory, find_data.cFileName);
        if (!path || turbo_agent_runtime_file_store_list_append(path, filter_key, filter_value,
                                                                records_json) != 0) {
          free(path);
          rc = -1;
          break;
        }
        free(path);
      } while (FindNextFileA(handle, &find_data));
      if (rc == 0 && GetLastError() != ERROR_NO_MORE_FILES) rc = -1;
      if (!FindClose(handle)) rc = -1;
    } else if (find_error != ERROR_FILE_NOT_FOUND && find_error != ERROR_PATH_NOT_FOUND) {
      rc = -1;
    } else {
      /* PATH_NOT_FOUND can also mean a collection is a regular file. Do not
       * publish an empty list for a damaged layout or an inaccessible path. */
      DWORD attributes = GetFileAttributesA(directory);
      if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) rc = -1;
      } else {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) rc = -1;
      }
    }
  }
#else
  {
    DIR *dir = opendir(directory);
    struct dirent *entry;

    if (!dir) {
      if (errno != ENOENT) {
        turbo_runtime_store_json_free(&records_json);
        free(directory);
        return -1;
      }
    } else {
      for (;;) {
        char *path;
        size_t len;
        errno = 0;
        entry = readdir(dir);
        if (!entry) { if (errno != 0) rc = -1; break; }
        len = strlen(entry->d_name);
        if (len < 6 || strcmp(entry->d_name + len - 5, ".json") != 0) {
          continue;
        }
        path = turbo_runtime_store_join_path(directory, entry->d_name);
        if (!path || turbo_agent_runtime_file_store_list_append(path, filter_key, filter_value,
                                                                records_json) != 0) {
          free(path);
          rc = -1;
          break;
        }
        free(path);
      }
      if (closedir(dir) != 0) rc = -1;
    }
  }
#endif

  free(directory);
  if (rc != 0) {
    turbo_runtime_store_json_free(&records_json);
    return -1;
  }
  {
    char *serialized = json_serialize(records_json, NULL);
    *out_records_json = serialized ? turbo_runtime_store_strdup(serialized) : NULL;
    json_serialize_free(serialized);
  }
  turbo_runtime_store_json_free(&records_json);
  return *out_records_json ? 0 : -1;
}

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

CXX_C_API turbo_agent_runtime_store_t turbo_agent_runtime_store_file_create(const char *root_dir) {
  turbo_agent_runtime_store_t store = {0};
  turbo_agent_runtime_file_store_t *user_data;
  char *threads_dir;
  char *runs_dir;
  char *checkpoints_dir;

  if (!root_dir) {
    return store;
  }
  user_data = (turbo_agent_runtime_file_store_t *)calloc(1, sizeof(*user_data));
  if (!user_data) {
    return store;
  }
  user_data->root_dir = turbo_runtime_store_strdup(root_dir);
  if (!user_data->root_dir) {
    free(user_data);
    return store;
  }
  threads_dir = turbo_runtime_store_join_path(root_dir, "threads");
  runs_dir = turbo_runtime_store_join_path(root_dir, "runs");
  checkpoints_dir =
      turbo_runtime_store_join_path(root_dir, "checkpoints");
  if (turbo_runtime_store_ensure_dir(root_dir) != 0 || !threads_dir || !runs_dir ||
      !checkpoints_dir || turbo_runtime_store_ensure_dir(threads_dir) != 0 ||
      turbo_runtime_store_ensure_dir(runs_dir) != 0 ||
      turbo_runtime_store_ensure_dir(checkpoints_dir) != 0) {
    free(threads_dir);
    free(runs_dir);
    free(checkpoints_dir);
    turbo_agent_runtime_file_store_destroy(user_data);
    return store;
  }
  free(threads_dir);
  free(runs_dir);
  free(checkpoints_dir);
  store.put = turbo_agent_runtime_file_store_put;
  store.get = turbo_agent_runtime_file_store_get;
  store.list = turbo_agent_runtime_file_store_list;
  store.user_data = user_data;
  store.user_data_free = turbo_agent_runtime_file_store_destroy;
  return store;
}

CXX_C_API turbo_agent_runtime_t *
turbo_agent_runtime_create(const turbo_agent_runtime_store_t *store) {
  turbo_agent_runtime_t *runtime;

  if (!store || !store->put || !store->get || !store->list) {
    return NULL;
  }
  runtime = (turbo_agent_runtime_t *)calloc(1, sizeof(*runtime));
  if (!runtime) {
    return NULL;
  }
  runtime->store = *store;
  cmeta_mutex_init(&runtime->store_mutex);
  if (!runtime->store_mutex) {
    free(runtime);
    return NULL;
  }
  return runtime;
}

CXX_C_API void turbo_agent_runtime_destroy(turbo_agent_runtime_t *runtime) {
  if (!runtime) {
    return;
  }
  if (runtime->store.user_data_free) {
    runtime->store.user_data_free(runtime->store.user_data);
  }
  cmeta_mutex_destroy(&runtime->store_mutex);
  free(runtime);
}
