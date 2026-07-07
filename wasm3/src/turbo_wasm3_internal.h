#ifndef TURBO_WASM3_INTERNAL_H
#define TURBO_WASM3_INTERNAL_H

#include "turbo_wasm3.h"

#include "CoroNet/turbo_coro_socket.h"

#if defined(_MSC_VER)
#define _Static_assert(...)
#define __attribute__(...)
#define _Noreturn
#endif

#include "extra/wasi_core.h"
#include "http_client.h"
#include "redis_client.h"
#include "turbo_error.h"
#include "turbo_fs.h"
#include "turbo_str.h"
#include "sqlite3.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define TURBO_WASM3_DEFAULT_MAX_SOCKETS 1024u
#define TURBO_WASM3_DEFAULT_MAX_DB_HANDLES 64u
#define TURBO_WASM3_DEFAULT_MAX_DB_STATEMENTS 512u
#define TURBO_WASM3_DEFAULT_MAX_HTTP_CLIENTS 64u
#define TURBO_WASM3_DEFAULT_MAX_HTTP_RESPONSES 512u
#define TURBO_WASM3_DEFAULT_MAX_HTTP_CHUNKS_PER_RESPONSE 1024u
#define TURBO_WASM3_DEFAULT_MAX_HTTP_STREAM_BYTES (16u * 1024u * 1024u)
#define TURBO_WASM3_DEFAULT_MAX_HTTP_HEADER_BYTES (64u * 1024u)
#define TURBO_WASM3_DEFAULT_MAX_HTTP_HEADER_LINES 256u
#define TURBO_WASM3_DEFAULT_MAX_REDIS_CLIENTS 64u
#define TURBO_WASM3_DEFAULT_MAX_REDIS_REPLIES 512u
#define TURBO_WASM3_DEFAULT_MAX_PARSER_DOCS 256u
#define TURBO_WASM3_NO_LIMIT ((size_t)-1)

typedef struct turbo_wasm3_socket_entry_s {
  uint32_t handle;
  coro_socket_t *socket;
} turbo_wasm3_socket_entry_t;

typedef struct turbo_wasm3_blob_s {
  uint8_t *bytes;
  uint32_t size;
} turbo_wasm3_blob_t;

typedef struct turbo_wasm3_db_entry_s {
  uint32_t handle;
  void *db;
  char *last_error;
} turbo_wasm3_db_entry_t;

typedef struct turbo_wasm3_db_stmt_entry_s {
  uint32_t handle;
  uint32_t db_handle;
  void *stmt;
  char *last_error;
} turbo_wasm3_db_stmt_entry_t;

typedef struct turbo_wasm3_http_client_entry_s {
  uint32_t handle;
  http_client_t *client;
  char *last_error;
} turbo_wasm3_http_client_entry_t;

typedef struct turbo_wasm3_http_response_entry_s {
  uint32_t handle;
  uint32_t client_handle;
  http_response_t *response;
  char *last_error;
  turbo_wasm3_blob_t *chunks;
  size_t chunk_count;
  size_t chunk_capacity;
  size_t chunk_bytes;
  int is_stream;
  int is_sse;
} turbo_wasm3_http_response_entry_t;

typedef struct turbo_wasm3_redis_client_entry_s {
  uint32_t handle;
  coro_context_t *ctx;
  redis_client_t *client;
  char *last_error;
} turbo_wasm3_redis_client_entry_t;

typedef struct turbo_wasm3_redis_reply_entry_s {
  uint32_t handle;
  uint32_t client_handle;
  redis_reply_t *reply;
} turbo_wasm3_redis_reply_entry_t;

typedef struct turbo_wasm3_host_linker_entry_s {
  turbo_wasm3_host_linker_fn linker;
  void *user_data;
} turbo_wasm3_host_linker_entry_t;

typedef struct turbo_wasm3_parser_entry_s {
  uint32_t handle;
  int type;
  void *doc;
} turbo_wasm3_parser_entry_t;

struct turbo_wasm3_socket_registry_s {
  uint32_t next_handle;
  size_t capacity;
  size_t count;
  size_t max_entries;
  turbo_wasm3_socket_entry_t *entries;
};

struct turbo_wasm3_db_registry_s {
  uint32_t next_handle;
  uint32_t next_stmt_handle;
  size_t capacity;
  size_t count;
  size_t max_entries;
  turbo_wasm3_db_entry_t *entries;
  size_t stmt_capacity;
  size_t stmt_count;
  size_t max_statements;
  turbo_wasm3_db_stmt_entry_t *stmt_entries;
  const turbo_wasm3_db_ops_t *ops;
  void *user_data;
  char *sqlite_base_dir;
};

struct turbo_wasm3_http_registry_s {
  uint32_t next_client_handle;
  uint32_t next_response_handle;
  size_t client_capacity;
  size_t client_count;
  size_t max_clients;
  turbo_wasm3_http_client_entry_t *clients;
  size_t response_capacity;
  size_t response_count;
  size_t max_responses;
  size_t max_chunks_per_response;
  size_t max_stream_bytes;
  turbo_wasm3_http_response_entry_t *responses;
};

struct turbo_wasm3_redis_registry_s {
  uint32_t next_client_handle;
  uint32_t next_reply_handle;
  size_t client_capacity;
  size_t client_count;
  size_t max_clients;
  turbo_wasm3_redis_client_entry_t *clients;
  size_t reply_capacity;
  size_t reply_count;
  size_t max_replies;
  turbo_wasm3_redis_reply_entry_t *replies;
};

struct turbo_wasm3_parser_registry_s {
  uint32_t next_handle;
  size_t capacity;
  size_t count;
  size_t max_entries;
  turbo_wasm3_parser_entry_t *entries;
};

struct turbo_wasm3_vm_s {
  IM3Environment env;
  IM3Runtime runtime;
  m3_wasi_context_t *wasi_context;
  turbo_wasm3_socket_registry_t *socket_registry;
  turbo_wasm3_db_registry_t *db_registry;
  turbo_wasm3_http_registry_t *http_registry;
  turbo_wasm3_redis_registry_t *redis_registry;
  turbo_wasm3_parser_registry_t *parser_registry;
  void *host_user_data;
  char **wasi_argv;
  uint32_t wasi_argc;
  turbo_wasm3_blob_t *blobs;
  size_t blob_count;
  size_t blob_capacity;
  turbo_wasm3_host_linker_entry_t *host_linkers;
  size_t host_linker_count;
  size_t host_linker_capacity;
};

typedef struct turbo_wasm3_redis_command_capture_s {
  redis_reply_t *reply;
} turbo_wasm3_redis_command_capture_t;

typedef struct turbo_wasm3_redis_open_task_state_s {
  const char *host;
  uint16_t port;
  coro_socket_t *socket;
  int rc;
  int done;
} turbo_wasm3_redis_open_task_state_t;

typedef struct turbo_wasm3_redis_command_task_state_s {
  turbo_wasm3_redis_client_entry_t *entry;
  uint32_t argc;
  const char *const *argv;
  const uint32_t *argv_lens;
  redis_reply_t *reply;
  int rc;
  int done;
} turbo_wasm3_redis_command_task_state_t;

static char *turbo_wasm3_strdup(const char *value) {
  size_t len;
  char *copy;

  if (!value) {
    return NULL;
  }

  len = strlen(value) + 1;
  copy = (char *)malloc(len);
  if (!copy) {
    return NULL;
  }

  memcpy(copy, value, len);
  return copy;
}

static int turbo_wasm3_checked_capacity(size_t current_capacity,
                                        size_t required,
                                        size_t minimum_capacity,
                                        size_t max_capacity,
                                        size_t element_size,
                                        size_t *out_capacity) {
  size_t capacity;

  if (!out_capacity || element_size == 0) {
    return TURBO_EINVAL;
  }
  if (required > max_capacity) {
    return TURBO_ENOBUFS;
  }
  if (required > SIZE_MAX / element_size) {
    return TURBO_ENOMEM;
  }

  capacity = current_capacity ? current_capacity : minimum_capacity;
  if (capacity == 0) {
    capacity = 1;
  }
  if (capacity > max_capacity) {
    capacity = max_capacity;
  }

  while (capacity < required) {
    if (capacity > max_capacity / 2) {
      capacity = max_capacity;
      break;
    }
    capacity *= 2;
  }
  if (capacity < required || capacity > SIZE_MAX / element_size) {
    return TURBO_ENOMEM;
  }

  *out_capacity = capacity;
  return 0;
}

static void turbo_wasm3_vm_clear_wasi_args(turbo_wasm3_vm_t *vm) {
  uint32_t i;

  if (!vm) {
    return;
  }

  if (vm->wasi_argv) {
    for (i = 0; i < vm->wasi_argc; ++i) {
      free(vm->wasi_argv[i]);
    }
    free(vm->wasi_argv);
  }

  vm->wasi_argv = NULL;
  vm->wasi_argc = 0;

  if (vm->wasi_context) {
    vm->wasi_context->argc = 0;
    vm->wasi_context->argv = NULL;
  }
}

static uint16_t turbo_wasm3_error_to_wasi(int err) {
  switch (err) {
  case 0: return __WASI_ERRNO_SUCCESS;
  case TURBO_EBADF: return __WASI_ERRNO_BADF;
  case TURBO_ECONNABORTED: return __WASI_ERRNO_CONNABORTED;
  case TURBO_ECONNREFUSED: return __WASI_ERRNO_CONNREFUSED;
  case TURBO_ECONNRESET: return __WASI_ERRNO_CONNRESET;
  case TURBO_EINTR: return __WASI_ERRNO_INTR;
  case TURBO_EINVAL: return __WASI_ERRNO_INVAL;
  case TURBO_EIO: return __WASI_ERRNO_IO;
  case TURBO_EISCONN: return __WASI_ERRNO_ISCONN;
  case TURBO_EMSGSIZE: return __WASI_ERRNO_MSGSIZE;
  case TURBO_ENOBUFS: return __WASI_ERRNO_NOBUFS;
  case TURBO_ENOMEM: return __WASI_ERRNO_NOMEM;
  case TURBO_ENOSYS: return __WASI_ERRNO_NOSYS;
  case TURBO_ENOTCONN: return __WASI_ERRNO_NOTCONN;
  case TURBO_ENOTSOCK: return __WASI_ERRNO_NOTSOCK;
  case TURBO_ENOTSUP: return __WASI_ERRNO_NOTSUP;
  case TURBO_EPIPE: return __WASI_ERRNO_PIPE;
  case TURBO_ESHUTDOWN: return __WASI_ERRNO_NOTCONN;
  case TURBO_ETIMEDOUT: return __WASI_ERRNO_TIMEDOUT;
  default: return __WASI_ERRNO_IO;
  }
}

static turbo_wasm3_socket_entry_t *
turbo_wasm3_socket_registry_find_entry(turbo_wasm3_socket_registry_t *registry,
                                       uint32_t wasi_fd) {
  size_t i;

  if (!registry) {
    return NULL;
  }

  for (i = 0; i < registry->count; ++i) {
    if (registry->entries[i].handle == wasi_fd) {
      return &registry->entries[i];
    }
  }

  return NULL;
}

static turbo_wasm3_db_entry_t *
turbo_wasm3_db_registry_find_entry(turbo_wasm3_db_registry_t *registry,
                                   uint32_t db_handle) {
  size_t i;

  if (!registry) {
    return NULL;
  }

  for (i = 0; i < registry->count; ++i) {
    if (registry->entries[i].handle == db_handle) {
      return &registry->entries[i];
    }
  }

  return NULL;
}

static turbo_wasm3_db_stmt_entry_t *
turbo_wasm3_db_registry_find_stmt_entry(turbo_wasm3_db_registry_t *registry,
                                        uint32_t stmt_handle) {
  size_t i;

  if (!registry) {
    return NULL;
  }

  for (i = 0; i < registry->stmt_count; ++i) {
    if (registry->stmt_entries[i].handle == stmt_handle) {
      return &registry->stmt_entries[i];
    }
  }

  return NULL;
}

static turbo_wasm3_redis_client_entry_t *
turbo_wasm3_redis_registry_find_client(turbo_wasm3_redis_registry_t *registry,
                                       uint32_t client_handle) {
  size_t i;

  if (!registry) {
    return NULL;
  }

  for (i = 0; i < registry->client_count; ++i) {
    if (registry->clients[i].handle == client_handle) {
      return &registry->clients[i];
    }
  }

  return NULL;
}

static turbo_wasm3_redis_reply_entry_t *
turbo_wasm3_redis_registry_find_reply(turbo_wasm3_redis_registry_t *registry,
                                      uint32_t reply_handle) {
  size_t i;

  if (!registry) {
    return NULL;
  }

  for (i = 0; i < registry->reply_count; ++i) {
    if (registry->replies[i].handle == reply_handle) {
      return &registry->replies[i];
    }
  }

  return NULL;
}

static int
turbo_wasm3_socket_registry_reserve(turbo_wasm3_socket_registry_t *registry,
                                    size_t required) {
  turbo_wasm3_socket_entry_t *entries;
  size_t new_capacity;
  int rc;

  if (!registry) {
    return TURBO_EINVAL;
  }
  if (required <= registry->capacity) {
    return 0;
  }

  rc = turbo_wasm3_checked_capacity(registry->capacity, required, 16,
                                    registry->max_entries, sizeof(*entries),
                                    &new_capacity);
  if (rc != 0) {
    return rc;
  }

  entries = (turbo_wasm3_socket_entry_t *)realloc(
      registry->entries, new_capacity * sizeof(*entries));
  if (!entries) {
    return TURBO_ENOMEM;
  }

  registry->entries = entries;
  registry->capacity = new_capacity;
  return 0;
}

static int turbo_wasm3_db_registry_reserve(turbo_wasm3_db_registry_t *registry,
                                           size_t required) {
  turbo_wasm3_db_entry_t *entries;
  size_t new_capacity;
  int rc;

  if (!registry) {
    return TURBO_EINVAL;
  }
  if (required <= registry->capacity) {
    return 0;
  }

  rc = turbo_wasm3_checked_capacity(registry->capacity, required, 4,
                                    registry->max_entries, sizeof(*entries),
                                    &new_capacity);
  if (rc != 0) {
    return rc;
  }

  entries = (turbo_wasm3_db_entry_t *)realloc(
      registry->entries, new_capacity * sizeof(*entries));
  if (!entries) {
    return TURBO_ENOMEM;
  }

  registry->entries = entries;
  registry->capacity = new_capacity;
  return 0;
}

static int
turbo_wasm3_db_registry_reserve_statements(turbo_wasm3_db_registry_t *registry,
                                           size_t required) {
  turbo_wasm3_db_stmt_entry_t *entries;
  size_t new_capacity;
  int rc;

  if (!registry) {
    return TURBO_EINVAL;
  }
  if (required <= registry->stmt_capacity) {
    return 0;
  }

  rc = turbo_wasm3_checked_capacity(registry->stmt_capacity, required, 4,
                                    registry->max_statements, sizeof(*entries),
                                    &new_capacity);
  if (rc != 0) {
    return rc;
  }

  entries = (turbo_wasm3_db_stmt_entry_t *)realloc(
      registry->stmt_entries, new_capacity * sizeof(*entries));
  if (!entries) {
    return TURBO_ENOMEM;
  }

  registry->stmt_entries = entries;
  registry->stmt_capacity = new_capacity;
  return 0;
}

static int turbo_wasm3_redis_registry_reserve_clients(
    turbo_wasm3_redis_registry_t *registry, size_t required) {
  turbo_wasm3_redis_client_entry_t *entries;
  size_t new_capacity;
  int rc;

  if (!registry) {
    return TURBO_EINVAL;
  }
  if (required <= registry->client_capacity) {
    return 0;
  }

  rc = turbo_wasm3_checked_capacity(registry->client_capacity, required, 4,
                                    registry->max_clients, sizeof(*entries),
                                    &new_capacity);
  if (rc != 0) {
    return rc;
  }

  entries = (turbo_wasm3_redis_client_entry_t *)realloc(
      registry->clients, new_capacity * sizeof(*entries));
  if (!entries) {
    return TURBO_ENOMEM;
  }

  registry->clients = entries;
  registry->client_capacity = new_capacity;
  return 0;
}

