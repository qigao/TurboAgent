#ifndef TURBO_RUNTIME_JSON_H
#define TURBO_RUNTIME_JSON_H

#include <platform.h>
#include <turbo_parser.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum turbo_runtime_json_status_e {
  TURBO_RUNTIME_JSON_OK = 0,
  TURBO_RUNTIME_JSON_ERROR = -1,
  TURBO_RUNTIME_JSON_INVALID_ARGUMENT = -2,
  TURBO_RUNTIME_JSON_OUT_OF_MEMORY = -3,
  TURBO_RUNTIME_JSON_TYPE_MISMATCH = -4,
  TURBO_RUNTIME_JSON_NOT_FOUND = -5
} turbo_runtime_json_status_t;

/** Release one owned TurboParser JSON tree. */
CXX_C_API void turbo_runtime_json_destroy(json_value_t *value);

/** Set or replace an object member. Ownership transfers only on success. */
CXX_C_API turbo_runtime_json_status_t turbo_runtime_json_object_set(
    json_value_t *object, const char *key, json_value_t *field_value);

/** Append an array element. Ownership transfers only on success. */
CXX_C_API turbo_runtime_json_status_t turbo_runtime_json_array_append(
    json_value_t *array, json_value_t *element_value);

/** Return object/array element count, or zero for other JSON kinds. */
CXX_C_API size_t turbo_runtime_json_value_size(const json_value_t *value);

/** Read a boolean or return default_value for another JSON kind. */
CXX_C_API int turbo_runtime_json_value_as_bool(const json_value_t *value,
                                               int default_value);

/** Read an exactly representable integral JSON number, otherwise return default_value. */
CXX_C_API int64_t turbo_runtime_json_value_as_int64(const json_value_t *value,
                                                    int64_t default_value);

/** Read a JSON number or return default_value for another JSON kind. */
CXX_C_API double turbo_runtime_json_value_as_double(const json_value_t *value,
                                                    double default_value);

/** Return a borrowed JSON string, or NULL for another JSON kind. */
CXX_C_API const char *turbo_runtime_json_value_as_string(const json_value_t *value);

#ifdef __cplusplus
}
#endif

#endif
