#include "turbo_runtime_binary_reader.h"

#include <turbo_str.h>
#include <stdlib.h>
#include <string.h>

static turbo_runtime_binary_reader_status_t
turbo_runtime_binary_reader_require(const turbo_runtime_binary_reader_t *reader, size_t size) {
  if (!reader) {
    return TURBO_RUNTIME_BINARY_READER_INVALID_ARGUMENT;
  }

  if (size > (reader->size - reader->offset)) {
    return TURBO_RUNTIME_BINARY_READER_EOF;
  }

  return TURBO_RUNTIME_BINARY_READER_OK;
}

void turbo_runtime_binary_reader_init(turbo_runtime_binary_reader_t *reader,
                                      const uint8_t *data, size_t size) {
  if (!reader) {
    return;
  }

  reader->data = data;
  reader->size = size;
  reader->offset = 0;
}

void turbo_runtime_binary_reader_rewind(turbo_runtime_binary_reader_t *reader) {
  if (!reader) {
    return;
  }
  reader->offset = 0;
}

size_t turbo_runtime_binary_reader_offset(const turbo_runtime_binary_reader_t *reader) {
  return reader ? reader->offset : 0;
}

size_t turbo_runtime_binary_reader_remaining(const turbo_runtime_binary_reader_t *reader) {
  if (!reader || reader->offset > reader->size) {
    return 0;
  }
  return reader->size - reader->offset;
}

turbo_runtime_binary_reader_status_t turbo_runtime_binary_reader_skip(
    turbo_runtime_binary_reader_t *reader, size_t size) {
  turbo_runtime_binary_reader_status_t status =
      turbo_runtime_binary_reader_require(reader, size);
  if (status != TURBO_RUNTIME_BINARY_READER_OK) {
    return status;
  }

  reader->offset += size;
  return TURBO_RUNTIME_BINARY_READER_OK;
}

turbo_runtime_binary_reader_status_t turbo_runtime_binary_reader_read_u8(
    turbo_runtime_binary_reader_t *reader, uint8_t *out_value) {
  turbo_runtime_binary_reader_status_t status =
      turbo_runtime_binary_reader_require(reader, 1);
  if (!out_value) {
    return TURBO_RUNTIME_BINARY_READER_INVALID_ARGUMENT;
  }
  if (status != TURBO_RUNTIME_BINARY_READER_OK) {
    return status;
  }

  *out_value = reader->data[reader->offset++];
  return TURBO_RUNTIME_BINARY_READER_OK;
}

turbo_runtime_binary_reader_status_t turbo_runtime_binary_reader_read_u16_le(
    turbo_runtime_binary_reader_t *reader, uint16_t *out_value) {
  turbo_runtime_binary_reader_status_t status =
      turbo_runtime_binary_reader_require(reader, 2);
  if (!out_value) {
    return TURBO_RUNTIME_BINARY_READER_INVALID_ARGUMENT;
  }
  if (status != TURBO_RUNTIME_BINARY_READER_OK) {
    return status;
  }

  *out_value = (uint16_t)reader->data[reader->offset] |
               (uint16_t)((uint16_t)reader->data[reader->offset + 1] << 8);
  reader->offset += 2;
  return TURBO_RUNTIME_BINARY_READER_OK;
}

turbo_runtime_binary_reader_status_t turbo_runtime_binary_reader_read_u32_le(
    turbo_runtime_binary_reader_t *reader, uint32_t *out_value) {
  turbo_runtime_binary_reader_status_t status =
      turbo_runtime_binary_reader_require(reader, 4);
  if (!out_value) {
    return TURBO_RUNTIME_BINARY_READER_INVALID_ARGUMENT;
  }
  if (status != TURBO_RUNTIME_BINARY_READER_OK) {
    return status;
  }

  *out_value = (uint32_t)reader->data[reader->offset] |
               ((uint32_t)reader->data[reader->offset + 1] << 8) |
               ((uint32_t)reader->data[reader->offset + 2] << 16) |
               ((uint32_t)reader->data[reader->offset + 3] << 24);
  reader->offset += 4;
  return TURBO_RUNTIME_BINARY_READER_OK;
}

turbo_runtime_binary_reader_status_t turbo_runtime_binary_reader_read_i32_le(
    turbo_runtime_binary_reader_t *reader, int32_t *out_value) {
  uint32_t raw_value;
  turbo_runtime_binary_reader_status_t status;

  if (!out_value) {
    return TURBO_RUNTIME_BINARY_READER_INVALID_ARGUMENT;
  }

  status = turbo_runtime_binary_reader_read_u32_le(reader, &raw_value);
  if (status != TURBO_RUNTIME_BINARY_READER_OK) {
    return status;
  }

  memcpy(out_value, &raw_value, sizeof(*out_value));
  return TURBO_RUNTIME_BINARY_READER_OK;
}

