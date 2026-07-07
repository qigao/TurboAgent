#ifndef TURBO_RUNTIME_BINARY_SCHEMA_H
#define TURBO_RUNTIME_BINARY_SCHEMA_H


#include <platform.h>

#include "turbo_runtime_data_bind.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_runtime_binary_schema_s turbo_runtime_binary_schema_t;

typedef enum {
  TURBO_RUNTIME_BINARY_SCHEMA_OK = 0,
  TURBO_RUNTIME_BINARY_SCHEMA_ERROR = -1,
  TURBO_RUNTIME_BINARY_SCHEMA_INVALID_ARGUMENT = -2,
  TURBO_RUNTIME_BINARY_SCHEMA_OUT_OF_MEMORY = -3,
  TURBO_RUNTIME_BINARY_SCHEMA_UNSUPPORTED = -4,
  TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED = -5
} turbo_runtime_binary_schema_status_t;

typedef enum {
  TURBO_RUNTIME_BINARY_VALUE_BOOL = 0,
  TURBO_RUNTIME_BINARY_VALUE_I32 = 1,
  TURBO_RUNTIME_BINARY_VALUE_I64 = 2,
  TURBO_RUNTIME_BINARY_VALUE_F32 = 3,
  TURBO_RUNTIME_BINARY_VALUE_F64 = 4,
  TURBO_RUNTIME_BINARY_VALUE_STRING16 = 5,
  TURBO_RUNTIME_BINARY_VALUE_BYTES16 = 6
} turbo_runtime_binary_value_type_t;

typedef enum {
  TURBO_RUNTIME_BINARY_FIELD_SCALAR = 0,
  TURBO_RUNTIME_BINARY_FIELD_REPEATED = 1,
  TURBO_RUNTIME_BINARY_FIELD_STRING_KEY_MAP = 2,
  TURBO_RUNTIME_BINARY_FIELD_OBJECT = 3
} turbo_runtime_binary_field_shape_t;

typedef struct turbo_runtime_binary_field_descriptor_s {
  const char *name;
  turbo_runtime_binary_field_shape_t shape;
  turbo_runtime_binary_value_type_t value_type;
  const turbo_runtime_binary_schema_t *object_schema;
} turbo_runtime_binary_field_descriptor_t;

typedef struct turbo_runtime_binary_abi_requirements_s {
  int create_bool;
  int create_int64;
  int create_double;
  int create_string;
  int create_bytes;
  int create_object;
  int create_array;
  int object_set;
  int array_append;
  int destroy_value;
} turbo_runtime_binary_abi_requirements_t;

CXX_C_API turbo_runtime_binary_schema_t *
turbo_runtime_binary_schema_create(const char *name);
CXX_C_API turbo_runtime_binary_schema_t *
turbo_runtime_binary_schema_clone(const turbo_runtime_binary_schema_t *schema);
CXX_C_API void turbo_runtime_binary_schema_destroy(
    turbo_runtime_binary_schema_t *schema);
CXX_C_API const char *turbo_runtime_binary_schema_name(
    const turbo_runtime_binary_schema_t *schema);
CXX_C_API size_t turbo_runtime_binary_schema_field_count(
    const turbo_runtime_binary_schema_t *schema);
CXX_C_API turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_get_field(
    const turbo_runtime_binary_schema_t *schema, size_t index,
    turbo_runtime_binary_field_descriptor_t *out_field);
CXX_C_API turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_add_scalar_field(
    turbo_runtime_binary_schema_t *schema, const char *name,
    turbo_runtime_binary_value_type_t value_type);
CXX_C_API turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_add_repeated_field(
    turbo_runtime_binary_schema_t *schema, const char *name,
    turbo_runtime_binary_value_type_t value_type);
CXX_C_API turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_add_string_key_map_field(
    turbo_runtime_binary_schema_t *schema, const char *name,
    turbo_runtime_binary_value_type_t value_type);
CXX_C_API turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_add_object_field(
    turbo_runtime_binary_schema_t *schema, const char *name,
    const turbo_runtime_binary_schema_t *object_schema);
CXX_C_API turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_collect_abi_requirements(
    const turbo_runtime_binary_schema_t *schema,
    turbo_runtime_binary_abi_requirements_t *out_requirements);
CXX_C_API turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_validate_value_api(
    const turbo_runtime_binary_schema_t *schema,
    const turbo_runtime_data_bind_value_api_t *api, char *error_buffer,
    size_t error_buffer_size);
CXX_C_API turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_validate_payload(
    const turbo_runtime_binary_schema_t *schema, const uint8_t *data,
    size_t size, char *error_buffer, size_t error_buffer_size);

/**
 * @brief Parse one payload through the schema into a value-builder backend.
 *
 * Returns a root object value on success. The caller owns the returned value and
 * must release it through `api->destroy_value(user_data, value)`.
 */
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_runtime_binary_schema_parse(
    const turbo_runtime_binary_schema_t *schema, const uint8_t *data,
    size_t size, const turbo_runtime_data_bind_value_api_t *api,
    void *builder_user_data, char *error_buffer, size_t error_buffer_size);

#ifdef __cplusplus
}
#endif

#endif
