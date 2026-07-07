/* turbo_agent_runtime_v1_file_store.c
 * Extracted from turbo_agent_runtime_v1.c  —  file-system KV store backend
 * implementing turbo_agent_runtime_store_t via JSON files on disk. */
#include "turbo_agent_runtime_v1_internal.h"

#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

/* ---- private type is in turbo_agent_runtime_v1_internal.h ---- */

/* ---- callbacks ---- */

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
  char *path;
  int rc;

  if (!store || !store->root_dir) {
    return -1;
  }
  path = turbo_agent_runtime_record_path(store->root_dir, collection, id);
  if (!path) {
    return -1;
  }
  rc = turbo_agent_runtime_write_text_file(path, record_json);
  free(path);
  return rc;
}

static int turbo_agent_runtime_file_store_get(void *user_data, const char *collection,
                                              const char *id, char **out_record_json) {
  turbo_agent_runtime_file_store_t *store = (turbo_agent_runtime_file_store_t *)user_data;
  char *path;
  int rc;

  if (!store || !store->root_dir || !out_record_json) {
    return -1;
  }
  path = turbo_agent_runtime_record_path(store->root_dir, collection, id);
  if (!path) {
    return -1;
  }
  rc = turbo_agent_runtime_read_text_file(path, out_record_json);
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
  if (turbo_agent_runtime_read_text_file(path, &record_text) != 0 ||
      turbo_agent_runtime_parse_json_string(record_text, &record_json) != 0) {
    free(record_text);
    turbo_free_json(&record_json);
    return -1;
  }
  free(record_text);
  if (turbo_agent_runtime_json_matches_filter(record_json, filter_key, filter_value)) {
    turbo_json_array_add(records_json, record_json);
    record_json = NULL;
  }
  turbo_free_json(&record_json);
  return 0;
}

static int turbo_agent_runtime_file_store_list(void *user_data, const char *collection,
                                               const char *filter_key,
                                               const char *filter_value,
                                               char **out_records_json) {
  turbo_agent_runtime_file_store_t *store = (turbo_agent_runtime_file_store_t *)user_data;
  char *directory;
  json_value_t *records_json;
  int rc = 0;

  if (!store || !store->root_dir || !collection || !out_records_json) {
    return -1;
  }
  *out_records_json = NULL;
  directory = turbo_agent_runtime_join_path(store->root_dir, collection);
  if (!directory) {
    return -1;
  }
  records_json = turbo_json_create_array();
  if (!records_json) {
    free(directory);
    return -1;
  }

#ifdef _WIN32
  {
    WIN32_FIND_DATAA find_data;
    HANDLE handle;
    char *pattern = turbo_agent_runtime_join_path(directory, "*.json");

    if (!pattern) {
      turbo_free_json(&records_json);
      free(directory);
      return -1;
    }
    handle = FindFirstFileA(pattern, &find_data);
    free(pattern);
    if (handle != INVALID_HANDLE_VALUE) {
      do {
        char *path;
        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          continue;
        }
        path = turbo_agent_runtime_join_path(directory, find_data.cFileName);
        if (!path || turbo_agent_runtime_file_store_list_append(path, filter_key, filter_value,
                                                                records_json) != 0) {
          free(path);
          rc = -1;
          break;
        }
        free(path);
      } while (FindNextFileA(handle, &find_data));
      FindClose(handle);
    }
  }
#else
  {
    DIR *dir = opendir(directory);
    struct dirent *entry;

    if (!dir) {
      turbo_free_json(&records_json);
      free(directory);
      return -1;
    }
    while ((entry = readdir(dir)) != NULL) {
      char *path;
      size_t len = strlen(entry->d_name);
      if (len < 6 || strcmp(entry->d_name + len - 5, ".json") != 0) {
        continue;
      }
      path = turbo_agent_runtime_join_path(directory, entry->d_name);
      if (!path || turbo_agent_runtime_file_store_list_append(path, filter_key, filter_value,
                                                              records_json) != 0) {
        free(path);
        rc = -1;
        break;
      }
      free(path);
    }
    closedir(dir);
  }
#endif

  free(directory);
  if (rc != 0) {
    turbo_free_json(&records_json);
    return -1;
  }
  *out_records_json = turbo_json_serialize(records_json, NULL);
  turbo_free_json(&records_json);
  return *out_records_json ? 0 : -1;
}

/* ---- factory ---- */

CXX_C_API turbo_agent_runtime_store_t
turbo_agent_runtime_store_file_create(const char *root_dir) {
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
  user_data->root_dir = turbo_agent_runtime_strdup(root_dir);
  if (!user_data->root_dir) {
    free(user_data);
    return store;
  }
  threads_dir = turbo_agent_runtime_join_path(root_dir, turbo_agent_runtime_threads_collection);
  runs_dir = turbo_agent_runtime_join_path(root_dir, turbo_agent_runtime_runs_collection);
  checkpoints_dir =
      turbo_agent_runtime_join_path(root_dir, turbo_agent_runtime_checkpoints_collection);
  if (turbo_agent_runtime_ensure_dir(root_dir) != 0 ||
      !threads_dir || !runs_dir || !checkpoints_dir ||
      turbo_agent_runtime_ensure_dir(threads_dir) != 0 ||
      turbo_agent_runtime_ensure_dir(runs_dir) != 0 ||
      turbo_agent_runtime_ensure_dir(checkpoints_dir) != 0) {
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
