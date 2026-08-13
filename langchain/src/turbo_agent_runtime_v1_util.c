/* turbo_agent_runtime_v1_util.c
 * Extracted from turbo_agent_runtime_v1.c  —  bind helpers, small
 * utilities, platform IO, JSON parse/filter helpers, and store adapter
 * layer.  No business logic lives here. */
#include "turbo_agent_runtime_v1_internal.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#endif

/* ------------------------------------------------------------------ */
/* atomic counter shared across the process (formerly file-static)    */
/* ------------------------------------------------------------------ */
static atomic_ullong turbo_agent_runtime_id_counter = 1;

/* ================================================================== */
/* bind helpers                                                       */
/* ================================================================== */

int turbo_agent_runtime_json_value_object_set_string(
    json_value_t *object, const char *key, const char *value) {
  json_value_t *field;

  if (!object || !key || !value) {
    return -1;
  }
  field = turbo_json_create_string(value);
  if (!field) {
    return -1;
  }
  if (turbo_runtime_json_object_set(object, key, field) != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(field);
    return -1;
  }
  return 0;
}

int turbo_agent_runtime_json_value_object_set_int64(
    json_value_t *object, const char *key, int64_t value) {
  json_value_t *field;

  if (!object || !key) {
    return -1;
  }
  field = turbo_json_create_int64(value);
  if (!field) {
    return -1;
  }
  if (turbo_runtime_json_object_set(object, key, field) != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(field);
    return -1;
  }
  return 0;
}

int turbo_agent_runtime_json_value_object_set_clone(
    json_value_t *object, const char *key,
    const json_value_t *value) {
  json_value_t *copy;

  if (!object || !key || !value) {
    return -1;
  }
  copy = turbo_json_clone(value);
  if (!copy) {
    return -1;
  }
  if (turbo_runtime_json_object_set(object, key, copy) != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(copy);
    return -1;
  }
  return 0;
}

int turbo_agent_runtime_json_value_object_set_optional_string(
    json_value_t *object, const char *key,
    const json_value_t *value) {
  const char *text = turbo_runtime_json_value_as_string(value);

  if (!text) {
    return 0;
  }
  return turbo_agent_runtime_json_value_object_set_string(object, key, text);
}

/* ================================================================== */
/* small utilities                                                    */
/* ================================================================== */

