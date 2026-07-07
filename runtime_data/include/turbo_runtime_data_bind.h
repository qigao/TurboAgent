#ifndef TURBO_RUNTIME_DATA_BIND_H
#define TURBO_RUNTIME_DATA_BIND_H

#include <platform.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_runtime_data_bind_value_s turbo_runtime_data_bind_value_t;
typedef struct json_value_s json_value_t;

typedef enum {
  TURBO_RUNTIME_DATA_BIND_OK = 0,
  TURBO_RUNTIME_DATA_BIND_ERROR = -1,
  TURBO_RUNTIME_DATA_BIND_INVALID_ARGUMENT = -2,
  TURBO_RUNTIME_DATA_BIND_OUT_OF_MEMORY = -3,
  TURBO_RUNTIME_DATA_BIND_TYPE_MISMATCH = -4,
  TURBO_RUNTIME_DATA_BIND_NOT_FOUND = -5
} turbo_runtime_data_bind_status_t;

typedef enum {
  TURBO_RUNTIME_DATA_BIND_VALUE_NULL = 0,
  TURBO_RUNTIME_DATA_BIND_VALUE_BOOL = 1,
  TURBO_RUNTIME_DATA_BIND_VALUE_INT64 = 2,
  TURBO_RUNTIME_DATA_BIND_VALUE_DOUBLE = 3,
  TURBO_RUNTIME_DATA_BIND_VALUE_STRING = 4,
  TURBO_RUNTIME_DATA_BIND_VALUE_BYTES = 5,
  TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT = 6,
  TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY = 7
} turbo_runtime_data_bind_value_kind_t;

/**
 * @brief Builder callbacks for schema-driven runtime codecs.
 *
 * This is the stable surface future MIR-backed codecs should target. Hosts may
 * provide their own value implementation, or they may reuse the default value
 * tree exported by this module.
 */
typedef struct turbo_runtime_data_bind_value_api_s {
  turbo_runtime_data_bind_value_t *(*create_null)(void *user_data);
  turbo_runtime_data_bind_value_t *(*create_bool)(void *user_data, int value);
  turbo_runtime_data_bind_value_t *(*create_int64)(void *user_data, int64_t value);
  turbo_runtime_data_bind_value_t *(*create_double)(void *user_data, double value);
  turbo_runtime_data_bind_value_t *(*create_string)(void *user_data, const char *value);
  turbo_runtime_data_bind_value_t *(*create_bytes)(void *user_data, const uint8_t *data,
                                                   size_t size);
  turbo_runtime_data_bind_value_t *(*create_object)(void *user_data);
  turbo_runtime_data_bind_value_t *(*create_array)(void *user_data);
  turbo_runtime_data_bind_status_t (*object_set)(void *user_data,
                                                 turbo_runtime_data_bind_value_t *object,
                                                 const char *key,
                                                 turbo_runtime_data_bind_value_t *value);
  turbo_runtime_data_bind_status_t (*array_append)(void *user_data,
                                                   turbo_runtime_data_bind_value_t *array,
                                                   turbo_runtime_data_bind_value_t *value);
  void (*destroy_value)(void *user_data, turbo_runtime_data_bind_value_t *value);
} turbo_runtime_data_bind_value_api_t;

/**
 * @brief Return the default in-memory value implementation as a builder API.
 */
CXX_C_API const turbo_runtime_data_bind_value_api_t *
turbo_runtime_data_bind_default_value_api(void);

/**
 * @brief Return a builder API that materializes values directly as `json_value_t`.
 *
 * The JSON adapter does not support raw bytes. `create_bytes` returns NULL for
 * that backend.
 */
CXX_C_API const turbo_runtime_data_bind_value_api_t *
turbo_runtime_data_bind_json_value_api(void);

/**
 * @brief Create one null value.
 */
CXX_C_API turbo_runtime_data_bind_value_t *turbo_runtime_data_bind_value_create_null(void);

/**
 * @brief Create one boolean value.
 */
CXX_C_API turbo_runtime_data_bind_value_t *turbo_runtime_data_bind_value_create_bool(int value);

/**
 * @brief Create one int64 value.
 */
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_value_create_int64(int64_t value);

