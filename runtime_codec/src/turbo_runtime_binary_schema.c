#include "turbo_runtime_binary_schema.h"
#include "turbo_runtime_binary_reader.h"

#include <turbo_str.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *turbo_runtime_binary_schema_strdup(const char *value) {
  if (!value) {
    return NULL;
  }
  return (char *)tstr_dup(value);
}

typedef struct turbo_runtime_binary_field_s {
  char *name;
  turbo_runtime_binary_field_shape_t shape;
  turbo_runtime_binary_value_type_t value_type;
  turbo_runtime_binary_schema_t *object_schema;
} turbo_runtime_binary_field_t;

struct turbo_runtime_binary_schema_s {
  char *name;
  turbo_runtime_binary_field_t *fields;
  size_t field_count;
  size_t field_capacity;
};



static void turbo_runtime_binary_schema_write_error(char *buffer, size_t buffer_size,
                                                    const char *message) {
  if (!buffer || buffer_size == 0) {
    return;
  }
  snprintf(buffer, buffer_size, "%s", message ? message : "");
}

static void turbo_runtime_binary_schema_write_field_error(char *buffer,
                                                          size_t buffer_size,
                                                          const char *field_name,
                                                          const char *message) {
  if (!buffer || buffer_size == 0) {
    return;
  }
  if (!field_name || !field_name[0]) {
    turbo_runtime_binary_schema_write_error(buffer, buffer_size, message);
    return;
  }
  snprintf(buffer, buffer_size, "%s: %s", field_name,
           message ? message : "error");
}

static int turbo_runtime_binary_value_type_is_valid(
    turbo_runtime_binary_value_type_t value_type) {
  return value_type >= TURBO_RUNTIME_BINARY_VALUE_BOOL &&
         value_type <= TURBO_RUNTIME_BINARY_VALUE_BYTES16;
}

turbo_runtime_binary_schema_t *
turbo_runtime_binary_schema_clone(const turbo_runtime_binary_schema_t *schema);

static void turbo_runtime_binary_schema_free_field(
    turbo_runtime_binary_field_t *field) {
  if (!field) {
    return;
  }
  tstr_free(field->name);
  turbo_runtime_binary_schema_destroy(field->object_schema);
  memset(field, 0, sizeof(*field));
}

static turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_reserve(turbo_runtime_binary_schema_t *schema,
                                    size_t needed) {
  turbo_runtime_binary_field_t *fields;
  size_t capacity;

  if (!schema) {
    return TURBO_RUNTIME_BINARY_SCHEMA_INVALID_ARGUMENT;
  }
  if (schema->field_capacity >= needed) {
    return TURBO_RUNTIME_BINARY_SCHEMA_OK;
  }

  capacity = schema->field_capacity ? schema->field_capacity * 2 : 4;
  if (capacity < needed) {
    capacity = needed;
  }

  fields = (turbo_runtime_binary_field_t *)realloc(
      schema->fields, capacity * sizeof(*fields));
  if (!fields) {
    return TURBO_RUNTIME_BINARY_SCHEMA_OUT_OF_MEMORY;
  }

  memset(fields + schema->field_capacity, 0,
         (capacity - schema->field_capacity) * sizeof(*fields));
  schema->fields = fields;
  schema->field_capacity = capacity;
  return TURBO_RUNTIME_BINARY_SCHEMA_OK;
}

static turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_add_field(
    turbo_runtime_binary_schema_t *schema, const char *name,
    turbo_runtime_binary_field_shape_t shape,
    turbo_runtime_binary_value_type_t value_type,
    const turbo_runtime_binary_schema_t *object_schema) {
  turbo_runtime_binary_field_t *field;
  turbo_runtime_binary_schema_status_t status;

  if (!schema || !name || !name[0]) {
    return TURBO_RUNTIME_BINARY_SCHEMA_INVALID_ARGUMENT;
  }
  if (shape == TURBO_RUNTIME_BINARY_FIELD_OBJECT) {
    if (!object_schema) {
      return TURBO_RUNTIME_BINARY_SCHEMA_INVALID_ARGUMENT;
    }
  } else if (object_schema || !turbo_runtime_binary_value_type_is_valid(value_type)) {
    return TURBO_RUNTIME_BINARY_SCHEMA_INVALID_ARGUMENT;
  }

  status = turbo_runtime_binary_schema_reserve(schema, schema->field_count + 1);
  if (status != TURBO_RUNTIME_BINARY_SCHEMA_OK) {
    return status;
  }

  field = &schema->fields[schema->field_count];
  field->name = turbo_runtime_binary_schema_strdup(name);
  if (!field->name) {
    return TURBO_RUNTIME_BINARY_SCHEMA_OUT_OF_MEMORY;
  }
  field->shape = shape;
  field->value_type = value_type;
  if (object_schema) {
    field->object_schema = turbo_runtime_binary_schema_clone(object_schema);
    if (!field->object_schema) {
      tstr_free(field->name);
      field->name = NULL;
      return TURBO_RUNTIME_BINARY_SCHEMA_OUT_OF_MEMORY;
    }
  }

  schema->field_count += 1;
  return TURBO_RUNTIME_BINARY_SCHEMA_OK;
}

static turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_collect_value_requirements(
    turbo_runtime_binary_value_type_t value_type,
    turbo_runtime_binary_abi_requirements_t *requirements) {
  if (!requirements || !turbo_runtime_binary_value_type_is_valid(value_type)) {
    return TURBO_RUNTIME_BINARY_SCHEMA_INVALID_ARGUMENT;
  }

  switch (value_type) {
    case TURBO_RUNTIME_BINARY_VALUE_BOOL:
      requirements->create_bool = 1;
      break;
    case TURBO_RUNTIME_BINARY_VALUE_I32:
    case TURBO_RUNTIME_BINARY_VALUE_I64:
      requirements->create_int64 = 1;
      break;
    case TURBO_RUNTIME_BINARY_VALUE_F32:
    case TURBO_RUNTIME_BINARY_VALUE_F64:
      requirements->create_double = 1;
      break;
    case TURBO_RUNTIME_BINARY_VALUE_STRING16:
      requirements->create_string = 1;
      break;
    case TURBO_RUNTIME_BINARY_VALUE_BYTES16:
      requirements->create_bytes = 1;
      break;
  }

  return TURBO_RUNTIME_BINARY_SCHEMA_OK;
}

static turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_collect_field_requirements(
    const turbo_runtime_binary_field_t *field,
    turbo_runtime_binary_abi_requirements_t *requirements);

static turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_validate_value_type(
    turbo_runtime_binary_reader_t *reader,
    turbo_runtime_binary_value_type_t value_type) {
  switch (value_type) {
    case TURBO_RUNTIME_BINARY_VALUE_BOOL: {
      uint8_t value;
      return turbo_runtime_binary_reader_read_u8(reader, &value) ==
                     TURBO_RUNTIME_BINARY_READER_OK
                 ? TURBO_RUNTIME_BINARY_SCHEMA_OK
                 : TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED;
    }
    case TURBO_RUNTIME_BINARY_VALUE_I32: {
      int32_t value;
      return turbo_runtime_binary_reader_read_i32_le(reader, &value) ==
                     TURBO_RUNTIME_BINARY_READER_OK
                 ? TURBO_RUNTIME_BINARY_SCHEMA_OK
                 : TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED;
    }
    case TURBO_RUNTIME_BINARY_VALUE_I64: {
      int64_t value;
      return turbo_runtime_binary_reader_read_i64_le(reader, &value) ==
                     TURBO_RUNTIME_BINARY_READER_OK
                 ? TURBO_RUNTIME_BINARY_SCHEMA_OK
                 : TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED;
    }
    case TURBO_RUNTIME_BINARY_VALUE_F32: {
      float value;
      return turbo_runtime_binary_reader_read_f32_le(reader, &value) ==
                     TURBO_RUNTIME_BINARY_READER_OK
                 ? TURBO_RUNTIME_BINARY_SCHEMA_OK
                 : TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED;
    }
    case TURBO_RUNTIME_BINARY_VALUE_F64: {
      double value;
      return turbo_runtime_binary_reader_read_f64_le(reader, &value) ==
                     TURBO_RUNTIME_BINARY_READER_OK
                 ? TURBO_RUNTIME_BINARY_SCHEMA_OK
                 : TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED;
    }
    case TURBO_RUNTIME_BINARY_VALUE_STRING16:
    case TURBO_RUNTIME_BINARY_VALUE_BYTES16: {
      const uint8_t *data;
      size_t size;
      return turbo_runtime_binary_reader_read_var_bytes16(reader, &data, &size) ==
                     TURBO_RUNTIME_BINARY_READER_OK
                 ? TURBO_RUNTIME_BINARY_SCHEMA_OK
                 : TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED;
    }
  }

  return TURBO_RUNTIME_BINARY_SCHEMA_UNSUPPORTED;
}

static turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_validate_fields(
    const turbo_runtime_binary_schema_t *schema,
    turbo_runtime_binary_reader_t *reader, char *error_buffer,
    size_t error_buffer_size) {
  size_t i;

  if (!schema || !reader) {
    return TURBO_RUNTIME_BINARY_SCHEMA_INVALID_ARGUMENT;
  }

  for (i = 0; i < schema->field_count; ++i) {
    const turbo_runtime_binary_field_t *field = &schema->fields[i];
    turbo_runtime_binary_schema_status_t status;

    switch (field->shape) {
      case TURBO_RUNTIME_BINARY_FIELD_SCALAR:
        status = turbo_runtime_binary_schema_validate_value_type(reader,
                                                                 field->value_type);
        if (status != TURBO_RUNTIME_BINARY_SCHEMA_OK) {
          turbo_runtime_binary_schema_write_field_error(
              error_buffer, error_buffer_size, field->name,
              status == TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED
                  ? "truncated scalar value"
                  : "unsupported scalar type");
          return status;
        }
        break;
      case TURBO_RUNTIME_BINARY_FIELD_REPEATED: {
        uint32_t count;
        size_t j;
        if (turbo_runtime_binary_reader_read_u32_le(reader, &count) !=
            TURBO_RUNTIME_BINARY_READER_OK) {
          turbo_runtime_binary_schema_write_field_error(
              error_buffer, error_buffer_size, field->name,
              "truncated repeated count");
          return TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED;
        }
        for (j = 0; j < (size_t)count; ++j) {
          status = turbo_runtime_binary_schema_validate_value_type(
              reader, field->value_type);
          if (status != TURBO_RUNTIME_BINARY_SCHEMA_OK) {
            turbo_runtime_binary_schema_write_field_error(
                error_buffer, error_buffer_size, field->name,
                status == TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED
                    ? "truncated repeated element"
                    : "unsupported repeated element");
            return status;
          }
        }
        break;
      }
      case TURBO_RUNTIME_BINARY_FIELD_STRING_KEY_MAP: {
        uint32_t count;
        size_t j;
        const uint8_t *key_data;
        size_t key_size;
        if (turbo_runtime_binary_reader_read_u32_le(reader, &count) !=
            TURBO_RUNTIME_BINARY_READER_OK) {
          turbo_runtime_binary_schema_write_field_error(
              error_buffer, error_buffer_size, field->name,
              "truncated map count");
          return TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED;
        }
        for (j = 0; j < (size_t)count; ++j) {
          if (turbo_runtime_binary_reader_read_var_bytes16(reader, &key_data,
                                                           &key_size) !=
              TURBO_RUNTIME_BINARY_READER_OK) {
            turbo_runtime_binary_schema_write_field_error(
                error_buffer, error_buffer_size, field->name,
                "truncated map key");
            return TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED;
          }
          status = turbo_runtime_binary_schema_validate_value_type(
              reader, field->value_type);
          if (status != TURBO_RUNTIME_BINARY_SCHEMA_OK) {
            turbo_runtime_binary_schema_write_field_error(
                error_buffer, error_buffer_size, field->name,
                status == TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED
                    ? "truncated map value"
                    : "unsupported map value");
            return status;
          }
        }
        break;
      }
      case TURBO_RUNTIME_BINARY_FIELD_OBJECT:
        status = turbo_runtime_binary_schema_validate_fields(
            field->object_schema, reader, error_buffer, error_buffer_size);
        if (status != TURBO_RUNTIME_BINARY_SCHEMA_OK) {
          return status;
        }
        break;
    }
  }

  return TURBO_RUNTIME_BINARY_SCHEMA_OK;
}

static turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_status_from_bind_status(
    turbo_runtime_data_bind_status_t status) {
  switch (status) {
    case TURBO_RUNTIME_DATA_BIND_OK:
      return TURBO_RUNTIME_BINARY_SCHEMA_OK;
    case TURBO_RUNTIME_DATA_BIND_OUT_OF_MEMORY:
      return TURBO_RUNTIME_BINARY_SCHEMA_OUT_OF_MEMORY;
    case TURBO_RUNTIME_DATA_BIND_INVALID_ARGUMENT:
    case TURBO_RUNTIME_DATA_BIND_TYPE_MISMATCH:
    case TURBO_RUNTIME_DATA_BIND_NOT_FOUND:
    case TURBO_RUNTIME_DATA_BIND_ERROR:
      return TURBO_RUNTIME_BINARY_SCHEMA_ERROR;
  }

  return TURBO_RUNTIME_BINARY_SCHEMA_ERROR;
}

static turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_store_object_value(
    turbo_runtime_data_bind_value_t *object, const char *key,
    turbo_runtime_data_bind_value_t *value,
    const turbo_runtime_data_bind_value_api_t *api, void *builder_user_data,
    char *error_buffer, size_t error_buffer_size, const char *field_name,
    const char *message) {
  turbo_runtime_data_bind_status_t bind_status;

  bind_status = api->object_set(builder_user_data, object, key, value);
  if (bind_status == TURBO_RUNTIME_DATA_BIND_OK) {
    return TURBO_RUNTIME_BINARY_SCHEMA_OK;
  }

  api->destroy_value(builder_user_data, value);
  turbo_runtime_binary_schema_write_field_error(error_buffer, error_buffer_size,
                                                field_name, message);
  return turbo_runtime_binary_schema_status_from_bind_status(bind_status);
}

static turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_append_array_value(
    turbo_runtime_data_bind_value_t *array, turbo_runtime_data_bind_value_t *value,
    const turbo_runtime_data_bind_value_api_t *api, void *builder_user_data,
    char *error_buffer, size_t error_buffer_size, const char *field_name,
    const char *message) {
  turbo_runtime_data_bind_status_t bind_status;

  bind_status = api->array_append(builder_user_data, array, value);
  if (bind_status == TURBO_RUNTIME_DATA_BIND_OK) {
    return TURBO_RUNTIME_BINARY_SCHEMA_OK;
  }

  api->destroy_value(builder_user_data, value);
  turbo_runtime_binary_schema_write_field_error(error_buffer, error_buffer_size,
                                                field_name, message);
  return turbo_runtime_binary_schema_status_from_bind_status(bind_status);
}