char *turbo_agent_runtime_strdup(const char *text) {
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

const char *turbo_agent_runtime_status_text(turbo_graph_exec_status_t status) {
  switch (status) {
  case TURBO_GRAPH_EXEC_OK:
  case TURBO_GRAPH_EXEC_STOP:
    return "completed";
  case TURBO_GRAPH_EXEC_INTERRUPTED:
    return "interrupted";
  default:
    return "failed";
  }
}

int turbo_agent_runtime_result_to_json(const turbo_graph_run_result_t *result,
                                       json_value_t **out_result) {
  json_value_t *json_result;

  if (!result || !out_result) {
    return -1;
  }

  json_result = turbo_json_create_object();
  if (!json_result) {
    return -1;
  }

  turbo_json_object_set_number(json_result, "status", (double)result->status);
  if (result->last_node) {
    turbo_json_object_set_string(json_result, "last_node", result->last_node);
  } else {
    turbo_json_object_set_null(json_result, "last_node");
  }
  if (result->next_node) {
    turbo_json_object_set_string(json_result, "next_node", result->next_node);
  } else {
    turbo_json_object_set_null(json_result, "next_node");
  }
  turbo_json_object_set_number(json_result, "steps", (double)result->steps);

  *out_result = json_result;
  return 0;
}

/* ================================================================== */
/* platform IO / ID generation                                        */
/* ================================================================== */

int turbo_agent_runtime_make_timestamp(char *buffer, size_t buffer_size) {
  time_t now;
  struct tm utc_tm;

  if (!buffer || buffer_size < 21) {
    return -1;
  }

  now = time(NULL);
#ifdef _WIN32
  if (gmtime_s(&utc_tm, &now) != 0) {
    return -1;
  }
#else
  if (!gmtime_r(&now, &utc_tm)) {
    return -1;
  }
#endif

  if (strftime(buffer, buffer_size, "%Y-%m-%dT%H:%M:%SZ", &utc_tm) == 0) {
    return -1;
  }

  return 0;
}

char *turbo_agent_runtime_make_id(const char *prefix) {
  unsigned long long counter;
  unsigned long long tick;
  int needed;
  char *buffer;

  if (!prefix) {
    return NULL;
  }

  counter = atomic_fetch_add(&turbo_agent_runtime_id_counter, 1);
  tick = (unsigned long long)turbo_hrtime();
  needed = snprintf(NULL, 0, "%s_%llx%llx", prefix, tick, counter);
  if (needed < 0) {
    return NULL;
  }
  buffer = (char *)malloc((size_t)needed + 1);
  if (!buffer) {
    return NULL;
  }
  snprintf(buffer, (size_t)needed + 1, "%s_%llx%llx", prefix, tick, counter);
  return buffer;
}

int turbo_agent_runtime_write_text_file(const char *path, const char *content) {
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

int turbo_agent_runtime_read_text_file(const char *path, char **out_content) {
  FILE *fp;
  long size;
  char *buffer;

  if (!path || !out_content) {
    return -1;
  }

  *out_content = NULL;
  fp = fopen(path, "rb");
  if (!fp) {
    return -1;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return -1;
  }
  size = ftell(fp);
  if (size < 0) {
    fclose(fp);
    return -1;
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return -1;
  }
  buffer = (char *)malloc((size_t)size + 1);
  if (!buffer) {
    fclose(fp);
    return -1;
  }
  if (size > 0 && fread(buffer, 1, (size_t)size, fp) != (size_t)size) {
    free(buffer);
    fclose(fp);
    return -1;
  }
  buffer[size] = '\0';
  fclose(fp);
  *out_content = buffer;
  return 0;
}

int turbo_agent_runtime_ensure_dir(const char *path) {
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

char *turbo_agent_runtime_join_path(const char *left, const char *right) {
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

char *turbo_agent_runtime_record_path(const char *root_dir, const char *collection,
                                      const char *id) {
  char *directory;
  char *filename;
  char *path;
  int needed;

  if (!root_dir || !collection || !id) {
    return NULL;
  }

  directory = turbo_agent_runtime_join_path(root_dir, collection);
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
  path = turbo_agent_runtime_join_path(directory, filename);
  free(filename);
  free(directory);
  return path;
}

/* ================================================================== */
/* JSON parse / filter helpers                                        */
/* ================================================================== */

int turbo_agent_runtime_parse_json_string(const char *json_text, json_value_t **out_json) {
  json_value_t *json_root = NULL;

  if (!json_text || !out_json) {
    return -1;
  }

  *out_json = NULL;
  if (turbo_parse_json((const uint8_t *)json_text, strlen(json_text), &json_root) != 0 ||
      !json_root) {
    turbo_free_json(&json_root);
    return -1;
  }
  *out_json = json_root;
  return 0;
}

int turbo_agent_runtime_json_matches_filter(const json_value_t *record,
                                            const char *filter_key,
                                            const char *filter_value) {
  const char *value;

  if (!filter_key || !filter_value) {
    return 1;
  }
  value = turbo_json_get_string(record, filter_key);
  return value && strcmp(value, filter_value) == 0;
}

/* ================================================================== */
/* store adapter: JSON layer over raw store vtable                    */
/* ================================================================== */

/* Collection name constants (owned here, externed in header). */
const char *const turbo_agent_runtime_threads_collection = "threads";
const char *const turbo_agent_runtime_runs_collection = "runs";
const char *const turbo_agent_runtime_checkpoints_collection = "checkpoints";

int turbo_agent_runtime_store_put_json(turbo_agent_runtime_t *runtime,
                                       const char *collection, const char *id,
                                       const json_value_t *record_json) {
  char *serialized;
  int rc;

  if (!runtime || !collection || !id || !record_json || !runtime->store.put) {
    return -1;
  }

  serialized = turbo_json_serialize(record_json, NULL);
  if (!serialized) {
    return -1;
  }
  rc = runtime->store.put(runtime->store.user_data, collection, id, serialized);
  turbo_json_serialize_free(serialized);
  return rc;
}

int turbo_agent_runtime_store_get_json(turbo_agent_runtime_t *runtime,
                                       const char *collection, const char *id,
                                       json_value_t **out_record_json) {
  char *serialized = NULL;
  int rc;

  if (!runtime || !collection || !id || !out_record_json || !runtime->store.get) {
    return -1;
  }

  *out_record_json = NULL;
  rc = runtime->store.get(runtime->store.user_data, collection, id, &serialized);
  if (rc != 0 || !serialized) {
    free(serialized);
    return -1;
  }
  rc = turbo_agent_runtime_parse_json_string(serialized, out_record_json);
  free(serialized);
  return rc;
}

int turbo_agent_runtime_store_list_json(turbo_agent_runtime_t *runtime,
                                        const char *collection,
                                        const char *filter_key,
                                        const char *filter_value,
                                        json_value_t **out_records_json) {
  char *serialized = NULL;
  int rc;

  if (!runtime || !collection || !out_records_json || !runtime->store.list) {
    return -1;
  }

  *out_records_json = NULL;
  rc = runtime->store.list(runtime->store.user_data, collection, filter_key, filter_value,
                           &serialized);
  if (rc != 0 || !serialized) {
    free(serialized);
    return -1;
  }
  rc = turbo_agent_runtime_parse_json_string(serialized, out_records_json);
  free(serialized);
  if (rc != 0 || !*out_records_json || turbo_json_type(*out_records_json) != TURBO_JSON_ARRAY) {
    turbo_free_json(out_records_json);
    return -1;
  }
  return 0;
}
