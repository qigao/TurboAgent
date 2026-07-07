#include "turbo_runtime_data_bind.h"
#include "turbo_parser.h"
#include <turbo_str.h>
#include <stdlib.h>
#include <string.h>

static char *turbo_runtime_data_bind_strdup(const char *value) {
  return (char *)tstr_dup(value);
}

typedef struct turbo_runtime_data_bind_entry_s {
  char *key;
  turbo_runtime_data_bind_value_t *value;
} turbo_runtime_data_bind_entry_t;

struct turbo_runtime_data_bind_value_s {
  turbo_runtime_data_bind_value_kind_t kind;
  union {
    int bool_value;
    int64_t int64_value;
    double double_value;
    struct {
      char *data;
      size_t size;
    } string_value;
    struct {
      uint8_t *data;
      size_t size;
    } bytes_value;
    struct {
      turbo_runtime_data_bind_entry_t *entries;
      size_t count;
      size_t capacity;
    } container;
  } as;
};

static turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_value_alloc(turbo_runtime_data_bind_value_kind_t kind) {
  turbo_runtime_data_bind_value_t *value =
      (turbo_runtime_data_bind_value_t *)calloc(1, sizeof(*value));
  if (!value) {
    return NULL;
  }
  value->kind = kind;
  return value;
}



static void *turbo_runtime_data_bind_memdup(const void *data, size_t size) {
  void *copy;

  if (!data || size == 0) {
    return NULL;
  }

  copy = malloc(size);
  if (!copy) {
    return NULL;
  }

  memcpy(copy, data, size);
  return copy;
}

static turbo_runtime_data_bind_status_t turbo_runtime_data_bind_container_reserve(
    turbo_runtime_data_bind_value_t *value, size_t needed) {
  turbo_runtime_data_bind_entry_t *entries;
  size_t capacity;

  if (!value) {
    return TURBO_RUNTIME_DATA_BIND_INVALID_ARGUMENT;
  }

  capacity = value->as.container.capacity;
  if (capacity >= needed) {
    return TURBO_RUNTIME_DATA_BIND_OK;
  }

  capacity = capacity ? capacity * 2 : 4;
  if (capacity < needed) {
    capacity = needed;
  }

  entries = (turbo_runtime_data_bind_entry_t *)realloc(
      value->as.container.entries, capacity * sizeof(*entries));
  if (!entries) {
    return TURBO_RUNTIME_DATA_BIND_OUT_OF_MEMORY;
  }

  memset(entries + value->as.container.capacity, 0,
         (capacity - value->as.container.capacity) * sizeof(*entries));
  value->as.container.entries = entries;
  value->as.container.capacity = capacity;
  return TURBO_RUNTIME_DATA_BIND_OK;
}

static turbo_runtime_data_bind_status_t
turbo_runtime_data_bind_default_object_set(void *user_data,
                                           turbo_runtime_data_bind_value_t *object,
                                           const char *key,
                                           turbo_runtime_data_bind_value_t *value);

static turbo_runtime_data_bind_status_t
turbo_runtime_data_bind_default_array_append(void *user_data,
                                             turbo_runtime_data_bind_value_t *array,
                                             turbo_runtime_data_bind_value_t *value);
static turbo_runtime_data_bind_status_t
turbo_runtime_data_bind_json_object_set(void *user_data,
                                        turbo_runtime_data_bind_value_t *object,
                                        const char *key,
                                        turbo_runtime_data_bind_value_t *value);
static turbo_runtime_data_bind_status_t
turbo_runtime_data_bind_json_array_append(void *user_data,
                                          turbo_runtime_data_bind_value_t *array,
                                          turbo_runtime_data_bind_value_t *value);

static turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_default_create_null(void *user_data) {
  (void)user_data;
  return turbo_runtime_data_bind_value_create_null();
}

static turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_default_create_bool(void *user_data, int value) {
  (void)user_data;
  return turbo_runtime_data_bind_value_create_bool(value);
}

static turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_default_create_int64(void *user_data, int64_t value) {
  (void)user_data;
  return turbo_runtime_data_bind_value_create_int64(value);
}

static turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_default_create_double(void *user_data, double value) {
  (void)user_data;
  return turbo_runtime_data_bind_value_create_double(value);
}

static turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_default_create_string(void *user_data, const char *value) {
  (void)user_data;
  return turbo_runtime_data_bind_value_create_string(value);
}

static turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_default_create_bytes(void *user_data, const uint8_t *data,
                                             size_t size) {
  (void)user_data;
  return turbo_runtime_data_bind_value_create_bytes(data, size);
}

static turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_default_create_object(void *user_data) {
  (void)user_data;
  return turbo_runtime_data_bind_value_create_object();
}

static turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_default_create_array(void *user_data) {
  (void)user_data;
  return turbo_runtime_data_bind_value_create_array();
}

static void turbo_runtime_data_bind_default_destroy_value(void *user_data,
                                                          turbo_runtime_data_bind_value_t *value) {
  (void)user_data;
  turbo_runtime_data_bind_value_destroy(value);
}

static turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_json_create_null(void *user_data) {
  (void)user_data;
  return (turbo_runtime_data_bind_value_t *)turbo_json_create_null();
}

static turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_json_create_bool(void *user_data, int value) {
  (void)user_data;
  return (turbo_runtime_data_bind_value_t *)turbo_json_create_bool(value ? true : false);
}

static turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_json_create_int64(void *user_data, int64_t value) {
  (void)user_data;
  return (turbo_runtime_data_bind_value_t *)turbo_json_create_number((double)value);
}

static turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_json_create_double(void *user_data, double value) {
  (void)user_data;
  return (turbo_runtime_data_bind_value_t *)turbo_json_create_number(value);
}

static turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_json_create_string(void *user_data, const char *value) {
  (void)user_data;
  return (turbo_runtime_data_bind_value_t *)turbo_json_create_string(value);
}

static turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_json_create_bytes(void *user_data, const uint8_t *data,
                                          size_t size) {
  (void)user_data;
  (void)data;
  (void)size;
  return NULL;
}

static turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_json_create_object(void *user_data) {
  (void)user_data;
  return (turbo_runtime_data_bind_value_t *)turbo_json_create_object();
}

static turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_json_create_array(void *user_data) {
  (void)user_data;
  return (turbo_runtime_data_bind_value_t *)turbo_json_create_array();
}

static void turbo_runtime_data_bind_json_destroy_value(void *user_data,
                                                       turbo_runtime_data_bind_value_t *value) {
  json_value_t *json_value;
  (void)user_data;

  json_value = (json_value_t *)value;
  turbo_free_json(&json_value);
}

static const turbo_runtime_data_bind_value_api_t
    turbo_runtime_data_bind_default_api = {
        turbo_runtime_data_bind_default_create_null,
        turbo_runtime_data_bind_default_create_bool,
        turbo_runtime_data_bind_default_create_int64,
        turbo_runtime_data_bind_default_create_double,
        turbo_runtime_data_bind_default_create_string,
        turbo_runtime_data_bind_default_create_bytes,
        turbo_runtime_data_bind_default_create_object,
        turbo_runtime_data_bind_default_create_array,
        turbo_runtime_data_bind_default_object_set,
        turbo_runtime_data_bind_default_array_append,
        turbo_runtime_data_bind_default_destroy_value};

static const turbo_runtime_data_bind_value_api_t turbo_runtime_data_bind_json_api = {
    turbo_runtime_data_bind_json_create_null,
    turbo_runtime_data_bind_json_create_bool,
    turbo_runtime_data_bind_json_create_int64,
    turbo_runtime_data_bind_json_create_double,
    turbo_runtime_data_bind_json_create_string,
    NULL,
    turbo_runtime_data_bind_json_create_object,
    turbo_runtime_data_bind_json_create_array,
    turbo_runtime_data_bind_json_object_set,
    turbo_runtime_data_bind_json_array_append,
    turbo_runtime_data_bind_json_destroy_value};

const turbo_runtime_data_bind_value_api_t *
turbo_runtime_data_bind_default_value_api(void) {
  return &turbo_runtime_data_bind_default_api;
}

const turbo_runtime_data_bind_value_api_t *
turbo_runtime_data_bind_json_value_api(void) {
  return &turbo_runtime_data_bind_json_api;
}

turbo_runtime_data_bind_value_t *turbo_runtime_data_bind_value_create_null(void) {
  return turbo_runtime_data_bind_value_alloc(TURBO_RUNTIME_DATA_BIND_VALUE_NULL);
}