static int turbo_wasm3_redis_registry_reserve_replies(
    turbo_wasm3_redis_registry_t *registry, size_t required) {
  turbo_wasm3_redis_reply_entry_t *entries;
  size_t new_capacity;
  int rc;

  if (!registry) {
    return TURBO_EINVAL;
  }
  if (required <= registry->reply_capacity) {
    return 0;
  }

  rc = turbo_wasm3_checked_capacity(registry->reply_capacity, required, 4,
                                    registry->max_replies, sizeof(*entries),
                                    &new_capacity);
  if (rc != 0) {
    return rc;
  }

  entries = (turbo_wasm3_redis_reply_entry_t *)realloc(
      registry->replies, new_capacity * sizeof(*entries));
  if (!entries) {
    return TURBO_ENOMEM;
  }

  registry->replies = entries;
  registry->reply_capacity = new_capacity;
  return 0;
}

static int turbo_wasm3_vm_reserve_blobs(turbo_wasm3_vm_t *vm, size_t required) {
  turbo_wasm3_blob_t *blobs;
  size_t new_capacity;
  int rc;

  if (!vm) {
    return TURBO_EINVAL;
  }
  if (required <= vm->blob_capacity) {
    return 0;
  }

  rc = turbo_wasm3_checked_capacity(vm->blob_capacity, required, 4,
                                    TURBO_WASM3_NO_LIMIT, sizeof(*blobs),
                                    &new_capacity);
  if (rc != 0) {
    return rc;
  }

  blobs = (turbo_wasm3_blob_t *)realloc(vm->blobs, new_capacity * sizeof(*blobs));
  if (!blobs) {
    return TURBO_ENOMEM;
  }

  vm->blobs = blobs;
  vm->blob_capacity = new_capacity;
  return 0;
}

static int
turbo_wasm3_vm_reserve_host_linkers(turbo_wasm3_vm_t *vm, size_t required) {
  turbo_wasm3_host_linker_entry_t *entries;
  size_t new_capacity;
  int rc;

  if (!vm) {
    return TURBO_EINVAL;
  }
  if (required <= vm->host_linker_capacity) {
    return 0;
  }

  rc = turbo_wasm3_checked_capacity(vm->host_linker_capacity, required, 4,
                                    TURBO_WASM3_NO_LIMIT, sizeof(*entries),
                                    &new_capacity);
  if (rc != 0) {
    return rc;
  }

  entries = (turbo_wasm3_host_linker_entry_t *)realloc(
      vm->host_linkers, new_capacity * sizeof(*entries));
  if (!entries) {
    return TURBO_ENOMEM;
  }

  vm->host_linkers = entries;
  vm->host_linker_capacity = new_capacity;
  return 0;
}

static M3Result turbo_wasm3_vm_store_blob(turbo_wasm3_vm_t *vm,
                                          const uint8_t *wasm_bytes,
                                          uint32_t wasm_size,
                                          const uint8_t **stored_bytes) {
  uint8_t *copy;
  int rc;

  if (!vm || !wasm_bytes || !wasm_size || !stored_bytes) {
    return m3Err_wasmMalformed;
  }

  rc = turbo_wasm3_vm_reserve_blobs(vm, vm->blob_count + 1);
  if (rc != 0) {
    return m3Err_mallocFailed;
  }

  copy = (uint8_t *)malloc(wasm_size);
  if (!copy) {
    return m3Err_mallocFailed;
  }

  memcpy(copy, wasm_bytes, wasm_size);
  vm->blobs[vm->blob_count].bytes = copy;
  vm->blobs[vm->blob_count].size = wasm_size;
  *stored_bytes = copy;
  vm->blob_count++;
  return m3Err_none;
}

static M3Result turbo_wasm3_vm_bind_wasi(turbo_wasm3_vm_t *vm) {
  if (!vm) {
    return m3Err_wasmMalformed;
  }

  if (!vm->wasi_context) {
    return "wasm3 wasi context unavailable";
  }

  if (vm->socket_registry) {
    int rc = turbo_wasm3_socket_registry_bind_wasi(vm->socket_registry,
                                                   vm->wasi_context);
    if (rc != 0) {
      return "failed to bind TurboNet socket registry";
    }
  }

  return m3Err_none;
}

static M3Result turbo_wasm3_suppress_lookup_failure(M3Result result) {
  if (result == m3Err_functionLookupFailed) {
    return m3Err_none;
  }

  return result;
}

/* Parser host function forward declarations */
m3ApiRawFunction(turbo_wasm3_host_socket_send);
m3ApiRawFunction(turbo_wasm3_host_socket_recv);
m3ApiRawFunction(turbo_wasm3_host_socket_release);

m3ApiRawFunction(turbo_wasm3_host_db_open);
m3ApiRawFunction(turbo_wasm3_host_db_close);
m3ApiRawFunction(turbo_wasm3_host_db_exec);
m3ApiRawFunction(turbo_wasm3_host_db_error);
m3ApiRawFunction(turbo_wasm3_host_db_stmt_error);
m3ApiRawFunction(turbo_wasm3_host_db_prepare);
m3ApiRawFunction(turbo_wasm3_host_db_finalize);
m3ApiRawFunction(turbo_wasm3_host_db_reset);
m3ApiRawFunction(turbo_wasm3_host_db_step);
m3ApiRawFunction(turbo_wasm3_host_db_bind_i64);
m3ApiRawFunction(turbo_wasm3_host_db_bind_f64);
m3ApiRawFunction(turbo_wasm3_host_db_bind_null);
m3ApiRawFunction(turbo_wasm3_host_db_bind_blob);
m3ApiRawFunction(turbo_wasm3_host_db_bind_text);
m3ApiRawFunction(turbo_wasm3_host_db_column_type);
m3ApiRawFunction(turbo_wasm3_host_db_column_i64);
m3ApiRawFunction(turbo_wasm3_host_db_column_f64);
m3ApiRawFunction(turbo_wasm3_host_db_column_blob);
m3ApiRawFunction(turbo_wasm3_host_db_column_text);

m3ApiRawFunction(turbo_wasm3_host_parser_json_parse);
m3ApiRawFunction(turbo_wasm3_host_parser_csv_parse);
m3ApiRawFunction(turbo_wasm3_host_parser_xml_parse);
m3ApiRawFunction(turbo_wasm3_host_parser_ini_parse);
m3ApiRawFunction(turbo_wasm3_host_parser_free);
m3ApiRawFunction(turbo_wasm3_host_json_get_string);
m3ApiRawFunction(turbo_wasm3_host_json_get_int);
m3ApiRawFunction(turbo_wasm3_host_json_get_bool);
m3ApiRawFunction(turbo_wasm3_host_json_array_size);
m3ApiRawFunction(turbo_wasm3_host_csv_row_count);
m3ApiRawFunction(turbo_wasm3_host_csv_column_count);
m3ApiRawFunction(turbo_wasm3_host_csv_get_cell);
m3ApiRawFunction(turbo_wasm3_host_csv_find_column);
m3ApiRawFunction(turbo_wasm3_host_xml_root_name);
m3ApiRawFunction(turbo_wasm3_host_xml_get_text);
m3ApiRawFunction(turbo_wasm3_host_xml_count);
m3ApiRawFunction(turbo_wasm3_host_ini_get_string);
m3ApiRawFunction(turbo_wasm3_host_ini_get_int);
m3ApiRawFunction(turbo_wasm3_host_ini_get_bool);
m3ApiRawFunction(turbo_wasm3_host_ini_get_double);

/* Parser registry forward declarations */
turbo_wasm3_parser_registry_t *turbo_wasm3_parser_registry_create(size_t initial_capacity);
void turbo_wasm3_parser_registry_destroy(turbo_wasm3_parser_registry_t *registry);
turbo_wasm3_redis_registry_t *turbo_wasm3_redis_registry_create(size_t initial_capacity);
void turbo_wasm3_redis_registry_destroy(turbo_wasm3_redis_registry_t *registry);

static char *turbo_wasm3_copy_guest_bytes(const uint8_t *data, uint32_t len) {
  char *copy;

  if (!data && len != 0) {
    return NULL;
  }

  copy = (char *)malloc((size_t)len + 1);
  if (!copy) {
    return NULL;
  }

  if (len != 0) {
    memcpy(copy, data, len);
  }
  copy[len] = '\0';
  return copy;
}

static void turbo_wasm3_error_slot_clear(char **slot) {
  if (!slot) {
    return;
  }

  free(*slot);
  *slot = NULL;
}

