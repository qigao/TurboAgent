#ifndef TURBO_RUNTIME_BINARY_READER_H
#define TURBO_RUNTIME_BINARY_READER_H


#include <platform.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  TURBO_RUNTIME_BINARY_READER_OK = 0,
  TURBO_RUNTIME_BINARY_READER_EOF = -1,
  TURBO_RUNTIME_BINARY_READER_INVALID_ARGUMENT = -2,
  TURBO_RUNTIME_BINARY_READER_OVERFLOW = -3
} turbo_runtime_binary_reader_status_t;

typedef struct turbo_runtime_binary_reader_s {
  const uint8_t *data;
  size_t size;
  size_t offset;
} turbo_runtime_binary_reader_t;

/**
 * @brief Initialize one binary reader over a caller-owned byte span.
 */
CXX_C_API void turbo_runtime_binary_reader_init(turbo_runtime_binary_reader_t *reader,
                                                const uint8_t *data, size_t size);

/**
 * @brief Reset the offset to the beginning of the current byte span.
 */
CXX_C_API void turbo_runtime_binary_reader_rewind(turbo_runtime_binary_reader_t *reader);

/**
 * @brief Return the current read offset.
 */
CXX_C_API size_t turbo_runtime_binary_reader_offset(
    const turbo_runtime_binary_reader_t *reader);

/**
 * @brief Return the number of unread bytes.
 */
CXX_C_API size_t turbo_runtime_binary_reader_remaining(
    const turbo_runtime_binary_reader_t *reader);

/**
 * @brief Advance by `size` bytes.
 */
CXX_C_API turbo_runtime_binary_reader_status_t turbo_runtime_binary_reader_skip(
    turbo_runtime_binary_reader_t *reader, size_t size);

/**
 * @brief Read one little-endian unsigned 8-bit integer.
 */
CXX_C_API turbo_runtime_binary_reader_status_t turbo_runtime_binary_reader_read_u8(
    turbo_runtime_binary_reader_t *reader, uint8_t *out_value);

/**
 * @brief Read one little-endian unsigned 16-bit integer.
 */
CXX_C_API turbo_runtime_binary_reader_status_t turbo_runtime_binary_reader_read_u16_le(
    turbo_runtime_binary_reader_t *reader, uint16_t *out_value);

/**
 * @brief Read one little-endian unsigned 32-bit integer.
 */
CXX_C_API turbo_runtime_binary_reader_status_t turbo_runtime_binary_reader_read_u32_le(
    turbo_runtime_binary_reader_t *reader, uint32_t *out_value);

/**
 * @brief Read one little-endian signed 32-bit integer.
 */
CXX_C_API turbo_runtime_binary_reader_status_t turbo_runtime_binary_reader_read_i32_le(
    turbo_runtime_binary_reader_t *reader, int32_t *out_value);

/**
 * @brief Read one little-endian signed 64-bit integer.
 */
CXX_C_API turbo_runtime_binary_reader_status_t turbo_runtime_binary_reader_read_i64_le(
    turbo_runtime_binary_reader_t *reader, int64_t *out_value);

/**
 * @brief Read one little-endian IEEE754 float.
 */
CXX_C_API turbo_runtime_binary_reader_status_t turbo_runtime_binary_reader_read_f32_le(
    turbo_runtime_binary_reader_t *reader, float *out_value);

/**
 * @brief Read one little-endian IEEE754 double.
 */
CXX_C_API turbo_runtime_binary_reader_status_t turbo_runtime_binary_reader_read_f64_le(
    turbo_runtime_binary_reader_t *reader, double *out_value);

/**
 * @brief Borrow a contiguous byte span and advance past it.
 */
CXX_C_API turbo_runtime_binary_reader_status_t turbo_runtime_binary_reader_read_bytes(
    turbo_runtime_binary_reader_t *reader, size_t size, const uint8_t **out_data);

/**
 * @brief Read a uint16-length-prefixed byte span.
 */
CXX_C_API turbo_runtime_binary_reader_status_t
turbo_runtime_binary_reader_read_var_bytes16(turbo_runtime_binary_reader_t *reader,
                                             const uint8_t **out_data, size_t *out_size);

/**
 * @brief Read a uint16-length-prefixed string and return a malloc-owned copy.
 */
CXX_C_API turbo_runtime_binary_reader_status_t
turbo_runtime_binary_reader_read_var_string16_copy(turbo_runtime_binary_reader_t *reader,
                                                   char **out_string, size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif
