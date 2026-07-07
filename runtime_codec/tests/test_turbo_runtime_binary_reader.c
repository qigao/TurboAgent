#include "tinytest.h"
#include "turbo_runtime_binary_reader.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

spec("turbo runtime codec binary reader") {

  it("should read little-endian scalars and var strings") {
    const uint8_t data[] = {
        0x7f,
        0x34, 0x12,
        0x78, 0x56, 0x34, 0x12,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x00, 0x00, 0x60, 0x40,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x40,
        0x05, 0x00, 'h', 'e', 'l', 'l', 'o'};
    turbo_runtime_binary_reader_t reader;
    uint8_t u8_value = 0;
    uint16_t u16_value = 0;
    uint32_t u32_value = 0;
    int64_t i64_value = 0;
    float f32_value = 0.0f;
    double f64_value = 0.0;
    char *string_value = NULL;
    size_t string_size = 0;

    turbo_runtime_binary_reader_init(&reader, data, sizeof(data));

    check_int_eq(
        turbo_runtime_binary_reader_read_u8(&reader, &u8_value),
        TURBO_RUNTIME_BINARY_READER_OK);
    check_int_eq(u8_value, 0x7f);
    check_int_eq(
        turbo_runtime_binary_reader_read_u16_le(&reader, &u16_value),
        TURBO_RUNTIME_BINARY_READER_OK);
    check_int_eq(u16_value, 0x1234);
    check_int_eq(
        turbo_runtime_binary_reader_read_u32_le(&reader, &u32_value),
        TURBO_RUNTIME_BINARY_READER_OK);
    check_size_eq(u32_value, 0x12345678u);
    check_int_eq(
        turbo_runtime_binary_reader_read_i64_le(&reader, &i64_value),
        TURBO_RUNTIME_BINARY_READER_OK);
    check_size_eq((size_t)i64_value, (size_t)0x1122334455667788ull);
    check_int_eq(
        turbo_runtime_binary_reader_read_f32_le(&reader, &f32_value),
        TURBO_RUNTIME_BINARY_READER_OK);
    check_true(fabsf(f32_value - 3.5f) < 0.0001f);
    check_int_eq(
        turbo_runtime_binary_reader_read_f64_le(&reader, &f64_value),
        TURBO_RUNTIME_BINARY_READER_OK);
    check_true(fabs(f64_value - 4.5) < 0.0001);
    check_int_eq(
        turbo_runtime_binary_reader_read_var_string16_copy(&reader, &string_value,
                                                           &string_size),
        TURBO_RUNTIME_BINARY_READER_OK);
    check_str_eq(string_value, "hello");
    check_size_eq(string_size, 5);
    check_size_eq(turbo_runtime_binary_reader_remaining(&reader), 0);

    free(string_value);
  }

  it("should detect truncation") {
    const uint8_t data[] = {0x03, 0x00, 'a'};
    turbo_runtime_binary_reader_t reader;
    char *string_value = NULL;

    turbo_runtime_binary_reader_init(&reader, data, sizeof(data));
    check_int_eq(
        turbo_runtime_binary_reader_read_var_string16_copy(&reader, &string_value,
                                                           NULL),
        TURBO_RUNTIME_BINARY_READER_EOF);
    check_null(string_value);
  }

  it("should support borrowed byte spans and rewind") {
    const uint8_t data[] = {0xaa, 0xbb, 0xcc, 0xdd, 0x02, 0x00, 0x10, 0x11};
    turbo_runtime_binary_reader_t reader;
    const uint8_t *view = NULL;
    size_t view_size = 0;
    uint16_t length = 0;

    turbo_runtime_binary_reader_init(&reader, data, sizeof(data));
    check_int_eq(
        turbo_runtime_binary_reader_read_bytes(&reader, 4, &view),
        TURBO_RUNTIME_BINARY_READER_OK);
    check_not_null(view);
    check_int_eq(view[2], 0xcc);
    check_int_eq(
        turbo_runtime_binary_reader_read_u16_le(&reader, &length),
        TURBO_RUNTIME_BINARY_READER_OK);
    check_int_eq(length, 2);
    check_int_eq(
        turbo_runtime_binary_reader_read_var_bytes16(&reader, &view, &view_size),
        TURBO_RUNTIME_BINARY_READER_EOF);

    turbo_runtime_binary_reader_rewind(&reader);
    check_size_eq(turbo_runtime_binary_reader_offset(&reader), 0);
    check_size_eq(turbo_runtime_binary_reader_remaining(&reader), sizeof(data));
  }
}