/**
 * @brief Create one double value.
 */
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_value_create_double(double value);

/**
 * @brief Create one string value by copying the input.
 */
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_value_create_string(const char *value);

/**
 * @brief Create one bytes value by copying the input buffer.
 */
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_value_create_bytes(const uint8_t *data, size_t size);

/**
 * @brief Create one object container.
 */
CXX_C_API turbo_runtime_data_bind_value_t *turbo_runtime_data_bind_value_create_object(void);

/**
 * @brief Create one array container.
 */
CXX_C_API turbo_runtime_data_bind_value_t *turbo_runtime_data_bind_value_create_array(void);

/**
 * @brief Destroy a value tree recursively.
 */
CXX_C_API void turbo_runtime_data_bind_value_destroy(turbo_runtime_data_bind_value_t *value);

/**
 * @brief Deep-clone one value tree recursively.
 */
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_value_clone(const turbo_runtime_data_bind_value_t *value);

/**
 * @brief Return the runtime kind of one value.
 */
CXX_C_API turbo_runtime_data_bind_value_kind_t
turbo_runtime_data_bind_value_kind(const turbo_runtime_data_bind_value_t *value);

/**
 * @brief Set or replace one object field. Ownership of `field_value` transfers on success.
 */
CXX_C_API turbo_runtime_data_bind_status_t turbo_runtime_data_bind_object_set(
    turbo_runtime_data_bind_value_t *object, const char *key,
    turbo_runtime_data_bind_value_t *field_value);

/**
 * @brief Append one element to an array. Ownership of `element_value` transfers on success.
 */
CXX_C_API turbo_runtime_data_bind_status_t turbo_runtime_data_bind_array_append(
    turbo_runtime_data_bind_value_t *array, turbo_runtime_data_bind_value_t *element_value);

/**
 * @brief Return the number of object fields or array elements.
 */
CXX_C_API size_t turbo_runtime_data_bind_value_size(
    const turbo_runtime_data_bind_value_t *value);

/**
 * @brief Lookup one object field by key.
 */
CXX_C_API const turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_object_get(const turbo_runtime_data_bind_value_t *object,
                                   const char *key);

/**
 * @brief Return one object field name by index.
 */
CXX_C_API const char *turbo_runtime_data_bind_object_key_at(
    const turbo_runtime_data_bind_value_t *object, size_t index);

/**
 * @brief Return one array element by index.
 */
CXX_C_API const turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_array_get(const turbo_runtime_data_bind_value_t *array,
                                  size_t index);

/**
 * @brief Read one boolean value.
 */
CXX_C_API int turbo_runtime_data_bind_value_as_bool(
    const turbo_runtime_data_bind_value_t *value, int default_value);

/**
 * @brief Read one int64 value.
 */
CXX_C_API int64_t turbo_runtime_data_bind_value_as_int64(
    const turbo_runtime_data_bind_value_t *value, int64_t default_value);

/**
 * @brief Read one double value.
 */
CXX_C_API double turbo_runtime_data_bind_value_as_double(
    const turbo_runtime_data_bind_value_t *value, double default_value);

/**
 * @brief Read one string value.
 */
CXX_C_API const char *turbo_runtime_data_bind_value_as_string(
    const turbo_runtime_data_bind_value_t *value);

/**
 * @brief Read one bytes payload pointer. Size may be queried separately.
 */
CXX_C_API const uint8_t *turbo_runtime_data_bind_value_as_bytes(
    const turbo_runtime_data_bind_value_t *value);

/**
 * @brief Return the stored string or bytes length.
 */
CXX_C_API size_t turbo_runtime_data_bind_value_data_size(
    const turbo_runtime_data_bind_value_t *value);

/**
 * @brief Convert one runtime value tree into a JSON DOM.
 *
 * Returns NULL if the value contains unsupported kinds such as raw bytes.
 */
CXX_C_API json_value_t *
turbo_runtime_data_bind_value_to_json(const turbo_runtime_data_bind_value_t *value);

/**
 * @brief Convert one JSON DOM into the default runtime value tree.
 */
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_runtime_data_bind_value_from_json(const json_value_t *value);

#ifdef __cplusplus
}
#endif

#endif