static turbo_runtime_data_bind_value_t *
turbo_runtime_binary_schema_parse_scalar_value(
    turbo_runtime_binary_reader_t *reader,
    turbo_runtime_binary_value_type_t value_type,
    const turbo_runtime_data_bind_value_api_t *api, void *builder_user_data,
    const char *field_name, char *error_buffer, size_t error_buffer_size,
    turbo_runtime_binary_schema_status_t *out_status) {
  turbo_runtime_data_bind_value_t *value;

  if (!reader || !api || !out_status) {
    if (out_status) {
      *out_status = TURBO_RUNTIME_BINARY_SCHEMA_INVALID_ARGUMENT;
    }
    return NULL;
  }

  *out_status = TURBO_RUNTIME_BINARY_SCHEMA_OK;
  value = NULL;

  switch (value_type) {
    case TURBO_RUNTIME_BINARY_VALUE_BOOL: {
      uint8_t raw_value;

      if (turbo_runtime_binary_reader_read_u8(reader, &raw_value) !=
          TURBO_RUNTIME_BINARY_READER_OK) {
        turbo_runtime_binary_schema_write_field_error(
            error_buffer, error_buffer_size, field_name, "truncated scalar value");
        *out_status = TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED;
        return NULL;
      }
      value = api->create_bool(builder_user_data, raw_value != 0);
      break;
    }
    case TURBO_RUNTIME_BINARY_VALUE_I32: {
      int32_t raw_value;

      if (turbo_runtime_binary_reader_read_i32_le(reader, &raw_value) !=
          TURBO_RUNTIME_BINARY_READER_OK) {
        turbo_runtime_binary_schema_write_field_error(
            error_buffer, error_buffer_size, field_name, "truncated scalar value");
        *out_status = TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED;
        return NULL;
      }
      value = api->create_int64(builder_user_data, (int64_t)raw_value);
      break;
    }
    case TURBO_RUNTIME_BINARY_VALUE_I64: {
      int64_t raw_value;

      if (turbo_runtime_binary_reader_read_i64_le(reader, &raw_value) !=
          TURBO_RUNTIME_BINARY_READER_OK) {
        turbo_runtime_binary_schema_write_field_error(
            error_buffer, error_buffer_size, field_name, "truncated scalar value");
        *out_status = TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED;
        return NULL;
      }
      value = api->create_int64(builder_user_data, raw_value);
      break;
    }
    case TURBO_RUNTIME_BINARY_VALUE_F32: {
      float raw_value;

      if (turbo_runtime_binary_reader_read_f32_le(reader, &raw_value) !=
          TURBO_RUNTIME_BINARY_READER_OK) {
        turbo_runtime_binary_schema_write_field_error(
            error_buffer, error_buffer_size, field_name, "truncated scalar value");
        *out_status = TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED;
        return NULL;
      }
      value = api->create_double(builder_user_data, (double)raw_value);
      break;
    }
    case TURBO_RUNTIME_BINARY_VALUE_F64: {
      double raw_value;

      if (turbo_runtime_binary_reader_read_f64_le(reader, &raw_value) !=
          TURBO_RUNTIME_BINARY_READER_OK) {
        turbo_runtime_binary_schema_write_field_error(
            error_buffer, error_buffer_size, field_name, "truncated scalar value");
        *out_status = TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED;
        return NULL;
      }
      value = api->create_double(builder_user_data, raw_value);
      break;
    }
    case TURBO_RUNTIME_BINARY_VALUE_STRING16: {
      char *text = NULL;

      if (turbo_runtime_binary_reader_read_var_string16_copy(reader, &text, NULL) !=
          TURBO_RUNTIME_BINARY_READER_OK) {
        turbo_runtime_binary_schema_write_field_error(
            error_buffer, error_buffer_size, field_name, "truncated scalar value");
        *out_status = TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED;
        return NULL;
      }
      value = api->create_string(builder_user_data, text);
      free(text);
      break;
    }
    case TURBO_RUNTIME_BINARY_VALUE_BYTES16: {
      const uint8_t *data;
      size_t size;

      if (turbo_runtime_binary_reader_read_var_bytes16(reader, &data, &size) !=
          TURBO_RUNTIME_BINARY_READER_OK) {
        turbo_runtime_binary_schema_write_field_error(
            error_buffer, error_buffer_size, field_name, "truncated scalar value");
        *out_status = TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED;
        return NULL;
      }
      value = api->create_bytes(builder_user_data, data, size);
      break;
    }
  }

  if (!value) {
    turbo_runtime_binary_schema_write_field_error(
        error_buffer, error_buffer_size, field_name, "failed to allocate value");
    *out_status = TURBO_RUNTIME_BINARY_SCHEMA_OUT_OF_MEMORY;
    return NULL;
  }

  return value;
}

static turbo_runtime_data_bind_value_t *turbo_runtime_binary_schema_parse_object_value(
    const turbo_runtime_binary_schema_t *schema,
    turbo_runtime_binary_reader_t *reader,
    const turbo_runtime_data_bind_value_api_t *api, void *builder_user_data,
    char *error_buffer, size_t error_buffer_size,
    turbo_runtime_binary_schema_status_t *out_status);

static turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_parse_field_into_object(
    const turbo_runtime_binary_field_t *field,
    turbo_runtime_data_bind_value_t *object, turbo_runtime_binary_reader_t *reader,
    const turbo_runtime_data_bind_value_api_t *api, void *builder_user_data,
    char *error_buffer, size_t error_buffer_size) {
  turbo_runtime_binary_schema_status_t status;
  turbo_runtime_data_bind_value_t *field_value;

  if (!field || !object || !reader || !api) {
    return TURBO_RUNTIME_BINARY_SCHEMA_INVALID_ARGUMENT;
  }

  switch (field->shape) {
    case TURBO_RUNTIME_BINARY_FIELD_SCALAR:
      field_value = turbo_runtime_binary_schema_parse_scalar_value(
          reader, field->value_type, api, builder_user_data, field->name,
          error_buffer, error_buffer_size, &status);
      if (!field_value) {
        return status;
      }
      return turbo_runtime_binary_schema_store_object_value(
          object, field->name, field_value, api, builder_user_data, error_buffer,
          error_buffer_size, field->name, "failed to store scalar field");

    case TURBO_RUNTIME_BINARY_FIELD_REPEATED: {
      uint32_t count;
      size_t i;
      turbo_runtime_data_bind_value_t *array_value;

      if (turbo_runtime_binary_reader_read_u32_le(reader, &count) !=
          TURBO_RUNTIME_BINARY_READER_OK) {
        turbo_runtime_binary_schema_write_field_error(
            error_buffer, error_buffer_size, field->name, "truncated repeated count");
        return TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED;
      }

      array_value = api->create_array(builder_user_data);
      if (!array_value) {
        turbo_runtime_binary_schema_write_field_error(
            error_buffer, error_buffer_size, field->name, "failed to allocate array");
        return TURBO_RUNTIME_BINARY_SCHEMA_OUT_OF_MEMORY;
      }

      for (i = 0; i < (size_t)count; ++i) {
        turbo_runtime_data_bind_value_t *element_value =
            turbo_runtime_binary_schema_parse_scalar_value(
                reader, field->value_type, api, builder_user_data, field->name,
                error_buffer, error_buffer_size, &status);
        if (!element_value) {
          api->destroy_value(builder_user_data, array_value);
          return status;
        }
        status = turbo_runtime_binary_schema_append_array_value(
            array_value, element_value, api, builder_user_data, error_buffer,
            error_buffer_size, field->name, "failed to append repeated element");
        if (status != TURBO_RUNTIME_BINARY_SCHEMA_OK) {
          api->destroy_value(builder_user_data, array_value);
          return status;
        }
      }

      return turbo_runtime_binary_schema_store_object_value(
          object, field->name, array_value, api, builder_user_data, error_buffer,
          error_buffer_size, field->name, "failed to store repeated field");
    }

    case TURBO_RUNTIME_BINARY_FIELD_STRING_KEY_MAP: {
      uint32_t count;
      size_t i;
      turbo_runtime_data_bind_value_t *map_value;

      if (turbo_runtime_binary_reader_read_u32_le(reader, &count) !=
          TURBO_RUNTIME_BINARY_READER_OK) {
        turbo_runtime_binary_schema_write_field_error(
            error_buffer, error_buffer_size, field->name, "truncated map count");
        return TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED;
      }

      map_value = api->create_object(builder_user_data);
      if (!map_value) {
        turbo_runtime_binary_schema_write_field_error(
            error_buffer, error_buffer_size, field->name, "failed to allocate map");
        return TURBO_RUNTIME_BINARY_SCHEMA_OUT_OF_MEMORY;
      }

      for (i = 0; i < (size_t)count; ++i) {
        char *key = NULL;
        turbo_runtime_data_bind_value_t *element_value;

        if (turbo_runtime_binary_reader_read_var_string16_copy(reader, &key, NULL) !=
            TURBO_RUNTIME_BINARY_READER_OK) {
          api->destroy_value(builder_user_data, map_value);
          turbo_runtime_binary_schema_write_field_error(
              error_buffer, error_buffer_size, field->name, "truncated map key");
          return TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED;
        }

        element_value = turbo_runtime_binary_schema_parse_scalar_value(
            reader, field->value_type, api, builder_user_data, field->name,
            error_buffer, error_buffer_size, &status);
        if (!element_value) {
          free(key);
          api->destroy_value(builder_user_data, map_value);
          return status;
        }

        status = turbo_runtime_binary_schema_store_object_value(
            map_value, key, element_value, api, builder_user_data, error_buffer,
            error_buffer_size, field->name, "failed to store map entry");
        free(key);
        if (status != TURBO_RUNTIME_BINARY_SCHEMA_OK) {
          api->destroy_value(builder_user_data, map_value);
          return status;
        }
      }

      return turbo_runtime_binary_schema_store_object_value(
          object, field->name, map_value, api, builder_user_data, error_buffer,
          error_buffer_size, field->name, "failed to store map field");
    }

    case TURBO_RUNTIME_BINARY_FIELD_OBJECT:
      field_value = turbo_runtime_binary_schema_parse_object_value(
          field->object_schema, reader, api, builder_user_data, error_buffer,
          error_buffer_size, &status);
      if (!field_value) {
        return status;
      }
      return turbo_runtime_binary_schema_store_object_value(
          object, field->name, field_value, api, builder_user_data, error_buffer,
          error_buffer_size, field->name, "failed to store object field");
  }

  return TURBO_RUNTIME_BINARY_SCHEMA_UNSUPPORTED;
}