turbo_runtime_binary_reader_status_t turbo_runtime_binary_reader_read_i64_le(
    turbo_runtime_binary_reader_t *reader, int64_t *out_value) {
  uint64_t raw_value;
  turbo_runtime_binary_reader_status_t status =
      turbo_runtime_binary_reader_require(reader, 8);
  if (!out_value) {
    return TURBO_RUNTIME_BINARY_READER_INVALID_ARGUMENT;
  }
  if (status != TURBO_RUNTIME_BINARY_READER_OK) {
    return status;
  }

  raw_value = (uint64_t)reader->data[reader->offset] |
              ((uint64_t)reader->data[reader->offset + 1] << 8) |
              ((uint64_t)reader->data[reader->offset + 2] << 16) |
              ((uint64_t)reader->data[reader->offset + 3] << 24) |
              ((uint64_t)reader->data[reader->offset + 4] << 32) |
              ((uint64_t)reader->data[reader->offset + 5] << 40) |
              ((uint64_t)reader->data[reader->offset + 6] << 48) |
              ((uint64_t)reader->data[reader->offset + 7] << 56);
  reader->offset += 8;
  memcpy(out_value, &raw_value, sizeof(*out_value));
  return TURBO_RUNTIME_BINARY_READER_OK;
}

turbo_runtime_binary_reader_status_t turbo_runtime_binary_reader_read_f32_le(
    turbo_runtime_binary_reader_t *reader, float *out_value) {
  uint32_t raw_value;
  turbo_runtime_binary_reader_status_t status;

  if (!out_value) {
    return TURBO_RUNTIME_BINARY_READER_INVALID_ARGUMENT;
  }

  status = turbo_runtime_binary_reader_read_u32_le(reader, &raw_value);
  if (status != TURBO_RUNTIME_BINARY_READER_OK) {
    return status;
  }

  memcpy(out_value, &raw_value, sizeof(*out_value));
  return TURBO_RUNTIME_BINARY_READER_OK;
}

turbo_runtime_binary_reader_status_t turbo_runtime_binary_reader_read_f64_le(
    turbo_runtime_binary_reader_t *reader, double *out_value) {
  uint64_t raw_value;
  turbo_runtime_binary_reader_status_t status =
      turbo_runtime_binary_reader_require(reader, 8);
  if (!out_value) {
    return TURBO_RUNTIME_BINARY_READER_INVALID_ARGUMENT;
  }
  if (status != TURBO_RUNTIME_BINARY_READER_OK) {
    return status;
  }

  raw_value = (uint64_t)reader->data[reader->offset] |
              ((uint64_t)reader->data[reader->offset + 1] << 8) |
              ((uint64_t)reader->data[reader->offset + 2] << 16) |
              ((uint64_t)reader->data[reader->offset + 3] << 24) |
              ((uint64_t)reader->data[reader->offset + 4] << 32) |
              ((uint64_t)reader->data[reader->offset + 5] << 40) |
              ((uint64_t)reader->data[reader->offset + 6] << 48) |
              ((uint64_t)reader->data[reader->offset + 7] << 56);
  reader->offset += 8;
  memcpy(out_value, &raw_value, sizeof(*out_value));
  return TURBO_RUNTIME_BINARY_READER_OK;
}

turbo_runtime_binary_reader_status_t turbo_runtime_binary_reader_read_bytes(
    turbo_runtime_binary_reader_t *reader, size_t size, const uint8_t **out_data) {
  turbo_runtime_binary_reader_status_t status =
      turbo_runtime_binary_reader_require(reader, size);
  if (!out_data) {
    return TURBO_RUNTIME_BINARY_READER_INVALID_ARGUMENT;
  }
  if (status != TURBO_RUNTIME_BINARY_READER_OK) {
    return status;
  }

  *out_data = reader->data + reader->offset;
  reader->offset += size;
  return TURBO_RUNTIME_BINARY_READER_OK;
}

turbo_runtime_binary_reader_status_t
turbo_runtime_binary_reader_read_var_bytes16(turbo_runtime_binary_reader_t *reader,
                                             const uint8_t **out_data,
                                             size_t *out_size) {
  uint16_t size16;
  turbo_runtime_binary_reader_status_t status;

  if (!out_data || !out_size) {
    return TURBO_RUNTIME_BINARY_READER_INVALID_ARGUMENT;
  }

  status = turbo_runtime_binary_reader_read_u16_le(reader, &size16);
  if (status != TURBO_RUNTIME_BINARY_READER_OK) {
    return status;
  }

  *out_size = (size_t)size16;
  return turbo_runtime_binary_reader_read_bytes(reader, *out_size, out_data);
}

turbo_runtime_binary_reader_status_t
turbo_runtime_binary_reader_read_var_string16_copy(turbo_runtime_binary_reader_t *reader,
                                                   char **out_string, size_t *out_size) {
  const uint8_t *data;
  size_t size;
  char *copy;
  turbo_runtime_binary_reader_status_t status;

  if (!out_string) {
    return TURBO_RUNTIME_BINARY_READER_INVALID_ARGUMENT;
  }

  status = turbo_runtime_binary_reader_read_var_bytes16(reader, &data, &size);
  if (status != TURBO_RUNTIME_BINARY_READER_OK) {
    return status;
  }

  copy = (char *)malloc(size + 1);
  if (!copy) {
    return TURBO_RUNTIME_BINARY_READER_OVERFLOW;
  }
  if (size != 0) {
    memcpy(copy, data, size);
  }
  copy[size] = '\0';

  *out_string = copy;
  if (out_size) {
    *out_size = size;
  }
  return TURBO_RUNTIME_BINARY_READER_OK;
}
