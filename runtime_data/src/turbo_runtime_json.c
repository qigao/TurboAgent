#include "turbo_runtime_json.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>

#define TURBO_RUNTIME_JSON_INT64_UPPER_EXCLUSIVE 9223372036854775808.0

int turbo_runtime_json_parse(const uint8_t *data, size_t len,
                             json_value_t **out_value) {
  json_value_t *value;

  if (!data || !out_value) {
    return TURBO_RUNTIME_JSON_INVALID_ARGUMENT;
  }
  *out_value = NULL;
  value = json_parse((const char *)data, len);
  if (!value) {
    return TURBO_RUNTIME_JSON_ERROR;
  }
  *out_value = value;
  return TURBO_RUNTIME_JSON_OK;
}

void turbo_runtime_json_reset(json_value_t **value) {
  if (!value || !*value) {
    return;
  }
  json_free(*value);
  *value = NULL;
}

void turbo_runtime_json_destroy(json_value_t *value) {
  json_free(value);
}

turbo_runtime_json_status_t turbo_runtime_json_object_set(
    json_value_t *object, const char *key, json_value_t *field_value) {
  if (!object || !key || !field_value) {
    return TURBO_RUNTIME_JSON_INVALID_ARGUMENT;
  }
  if (json_type(object) != JSON_OBJECT) {
    return TURBO_RUNTIME_JSON_TYPE_MISMATCH;
  }
  return json_object_add_checked(object, key, field_value)
             ? TURBO_RUNTIME_JSON_OK
             : TURBO_RUNTIME_JSON_OUT_OF_MEMORY;
}

turbo_runtime_json_status_t turbo_runtime_json_array_append(
    json_value_t *array, json_value_t *element_value) {
  if (!array || !element_value) {
    return TURBO_RUNTIME_JSON_INVALID_ARGUMENT;
  }
  if (json_type(array) != JSON_ARRAY) {
    return TURBO_RUNTIME_JSON_TYPE_MISMATCH;
  }
  return json_array_add_checked(array, element_value)
             ? TURBO_RUNTIME_JSON_OK
             : TURBO_RUNTIME_JSON_OUT_OF_MEMORY;
}

size_t turbo_runtime_json_value_size(const json_value_t *value) {
  if (!value) {
    return 0;
  }
  switch (json_type(value)) {
    case JSON_OBJECT:
      return json_object_size(value);
    case JSON_ARRAY:
      return json_array_size(value);
    default:
      return 0;
  }
}

int turbo_runtime_json_value_as_bool(const json_value_t *value, int default_value) {
  return value && json_type(value) == JSON_BOOL
             ? (json_bool(value) ? 1 : 0)
             : default_value;
}

int64_t turbo_runtime_json_value_as_int64(const json_value_t *value,
                                          int64_t default_value) {
  char integer_text[22];
  const char *number_text;
  char *end;
  size_t number_text_length = 0;
  intmax_t integer_value;
  double number;

  if (!value || json_type(value) != JSON_NUMBER) {
    return default_value;
  }
  number_text = json_number_text(value, &number_text_length);
  if (number_text && number_text_length > 0 &&
      number_text_length < sizeof(integer_text) &&
      !memchr(number_text, '.', number_text_length) &&
      !memchr(number_text, 'e', number_text_length) &&
      !memchr(number_text, 'E', number_text_length)) {
    memcpy(integer_text, number_text, number_text_length);
    integer_text[number_text_length] = '\0';
    errno = 0;
    integer_value = strtoimax(integer_text, &end, 10);
    if (errno == 0 && end == integer_text + number_text_length &&
        integer_value >= INT64_MIN && integer_value <= INT64_MAX) {
      return (int64_t)integer_value;
    }
    return default_value;
  }

  number = json_number(value);
  if (!isfinite(number) || trunc(number) != number ||
      number < -TURBO_RUNTIME_JSON_INT64_UPPER_EXCLUSIVE ||
      number >= TURBO_RUNTIME_JSON_INT64_UPPER_EXCLUSIVE) {
    return default_value;
  }
  return (int64_t)number;
}

double turbo_runtime_json_value_as_double(const json_value_t *value,
                                          double default_value) {
  return value && json_type(value) == JSON_NUMBER
             ? json_number(value)
             : default_value;
}

const char *turbo_runtime_json_value_as_string(const json_value_t *value) {
  return value && json_type(value) == JSON_STRING
             ? json_string(value)
             : NULL;
}