static turbo_runtime_data_bind_value_t *turbo_runtime_binary_schema_parse_object_value(
    const turbo_runtime_binary_schema_t *schema,
    turbo_runtime_binary_reader_t *reader,
    const turbo_runtime_data_bind_value_api_t *api, void *builder_user_data,
    char *error_buffer, size_t error_buffer_size,
    turbo_runtime_binary_schema_status_t *out_status) {
  turbo_runtime_data_bind_value_t *object_value;
  size_t i;

  if (!schema || !reader || !api || !out_status) {
    if (out_status) {
      *out_status = TURBO_RUNTIME_BINARY_SCHEMA_INVALID_ARGUMENT;
    }
    return NULL;
  }

  object_value = api->create_object(builder_user_data);
  if (!object_value) {
    turbo_runtime_binary_schema_write_error(error_buffer, error_buffer_size,
                                            "failed to allocate object");
    *out_status = TURBO_RUNTIME_BINARY_SCHEMA_OUT_OF_MEMORY;
    return NULL;
  }

  for (i = 0; i < schema->field_count; ++i) {
    *out_status = turbo_runtime_binary_schema_parse_field_into_object(
        &schema->fields[i], object_value, reader, api, builder_user_data,
        error_buffer, error_buffer_size);
    if (*out_status != TURBO_RUNTIME_BINARY_SCHEMA_OK) {
      api->destroy_value(builder_user_data, object_value);
      return NULL;
    }
  }

  *out_status = TURBO_RUNTIME_BINARY_SCHEMA_OK;
  return object_value;
}

turbo_runtime_binary_schema_t *
turbo_runtime_binary_schema_clone(const turbo_runtime_binary_schema_t *schema) {
  turbo_runtime_binary_schema_t *copy;
  size_t i;

  if (!schema) {
    return NULL;
  }

  copy = turbo_runtime_binary_schema_create(schema->name);
  if (!copy) {
    return NULL;
  }

  for (i = 0; i < schema->field_count; ++i) {
    if (turbo_runtime_binary_schema_add_field(
            copy, schema->fields[i].name, schema->fields[i].shape,
            schema->fields[i].value_type, schema->fields[i].object_schema) !=
        TURBO_RUNTIME_BINARY_SCHEMA_OK) {
      turbo_runtime_binary_schema_destroy(copy);
      return NULL;
    }
  }

  return copy;
}

turbo_runtime_binary_schema_t *
turbo_runtime_binary_schema_create(const char *name) {
  turbo_runtime_binary_schema_t *schema;

  if (!name || !name[0]) {
    return NULL;
  }

  schema = (turbo_runtime_binary_schema_t *)calloc(1, sizeof(*schema));
  if (!schema) {
    return NULL;
  }

  schema->name = turbo_runtime_binary_schema_strdup(name);
  if (!schema->name) {
    free(schema);
    return NULL;
  }

  return schema;
}

void turbo_runtime_binary_schema_destroy(turbo_runtime_binary_schema_t *schema) {
  size_t i;

  if (!schema) {
    return;
  }

  for (i = 0; i < schema->field_count; ++i) {
    turbo_runtime_binary_schema_free_field(&schema->fields[i]);
  }
  free(schema->fields);
  tstr_free(schema->name);
  free(schema);
}

const char *turbo_runtime_binary_schema_name(
    const turbo_runtime_binary_schema_t *schema) {
  return schema ? schema->name : NULL;
}

size_t turbo_runtime_binary_schema_field_count(
    const turbo_runtime_binary_schema_t *schema) {
  return schema ? schema->field_count : 0;
}

turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_get_field(
    const turbo_runtime_binary_schema_t *schema, size_t index,
    turbo_runtime_binary_field_descriptor_t *out_field) {
  if (!schema || !out_field || index >= schema->field_count) {
    return TURBO_RUNTIME_BINARY_SCHEMA_INVALID_ARGUMENT;
  }

  out_field->name = schema->fields[index].name;
  out_field->shape = schema->fields[index].shape;
  out_field->value_type = schema->fields[index].value_type;
  out_field->object_schema = schema->fields[index].object_schema;
  return TURBO_RUNTIME_BINARY_SCHEMA_OK;
}

turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_add_scalar_field(
    turbo_runtime_binary_schema_t *schema, const char *name,
    turbo_runtime_binary_value_type_t value_type) {
  return turbo_runtime_binary_schema_add_field(
      schema, name, TURBO_RUNTIME_BINARY_FIELD_SCALAR, value_type, NULL);
}

turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_add_repeated_field(
    turbo_runtime_binary_schema_t *schema, const char *name,
    turbo_runtime_binary_value_type_t value_type) {
  return turbo_runtime_binary_schema_add_field(
      schema, name, TURBO_RUNTIME_BINARY_FIELD_REPEATED, value_type, NULL);
}

turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_add_string_key_map_field(
    turbo_runtime_binary_schema_t *schema, const char *name,
    turbo_runtime_binary_value_type_t value_type) {
  return turbo_runtime_binary_schema_add_field(
      schema, name, TURBO_RUNTIME_BINARY_FIELD_STRING_KEY_MAP, value_type,
      NULL);
}

turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_add_object_field(
    turbo_runtime_binary_schema_t *schema, const char *name,
    const turbo_runtime_binary_schema_t *object_schema) {
  return turbo_runtime_binary_schema_add_field(
      schema, name, TURBO_RUNTIME_BINARY_FIELD_OBJECT,
      TURBO_RUNTIME_BINARY_VALUE_BOOL, object_schema);
}

static turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_collect_field_requirements(
    const turbo_runtime_binary_field_t *field,
    turbo_runtime_binary_abi_requirements_t *requirements) {
  if (!field || !requirements) {
    return TURBO_RUNTIME_BINARY_SCHEMA_INVALID_ARGUMENT;
  }

  requirements->create_object = 1;
  requirements->object_set = 1;
  requirements->destroy_value = 1;

  switch (field->shape) {
    case TURBO_RUNTIME_BINARY_FIELD_SCALAR:
      return turbo_runtime_binary_schema_collect_value_requirements(
          field->value_type, requirements);
    case TURBO_RUNTIME_BINARY_FIELD_REPEATED:
      requirements->create_array = 1;
      requirements->array_append = 1;
      return turbo_runtime_binary_schema_collect_value_requirements(
          field->value_type, requirements);
    case TURBO_RUNTIME_BINARY_FIELD_STRING_KEY_MAP:
      requirements->create_object = 1;
      requirements->object_set = 1;
      return turbo_runtime_binary_schema_collect_value_requirements(
          field->value_type, requirements);
    case TURBO_RUNTIME_BINARY_FIELD_OBJECT:
      return turbo_runtime_binary_schema_collect_abi_requirements(
          field->object_schema, requirements);
  }

  return TURBO_RUNTIME_BINARY_SCHEMA_UNSUPPORTED;
}

turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_collect_abi_requirements(
    const turbo_runtime_binary_schema_t *schema,
    turbo_runtime_binary_abi_requirements_t *out_requirements) {
  size_t i;

  if (!schema || !out_requirements) {
    return TURBO_RUNTIME_BINARY_SCHEMA_INVALID_ARGUMENT;
  }

  if (out_requirements->create_object == 0 && out_requirements->object_set == 0 &&
      out_requirements->destroy_value == 0) {
    memset(out_requirements, 0, sizeof(*out_requirements));
  }

  out_requirements->create_object = 1;
  out_requirements->object_set = 1;
  out_requirements->destroy_value = 1;

  for (i = 0; i < schema->field_count; ++i) {
    turbo_runtime_binary_schema_status_t status =
        turbo_runtime_binary_schema_collect_field_requirements(
            &schema->fields[i], out_requirements);
    if (status != TURBO_RUNTIME_BINARY_SCHEMA_OK) {
      return status;
    }
  }

  return TURBO_RUNTIME_BINARY_SCHEMA_OK;
}

turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_validate_value_api(
    const turbo_runtime_binary_schema_t *schema,
    const turbo_runtime_data_bind_value_api_t *api, char *error_buffer,
    size_t error_buffer_size) {
  turbo_runtime_binary_abi_requirements_t requirements;

  if (error_buffer && error_buffer_size != 0) {
    error_buffer[0] = '\0';
  }

  if (!schema || !api) {
    turbo_runtime_binary_schema_write_error(error_buffer, error_buffer_size,
                                            "schema and value api are required");
    return TURBO_RUNTIME_BINARY_SCHEMA_INVALID_ARGUMENT;
  }

  memset(&requirements, 0, sizeof(requirements));
  if (turbo_runtime_binary_schema_collect_abi_requirements(schema,
                                                           &requirements) !=
      TURBO_RUNTIME_BINARY_SCHEMA_OK) {
    turbo_runtime_binary_schema_write_error(error_buffer, error_buffer_size,
                                            "failed to collect abi requirements");
    return TURBO_RUNTIME_BINARY_SCHEMA_ERROR;
  }

  if (requirements.create_bool && !api->create_bool) {
    turbo_runtime_binary_schema_write_error(error_buffer, error_buffer_size,
                                            "schema requires create_bool");
    return TURBO_RUNTIME_BINARY_SCHEMA_UNSUPPORTED;
  }
  if (requirements.create_int64 && !api->create_int64) {
    turbo_runtime_binary_schema_write_error(error_buffer, error_buffer_size,
                                            "schema requires create_int64");
    return TURBO_RUNTIME_BINARY_SCHEMA_UNSUPPORTED;
  }
  if (requirements.create_double && !api->create_double) {
    turbo_runtime_binary_schema_write_error(error_buffer, error_buffer_size,
                                            "schema requires create_double");
    return TURBO_RUNTIME_BINARY_SCHEMA_UNSUPPORTED;
  }
  if (requirements.create_string && !api->create_string) {
    turbo_runtime_binary_schema_write_error(error_buffer, error_buffer_size,
                                            "schema requires create_string");
    return TURBO_RUNTIME_BINARY_SCHEMA_UNSUPPORTED;
  }
  if (requirements.create_bytes && !api->create_bytes) {
    turbo_runtime_binary_schema_write_error(error_buffer, error_buffer_size,
                                            "schema requires create_bytes");
    return TURBO_RUNTIME_BINARY_SCHEMA_UNSUPPORTED;
  }
  if (requirements.create_object && !api->create_object) {
    turbo_runtime_binary_schema_write_error(error_buffer, error_buffer_size,
                                            "schema requires create_object");
    return TURBO_RUNTIME_BINARY_SCHEMA_UNSUPPORTED;
  }
  if (requirements.create_array && !api->create_array) {
    turbo_runtime_binary_schema_write_error(error_buffer, error_buffer_size,
                                            "schema requires create_array");
    return TURBO_RUNTIME_BINARY_SCHEMA_UNSUPPORTED;
  }
  if (requirements.object_set && !api->object_set) {
    turbo_runtime_binary_schema_write_error(error_buffer, error_buffer_size,
                                            "schema requires object_set");
    return TURBO_RUNTIME_BINARY_SCHEMA_UNSUPPORTED;
  }
  if (requirements.array_append && !api->array_append) {
    turbo_runtime_binary_schema_write_error(error_buffer, error_buffer_size,
                                            "schema requires array_append");
    return TURBO_RUNTIME_BINARY_SCHEMA_UNSUPPORTED;
  }
  if (requirements.destroy_value && !api->destroy_value) {
    turbo_runtime_binary_schema_write_error(error_buffer, error_buffer_size,
                                            "schema requires destroy_value");
    return TURBO_RUNTIME_BINARY_SCHEMA_UNSUPPORTED;
  }

  return TURBO_RUNTIME_BINARY_SCHEMA_OK;
}

turbo_runtime_binary_schema_status_t
turbo_runtime_binary_schema_validate_payload(
    const turbo_runtime_binary_schema_t *schema, const uint8_t *data,
    size_t size, char *error_buffer, size_t error_buffer_size) {
  turbo_runtime_binary_reader_t reader;
  turbo_runtime_binary_schema_status_t status;

  if (error_buffer && error_buffer_size != 0) {
    error_buffer[0] = '\0';
  }

  if (!schema || (!data && size != 0)) {
    turbo_runtime_binary_schema_write_error(error_buffer, error_buffer_size,
                                            "schema and payload are required");
    return TURBO_RUNTIME_BINARY_SCHEMA_INVALID_ARGUMENT;
  }

  turbo_runtime_binary_reader_init(&reader, data, size);
  status = turbo_runtime_binary_schema_validate_fields(
      schema, &reader, error_buffer, error_buffer_size);
  if (status != TURBO_RUNTIME_BINARY_SCHEMA_OK) {
    return status;
  }
  if (turbo_runtime_binary_reader_remaining(&reader) != 0) {
    turbo_runtime_binary_schema_write_error(error_buffer, error_buffer_size,
                                            "trailing bytes after payload");
    return TURBO_RUNTIME_BINARY_SCHEMA_ERROR;
  }

  return TURBO_RUNTIME_BINARY_SCHEMA_OK;
}

turbo_runtime_data_bind_value_t *turbo_runtime_binary_schema_parse(
    const turbo_runtime_binary_schema_t *schema, const uint8_t *data,
    size_t size, const turbo_runtime_data_bind_value_api_t *api,
    void *builder_user_data, char *error_buffer, size_t error_buffer_size) {
  turbo_runtime_binary_reader_t reader;
  turbo_runtime_binary_schema_status_t status;
  turbo_runtime_data_bind_value_t *root;

  if (error_buffer && error_buffer_size != 0) {
    error_buffer[0] = '\0';
  }

  if (!schema || !api || (!data && size != 0)) {
    turbo_runtime_binary_schema_write_error(
        error_buffer, error_buffer_size,
        "schema, payload, and value api are required");
    return NULL;
  }

  status = turbo_runtime_binary_schema_validate_value_api(
      schema, api, error_buffer, error_buffer_size);
  if (status != TURBO_RUNTIME_BINARY_SCHEMA_OK) {
    return NULL;
  }

  turbo_runtime_binary_reader_init(&reader, data, size);
  root = turbo_runtime_binary_schema_parse_object_value(
      schema, &reader, api, builder_user_data, error_buffer, error_buffer_size,
      &status);
  if (!root) {
    return NULL;
  }

  if (turbo_runtime_binary_reader_remaining(&reader) != 0) {
    api->destroy_value(builder_user_data, root);
    turbo_runtime_binary_schema_write_error(error_buffer, error_buffer_size,
                                            "trailing bytes after payload");
    return NULL;
  }

  return root;
}