turbo_runtime_data_bind_value_t *turbo_runtime_data_bind_value_create_bool(int value) {
  turbo_runtime_data_bind_value_t *node =
      turbo_runtime_data_bind_value_alloc(TURBO_RUNTIME_DATA_BIND_VALUE_BOOL);
  if (!node) {
    return NULL;
  }
  node->as.bool_value = value ? 1 : 0;
  return node;
}

turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_value_create_int64(int64_t value) {
  turbo_runtime_data_bind_value_t *node =
      turbo_runtime_data_bind_value_alloc(TURBO_RUNTIME_DATA_BIND_VALUE_INT64);
  if (!node) {
    return NULL;
  }
  node->as.int64_value = value;
  return node;
}

turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_value_create_double(double value) {
  turbo_runtime_data_bind_value_t *node =
      turbo_runtime_data_bind_value_alloc(TURBO_RUNTIME_DATA_BIND_VALUE_DOUBLE);
  if (!node) {
    return NULL;
  }
  node->as.double_value = value;
  return node;
}

turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_value_create_string(const char *value) {
  turbo_runtime_data_bind_value_t *node;
  char *copy;

  if (!value) {
    return NULL;
  }

  copy = turbo_runtime_data_bind_strdup(value);
  if (!copy) {
    return NULL;
  }

  node = turbo_runtime_data_bind_value_alloc(TURBO_RUNTIME_DATA_BIND_VALUE_STRING);
  if (!node) {
    free(copy);
    return NULL;
  }

  node->as.string_value.data = copy;
  node->as.string_value.size = tstr_len(copy);
  return node;
}

turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_value_create_bytes(const uint8_t *data, size_t size) {
  turbo_runtime_data_bind_value_t *node;
  uint8_t *copy = NULL;

  if (!data && size != 0) {
    return NULL;
  }

  if (size != 0) {
    copy = (uint8_t *)turbo_runtime_data_bind_memdup(data, size);
    if (!copy) {
      return NULL;
    }
  }

  node = turbo_runtime_data_bind_value_alloc(TURBO_RUNTIME_DATA_BIND_VALUE_BYTES);
  if (!node) {
    free(copy);
    return NULL;
  }

  node->as.bytes_value.data = copy;
  node->as.bytes_value.size = size;
  return node;
}

turbo_runtime_data_bind_value_t *turbo_runtime_data_bind_value_create_object(void) {
  return turbo_runtime_data_bind_value_alloc(TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT);
}

turbo_runtime_data_bind_value_t *turbo_runtime_data_bind_value_create_array(void) {
  return turbo_runtime_data_bind_value_alloc(TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY);
}

void turbo_runtime_data_bind_value_destroy(turbo_runtime_data_bind_value_t *value) {
  size_t i;

  if (!value) {
    return;
  }

  if (value->kind == TURBO_RUNTIME_DATA_BIND_VALUE_STRING) {
    tstr_free(value->as.string_value.data);
  }

  if (value->kind == TURBO_RUNTIME_DATA_BIND_VALUE_BYTES) {
    free(value->as.bytes_value.data);
  }

  if (value->kind == TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT ||
      value->kind == TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    for (i = 0; i < value->as.container.count; ++i) {
      tstr_free(value->as.container.entries[i].key);
      turbo_runtime_data_bind_value_destroy(value->as.container.entries[i].value);
    }
    free(value->as.container.entries);
  }

  free(value);
}

turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_value_clone(const turbo_runtime_data_bind_value_t *value) {
  turbo_runtime_data_bind_value_t *copy;
  size_t i;

  if (!value) {
    return NULL;
  }

  switch (value->kind) {
    case TURBO_RUNTIME_DATA_BIND_VALUE_NULL:
      return turbo_runtime_data_bind_value_create_null();
    case TURBO_RUNTIME_DATA_BIND_VALUE_BOOL:
      return turbo_runtime_data_bind_value_create_bool(value->as.bool_value);
    case TURBO_RUNTIME_DATA_BIND_VALUE_INT64:
      return turbo_runtime_data_bind_value_create_int64(value->as.int64_value);
    case TURBO_RUNTIME_DATA_BIND_VALUE_DOUBLE:
      return turbo_runtime_data_bind_value_create_double(value->as.double_value);
    case TURBO_RUNTIME_DATA_BIND_VALUE_STRING:
      return turbo_runtime_data_bind_value_create_string(value->as.string_value.data);
    case TURBO_RUNTIME_DATA_BIND_VALUE_BYTES:
      return turbo_runtime_data_bind_value_create_bytes(value->as.bytes_value.data,
                                                        value->as.bytes_value.size);
    case TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT:
      copy = turbo_runtime_data_bind_value_create_object();
      break;
    case TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY:
      copy = turbo_runtime_data_bind_value_create_array();
      break;
    default:
      return NULL;
  }

  if (!copy) {
    return NULL;
  }

  for (i = 0; i < value->as.container.count; ++i) {
    turbo_runtime_data_bind_value_t *child =
        turbo_runtime_data_bind_value_clone(value->as.container.entries[i].value);

    if (!child) {
      turbo_runtime_data_bind_value_destroy(copy);
      return NULL;
    }

    if (value->kind == TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
      if (turbo_runtime_data_bind_object_set(copy, value->as.container.entries[i].key, child) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
        turbo_runtime_data_bind_value_destroy(child);
        turbo_runtime_data_bind_value_destroy(copy);
        return NULL;
      }
    } else {
      if (turbo_runtime_data_bind_array_append(copy, child) != TURBO_RUNTIME_DATA_BIND_OK) {
        turbo_runtime_data_bind_value_destroy(child);
        turbo_runtime_data_bind_value_destroy(copy);
        return NULL;
      }
    }
  }

  return copy;
}

turbo_runtime_data_bind_value_kind_t
turbo_runtime_data_bind_value_kind(const turbo_runtime_data_bind_value_t *value) {
  if (!value) {
    return TURBO_RUNTIME_DATA_BIND_VALUE_NULL;
  }
  return value->kind;
}

turbo_runtime_data_bind_status_t turbo_runtime_data_bind_object_set(
    turbo_runtime_data_bind_value_t *object, const char *key,
    turbo_runtime_data_bind_value_t *field_value) {
  size_t i;
  char *key_copy;
  turbo_runtime_data_bind_status_t status;

  if (!object || !key || !field_value) {
    return TURBO_RUNTIME_DATA_BIND_INVALID_ARGUMENT;
  }

  if (object->kind != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    return TURBO_RUNTIME_DATA_BIND_TYPE_MISMATCH;
  }

  for (i = 0; i < object->as.container.count; ++i) {
    if (strcmp(object->as.container.entries[i].key, key) != 0) {
      continue;
    }

    turbo_runtime_data_bind_value_destroy(object->as.container.entries[i].value);
    object->as.container.entries[i].value = field_value;
    return TURBO_RUNTIME_DATA_BIND_OK;
  }

  status = turbo_runtime_data_bind_container_reserve(
      object, object->as.container.count + 1);
  if (status != TURBO_RUNTIME_DATA_BIND_OK) {
    return status;
  }

  key_copy = turbo_runtime_data_bind_strdup(key);
  if (!key_copy) {
    return TURBO_RUNTIME_DATA_BIND_OUT_OF_MEMORY;
  }

  i = object->as.container.count++;
  object->as.container.entries[i].key = key_copy;
  object->as.container.entries[i].value = field_value;
  return TURBO_RUNTIME_DATA_BIND_OK;
}

turbo_runtime_data_bind_status_t turbo_runtime_data_bind_array_append(
    turbo_runtime_data_bind_value_t *array, turbo_runtime_data_bind_value_t *element_value) {
  turbo_runtime_data_bind_status_t status;
  size_t i;

  if (!array || !element_value) {
    return TURBO_RUNTIME_DATA_BIND_INVALID_ARGUMENT;
  }

  if (array->kind != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    return TURBO_RUNTIME_DATA_BIND_TYPE_MISMATCH;
  }

  status = turbo_runtime_data_bind_container_reserve(
      array, array->as.container.count + 1);
  if (status != TURBO_RUNTIME_DATA_BIND_OK) {
    return status;
  }

  i = array->as.container.count++;
  array->as.container.entries[i].value = element_value;
  return TURBO_RUNTIME_DATA_BIND_OK;
}

size_t turbo_runtime_data_bind_value_size(const turbo_runtime_data_bind_value_t *value) {
  if (!value) {
    return 0;
  }

  if (value->kind != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT &&
      value->kind != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    return 0;
  }

  return value->as.container.count;
}

const turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_object_get(const turbo_runtime_data_bind_value_t *object,
                                   const char *key) {
  size_t i;

  if (!object || !key || object->kind != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    return NULL;
  }

  for (i = 0; i < object->as.container.count; ++i) {
    if (strcmp(object->as.container.entries[i].key, key) == 0) {
      return object->as.container.entries[i].value;
    }
  }

  return NULL;
}

