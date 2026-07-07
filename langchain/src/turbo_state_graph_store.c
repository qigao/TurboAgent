#ifdef TURBO_STATE_GRAPH_STORE_COMPILE_IMPL

#include "turbo_state_graph_store.h"

#include "turbo_parser.h"
#include "turbo_state_graph.h"

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

typedef struct turbo_state_graph_store_memory_record_s {
  char *snapshot_id;
  char *snapshot_json;
  struct turbo_state_graph_store_memory_record_s *next;
} turbo_state_graph_store_memory_record_t;

typedef struct {
  turbo_state_graph_store_memory_record_t *head;
} turbo_state_graph_store_memory_backend_t;

typedef struct {
  char *root_dir;
} turbo_state_graph_store_file_backend_t;

static char *turbo_state_graph_store_strdup(const char *text) {
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

static int turbo_state_graph_store_parse_json(const char *json_text, json_value_t **out_json) {
  json_value_t *json = NULL;

  if (!json_text || !out_json) {
    return -1;
  }
  if (turbo_parse_json((const uint8_t *)json_text, strlen(json_text), &json) != 0 || !json) {
    turbo_free_json(&json);
    return -1;
  }
  *out_json = json;
  return 0;
}

static int turbo_state_graph_store_count_runs_by_status(const json_value_t *runs,
                                                        const char *status_text) {
  size_t i;
  int count = 0;

  if (!runs || turbo_json_type(runs) != TURBO_JSON_ARRAY || !status_text) {
    return 0;
  }
  for (i = 0; i < turbo_json_array_size(runs); ++i) {
    const json_value_t *run = turbo_json_array_get(runs, i);
    if (run && turbo_json_type(run) == TURBO_JSON_OBJECT) {
      const char *status = turbo_json_get_string(run, "status");
      if (status && strcmp(status, status_text) == 0) {
        count += 1;
      }
    }
  }
  return count;
}

static int turbo_state_graph_store_descriptor_set_nullable_string(json_value_t *object,
                                                                  const char *key,
                                                                  const char *value) {
  if (!object || !key) {
    return -1;
  }
  turbo_json_object_add(object, key,
                        value ? turbo_json_create_string(value) : turbo_json_create_null());
  return 0;
}

static int turbo_state_graph_store_make_snapshot_descriptor(const char *snapshot_id,
                                                            const json_value_t *snapshot,
                                                            json_value_t **out_descriptor) {
  const json_value_t *topology = NULL;
  const json_value_t *runtime = NULL;
  const json_value_t *channels = NULL;
  const json_value_t *nodes = NULL;
  const json_value_t *edges = NULL;
  const json_value_t *threads = NULL;
  const json_value_t *runs = NULL;
  const json_value_t *history = NULL;
  json_value_t *descriptor = NULL;
  const char *graph_name = NULL;
  const char *entry_node = NULL;

  if (!snapshot_id || !snapshot || !out_descriptor ||
      turbo_json_type(snapshot) != TURBO_JSON_OBJECT) {
    return -1;
  }
  topology = turbo_json_object_get(snapshot, "topology");
  runtime = turbo_json_object_get(snapshot, "runtime");
  if (!topology || !runtime || turbo_json_type(topology) != TURBO_JSON_OBJECT ||
      turbo_json_type(runtime) != TURBO_JSON_OBJECT) {
    return -1;
  }

  channels = turbo_json_object_get(topology, "channels");
  nodes = turbo_json_object_get(topology, "nodes");
  edges = turbo_json_object_get(topology, "edges");
  threads = turbo_json_object_get(runtime, "threads");
  runs = turbo_json_object_get(runtime, "runs");
  history = turbo_json_object_get(runtime, "history");
  if (!channels || !nodes || !edges || !threads || !runs || !history ||
      turbo_json_type(channels) != TURBO_JSON_ARRAY ||
      turbo_json_type(nodes) != TURBO_JSON_ARRAY ||
      turbo_json_type(edges) != TURBO_JSON_ARRAY ||
      turbo_json_type(threads) != TURBO_JSON_ARRAY ||
      turbo_json_type(runs) != TURBO_JSON_ARRAY ||
      turbo_json_type(history) != TURBO_JSON_ARRAY) {
    return -1;
  }

  descriptor = turbo_json_create_object();
  if (!descriptor) {
    return -1;
  }
  graph_name = turbo_json_get_string(topology, "graph_name");
  entry_node = turbo_json_get_string(topology, "entry_node");
  turbo_json_object_set_string(descriptor, "snapshot_id", snapshot_id);
  turbo_json_object_set_number(
      descriptor, "snapshot_version",
      turbo_json_get_double(snapshot, "snapshot_version", 0));
  turbo_state_graph_store_descriptor_set_nullable_string(descriptor, "graph_name", graph_name);
  turbo_state_graph_store_descriptor_set_nullable_string(descriptor, "entry_node", entry_node);
  turbo_json_object_set_number(descriptor, "channel_count",
                               (double)turbo_json_array_size(channels));
  turbo_json_object_set_number(descriptor, "node_count",
                               (double)turbo_json_array_size(nodes));
  turbo_json_object_set_number(descriptor, "edge_count",
                               (double)turbo_json_array_size(edges));
  turbo_json_object_set_number(descriptor, "thread_count",
                               (double)turbo_json_array_size(threads));
  turbo_json_object_set_number(descriptor, "run_count",
                               (double)turbo_json_array_size(runs));
  turbo_json_object_set_number(descriptor, "history_count",
                               (double)turbo_json_array_size(history));
  turbo_json_object_set_number(descriptor, "checkpoint_count",
                               (double)turbo_json_array_size(history));
  turbo_json_object_set_number(descriptor, "next_thread_id",
                               turbo_json_get_double(runtime, "next_thread_id", 0));
  turbo_json_object_set_number(descriptor, "next_run_id",
                               turbo_json_get_double(runtime, "next_run_id", 0));
  turbo_json_object_set_number(descriptor, "next_history_id",
                               turbo_json_get_double(runtime, "next_history_id", 0));
  turbo_json_object_set_number(descriptor, "next_checkpoint_id",
                               turbo_json_get_double(runtime, "next_checkpoint_id", 0));
  turbo_json_object_set_number(
      descriptor, "interrupted_run_count",
      (double)turbo_state_graph_store_count_runs_by_status(runs, "interrupted"));
  turbo_json_object_set_number(
      descriptor, "completed_run_count",
      (double)turbo_state_graph_store_count_runs_by_status(runs, "completed"));

  *out_descriptor = descriptor;
  return 0;
}

static int turbo_state_graph_store_snapshot_array_has_string_field(
    const json_value_t *array, const char *field, const char *expected) {
  size_t i;

  if (!expected || !array || turbo_json_type(array) != TURBO_JSON_ARRAY) {
    return 0;
  }
  for (i = 0; i < turbo_json_array_size(array); ++i) {
    const json_value_t *entry = turbo_json_array_get(array, i);
    const char *value = entry && turbo_json_type(entry) == TURBO_JSON_OBJECT
                            ? turbo_json_get_string(entry, field)
                            : NULL;
    if (value && strcmp(value, expected) == 0) {
      return 1;
    }
  }
  return 0;
}

static int turbo_state_graph_store_snapshot_matches_options(
    const json_value_t *snapshot, const turbo_state_graph_store_list_options_t *options) {
  const json_value_t *topology = NULL;
  const json_value_t *runtime = NULL;
  const json_value_t *threads = NULL;
  const json_value_t *runs = NULL;
  const json_value_t *history = NULL;
  size_t i;

  if (!snapshot) {
    return 0;
  }
  if (!options) {
    return 1;
  }

  topology = turbo_json_object_get(snapshot, "topology");
  runtime = turbo_json_object_get(snapshot, "runtime");
  if (!topology || !runtime || turbo_json_type(topology) != TURBO_JSON_OBJECT ||
      turbo_json_type(runtime) != TURBO_JSON_OBJECT) {
    return 0;
  }

  if (options->graph_name) {
    const char *graph_name = turbo_json_get_string(topology, "graph_name");
    if (!graph_name || strcmp(graph_name, options->graph_name) != 0) {
      return 0;
    }
  }

  threads = turbo_json_object_get(runtime, "threads");
  runs = turbo_json_object_get(runtime, "runs");
  history = turbo_json_object_get(runtime, "history");
  if (!threads || !runs || !history || turbo_json_type(threads) != TURBO_JSON_ARRAY ||
      turbo_json_type(runs) != TURBO_JSON_ARRAY || turbo_json_type(history) != TURBO_JSON_ARRAY) {
    return 0;
  }

  if (options->thread_id &&
      !turbo_state_graph_store_snapshot_array_has_string_field(threads, "thread_id",
                                                               options->thread_id)) {
    return 0;
  }
  if (options->run_id &&
      !turbo_state_graph_store_snapshot_array_has_string_field(runs, "run_id",
                                                               options->run_id)) {
    return 0;
  }
  if (options->history_entry_id &&
      !turbo_state_graph_store_snapshot_array_has_string_field(history, "history_entry_id",
                                                               options->history_entry_id)) {
    return 0;
  }
  if (options->checkpoint_id &&
      !turbo_state_graph_store_snapshot_array_has_string_field(history, "checkpoint_id",
                                                               options->checkpoint_id)) {
    return 0;
  }
  if (options->run_status) {
    for (i = 0; i < turbo_json_array_size(runs); ++i) {
      const json_value_t *run = turbo_json_array_get(runs, i);
      const char *status = run && turbo_json_type(run) == TURBO_JSON_OBJECT
                               ? turbo_json_get_string(run, "status")
                               : NULL;
      if (status && strcmp(status, options->run_status) == 0) {
        return 1;
      }
    }
    return 0;
  }

  return 1;
}

static int turbo_state_graph_store_descriptor_is_newer(const json_value_t *candidate,
                                                       const json_value_t *current_best) {
  double candidate_history;
  double current_history;
  double candidate_checkpoint;
  double current_checkpoint;
  double candidate_run;
  double current_run;
  double candidate_thread;
  double current_thread;

  if (!candidate) {
    return 0;
  }
  if (!current_best) {
    return 1;
  }

  candidate_history = turbo_json_get_double(candidate, "next_history_id", 0);
  current_history = turbo_json_get_double(current_best, "next_history_id", 0);
  if (candidate_history != current_history) {
    return candidate_history > current_history;
  }

  candidate_checkpoint = turbo_json_get_double(candidate, "next_checkpoint_id", 0);
  current_checkpoint = turbo_json_get_double(current_best, "next_checkpoint_id", 0);
  if (candidate_checkpoint != current_checkpoint) {
    return candidate_checkpoint > current_checkpoint;
  }

  candidate_run = turbo_json_get_double(candidate, "next_run_id", 0);
  current_run = turbo_json_get_double(current_best, "next_run_id", 0);
  if (candidate_run != current_run) {
    return candidate_run > current_run;
  }

  candidate_thread = turbo_json_get_double(candidate, "next_thread_id", 0);
  current_thread = turbo_json_get_double(current_best, "next_thread_id", 0);
  if (candidate_thread != current_thread) {
    return candidate_thread > current_thread;
  }

  return strcmp(turbo_json_get_string(candidate, "snapshot_id"),
                turbo_json_get_string(current_best, "snapshot_id")) > 0;
}

static int turbo_state_graph_store_ensure_dir(const char *path) {
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

static char *turbo_state_graph_store_join_path(const char *left, const char *right) {
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

static int turbo_state_graph_store_write_text_file(const char *path, const char *content) {
  FILE *fp;
  size_t len;

  if (!path || !content) {
    return -1;
  }
  fp = fopen(path, "wb");
  if (!fp) {
    return -1;
  }
  len = strlen(content);
  if (len > 0 && fwrite(content, 1, len, fp) != len) {
    fclose(fp);
    return -1;
  }
  fclose(fp);
  return 0;
}

static int turbo_state_graph_store_read_text_file(const char *path, char **out_content) {
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

static char turbo_state_graph_store_hex_digit(unsigned value) {
  return (char)(value < 10 ? ('0' + value) : ('a' + (value - 10)));
}

static char *turbo_state_graph_store_hex_encode(const char *text) {
  size_t len;
  size_t i;
  char *encoded;

  if (!text) {
    return NULL;
  }
  len = strlen(text);
  encoded = (char *)malloc(len * 2 + 1);
  if (!encoded) {
    return NULL;
  }
  for (i = 0; i < len; ++i) {
    unsigned char ch = (unsigned char)text[i];
    encoded[i * 2] = turbo_state_graph_store_hex_digit((unsigned)(ch >> 4));
    encoded[i * 2 + 1] = turbo_state_graph_store_hex_digit((unsigned)(ch & 0x0f));
  }
  encoded[len * 2] = '\0';
  return encoded;
}

static int turbo_state_graph_store_hex_value(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

static char *turbo_state_graph_store_hex_decode(const char *text) {
  size_t len;
  size_t i;
  char *decoded;

  if (!text) {
    return NULL;
  }
  len = strlen(text);
  if ((len % 2) != 0) {
    return NULL;
  }
  decoded = (char *)malloc(len / 2 + 1);
  if (!decoded) {
    return NULL;
  }
  for (i = 0; i < len; i += 2) {
    int hi = turbo_state_graph_store_hex_value(text[i]);
    int lo = turbo_state_graph_store_hex_value(text[i + 1]);
    if (hi < 0 || lo < 0) {
      free(decoded);
      return NULL;
    }
    decoded[i / 2] = (char)((hi << 4) | lo);
  }
  decoded[len / 2] = '\0';
  return decoded;
}

static char *turbo_state_graph_store_file_snapshot_dir(const char *root_dir) {
  return turbo_state_graph_store_join_path(root_dir, "snapshots");
}

static char *turbo_state_graph_store_file_snapshot_path(const char *root_dir,
                                                        const char *snapshot_id) {
  char *snapshots_dir = NULL;
  char *encoded_id = NULL;
  char *filename = NULL;
  char *path = NULL;
  int needed;

  if (!root_dir || !snapshot_id) {
    return NULL;
  }
  snapshots_dir = turbo_state_graph_store_file_snapshot_dir(root_dir);
  encoded_id = turbo_state_graph_store_hex_encode(snapshot_id);
  if (!snapshots_dir || !encoded_id) {
    free(snapshots_dir);
    free(encoded_id);
    return NULL;
  }
  needed = snprintf(NULL, 0, "%s.json", encoded_id);
  if (needed >= 0) {
    filename = (char *)malloc((size_t)needed + 1);
    if (filename) {
      snprintf(filename, (size_t)needed + 1, "%s.json", encoded_id);
      path = turbo_state_graph_store_join_path(snapshots_dir, filename);
    }
  }
  free(filename);
  free(encoded_id);
  free(snapshots_dir);
  return path;
}

static void turbo_state_graph_store_memory_backend_destroy(void *user_data) {
  turbo_state_graph_store_memory_backend_t *backend =
      (turbo_state_graph_store_memory_backend_t *)user_data;
  turbo_state_graph_store_memory_record_t *record;

  if (!backend) {
    return;
  }
  record = backend->head;
  while (record) {
    turbo_state_graph_store_memory_record_t *next = record->next;
    free(record->snapshot_id);
    free(record->snapshot_json);
    free(record);
    record = next;
  }
  free(backend);
}

static turbo_state_graph_store_memory_record_t *turbo_state_graph_store_memory_find(
    turbo_state_graph_store_memory_backend_t *backend, const char *snapshot_id) {
  turbo_state_graph_store_memory_record_t *record;

  if (!backend || !snapshot_id) {
    return NULL;
  }
  for (record = backend->head; record; record = record->next) {
    if (strcmp(record->snapshot_id, snapshot_id) == 0) {
      return record;
    }
  }
  return NULL;
}

static int turbo_state_graph_store_memory_load(void *user_data, const char *snapshot_id,
                                               char **out_snapshot_json) {
  turbo_state_graph_store_memory_backend_t *backend =
      (turbo_state_graph_store_memory_backend_t *)user_data;
  turbo_state_graph_store_memory_record_t *record;

  if (!backend || !snapshot_id || !out_snapshot_json) {
    return -1;
  }
  *out_snapshot_json = NULL;
  record = turbo_state_graph_store_memory_find(backend, snapshot_id);
  if (!record) {
    return -1;
  }
  *out_snapshot_json = turbo_state_graph_store_strdup(record->snapshot_json);
  return *out_snapshot_json ? 0 : -1;
}

static int turbo_state_graph_store_memory_save(void *user_data, const char *snapshot_id,
                                               const char *snapshot_json) {
  turbo_state_graph_store_memory_backend_t *backend =
      (turbo_state_graph_store_memory_backend_t *)user_data;
  turbo_state_graph_store_memory_record_t *record;
  char *snapshot_copy;

  if (!backend || !snapshot_id || !snapshot_json) {
    return -1;
  }
  snapshot_copy = turbo_state_graph_store_strdup(snapshot_json);
  if (!snapshot_copy) {
    return -1;
  }
  record = turbo_state_graph_store_memory_find(backend, snapshot_id);
  if (record) {
    free(record->snapshot_json);
    record->snapshot_json = snapshot_copy;
    return 0;
  }
  record = (turbo_state_graph_store_memory_record_t *)calloc(1, sizeof(*record));
  if (!record) {
    free(snapshot_copy);
    return -1;
  }
  record->snapshot_id = turbo_state_graph_store_strdup(snapshot_id);
  record->snapshot_json = snapshot_copy;
  if (!record->snapshot_id) {
    free(record->snapshot_id);
    free(record->snapshot_json);
    free(record);
    return -1;
  }
  record->next = backend->head;
  backend->head = record;
  return 0;
}

static int turbo_state_graph_store_memory_list(void *user_data, char **out_snapshot_ids_json) {
  turbo_state_graph_store_memory_backend_t *backend =
      (turbo_state_graph_store_memory_backend_t *)user_data;
  turbo_state_graph_store_memory_record_t *record;
  json_value_t *array = NULL;
  char *serialized = NULL;

  if (!backend || !out_snapshot_ids_json) {
    return -1;
  }
  *out_snapshot_ids_json = NULL;
  array = turbo_json_create_array();
  if (!array) {
    return -1;
  }
  for (record = backend->head; record; record = record->next) {
    turbo_json_array_add(array, turbo_json_create_string(record->snapshot_id));
  }
  serialized = turbo_json_serialize(array, NULL);
  turbo_free_json(&array);
  if (!serialized) {
    return -1;
  }
  *out_snapshot_ids_json = serialized;
  return 0;
}

static int turbo_state_graph_store_memory_delete(void *user_data, const char *snapshot_id) {
  turbo_state_graph_store_memory_backend_t *backend =
      (turbo_state_graph_store_memory_backend_t *)user_data;
  turbo_state_graph_store_memory_record_t *record;
  turbo_state_graph_store_memory_record_t *prev = NULL;

  if (!backend || !snapshot_id) {
    return -1;
  }
  for (record = backend->head; record; record = record->next) {
    if (strcmp(record->snapshot_id, snapshot_id) == 0) {
      if (prev) {
        prev->next = record->next;
      } else {
        backend->head = record->next;
      }
      free(record->snapshot_id);
      free(record->snapshot_json);
      free(record);
      return 0;
    }
    prev = record;
  }
  return -1;
}

static void turbo_state_graph_store_file_backend_destroy(void *user_data) {
  turbo_state_graph_store_file_backend_t *backend =
      (turbo_state_graph_store_file_backend_t *)user_data;

  if (!backend) {
    return;
  }
  free(backend->root_dir);
  free(backend);
}

static int turbo_state_graph_store_file_load(void *user_data, const char *snapshot_id,
                                             char **out_snapshot_json) {
  turbo_state_graph_store_file_backend_t *backend =
      (turbo_state_graph_store_file_backend_t *)user_data;
  char *path;
  int rc;

  if (!backend || !backend->root_dir || !snapshot_id || !out_snapshot_json) {
    return -1;
  }
  path = turbo_state_graph_store_file_snapshot_path(backend->root_dir, snapshot_id);
  if (!path) {
    return -1;
  }
  rc = turbo_state_graph_store_read_text_file(path, out_snapshot_json);
  free(path);
  return rc;
}

static int turbo_state_graph_store_file_save(void *user_data, const char *snapshot_id,
                                             const char *snapshot_json) {
  turbo_state_graph_store_file_backend_t *backend =
      (turbo_state_graph_store_file_backend_t *)user_data;
  char *path;
  int rc;

  if (!backend || !backend->root_dir || !snapshot_id || !snapshot_json) {
    return -1;
  }
  path = turbo_state_graph_store_file_snapshot_path(backend->root_dir, snapshot_id);
  if (!path) {
    return -1;
  }
  rc = turbo_state_graph_store_write_text_file(path, snapshot_json);
  free(path);
  return rc;
}

static int turbo_state_graph_store_file_list_append(const char *file_name, json_value_t *array) {
  size_t len;
  char *encoded_id = NULL;
  char *snapshot_id = NULL;
  int rc = -1;

  if (!file_name || !array) {
    return -1;
  }
  len = strlen(file_name);
  if (len < 6 || strcmp(file_name + len - 5, ".json") != 0) {
    return 0;
  }
  encoded_id = (char *)malloc(len - 4);
  if (!encoded_id) {
    return -1;
  }
  memcpy(encoded_id, file_name, len - 5);
  encoded_id[len - 5] = '\0';
  snapshot_id = turbo_state_graph_store_hex_decode(encoded_id);
  if (snapshot_id) {
    turbo_json_array_add(array, turbo_json_create_string(snapshot_id));
    rc = 0;
  }
  free(snapshot_id);
  free(encoded_id);
  return rc;
}

static int turbo_state_graph_store_file_list(void *user_data, char **out_snapshot_ids_json) {
  turbo_state_graph_store_file_backend_t *backend =
      (turbo_state_graph_store_file_backend_t *)user_data;
  char *snapshots_dir = NULL;
  json_value_t *array = NULL;
  char *serialized = NULL;
  int rc = 0;

  if (!backend || !backend->root_dir || !out_snapshot_ids_json) {
    return -1;
  }
  *out_snapshot_ids_json = NULL;
  snapshots_dir = turbo_state_graph_store_file_snapshot_dir(backend->root_dir);
  array = turbo_json_create_array();
  if (!snapshots_dir || !array) {
    free(snapshots_dir);
    turbo_free_json(&array);
    return -1;
  }

#ifdef _WIN32
  {
    WIN32_FIND_DATAA find_data;
    HANDLE handle;
    char *pattern = turbo_state_graph_store_join_path(snapshots_dir, "*.json");

    if (!pattern) {
      free(snapshots_dir);
      turbo_free_json(&array);
      return -1;
    }
    handle = FindFirstFileA(pattern, &find_data);
    free(pattern);
    if (handle != INVALID_HANDLE_VALUE) {
      do {
        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          continue;
        }
        if (turbo_state_graph_store_file_list_append(find_data.cFileName, array) != 0) {
          rc = -1;
          break;
        }
      } while (FindNextFileA(handle, &find_data));
      FindClose(handle);
    }
  }
#else
  {
    DIR *dir = opendir(snapshots_dir);
    struct dirent *entry;

    if (!dir) {
      free(snapshots_dir);
      turbo_free_json(&array);
      return -1;
    }
    while ((entry = readdir(dir)) != NULL) {
      if (turbo_state_graph_store_file_list_append(entry->d_name, array) != 0) {
        rc = -1;
        break;
      }
    }
    closedir(dir);
  }
#endif

  free(snapshots_dir);
  if (rc != 0) {
    turbo_free_json(&array);
    return -1;
  }
  serialized = turbo_json_serialize(array, NULL);
  turbo_free_json(&array);
  if (!serialized) {
    return -1;
  }
  *out_snapshot_ids_json = serialized;
  return 0;
}

static int turbo_state_graph_store_file_delete(void *user_data, const char *snapshot_id) {
  turbo_state_graph_store_file_backend_t *backend =
      (turbo_state_graph_store_file_backend_t *)user_data;
  char *path;
  int rc;

  if (!backend || !backend->root_dir || !snapshot_id) {
    return -1;
  }
  path = turbo_state_graph_store_file_snapshot_path(backend->root_dir, snapshot_id);
  if (!path) {
    return -1;
  }
  rc = remove(path);
  free(path);
  return rc == 0 ? 0 : -1;
}

CXX_C_API turbo_state_graph_store_t turbo_state_graph_store_memory_create(void) {
  turbo_state_graph_store_t store = {0};
  turbo_state_graph_store_memory_backend_t *backend =
      (turbo_state_graph_store_memory_backend_t *)calloc(1, sizeof(*backend));

  if (!backend) {
    return store;
  }
  store.load = turbo_state_graph_store_memory_load;
  store.save = turbo_state_graph_store_memory_save;
  store.list = turbo_state_graph_store_memory_list;
  store.remove = turbo_state_graph_store_memory_delete;
  store.user_data = backend;
  store.user_data_free = turbo_state_graph_store_memory_backend_destroy;
  return store;
}

CXX_C_API turbo_state_graph_store_t turbo_state_graph_store_file_create(const char *root_dir) {
  turbo_state_graph_store_t store = {0};
  turbo_state_graph_store_file_backend_t *backend = NULL;
  char *snapshots_dir = NULL;

  if (!root_dir) {
    return store;
  }
  backend = (turbo_state_graph_store_file_backend_t *)calloc(1, sizeof(*backend));
  if (!backend) {
    return store;
  }
  backend->root_dir = turbo_state_graph_store_strdup(root_dir);
  snapshots_dir = turbo_state_graph_store_file_snapshot_dir(root_dir);
  if (!backend->root_dir || !snapshots_dir ||
      turbo_state_graph_store_ensure_dir(root_dir) != 0 ||
      turbo_state_graph_store_ensure_dir(snapshots_dir) != 0) {
    free(snapshots_dir);
    turbo_state_graph_store_file_backend_destroy(backend);
    return store;
  }
  free(snapshots_dir);
  store.load = turbo_state_graph_store_file_load;
  store.save = turbo_state_graph_store_file_save;
  store.list = turbo_state_graph_store_file_list;
  store.remove = turbo_state_graph_store_file_delete;
  store.user_data = backend;
  store.user_data_free = turbo_state_graph_store_file_backend_destroy;
  return store;
}

CXX_C_API void turbo_state_graph_store_destroy(turbo_state_graph_store_t *store) {
  if (!store) {
    return;
  }
  if (store->user_data_free && store->user_data) {
    store->user_data_free(store->user_data);
  }
  memset(store, 0, sizeof(*store));
}

CXX_C_API int turbo_state_graph_store_delete(const turbo_state_graph_store_t *store,
                                             const char *snapshot_id) {
  if (!store || !store->remove || !snapshot_id) {
    return -1;
  }
  return store->remove(store->user_data, snapshot_id);
}

CXX_C_API int turbo_state_graph_store_list(const turbo_state_graph_store_t *store,
                                           json_value_t **out_snapshot_ids_json) {
  char *serialized = NULL;
  json_value_t *json = NULL;
  int rc;

  if (!store || !store->list || !out_snapshot_ids_json) {
    return -1;
  }
  *out_snapshot_ids_json = NULL;
  rc = store->list(store->user_data, &serialized);
  if (rc != 0 || !serialized) {
    turbo_json_serialize_free(serialized);
    return -1;
  }
  rc = turbo_state_graph_store_parse_json(serialized, &json);
  turbo_json_serialize_free(serialized);
  if (rc != 0) {
    turbo_free_json(&json);
    return -1;
  }
  *out_snapshot_ids_json = json;
  return 0;
}

CXX_C_API int turbo_state_graph_store_get_snapshot_descriptor(
    const turbo_state_graph_store_t *store, const char *snapshot_id,
    json_value_t **out_descriptor_json) {
  char *snapshot_json = NULL;
  json_value_t *snapshot = NULL;
  json_value_t *descriptor = NULL;
  int rc;

  if (!store || !store->load || !snapshot_id || !out_descriptor_json) {
    return -1;
  }
  *out_descriptor_json = NULL;
  if (store->load(store->user_data, snapshot_id, &snapshot_json) != 0 || !snapshot_json) {
    free(snapshot_json);
    return -1;
  }
  rc = turbo_state_graph_store_parse_json(snapshot_json, &snapshot);
  free(snapshot_json);
  if (rc != 0) {
    turbo_free_json(&snapshot);
    return -1;
  }
  rc = turbo_state_graph_store_make_snapshot_descriptor(snapshot_id, snapshot, &descriptor);
  turbo_free_json(&snapshot);
  if (rc != 0) {
    turbo_free_json(&descriptor);
    return -1;
  }
  *out_descriptor_json = descriptor;
  return 0;
}

CXX_C_API int turbo_state_graph_store_list_snapshot_descriptors(
    const turbo_state_graph_store_t *store, json_value_t **out_descriptors_json) {
  json_value_t *snapshot_ids = NULL;
  json_value_t *descriptors = NULL;
  size_t i;

  if (!store || !out_descriptors_json) {
    return -1;
  }
  *out_descriptors_json = NULL;
  if (turbo_state_graph_store_list(store, &snapshot_ids) != 0) {
    return -1;
  }
  descriptors = turbo_json_create_array();
  if (!descriptors) {
    turbo_free_json(&snapshot_ids);
    return -1;
  }
  for (i = 0; i < turbo_json_array_size(snapshot_ids); ++i) {
    const json_value_t *entry = turbo_json_array_get(snapshot_ids, i);
    json_value_t *descriptor = NULL;
    const char *snapshot_id = entry && turbo_json_type(entry) == TURBO_JSON_STRING
                                  ? turbo_json_string(entry)
                                  : NULL;
    if (!snapshot_id ||
        turbo_state_graph_store_get_snapshot_descriptor(store, snapshot_id, &descriptor) != 0) {
      turbo_free_json(&descriptors);
      turbo_free_json(&snapshot_ids);
      return -1;
    }
    turbo_json_array_add(descriptors, descriptor);
  }
  turbo_free_json(&snapshot_ids);
  *out_descriptors_json = descriptors;
  return 0;
}

CXX_C_API int turbo_state_graph_store_list_snapshot_descriptors_filtered(
    const turbo_state_graph_store_t *store,
    const turbo_state_graph_store_list_options_t *options,
    json_value_t **out_descriptors_json) {
  json_value_t *snapshot_ids = NULL;
  json_value_t *descriptors = NULL;
  size_t i;
  size_t emitted = 0;

  if (!store || !out_descriptors_json) {
    return -1;
  }
  *out_descriptors_json = NULL;
  if (turbo_state_graph_store_list(store, &snapshot_ids) != 0) {
    return -1;
  }
  descriptors = turbo_json_create_array();
  if (!descriptors) {
    turbo_free_json(&snapshot_ids);
    return -1;
  }

  for (i = 0; i < turbo_json_array_size(snapshot_ids); ++i) {
    const json_value_t *entry = turbo_json_array_get(snapshot_ids, i);
    const char *snapshot_id = entry && turbo_json_type(entry) == TURBO_JSON_STRING
                                  ? turbo_json_string(entry)
                                  : NULL;
    char *snapshot_json = NULL;
    json_value_t *snapshot = NULL;
    json_value_t *descriptor = NULL;

    if (!snapshot_id) {
      turbo_free_json(&descriptors);
      turbo_free_json(&snapshot_ids);
      return -1;
    }
    if (options && options->limit > 0 && emitted >= options->limit) {
      break;
    }
    if (store->load(store->user_data, snapshot_id, &snapshot_json) != 0 || !snapshot_json) {
      free(snapshot_json);
      turbo_free_json(&descriptors);
      turbo_free_json(&snapshot_ids);
      return -1;
    }
    if (turbo_state_graph_store_parse_json(snapshot_json, &snapshot) != 0) {
      free(snapshot_json);
      turbo_free_json(&snapshot);
      turbo_free_json(&descriptors);
      turbo_free_json(&snapshot_ids);
      return -1;
    }
    free(snapshot_json);
    if (!turbo_state_graph_store_snapshot_matches_options(snapshot, options)) {
      turbo_free_json(&snapshot);
      continue;
    }
    if (turbo_state_graph_store_make_snapshot_descriptor(snapshot_id, snapshot, &descriptor) !=
        0) {
      turbo_free_json(&descriptor);
      turbo_free_json(&snapshot);
      turbo_free_json(&descriptors);
      turbo_free_json(&snapshot_ids);
      return -1;
    }
    turbo_json_array_add(descriptors, descriptor);
    turbo_free_json(&snapshot);
    emitted += 1;
  }

  turbo_free_json(&snapshot_ids);
  *out_descriptors_json = descriptors;
  return 0;
}

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_descriptor(
    const turbo_state_graph_store_t *store,
    const turbo_state_graph_store_list_options_t *options,
    json_value_t **out_descriptor_json) {
  json_value_t *descriptors = NULL;
  json_value_t *best = NULL;
  size_t i;

  if (!store || !out_descriptor_json) {
    return -1;
  }
  *out_descriptor_json = NULL;
  if (turbo_state_graph_store_list_snapshot_descriptors_filtered(store, options, &descriptors) !=
      0) {
    return -1;
  }

  for (i = 0; i < turbo_json_array_size(descriptors); ++i) {
    const json_value_t *entry = turbo_json_array_get(descriptors, i);
    if (entry && turbo_json_type(entry) == TURBO_JSON_OBJECT &&
        turbo_state_graph_store_descriptor_is_newer(entry, best)) {
      turbo_free_json(&best);
      best = turbo_json_clone(entry);
      if (!best) {
        turbo_free_json(&descriptors);
        return -1;
      }
    }
  }

  turbo_free_json(&descriptors);
  if (!best) {
    return -1;
  }
  *out_descriptor_json = best;
  return 0;
}

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_descriptor_for_thread(
    const turbo_state_graph_store_t *store, const char *thread_id,
    json_value_t **out_descriptor_json) {
  turbo_state_graph_store_list_options_t options = {0};

  if (!thread_id) {
    return -1;
  }
  options.thread_id = thread_id;
  return turbo_state_graph_store_get_latest_snapshot_descriptor(store, &options,
                                                               out_descriptor_json);
}

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_descriptor_for_run(
    const turbo_state_graph_store_t *store, const char *run_id,
    json_value_t **out_descriptor_json) {
  turbo_state_graph_store_list_options_t options = {0};

  if (!run_id) {
    return -1;
  }
  options.run_id = run_id;
  return turbo_state_graph_store_get_latest_snapshot_descriptor(store, &options,
                                                               out_descriptor_json);
}

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_descriptor_for_history_entry(
    const turbo_state_graph_store_t *store, const char *history_entry_id,
    json_value_t **out_descriptor_json) {
  turbo_state_graph_store_list_options_t options = {0};

  if (!history_entry_id) {
    return -1;
  }
  options.history_entry_id = history_entry_id;
  return turbo_state_graph_store_get_latest_snapshot_descriptor(store, &options,
                                                               out_descriptor_json);
}

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_descriptor_for_checkpoint(
    const turbo_state_graph_store_t *store, const char *checkpoint_id,
    json_value_t **out_descriptor_json) {
  turbo_state_graph_store_list_options_t options = {0};

  if (!checkpoint_id) {
    return -1;
  }
  options.checkpoint_id = checkpoint_id;
  return turbo_state_graph_store_get_latest_snapshot_descriptor(store, &options,
                                                               out_descriptor_json);
}

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_id(
    const turbo_state_graph_store_t *store,
    const turbo_state_graph_store_list_options_t *options, char **out_snapshot_id) {
  json_value_t *descriptor = NULL;
  const char *snapshot_id = NULL;
  char *owned_snapshot_id = NULL;

  if (!store || !out_snapshot_id) {
    return -1;
  }
  *out_snapshot_id = NULL;
  if (turbo_state_graph_store_get_latest_snapshot_descriptor(store, options, &descriptor) != 0) {
    return -1;
  }
  snapshot_id = turbo_json_get_string(descriptor, "snapshot_id");
  if (!snapshot_id || snapshot_id[0] == '\0') {
    turbo_free_json(&descriptor);
    return -1;
  }
  owned_snapshot_id = turbo_state_graph_store_strdup(snapshot_id);
  turbo_free_json(&descriptor);
  if (!owned_snapshot_id) {
    return -1;
  }
  *out_snapshot_id = owned_snapshot_id;
  return 0;
}

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_id_for_thread(
    const turbo_state_graph_store_t *store, const char *thread_id,
    char **out_snapshot_id) {
  turbo_state_graph_store_list_options_t options = {0};

  if (!thread_id) {
    return -1;
  }
  options.thread_id = thread_id;
  return turbo_state_graph_store_get_latest_snapshot_id(store, &options, out_snapshot_id);
}

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_id_for_run(
    const turbo_state_graph_store_t *store, const char *run_id, char **out_snapshot_id) {
  turbo_state_graph_store_list_options_t options = {0};

  if (!run_id) {
    return -1;
  }
  options.run_id = run_id;
  return turbo_state_graph_store_get_latest_snapshot_id(store, &options, out_snapshot_id);
}

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_id_for_history_entry(
    const turbo_state_graph_store_t *store, const char *history_entry_id,
    char **out_snapshot_id) {
  turbo_state_graph_store_list_options_t options = {0};

  if (!history_entry_id) {
    return -1;
  }
  options.history_entry_id = history_entry_id;
  return turbo_state_graph_store_get_latest_snapshot_id(store, &options, out_snapshot_id);
}

CXX_C_API int turbo_state_graph_store_get_latest_snapshot_id_for_checkpoint(
    const turbo_state_graph_store_t *store, const char *checkpoint_id,
    char **out_snapshot_id) {
  turbo_state_graph_store_list_options_t options = {0};

  if (!checkpoint_id) {
    return -1;
  }
  options.checkpoint_id = checkpoint_id;
  return turbo_state_graph_store_get_latest_snapshot_id(store, &options, out_snapshot_id);
}

CXX_C_API turbo_state_graph_status_t turbo_state_graph_store_load_latest_snapshot(
    const turbo_state_graph_store_t *store,
    const turbo_state_graph_store_list_options_t *options,
    turbo_state_graph_t *graph) {
  json_value_t *descriptor = NULL;
  const char *snapshot_id = NULL;
  turbo_state_graph_status_t status;

  if (!store || !graph) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  if (turbo_state_graph_store_get_latest_snapshot_descriptor(store, options, &descriptor) != 0) {
    return TURBO_STATE_GRAPH_ERROR;
  }
  snapshot_id = turbo_json_get_string(descriptor, "snapshot_id");
  if (!snapshot_id) {
    turbo_free_json(&descriptor);
    return TURBO_STATE_GRAPH_ERROR;
  }
  status = turbo_state_graph_store_load_snapshot(store, snapshot_id, graph);
  turbo_free_json(&descriptor);
  return status;
}

CXX_C_API turbo_state_graph_status_t turbo_state_graph_store_load_latest_snapshot_for_thread(
    const turbo_state_graph_store_t *store, const char *thread_id,
    turbo_state_graph_t *graph) {
  turbo_state_graph_store_list_options_t options = {0};

  if (!thread_id) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  options.thread_id = thread_id;
  return turbo_state_graph_store_load_latest_snapshot(store, &options, graph);
}

CXX_C_API turbo_state_graph_status_t turbo_state_graph_store_load_latest_snapshot_for_run(
    const turbo_state_graph_store_t *store, const char *run_id,
    turbo_state_graph_t *graph) {
  turbo_state_graph_store_list_options_t options = {0};

  if (!run_id) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  options.run_id = run_id;
  return turbo_state_graph_store_load_latest_snapshot(store, &options, graph);
}

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_store_load_latest_snapshot_for_history_entry(
    const turbo_state_graph_store_t *store, const char *history_entry_id,
    turbo_state_graph_t *graph) {
  turbo_state_graph_store_list_options_t options = {0};

  if (!history_entry_id) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  options.history_entry_id = history_entry_id;
  return turbo_state_graph_store_load_latest_snapshot(store, &options, graph);
}

CXX_C_API turbo_state_graph_status_t
turbo_state_graph_store_load_latest_snapshot_for_checkpoint(
    const turbo_state_graph_store_t *store, const char *checkpoint_id,
    turbo_state_graph_t *graph) {
  turbo_state_graph_store_list_options_t options = {0};

  if (!checkpoint_id) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  options.checkpoint_id = checkpoint_id;
  return turbo_state_graph_store_load_latest_snapshot(store, &options, graph);
}

CXX_C_API turbo_state_graph_status_t turbo_state_graph_store_save_snapshot(
    const turbo_state_graph_store_t *store, const char *snapshot_id,
    const turbo_state_graph_t *graph) {
  char *snapshot_json = NULL;
  turbo_state_graph_status_t status = TURBO_STATE_GRAPH_OK;

  if (!store || !store->save || !snapshot_id || !graph) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  snapshot_json = turbo_state_graph_serialize_snapshot(graph, NULL);
  if (!snapshot_json) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }
  if (store->save(store->user_data, snapshot_id, snapshot_json) != 0) {
    status = TURBO_STATE_GRAPH_ERROR;
  }
  turbo_json_serialize_free(snapshot_json);
  return status;
}

CXX_C_API turbo_state_graph_status_t turbo_state_graph_store_load_snapshot(
    const turbo_state_graph_store_t *store, const char *snapshot_id,
    turbo_state_graph_t *graph) {
  char *snapshot_json = NULL;
  turbo_state_graph_status_t status;

  if (!store || !store->load || !snapshot_id || !graph) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  if (store->load(store->user_data, snapshot_id, &snapshot_json) != 0 || !snapshot_json) {
    free(snapshot_json);
    return TURBO_STATE_GRAPH_ERROR;
  }
  status = turbo_state_graph_load_snapshot(graph, snapshot_json, strlen(snapshot_json));
  free(snapshot_json);
  return status;
}

#endif