static int turbo_wasm3_error_slot_set(char **slot, const char *message,
                                      uint32_t message_len) {
  char *copy;

  if (!slot) {
    return TURBO_EINVAL;
  }

  turbo_wasm3_error_slot_clear(slot);
  if (!message || message_len == 0) {
    return 0;
  }

  copy = (char *)malloc((size_t)message_len + 1);
  if (!copy) {
    return TURBO_ENOMEM;
  }

  memcpy(copy, message, message_len);
  copy[message_len] = '\0';
  *slot = copy;
  return 0;
}

static int turbo_wasm3_error_buffer_write(const char *message, char *buffer,
                                          size_t buffer_size,
                                          uint32_t *out_len) {
  size_t len;

  if (!buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  if (!message) {
    buffer[0] = '\0';
    *out_len = 0;
    return 0;
  }

  len = strlen(message);
  if (len >= buffer_size) {
    len = buffer_size - 1;
  }
  memcpy(buffer, message, len);
  buffer[len] = '\0';
  *out_len = (uint32_t)len;
  return 0;
}

static void turbo_wasm3_db_entry_clear_error(turbo_wasm3_db_entry_t *entry) {
  if (!entry) {
    return;
  }

  turbo_wasm3_error_slot_clear(&entry->last_error);
}

static int turbo_wasm3_db_entry_set_error(turbo_wasm3_db_entry_t *entry,
                                          const char *message,
                                          uint32_t message_len) {
  if (!entry) {
    return TURBO_EINVAL;
  }

  return turbo_wasm3_error_slot_set(&entry->last_error, message, message_len);
}

static void turbo_wasm3_db_stmt_entry_clear_error(
    turbo_wasm3_db_stmt_entry_t *entry) {
  if (!entry) {
    return;
  }

  turbo_wasm3_error_slot_clear(&entry->last_error);
}

static int turbo_wasm3_db_stmt_entry_set_error(
    turbo_wasm3_db_stmt_entry_t *entry, const char *message,
    uint32_t message_len) {
  if (!entry) {
    return TURBO_EINVAL;
  }

  return turbo_wasm3_error_slot_set(&entry->last_error, message, message_len);
}

static void turbo_wasm3_db_entry_capture_provider_error(
    turbo_wasm3_db_registry_t *registry, turbo_wasm3_db_entry_t *entry) {
  char buffer[256];
  uint32_t out_len = 0;

  if (!registry || !entry || !registry->ops || !registry->ops->error) {
    return;
  }

  if (registry->ops->error(registry->user_data, entry->db, buffer, sizeof(buffer),
                           &out_len) != 0) {
    return;
  }

  turbo_wasm3_db_entry_set_error(entry, buffer, out_len);
}

static void turbo_wasm3_db_stmt_entry_capture_provider_error(
    turbo_wasm3_db_registry_t *registry, turbo_wasm3_db_stmt_entry_t *stmt_entry,
    turbo_wasm3_db_entry_t *db_entry) {
  if (!stmt_entry) {
    return;
  }

  if (db_entry) {
    turbo_wasm3_db_entry_capture_provider_error(registry, db_entry);
    if (db_entry->last_error) {
      turbo_wasm3_db_stmt_entry_set_error(stmt_entry, db_entry->last_error,
                                          (uint32_t)strlen(db_entry->last_error));
      return;
    }
  }

  turbo_wasm3_db_stmt_entry_clear_error(stmt_entry);
}

static void turbo_wasm3_http_client_entry_clear_error(
    turbo_wasm3_http_client_entry_t *entry) {
  if (!entry) {
    return;
  }

  turbo_wasm3_error_slot_clear(&entry->last_error);
}

static int turbo_wasm3_http_client_entry_set_error(
    turbo_wasm3_http_client_entry_t *entry, const char *message,
    uint32_t message_len) {
  if (!entry) {
    return TURBO_EINVAL;
  }

  return turbo_wasm3_error_slot_set(&entry->last_error, message, message_len);
}

static void turbo_wasm3_http_response_entry_clear_error(
    turbo_wasm3_http_response_entry_t *entry) {
  if (!entry) {
    return;
  }

  turbo_wasm3_error_slot_clear(&entry->last_error);
}

static int turbo_wasm3_http_response_entry_set_error(
    turbo_wasm3_http_response_entry_t *entry, const char *message,
    uint32_t message_len) {
  if (!entry) {
    return TURBO_EINVAL;
  }

  return turbo_wasm3_error_slot_set(&entry->last_error, message, message_len);
}

static void turbo_wasm3_http_response_entry_clear_chunks(
    turbo_wasm3_http_response_entry_t *entry) {
  size_t i;

  if (!entry) {
    return;
  }

  for (i = 0; i < entry->chunk_count; ++i) {
    free(entry->chunks[i].bytes);
  }
  free(entry->chunks);
  entry->chunks = NULL;
  entry->chunk_count = 0;
  entry->chunk_capacity = 0;
  entry->chunk_bytes = 0;
  entry->is_stream = 0;
  entry->is_sse = 0;
}

static void turbo_wasm3_http_response_entry_reset(
    turbo_wasm3_http_response_entry_t *entry) {
  if (!entry) {
    return;
  }

  turbo_wasm3_http_response_entry_clear_error(entry);
  turbo_wasm3_http_response_entry_clear_chunks(entry);
  if (entry->response) {
    http_response_free(entry->response);
  }
  memset(entry, 0, sizeof(*entry));
}

static int turbo_wasm3_http_copy_buffer(const void *src, size_t src_len, void *buffer,
                                        size_t buffer_size, uint32_t *out_len) {
  if (!buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  if (!src || src_len == 0) {
    *out_len = 0;
    return 0;
  }

  if (src_len > buffer_size) {
    memcpy(buffer, src, buffer_size);
    *out_len = (uint32_t)buffer_size;
    return TURBO_EMSGSIZE;
  }

  memcpy(buffer, src, src_len);
  *out_len = (uint32_t)src_len;
  return 0;
}

static void turbo_wasm3_redis_client_entry_clear_error(
    turbo_wasm3_redis_client_entry_t *entry) {
  if (!entry) {
    return;
  }

  turbo_wasm3_error_slot_clear(&entry->last_error);
}

static int turbo_wasm3_redis_client_entry_set_error(
    turbo_wasm3_redis_client_entry_t *entry, const char *message,
    uint32_t message_len) {
  if (!entry) {
    return TURBO_EINVAL;
  }

  return turbo_wasm3_error_slot_set(&entry->last_error, message, message_len);
}

static redis_reply_t *turbo_wasm3_redis_reply_clone(const redis_reply_t *reply) {
  redis_reply_t *copy;
  size_t i;

  if (!reply) {
    return NULL;
  }

  copy = (redis_reply_t *)calloc(1, sizeof(*copy));
  if (!copy) {
    return NULL;
  }

  copy->type = reply->type;
  copy->integer = reply->integer;
  copy->len = reply->len;
  copy->element_count = reply->element_count;

  if (reply->str && reply->len != 0) {
    copy->str = tstr_dup_len(reply->str, reply->len);
    if (!copy->str) {
      free(copy);
      return NULL;
    }
  } else if (reply->str) {
    copy->str = tstr_dup("");
    if (!copy->str) {
      free(copy);
      return NULL;
    }
  }

  if (reply->element_count != 0) {
    copy->elements = (redis_reply_t **)calloc(reply->element_count,
                                              sizeof(*copy->elements));
    if (!copy->elements) {
      redis_reply_free(copy);
      return NULL;
    }

    for (i = 0; i < reply->element_count; ++i) {
      copy->elements[i] = turbo_wasm3_redis_reply_clone(reply->elements[i]);
      if (!copy->elements[i]) {
        redis_reply_free(copy);
        return NULL;
      }
    }
  }

  return copy;
}

static void turbo_wasm3_redis_reply_entry_reset(
    turbo_wasm3_redis_reply_entry_t *entry) {
  if (!entry) {
    return;
  }

  if (entry->reply) {
    redis_reply_free(entry->reply);
  }
  memset(entry, 0, sizeof(*entry));
}

static void turbo_wasm3_redis_capture_reply_cb(redis_client_t *client,
                                               redis_reply_t *reply,
                                               void *user_data) {
  turbo_wasm3_redis_command_capture_t *capture =
      (turbo_wasm3_redis_command_capture_t *)user_data;

  (void)client;
  if (!capture || capture->reply || !reply) {
    return;
  }

  capture->reply = turbo_wasm3_redis_reply_clone(reply);
}

static void turbo_wasm3_redis_open_task(coro_t *co, void *arg) {
  turbo_wasm3_redis_open_task_state_t *state =
      (turbo_wasm3_redis_open_task_state_t *)arg;
  coro_context_t *ctx = coro_context_current();

  (void)co;
  state->rc = TURBO_EIO;
  state->socket = NULL;
  if (!ctx || !state || !state->host || state->port == 0) {
    state->done = 1;
    return;
  }

  state->socket = coro_socket_create_tcpv4(ctx);
  if (!state->socket) {
    state->rc = TURBO_ENOMEM;
    state->done = 1;
    return;
  }

  coro_socket_set_timeout(state->socket, 5000);
  state->rc = coro_socket_connect(state->socket, state->host, (int)state->port);
  if (state->rc != 0) {
    coro_socket_destroy(state->socket);
    state->socket = NULL;
  }
  state->done = 1;
}

static void turbo_wasm3_redis_command_task(coro_t *co, void *arg) {
  turbo_wasm3_redis_command_task_state_t *state =
      (turbo_wasm3_redis_command_task_state_t *)arg;
  size_t *arg_lens = NULL;
  turbo_wasm3_redis_command_capture_t capture = {0};
  uint32_t i;

  (void)co;
  state->rc = TURBO_EIO;
  if (!state || !state->entry || !state->entry->client || state->argc == 0 ||
      !state->argv || !state->argv_lens) {
    state->done = 1;
    return;
  }

  arg_lens = (size_t *)calloc(state->argc, sizeof(*arg_lens));
  if (!arg_lens) {
    state->rc = TURBO_ENOMEM;
    state->done = 1;
    return;
  }

  for (i = 0; i < state->argc; ++i) {
    arg_lens[i] = (size_t)state->argv_lens[i];
  }

  state->rc = redis_commandv(state->entry->client, (int)state->argc,
                             (const char **)state->argv, arg_lens,
                             turbo_wasm3_redis_capture_reply_cb, &capture);
  free(arg_lens);
  if (state->rc == 0) {
    state->reply = capture.reply;
  }
  state->done = 1;
}

static int turbo_wasm3_redis_run_until_done(coro_context_t *ctx, int *done) {
  uint64_t deadline_ms;

  if (!ctx || !done) {
    return TURBO_EINVAL;
  }

  deadline_ms = turbo_monotonic_ms() + 2000;
  while (!*done && turbo_monotonic_ms() < deadline_ms) {
    coro_context_run(ctx, TURBO_RUN_ONCE);
  }

  return *done ? 0 : TURBO_ETIMEDOUT;
}

static int turbo_wasm3_redis_registry_store_reply(
    turbo_wasm3_redis_registry_t *registry, uint32_t client_handle,
    redis_reply_t *reply, uint32_t *out_reply_handle) {
  turbo_wasm3_redis_reply_entry_t *entry;
  int rc;

  if (!registry || !reply || !out_reply_handle) {
    return TURBO_EINVAL;
  }

  rc = turbo_wasm3_redis_registry_reserve_replies(registry,
                                                  registry->reply_count + 1);
  if (rc != 0) {
    return rc;
  }

  entry = &registry->replies[registry->reply_count];
  entry->handle = registry->next_reply_handle++;
  entry->client_handle = client_handle;
  entry->reply = reply;
  *out_reply_handle = entry->handle;
  registry->reply_count++;
  return 0;
}

static int turbo_wasm3_http_copy_string(const char *src, char *buffer,
                                        size_t buffer_size, uint32_t *out_len) {
  size_t len = src ? strlen(src) : 0;

  if (!buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  if (!src) {
    buffer[0] = '\0';
    *out_len = 0;
    return 0;
  }

  if (len >= buffer_size) {
    memcpy(buffer, src, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
    *out_len = (uint32_t)(buffer_size - 1);
    return TURBO_EMSGSIZE;
  }

  if (len != 0) {
    memcpy(buffer, src, len);
  }
  buffer[len] = '\0';
  *out_len = (uint32_t)len;
  return 0;
}

static int turbo_wasm3_path_is_sep(char ch) {
  return ch == '/' || ch == '\\';
}

static char turbo_wasm3_ascii_lower(char ch) {
  if (ch >= 'A' && ch <= 'Z') {
    return (char)(ch - 'A' + 'a');
  }
  return ch;
}

static int turbo_wasm3_path_char_equal(char left, char right) {
  if (turbo_wasm3_path_is_sep(left) && turbo_wasm3_path_is_sep(right)) {
    return 1;
  }
#ifdef _WIN32
  return turbo_wasm3_ascii_lower(left) == turbo_wasm3_ascii_lower(right);
#else
  return left == right;
#endif
}

static int turbo_wasm3_path_has_parent_segment(const char *path) {
  const char *segment = path;
  const char *cursor = path;

  if (!path) {
    return 1;
  }

  while (1) {
    if (*cursor == '\0' || turbo_wasm3_path_is_sep(*cursor)) {
      if ((size_t)(cursor - segment) == 2 && segment[0] == '.' &&
          segment[1] == '.') {
        return 1;
      }
      if (*cursor == '\0') {
        return 0;
      }
      segment = cursor + 1;
    }
    ++cursor;
  }
}

static int turbo_wasm3_path_is_under_base(const char *base, const char *path) {
  size_t base_len;
  size_t i;

  if (!base || !path) {
    return 0;
  }

  base_len = strlen(base);
  while (base_len > 0 && turbo_wasm3_path_is_sep(base[base_len - 1])) {
    --base_len;
  }
  if (base_len == 0) {
    return 0;
  }

  for (i = 0; i < base_len; ++i) {
    if (path[i] == '\0' || !turbo_wasm3_path_char_equal(base[i], path[i])) {
      return 0;
    }
  }

  return path[base_len] == '\0' || turbo_wasm3_path_is_sep(path[base_len]);
}

static int turbo_wasm3_sqlite_resolve_target(
    turbo_wasm3_db_registry_t *registry, const char *target,
    char **out_target) {
  char joined[TURBO_FS_MAX_PATH];
  char *resolved;

  if (!target || !out_target) {
    return TURBO_EINVAL;
  }
  *out_target = NULL;

  if (strcmp(target, ":memory:") == 0) {
    resolved = turbo_wasm3_strdup(target);
    if (!resolved) {
      return TURBO_ENOMEM;
    }
    *out_target = resolved;
    return 0;
  }

  if (!registry || !registry->sqlite_base_dir || target[0] == '\0' ||
      turbo_wasm3_path_has_parent_segment(target)) {
    return TURBO_EPERM;
  }

  if (turbo_fs_path_is_absolute(target)) {
    if (!turbo_wasm3_path_is_under_base(registry->sqlite_base_dir, target)) {
      return TURBO_EPERM;
    }
    resolved = turbo_wasm3_strdup(target);
    if (!resolved) {
      return TURBO_ENOMEM;
    }
    *out_target = resolved;
    return 0;
  }
  if (strchr(target, ':')) {
    return TURBO_EPERM;
  }

  if (turbo_fs_path_join(joined, sizeof(joined), registry->sqlite_base_dir,
                         target) != 0) {
    return TURBO_ENAMETOOLONG;
  }

  resolved = turbo_wasm3_strdup(joined);
  if (!resolved) {
    return TURBO_ENOMEM;
  }
  *out_target = resolved;
  return 0;
}

typedef struct turbo_wasm3_http_header_block_s {
  char *storage;
  const char **lines;
  int count;
} turbo_wasm3_http_header_block_t;

static void
turbo_wasm3_http_header_block_clear(turbo_wasm3_http_header_block_t *block) {
  if (!block) {
    return;
  }

  free((void *)block->lines);
  free(block->storage);
  block->storage = NULL;
  block->lines = NULL;
  block->count = 0;
}

static int turbo_wasm3_http_parse_headers(const char *headers, size_t headers_len,
                                          turbo_wasm3_http_header_block_t *out_block) {
  char *storage = NULL;
  const char **lines = NULL;
  size_t i;
  size_t line_count = 0;
  char *cursor;
  int index = 0;

  if (!out_block) {
    return TURBO_EINVAL;
  }

  memset(out_block, 0, sizeof(*out_block));
  if (!headers || headers_len == 0) {
    return 0;
  }
  if (headers_len > TURBO_WASM3_DEFAULT_MAX_HTTP_HEADER_BYTES) {
    return TURBO_ENOBUFS;
  }

  storage = (char *)malloc(headers_len + 1);
  if (!storage) {
    return TURBO_ENOMEM;
  }

  memcpy(storage, headers, headers_len);
  storage[headers_len] = '\0';

  for (i = 0; i < headers_len; ++i) {
    if (storage[i] == '\r') {
      storage[i] = '\n';
    }
  }

  cursor = storage;
  while (*cursor != '\0') {
    char *line_start = cursor;

    while (*cursor != '\0' && *cursor != '\n') {
      ++cursor;
    }
    if (*cursor == '\n') {
      *cursor++ = '\0';
    }
    if (*line_start != '\0') {
      ++line_count;
      if (line_count > TURBO_WASM3_DEFAULT_MAX_HTTP_HEADER_LINES) {
        free(storage);
        return TURBO_ENOBUFS;
      }
    }
  }

  if (line_count == 0) {
    out_block->storage = storage;
    return 0;
  }

  lines = (const char **)calloc(line_count, sizeof(*lines));
  if (!lines) {
    free(storage);
    return TURBO_ENOMEM;
  }

  cursor = storage;
  while (cursor < storage + headers_len) {
    if (*cursor != '\0') {
      lines[index++] = cursor;
      cursor += strlen(cursor) + 1;
      continue;
    }
    ++cursor;
  }

  out_block->storage = storage;
  out_block->lines = lines;
  out_block->count = (int)line_count;
  return 0;
}

static turbo_wasm3_http_client_entry_t *
turbo_wasm3_http_registry_find_client(turbo_wasm3_http_registry_t *registry,
                                      uint32_t client_handle) {
  size_t i;

  if (!registry) {
    return NULL;
  }

  for (i = 0; i < registry->client_count; ++i) {
    if (registry->clients[i].handle == client_handle) {
      return &registry->clients[i];
    }
  }

  return NULL;
}

static turbo_wasm3_http_response_entry_t *
turbo_wasm3_http_registry_find_response(turbo_wasm3_http_registry_t *registry,
                                        uint32_t response_handle) {
  size_t i;

  if (!registry) {
    return NULL;
  }

  for (i = 0; i < registry->response_count; ++i) {
    if (registry->responses[i].handle == response_handle) {
      return &registry->responses[i];
    }
  }

  return NULL;
}

static int turbo_wasm3_http_registry_reserve_clients(
    turbo_wasm3_http_registry_t *registry, size_t required) {
  turbo_wasm3_http_client_entry_t *entries;
  size_t new_capacity;
  int rc;

  if (!registry) {
    return TURBO_EINVAL;
  }
  if (required <= registry->client_capacity) {
    return 0;
  }

  rc = turbo_wasm3_checked_capacity(registry->client_capacity, required, 4,
                                    registry->max_clients, sizeof(*entries),
                                    &new_capacity);
  if (rc != 0) {
    return rc;
  }

  entries = (turbo_wasm3_http_client_entry_t *)realloc(
      registry->clients, new_capacity * sizeof(*entries));
  if (!entries) {
    return TURBO_ENOMEM;
  }

  registry->clients = entries;
  registry->client_capacity = new_capacity;
  return 0;
}

static int turbo_wasm3_http_registry_reserve_responses(
    turbo_wasm3_http_registry_t *registry, size_t required) {
  turbo_wasm3_http_response_entry_t *entries;
  size_t new_capacity;
  int rc;

  if (!registry) {
    return TURBO_EINVAL;
  }
  if (required <= registry->response_capacity) {
    return 0;
  }

  rc = turbo_wasm3_checked_capacity(registry->response_capacity, required, 4,
                                    registry->max_responses, sizeof(*entries),
                                    &new_capacity);
  if (rc != 0) {
    return rc;
  }

  entries = (turbo_wasm3_http_response_entry_t *)realloc(
      registry->responses, new_capacity * sizeof(*entries));
  if (!entries) {
    return TURBO_ENOMEM;
  }

  registry->responses = entries;
  registry->response_capacity = new_capacity;
  return 0;
}

static int turbo_wasm3_http_response_entry_reserve_chunks(
    turbo_wasm3_http_response_entry_t *entry, size_t required,
    size_t max_chunks) {
  turbo_wasm3_blob_t *chunks;
  size_t new_capacity;
  int rc;

  if (!entry) {
    return TURBO_EINVAL;
  }
  if (required <= entry->chunk_capacity) {
    return 0;
  }

  rc = turbo_wasm3_checked_capacity(entry->chunk_capacity, required, 4,
                                    max_chunks, sizeof(*chunks),
                                    &new_capacity);
  if (rc != 0) {
    return rc;
  }

  chunks = (turbo_wasm3_blob_t *)realloc(entry->chunks,
                                         new_capacity * sizeof(*chunks));
  if (!chunks) {
    return TURBO_ENOMEM;
  }

  entry->chunks = chunks;
  entry->chunk_capacity = new_capacity;
  return 0;
}

static int turbo_wasm3_http_response_entry_append_chunk(
    turbo_wasm3_http_response_entry_t *entry, const void *data, size_t len,
    size_t max_chunks, size_t max_stream_bytes) {
  uint8_t *copy = NULL;
  int rc;

  if (!entry) {
    return TURBO_EINVAL;
  }
  if (len == 0) {
    return 0;
  }
  if (len > UINT32_MAX || entry->chunk_count >= max_chunks ||
      len > max_stream_bytes || entry->chunk_bytes > max_stream_bytes - len) {
    return TURBO_ENOBUFS;
  }

  rc = turbo_wasm3_http_response_entry_reserve_chunks(
      entry, entry->chunk_count + 1, max_chunks);
  if (rc != 0) {
    return rc;
  }

  copy = (uint8_t *)malloc(len);
  if (!copy) {
    return TURBO_ENOMEM;
  }

  memcpy(copy, data, len);
  entry->chunks[entry->chunk_count].bytes = copy;
  entry->chunks[entry->chunk_count].size = (uint32_t)len;
  entry->chunk_count++;
  entry->chunk_bytes += len;
  entry->is_stream = 1;
  return 0;
}

typedef struct turbo_wasm3_http_capture_ctx_s {
  turbo_wasm3_http_registry_t *registry;
  turbo_wasm3_http_response_entry_t *entry;
  int rc;
} turbo_wasm3_http_capture_ctx_t;

static void turbo_wasm3_http_capture_chunk_cb(const char *data, size_t len,
                                              void *user_data) {
  turbo_wasm3_http_capture_ctx_t *ctx =
      (turbo_wasm3_http_capture_ctx_t *)user_data;
  int rc;

  if (!ctx || !ctx->registry || !ctx->entry || ctx->rc != 0) {
    return;
  }

  rc = turbo_wasm3_http_response_entry_append_chunk(
      ctx->entry, data, len, ctx->registry->max_chunks_per_response,
      ctx->registry->max_stream_bytes);
  if (rc != 0) {
    ctx->rc = rc;
  }
}

static void turbo_wasm3_http_capture_client_error(
    turbo_wasm3_http_client_entry_t *entry, const char *message) {
  if (!entry) {
    return;
  }

  turbo_wasm3_http_client_entry_set_error(entry, message ? message : "http client error",
                                          (uint32_t)strlen(message ? message
                                                                   : "http client error"));
}

static void turbo_wasm3_http_capture_response_error(
    turbo_wasm3_http_response_entry_t *response_entry, http_response_t *response) {
  const char *message = NULL;

  if (!response_entry) {
    return;
  }

  if (response && response->error && response->error[0] != '\0') {
    message = response->error;
  } else {
    message = "http response error";
  }

  turbo_wasm3_http_response_entry_set_error(response_entry, message,
                                            (uint32_t)strlen(message));
}

static int turbo_wasm3_http_registry_request_core(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle, int method,
    const char *url, const char **headers, int header_count, const void *body,
    size_t body_len, int use_stream_api, int use_sse_api,
    uint32_t *response_handle) {
  turbo_wasm3_http_client_entry_t *client_entry;
  turbo_wasm3_http_response_entry_t *entry;
  turbo_wasm3_http_capture_ctx_t capture_ctx = {0};
  http_response_t *response = NULL;
  int rc;

  if (!registry || !url || !response_handle) {
    return TURBO_EINVAL;
  }

  client_entry = turbo_wasm3_http_registry_find_client(registry, client_handle);
  if (!client_entry) {
    return TURBO_EBADF;
  }

  rc = turbo_wasm3_http_registry_reserve_responses(registry,
                                                   registry->response_count + 1);
  if (rc != 0) {
    return rc;
  }

  entry = &registry->responses[registry->response_count];
  memset(entry, 0, sizeof(*entry));
  entry->handle = registry->next_response_handle++;
  entry->client_handle = client_handle;
  entry->is_stream = use_stream_api ? 1 : 0;
  entry->is_sse = use_sse_api ? 1 : 0;

  turbo_wasm3_http_client_entry_clear_error(client_entry);
  if (use_sse_api) {
    capture_ctx.registry = registry;
    capture_ctx.entry = entry;
    response = http_sse_get(client_entry->client, url,
                            turbo_wasm3_http_capture_chunk_cb, &capture_ctx);
  } else if (use_stream_api) {
    capture_ctx.registry = registry;
    capture_ctx.entry = entry;
    response = http_receive_stream_get(client_entry->client, url,
                                       turbo_wasm3_http_capture_chunk_cb,
                                       &capture_ctx);
  } else {
    response = http_request(client_entry->client, method, url, headers,
                            header_count, (const char *)body, body_len);
  }

  if (!response) {
    turbo_wasm3_http_capture_client_error(client_entry,
                                          "http request returned null");
    return TURBO_EIO;
  }

  entry->response = response;
  if (capture_ctx.rc != 0) {
    rc = capture_ctx.rc;
    turbo_wasm3_http_capture_response_error(entry, response);
    turbo_wasm3_http_response_entry_reset(entry);
    return rc;
  }

  if (response->error && response->error[0] != '\0') {
    turbo_wasm3_http_capture_response_error(entry, response);
  }

  *response_handle = entry->handle;
  registry->response_count++;
  return 0;
}

static int turbo_wasm3_sqlite_column_type_value(void *stmt, uint32_t index,
                                                int32_t *out_type) {
  int type;

  if (!stmt || !out_type) {
    return TURBO_EINVAL;
  }

  type = sqlite3_column_type((sqlite3_stmt *)stmt, (int)index);
  switch (type) {
    case SQLITE_INTEGER:
      *out_type = TURBO_WASM3_DB_TYPE_INT64;
      return 0;
    case SQLITE_FLOAT:
      *out_type = TURBO_WASM3_DB_TYPE_DOUBLE;
      return 0;
    case SQLITE_TEXT:
      *out_type = TURBO_WASM3_DB_TYPE_TEXT;
      return 0;
    case SQLITE_BLOB:
      *out_type = TURBO_WASM3_DB_TYPE_BLOB;
      return 0;
    case SQLITE_NULL:
      *out_type = TURBO_WASM3_DB_TYPE_NULL;
      return 0;
    default:
      return TURBO_EIO;
  }
}

static int turbo_wasm3_sqlite_open(void *user_data, const char *target,
                                   void **out_db) {
  turbo_wasm3_db_registry_t *registry = (turbo_wasm3_db_registry_t *)user_data;
  char *resolved_target = NULL;
  sqlite3 *db = NULL;
  int rc;

  if (!target || !out_db) {
    return TURBO_EINVAL;
  }

  rc = turbo_wasm3_sqlite_resolve_target(registry, target, &resolved_target);
  if (rc != 0) {
    return rc;
  }

  rc = sqlite3_open(resolved_target, &db);
  free(resolved_target);
  if (rc != SQLITE_OK) {
    if (db) {
      sqlite3_close(db);
    }
    return TURBO_EIO;
  }

  *out_db = db;
  return 0;
}

static int turbo_wasm3_sqlite_close(void *user_data, void *db) {
  (void)user_data;

  if (!db) {
    return TURBO_EINVAL;
  }

  return sqlite3_close((sqlite3 *)db) == SQLITE_OK ? 0 : TURBO_EBUSY;
}

static int turbo_wasm3_sqlite_exec(void *user_data, void *db, const char *sql,
                                   uint64_t *changes) {
  char *errmsg = NULL;
  int rc;

  (void)user_data;

  if (!db || !sql || !changes) {
    return TURBO_EINVAL;
  }

  *changes = 0;
  rc = sqlite3_exec((sqlite3 *)db, sql, NULL, NULL, &errmsg);
  if (errmsg) {
    sqlite3_free(errmsg);
  }
  if (rc != SQLITE_OK) {
    return TURBO_EINVAL;
  }

  *changes = (uint64_t)sqlite3_changes64((sqlite3 *)db);
  return 0;
}

static int turbo_wasm3_sqlite_error(void *user_data, void *db, char *buffer,
                                    size_t buffer_size, uint32_t *out_len) {
  const char *msg;
  size_t len;

  (void)user_data;

  if (!db || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  msg = sqlite3_errmsg((sqlite3 *)db);
  if (!msg) {
    buffer[0] = '\0';
    *out_len = 0;
    return 0;
  }

  len = strlen(msg);
  if (len >= buffer_size) {
    len = buffer_size - 1;
  }

  memcpy(buffer, msg, len);
  buffer[len] = '\0';
  *out_len = (uint32_t)len;
  return 0;
}

static int turbo_wasm3_sqlite_prepare(void *user_data, void *db, const char *sql,
                                      void **out_stmt) {
  sqlite3_stmt *stmt = NULL;
  int rc;

  (void)user_data;

  if (!db || !sql || !out_stmt) {
    return TURBO_EINVAL;
  }

  rc = sqlite3_prepare_v2((sqlite3 *)db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    if (stmt) {
      sqlite3_finalize(stmt);
    }
    return TURBO_EINVAL;
  }

  *out_stmt = stmt;
  return 0;
}

static int turbo_wasm3_sqlite_finalize(void *user_data, void *stmt) {
  (void)user_data;

  if (!stmt) {
    return TURBO_EINVAL;
  }

  return sqlite3_finalize((sqlite3_stmt *)stmt) == SQLITE_OK ? 0 : TURBO_EIO;
}

static int turbo_wasm3_sqlite_reset(void *user_data, void *stmt) {
  int rc;

  (void)user_data;

  if (!stmt) {
    return TURBO_EINVAL;
  }

  rc = sqlite3_reset((sqlite3_stmt *)stmt);
  sqlite3_clear_bindings((sqlite3_stmt *)stmt);
  return rc == SQLITE_OK ? 0 : TURBO_EIO;
}

static int turbo_wasm3_sqlite_step(void *user_data, void *stmt,
                                   int32_t *out_state) {
  int rc;

  (void)user_data;

  if (!stmt || !out_state) {
    return TURBO_EINVAL;
  }

  rc = sqlite3_step((sqlite3_stmt *)stmt);
  if (rc == SQLITE_ROW) {
    *out_state = TURBO_WASM3_DB_STEP_ROW;
    return 0;
  }
  if (rc == SQLITE_DONE) {
    *out_state = TURBO_WASM3_DB_STEP_DONE;
    return 0;
  }

  return TURBO_EIO;
}

static int turbo_wasm3_sqlite_bind_int64(void *user_data, void *stmt,
                                         uint32_t index, int64_t value) {
  (void)user_data;

  if (!stmt || index == 0) {
    return TURBO_EINVAL;
  }

  return sqlite3_bind_int64((sqlite3_stmt *)stmt, (int)index, (sqlite3_int64)value) ==
                 SQLITE_OK
             ? 0
             : TURBO_EINVAL;
}

static int turbo_wasm3_sqlite_bind_double(void *user_data, void *stmt,
                                          uint32_t index, double value) {
  (void)user_data;

  if (!stmt || index == 0) {
    return TURBO_EINVAL;
  }

  return sqlite3_bind_double((sqlite3_stmt *)stmt, (int)index, value) == SQLITE_OK
             ? 0
             : TURBO_EINVAL;
}

static int turbo_wasm3_sqlite_bind_null(void *user_data, void *stmt,
                                        uint32_t index) {
  (void)user_data;

  if (!stmt || index == 0) {
    return TURBO_EINVAL;
  }

  return sqlite3_bind_null((sqlite3_stmt *)stmt, (int)index) == SQLITE_OK ? 0
                                                                           : TURBO_EINVAL;
}

static int turbo_wasm3_sqlite_bind_blob(void *user_data, void *stmt,
                                        uint32_t index, const void *value,
                                        size_t value_len) {
  (void)user_data;

  if (!stmt || index == 0 || (!value && value_len != 0)) {
    return TURBO_EINVAL;
  }
  if (value_len > INT_MAX) {
    return TURBO_EMSGSIZE;
  }

  return sqlite3_bind_blob((sqlite3_stmt *)stmt, (int)index, value, (int)value_len,
                           SQLITE_TRANSIENT) == SQLITE_OK
             ? 0
             : TURBO_EINVAL;
}

static int turbo_wasm3_sqlite_bind_text(void *user_data, void *stmt,
                                        uint32_t index, const char *value,
                                        size_t value_len) {
  (void)user_data;

  if (!stmt || index == 0 || (!value && value_len != 0)) {
    return TURBO_EINVAL;
  }
  if (value_len > INT_MAX) {
    return TURBO_EMSGSIZE;
  }

  return sqlite3_bind_text((sqlite3_stmt *)stmt, (int)index, value,
                           (int)value_len, SQLITE_TRANSIENT) == SQLITE_OK
             ? 0
             : TURBO_EINVAL;
}

static int turbo_wasm3_sqlite_column_type(void *user_data, void *stmt,
                                          uint32_t index, int32_t *out_type) {
  (void)user_data;

  return turbo_wasm3_sqlite_column_type_value(stmt, index, out_type);
}

static int turbo_wasm3_sqlite_column_int64(void *user_data, void *stmt,
                                           uint32_t index, int64_t *out_value) {
  (void)user_data;

  if (!stmt || !out_value) {
    return TURBO_EINVAL;
  }

  *out_value = (int64_t)sqlite3_column_int64((sqlite3_stmt *)stmt, (int)index);
  return 0;
}

static int turbo_wasm3_sqlite_column_double(void *user_data, void *stmt,
                                            uint32_t index, double *out_value) {
  (void)user_data;

  if (!stmt || !out_value) {
    return TURBO_EINVAL;
  }

  *out_value = sqlite3_column_double((sqlite3_stmt *)stmt, (int)index);
  return 0;
}

static int turbo_wasm3_sqlite_column_blob(void *user_data, void *stmt,
                                          uint32_t index, void *buffer,
                                          size_t buffer_size,
                                          uint32_t *out_len) {
  const void *blob;
  int len;
  int32_t type;

  (void)user_data;

  if (!stmt || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  if (turbo_wasm3_sqlite_column_type_value(stmt, index, &type) != 0) {
    return TURBO_EIO;
  }
  if (type == TURBO_WASM3_DB_TYPE_NULL) {
    *out_len = 0;
    return 0;
  }

  blob = sqlite3_column_blob((sqlite3_stmt *)stmt, (int)index);
  if (!blob) {
    *out_len = 0;
    return 0;
  }

  len = sqlite3_column_bytes((sqlite3_stmt *)stmt, (int)index);
  if ((size_t)len > buffer_size) {
    memcpy(buffer, blob, buffer_size);
    *out_len = (uint32_t)buffer_size;
    return TURBO_EMSGSIZE;
  }

  if (len != 0) {
    memcpy(buffer, blob, (size_t)len);
  }
  *out_len = (uint32_t)len;
  return 0;
}

static int turbo_wasm3_sqlite_column_text(void *user_data, void *stmt,
                                          uint32_t index, char *buffer,
                                          size_t buffer_size,
                                          uint32_t *out_len) {
  const unsigned char *text;
  int len;

  (void)user_data;

  if (!stmt || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  text = sqlite3_column_text((sqlite3_stmt *)stmt, (int)index);
  if (!text) {
    buffer[0] = '\0';
    *out_len = 0;
    return 0;
  }

  len = sqlite3_column_bytes((sqlite3_stmt *)stmt, (int)index);
  if ((size_t)len >= buffer_size) {
    memcpy(buffer, text, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
    *out_len = (uint32_t)(buffer_size - 1);
    return TURBO_EMSGSIZE;
  }

  memcpy(buffer, text, (size_t)len);
  buffer[len] = '\0';
  *out_len = (uint32_t)len;
  return 0;
}

static const turbo_wasm3_db_ops_t turbo_wasm3_sqlite_db_ops = {
    turbo_wasm3_sqlite_open,
    turbo_wasm3_sqlite_close,
    turbo_wasm3_sqlite_exec,
    turbo_wasm3_sqlite_error,
    turbo_wasm3_sqlite_prepare,
    turbo_wasm3_sqlite_finalize,
    turbo_wasm3_sqlite_reset,
    turbo_wasm3_sqlite_step,
    turbo_wasm3_sqlite_bind_int64,
    turbo_wasm3_sqlite_bind_double,
    turbo_wasm3_sqlite_bind_null,
    turbo_wasm3_sqlite_bind_blob,
    turbo_wasm3_sqlite_bind_text,
    turbo_wasm3_sqlite_column_type,
    turbo_wasm3_sqlite_column_int64,
    turbo_wasm3_sqlite_column_double,
    turbo_wasm3_sqlite_column_blob,
    turbo_wasm3_sqlite_column_text,
};

static uint16_t
turbo_wasm3_socket_send_cb(void *user_data, uint32_t handle, const uint8_t *data,
                           uint32_t data_len, uint16_t si_flags,
                           uint32_t *so_datalen) {
  turbo_wasm3_socket_registry_t *registry =
      (turbo_wasm3_socket_registry_t *)user_data;
  turbo_wasm3_socket_entry_t *entry;
  int rc;

  if (si_flags != 0 || !so_datalen) {
    return __WASI_ERRNO_INVAL;
  }

  entry = turbo_wasm3_socket_registry_find_entry(registry, handle);
  if (!entry) {
    return __WASI_ERRNO_BADF;
  }

  rc = coro_socket_send(entry->socket, (const char *)data, (size_t)data_len);
  if (rc != 0) {
    return turbo_wasm3_error_to_wasi(rc);
  }

  *so_datalen = data_len;
  return __WASI_ERRNO_SUCCESS;
}

static uint16_t
turbo_wasm3_socket_recv_cb(void *user_data, uint32_t handle, uint8_t *data,
                           uint32_t data_len, uint16_t ri_flags,
                           uint32_t *ro_datalen, uint16_t *ro_flags) {
  turbo_wasm3_socket_registry_t *registry =
      (turbo_wasm3_socket_registry_t *)user_data;
  turbo_wasm3_socket_entry_t *entry;
  char *recv_data = NULL;
  size_t recv_len = 0;
  int rc;

  if (!ro_datalen || !ro_flags) {
    return __WASI_ERRNO_INVAL;
  }

  entry = turbo_wasm3_socket_registry_find_entry(registry, handle);
  if (!entry) {
    return __WASI_ERRNO_BADF;
  }

  if ((ri_flags & ~((uint16_t)0x0003)) != 0) {
    return __WASI_ERRNO_INVAL;
  }

  rc = coro_socket_recv(entry->socket, &recv_data, &recv_len);
  if (rc != 0) {
    if (recv_data) {
      coro_socket_free_recv(recv_data);
    }
    return turbo_wasm3_error_to_wasi(rc);
  }

  *ro_flags = 0;
  if (recv_len > data_len) {
    recv_len = data_len;
    *ro_flags = __WASI_ROFLAGS_RECV_DATA_TRUNCATED;
  }

  if (recv_len != 0 && data && recv_data) {
    memcpy(data, recv_data, recv_len);
  }
  if (recv_data) {
    coro_socket_free_recv(recv_data);
  }

  *ro_datalen = (uint32_t)recv_len;
  return __WASI_ERRNO_SUCCESS;
}

static uint16_t
turbo_wasm3_socket_shutdown_cb(void *user_data, uint32_t handle, uint8_t how) {
  turbo_wasm3_socket_registry_t *registry =
      (turbo_wasm3_socket_registry_t *)user_data;
  turbo_wasm3_socket_entry_t *entry;

  (void)how;

  entry = turbo_wasm3_socket_registry_find_entry(registry, handle);
  if (!entry) {
    return __WASI_ERRNO_BADF;
  }

  return __WASI_ERRNO_NOSYS;
}

static const m3_wasi_socket_ops_t turbo_wasm3_socket_ops = {
    turbo_wasm3_socket_send_cb,
    turbo_wasm3_socket_recv_cb,
    turbo_wasm3_socket_shutdown_cb,
};

static turbo_wasm3_vm_t *turbo_wasm3_import_vm(IM3ImportContext ctx) {
  return ctx ? (turbo_wasm3_vm_t *)ctx->userdata : NULL;
}

#endif /* TURBO_WASM3_INTERNAL_H */