const char *turbo_runtime_data_bind_object_key_at(
    const turbo_runtime_data_bind_value_t *object, size_t index) {
  if (!object || object->kind != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT ||
      index >= object->as.container.count) {
    return NULL;
  }

  return object->as.container.entries[index].key;
}

const turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_array_get(const turbo_runtime_data_bind_value_t *array, size_t index) {
  if (!array || array->kind != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY ||
      index >= array->as.container.count) {
    return NULL;
  }

  return array->as.container.entries[index].value;
}

int turbo_runtime_data_bind_value_as_bool(const turbo_runtime_data_bind_value_t *value,
                                          int default_value) {
  if (!value || value->kind != TURBO_RUNTIME_DATA_BIND_VALUE_BOOL) {
    return default_value;
  }
  return value->as.bool_value;
}

int64_t turbo_runtime_data_bind_value_as_int64(const turbo_runtime_data_bind_value_t *value,
                                               int64_t default_value) {
  if (!value || value->kind != TURBO_RUNTIME_DATA_BIND_VALUE_INT64) {
    return default_value;
  }
  return value->as.int64_value;
}

double turbo_runtime_data_bind_value_as_double(const turbo_runtime_data_bind_value_t *value,
                                               double default_value) {
  if (!value || value->kind != TURBO_RUNTIME_DATA_BIND_VALUE_DOUBLE) {
    return default_value;
  }
  return value->as.double_value;
}

const char *turbo_runtime_data_bind_value_as_string(
    const turbo_runtime_data_bind_value_t *value) {
  if (!value || value->kind != TURBO_RUNTIME_DATA_BIND_VALUE_STRING) {
    return NULL;
  }
  return value->as.string_value.data;
}

const uint8_t *turbo_runtime_data_bind_value_as_bytes(
    const turbo_runtime_data_bind_value_t *value) {
  if (!value || value->kind != TURBO_RUNTIME_DATA_BIND_VALUE_BYTES) {
    return NULL;
  }
  return value->as.bytes_value.data;
}

size_t turbo_runtime_data_bind_value_data_size(
    const turbo_runtime_data_bind_value_t *value) {
  if (!value) {
    return 0;
  }

  if (value->kind == TURBO_RUNTIME_DATA_BIND_VALUE_STRING) {
    return value->as.string_value.size;
  }

  if (value->kind == TURBO_RUNTIME_DATA_BIND_VALUE_BYTES) {
    return value->as.bytes_value.size;
  }

  return 0;
}

json_value_t *turbo_runtime_data_bind_value_to_json(
    const turbo_runtime_data_bind_value_t *value) {
  json_value_t *json_value;
  size_t i;

  if (!value) {
    return NULL;
  }

  switch (value->kind) {
    case TURBO_RUNTIME_DATA_BIND_VALUE_NULL:
      return turbo_json_create_null();
    case TURBO_RUNTIME_DATA_BIND_VALUE_BOOL:
      return turbo_json_create_bool(value->as.bool_value ? true : false);
    case TURBO_RUNTIME_DATA_BIND_VALUE_INT64:
      return turbo_json_create_number((double)value->as.int64_value);
    case TURBO_RUNTIME_DATA_BIND_VALUE_DOUBLE:
      return turbo_json_create_number(value->as.double_value);
    case TURBO_RUNTIME_DATA_BIND_VALUE_STRING:
      return turbo_json_create_string(value->as.string_value.data);
    case TURBO_RUNTIME_DATA_BIND_VALUE_BYTES:
      return NULL;
    case TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY:
      json_value = turbo_json_create_array();
      if (!json_value) {
        return NULL;
      }
      for (i = 0; i < value->as.container.count; ++i) {
        json_value_t *child =
            turbo_runtime_data_bind_value_to_json(value->as.container.entries[i].value);
        if (!child) {
          turbo_free_json(&json_value);
          return NULL;
        }
        turbo_json_array_add(json_value, child);
      }
      return json_value;
    case TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT:
      json_value = turbo_json_create_object();
      if (!json_value) {
        return NULL;
      }
      for (i = 0; i < value->as.container.count; ++i) {
        json_value_t *child =
            turbo_runtime_data_bind_value_to_json(value->as.container.entries[i].value);
        if (!child) {
          turbo_free_json(&json_value);
          return NULL;
        }
        turbo_json_object_add(json_value, value->as.container.entries[i].key, child);
      }
      return json_value;
  }

  return NULL;
}

turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_value_from_json(const json_value_t *value) {
  turbo_runtime_data_bind_value_t *node;
  size_t i;

  if (!value) {
    return NULL;
  }

  switch (turbo_json_type(value)) {
    case TURBO_JSON_NULL:
      return turbo_runtime_data_bind_value_create_null();
    case TURBO_JSON_BOOL:
      return turbo_runtime_data_bind_value_create_bool(turbo_json_bool(value));
    case TURBO_JSON_NUMBER: {
      double number = turbo_json_number(value);
      int64_t int_value = (int64_t)number;
      if (number >= (double)INT64_MIN && number <= (double)INT64_MAX &&
          (double)int_value == number) {
        return turbo_runtime_data_bind_value_create_int64(int_value);
      }
      return turbo_runtime_data_bind_value_create_double(number);
    }
    case TURBO_JSON_STRING:
      return turbo_runtime_data_bind_value_create_string(turbo_json_string(value));
    case TURBO_JSON_ARRAY:
      node = turbo_runtime_data_bind_value_create_array();
      if (!node) {
        return NULL;
      }
      for (i = 0; i < turbo_json_array_size(value); ++i) {
        turbo_runtime_data_bind_value_t *child =
            turbo_runtime_data_bind_value_from_json(turbo_json_array_get(value, i));
        if (!child) {
          turbo_runtime_data_bind_value_destroy(node);
          return NULL;
        }
        if (turbo_runtime_data_bind_array_append(node, child) !=
            TURBO_RUNTIME_DATA_BIND_OK) {
          turbo_runtime_data_bind_value_destroy(child);
          turbo_runtime_data_bind_value_destroy(node);
          return NULL;
        }
      }
      return node;
    case TURBO_JSON_OBJECT:
      node = turbo_runtime_data_bind_value_create_object();
      if (!node) {
        return NULL;
      }
      for (i = 0; i < turbo_json_object_size(value); ++i) {
        turbo_runtime_data_bind_value_t *child = turbo_runtime_data_bind_value_from_json(
            turbo_json_object_value(value, i));
        if (!child) {
          turbo_runtime_data_bind_value_destroy(node);
          return NULL;
        }
        if (turbo_runtime_data_bind_object_set(
                node, turbo_json_object_key(value, i), child) !=
            TURBO_RUNTIME_DATA_BIND_OK) {
          turbo_runtime_data_bind_value_destroy(child);
          turbo_runtime_data_bind_value_destroy(node);
          return NULL;
        }
      }
      return node;
  }

  return NULL;
}

static turbo_runtime_data_bind_status_t
turbo_runtime_data_bind_default_object_set(void *user_data,
                                           turbo_runtime_data_bind_value_t *object,
                                           const char *key,
                                           turbo_runtime_data_bind_value_t *value) {
  (void)user_data;
  return turbo_runtime_data_bind_object_set(object, key, value);
}

static turbo_runtime_data_bind_status_t
turbo_runtime_data_bind_default_array_append(void *user_data,
                                             turbo_runtime_data_bind_value_t *array,
                                             turbo_runtime_data_bind_value_t *value) {
  (void)user_data;
  return turbo_runtime_data_bind_array_append(array, value);
}

static turbo_runtime_data_bind_status_t
turbo_runtime_data_bind_json_object_set(void *user_data,
                                        turbo_runtime_data_bind_value_t *object,
                                        const char *key,
                                        turbo_runtime_data_bind_value_t *value) {
  (void)user_data;
  if (!object || !key || !value) {
    return TURBO_RUNTIME_DATA_BIND_INVALID_ARGUMENT;
  }
  turbo_json_object_add((json_value_t *)object, key, (json_value_t *)value);
  return TURBO_RUNTIME_DATA_BIND_OK;
}

static turbo_runtime_data_bind_status_t
turbo_runtime_data_bind_json_array_append(void *user_data,
                                          turbo_runtime_data_bind_value_t *array,
                                          turbo_runtime_data_bind_value_t *value) {
  (void)user_data;
  if (!array || !value) {
    return TURBO_RUNTIME_DATA_BIND_INVALID_ARGUMENT;
  }
  turbo_json_array_add((json_value_t *)array, (json_value_t *)value);
  return TURBO_RUNTIME_DATA_BIND_OK;
}
