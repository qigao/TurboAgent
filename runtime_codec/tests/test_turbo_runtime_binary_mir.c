#include "tinytest.h"
#include "turbo_runtime_binary_mir.h"

#include <stdint.h>
#include <string.h>

spec("turbo runtime codec binary mir") {

  it("should build a mir extern plan from schema requirements") {
    turbo_runtime_binary_schema_t *schema =
        turbo_runtime_binary_schema_create("root");
    turbo_runtime_binary_mir_plan_t *plan;
    turbo_runtime_binary_mir_symbol_descriptor_t symbol = {0};

    check_not_null(schema);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            schema, "name", TURBO_RUNTIME_BINARY_VALUE_STRING16),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_repeated_field(
            schema, "scores", TURBO_RUNTIME_BINARY_VALUE_I32),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);

    plan = turbo_runtime_binary_mir_plan_create(schema);
    check_not_null(plan);
    check_size_eq(turbo_runtime_binary_mir_plan_symbol_count(plan), 7);
    check_int_eq(
        turbo_runtime_binary_mir_plan_get_symbol(plan, 0, &symbol),
        TURBO_RUNTIME_BINARY_MIR_OK);
    check_str_eq(symbol.name, "db_create_int64");
    check_int_eq(symbol.result_type, TURBO_RUNTIME_BINARY_MIR_ABI_PTR);
    check_size_eq(symbol.argument_count, 2);

    check_int_eq(
        turbo_runtime_binary_mir_plan_get_symbol(plan, 6, &symbol),
        TURBO_RUNTIME_BINARY_MIR_OK);
    check_str_eq(symbol.name, "db_destroy_value");

    turbo_runtime_binary_mir_plan_destroy(plan);
    turbo_runtime_binary_schema_destroy(schema);
  }

  it("should build a jit parser that returns runtime values") {
    turbo_runtime_binary_schema_t *schema =
        turbo_runtime_binary_schema_create("root");
    turbo_runtime_binary_mir_compiler_t *compiler;
    turbo_runtime_binary_mir_parser_fn parser;
    turbo_runtime_data_bind_value_t *value;
    const uint8_t payload[] = {0x03, 0x00, 'b', 'o', 'b'};
    char error[128];

    check_not_null(schema);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            schema, "name", TURBO_RUNTIME_BINARY_VALUE_STRING16),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);

    compiler = turbo_runtime_binary_mir_compiler_create(schema);
    check_not_null(compiler);
    check_str_eq(turbo_runtime_binary_mir_compiler_entry_name(compiler),
                 "parse_root");
    check_not_null(turbo_runtime_binary_mir_compiler_plan(compiler));
    check_int_eq(
        turbo_runtime_binary_mir_compiler_build(
            compiler, turbo_runtime_data_bind_default_value_api(), error,
            sizeof(error)),
        TURBO_RUNTIME_BINARY_MIR_OK);
    parser = turbo_runtime_binary_mir_compiler_parser(compiler);
    check_not_null(parser);
    value = parser(payload, sizeof(payload), NULL);
    check_not_null(value);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(value, "name")),
                 "bob");
    turbo_runtime_data_bind_default_value_api()->destroy_value(NULL, value);

    turbo_runtime_binary_mir_compiler_destroy(compiler);
    turbo_runtime_binary_schema_destroy(schema);
  }

  it("should reject apis that cannot satisfy the mir plan") {
    turbo_runtime_binary_schema_t *schema =
        turbo_runtime_binary_schema_create("root");
    turbo_runtime_binary_mir_compiler_t *compiler;
    char error[128];

    check_not_null(schema);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            schema, "blob", TURBO_RUNTIME_BINARY_VALUE_BYTES16),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);

    compiler = turbo_runtime_binary_mir_compiler_create(schema);
    check_not_null(compiler);
    check_int_eq(
        turbo_runtime_binary_mir_compiler_build(
            compiler, turbo_runtime_data_bind_json_value_api(), error,
            sizeof(error)),
        TURBO_RUNTIME_BINARY_MIR_UNSUPPORTED);
    check_true(strstr(error, "create_bytes") != NULL);

    turbo_runtime_binary_mir_compiler_destroy(compiler);
    turbo_runtime_binary_schema_destroy(schema);
  }

  it("should keep mixed schemas working through the fallback parser path") {
    turbo_runtime_binary_schema_t *schema =
        turbo_runtime_binary_schema_create("root");
    turbo_runtime_binary_mir_compiler_t *compiler;
    turbo_runtime_binary_mir_parser_fn parser;
    turbo_runtime_data_bind_value_t *value;
    const turbo_runtime_data_bind_value_t *scores;
    const uint8_t payload[] = {
        0x01,
        0x02, 0x00, 0x00, 0x00,
        0x05, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x00, 0x00};
    char error[128];

    check_not_null(schema);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            schema, "flag", TURBO_RUNTIME_BINARY_VALUE_BOOL),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_repeated_field(
            schema, "scores", TURBO_RUNTIME_BINARY_VALUE_I32),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);

    compiler = turbo_runtime_binary_mir_compiler_create(schema);
    check_not_null(compiler);
    check_int_eq(
        turbo_runtime_binary_mir_compiler_build(
            compiler, turbo_runtime_data_bind_default_value_api(), error,
            sizeof(error)),
        TURBO_RUNTIME_BINARY_MIR_OK);
    parser = turbo_runtime_binary_mir_compiler_parser(compiler);
    check_not_null(parser);

    value = parser(payload, sizeof(payload), NULL);
    check_not_null(value);
    check_int_eq(
        turbo_runtime_data_bind_value_as_bool(
            turbo_runtime_data_bind_object_get(value, "flag"), 0),
        1);
    scores = turbo_runtime_data_bind_object_get(value, "scores");
    check_not_null(scores);
    check_size_eq(turbo_runtime_data_bind_value_size(scores), 2);
    check_int_eq(
        (int)turbo_runtime_data_bind_value_as_int64(
            turbo_runtime_data_bind_array_get(scores, 0), 0),
        5);
    check_int_eq(
        (int)turbo_runtime_data_bind_value_as_int64(
            turbo_runtime_data_bind_array_get(scores, 1), 0),
        8);

    turbo_runtime_data_bind_default_value_api()->destroy_value(NULL, value);
    turbo_runtime_binary_mir_compiler_destroy(compiler);
    turbo_runtime_binary_schema_destroy(schema);
  }

  it("should parse multiple scalar field kinds through the scalar mir fast path") {
    turbo_runtime_binary_schema_t *schema =
        turbo_runtime_binary_schema_create("root");
    turbo_runtime_binary_mir_compiler_t *compiler;
    turbo_runtime_binary_mir_parser_fn parser;
    turbo_runtime_data_bind_value_t *value;
    const turbo_runtime_data_bind_value_t *blob;
    const uint8_t payload[] = {
        0x01,
        0x2a, 0x00, 0x00, 0x00,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x00, 0x00, 0x60, 0x40,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x40,
        0x02, 0x00, 'o', 'k',
        0x02, 0x00, 0xca, 0xfe};
    char error[128];

    check_not_null(schema);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            schema, "flag", TURBO_RUNTIME_BINARY_VALUE_BOOL),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            schema, "count", TURBO_RUNTIME_BINARY_VALUE_I32),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            schema, "big", TURBO_RUNTIME_BINARY_VALUE_I64),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            schema, "ratio", TURBO_RUNTIME_BINARY_VALUE_F32),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            schema, "score", TURBO_RUNTIME_BINARY_VALUE_F64),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            schema, "name", TURBO_RUNTIME_BINARY_VALUE_STRING16),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            schema, "blob", TURBO_RUNTIME_BINARY_VALUE_BYTES16),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);

    compiler = turbo_runtime_binary_mir_compiler_create(schema);
    check_not_null(compiler);
    check_int_eq(
        turbo_runtime_binary_mir_compiler_build(
            compiler, turbo_runtime_data_bind_default_value_api(), error,
            sizeof(error)),
        TURBO_RUNTIME_BINARY_MIR_OK);
    parser = turbo_runtime_binary_mir_compiler_parser(compiler);
    check_not_null(parser);

    value = parser(payload, sizeof(payload), NULL);
    check_not_null(value);
    check_int_eq(
        turbo_runtime_data_bind_value_as_bool(
            turbo_runtime_data_bind_object_get(value, "flag"), 0),
        1);
    check_int_eq(
        (int)turbo_runtime_data_bind_value_as_int64(
            turbo_runtime_data_bind_object_get(value, "count"), 0),
        42);
    check_true(
        turbo_runtime_data_bind_value_as_int64(
            turbo_runtime_data_bind_object_get(value, "big"), 0) ==
        INT64_C(0x1122334455667788));
    check_double_eq(
        turbo_runtime_data_bind_value_as_double(
            turbo_runtime_data_bind_object_get(value, "ratio"), 0.0),
        3.5, 0.000001);
    check_double_eq(
        turbo_runtime_data_bind_value_as_double(
            turbo_runtime_data_bind_object_get(value, "score"), 0.0),
        2.5, 0.000001);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(value, "name")),
                 "ok");
    blob = turbo_runtime_data_bind_object_get(value, "blob");
    check_not_null(blob);
    check_size_eq(turbo_runtime_data_bind_value_data_size(blob), 2);
    check_int_eq((int)turbo_runtime_data_bind_value_as_bytes(blob)[0], 0xca);
    check_int_eq((int)turbo_runtime_data_bind_value_as_bytes(blob)[1], 0xfe);

    turbo_runtime_data_bind_default_value_api()->destroy_value(NULL, value);
    turbo_runtime_binary_mir_compiler_destroy(compiler);
    turbo_runtime_binary_schema_destroy(schema);
  }

  it("should reject truncated payloads on the scalar mir fast path") {
    turbo_runtime_binary_schema_t *schema =
        turbo_runtime_binary_schema_create("root");
    turbo_runtime_binary_mir_compiler_t *compiler;
    turbo_runtime_binary_mir_parser_fn parser;
    const uint8_t payload[] = {
        0x01,
        0x2a, 0x00, 0x00, 0x00,
        0x02, 0x00, 'o'};
    char error[128];

    check_not_null(schema);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            schema, "flag", TURBO_RUNTIME_BINARY_VALUE_BOOL),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            schema, "count", TURBO_RUNTIME_BINARY_VALUE_I32),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            schema, "name", TURBO_RUNTIME_BINARY_VALUE_STRING16),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);

    compiler = turbo_runtime_binary_mir_compiler_create(schema);
    check_not_null(compiler);
    check_int_eq(
        turbo_runtime_binary_mir_compiler_build(
            compiler, turbo_runtime_data_bind_default_value_api(), error,
            sizeof(error)),
        TURBO_RUNTIME_BINARY_MIR_OK);
    parser = turbo_runtime_binary_mir_compiler_parser(compiler);
    check_not_null(parser);
    check_null(parser(payload, sizeof(payload), NULL));

    turbo_runtime_binary_mir_compiler_destroy(compiler);
    turbo_runtime_binary_schema_destroy(schema);
  }

  it("should reject trailing bytes on the scalar mir fast path") {
    turbo_runtime_binary_schema_t *schema =
        turbo_runtime_binary_schema_create("root");
    turbo_runtime_binary_mir_compiler_t *compiler;
    turbo_runtime_binary_mir_parser_fn parser;
    const uint8_t payload[] = {
        0x01,
        0x2a, 0x00, 0x00, 0x00,
        0xaa};
    char error[128];

    check_not_null(schema);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            schema, "flag", TURBO_RUNTIME_BINARY_VALUE_BOOL),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            schema, "count", TURBO_RUNTIME_BINARY_VALUE_I32),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);

    compiler = turbo_runtime_binary_mir_compiler_create(schema);
    check_not_null(compiler);
    check_int_eq(
        turbo_runtime_binary_mir_compiler_build(
            compiler, turbo_runtime_data_bind_default_value_api(), error,
            sizeof(error)),
        TURBO_RUNTIME_BINARY_MIR_OK);
    parser = turbo_runtime_binary_mir_compiler_parser(compiler);
    check_not_null(parser);
    check_null(parser(payload, sizeof(payload), NULL));

    turbo_runtime_binary_mir_compiler_destroy(compiler);
    turbo_runtime_binary_schema_destroy(schema);
  }

  it("should reject string16 payloads whose declared length exceeds the buffer") {
    turbo_runtime_binary_schema_t *schema =
        turbo_runtime_binary_schema_create("root");
    turbo_runtime_binary_mir_compiler_t *compiler;
    turbo_runtime_binary_mir_parser_fn parser;
    const uint8_t payload[] = {0x04, 0x00, 'o', 'k'};
    char error[128];

    check_not_null(schema);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            schema, "name", TURBO_RUNTIME_BINARY_VALUE_STRING16),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);

    compiler = turbo_runtime_binary_mir_compiler_create(schema);
    check_not_null(compiler);
    check_int_eq(
        turbo_runtime_binary_mir_compiler_build(
            compiler, turbo_runtime_data_bind_default_value_api(), error,
            sizeof(error)),
        TURBO_RUNTIME_BINARY_MIR_OK);
    parser = turbo_runtime_binary_mir_compiler_parser(compiler);
    check_not_null(parser);
    check_null(parser(payload, sizeof(payload), NULL));

    turbo_runtime_binary_mir_compiler_destroy(compiler);
    turbo_runtime_binary_schema_destroy(schema);
  }

  it("should reject bytes16 payloads whose declared length exceeds the buffer") {
    turbo_runtime_binary_schema_t *schema =
        turbo_runtime_binary_schema_create("root");
    turbo_runtime_binary_mir_compiler_t *compiler;
    turbo_runtime_binary_mir_parser_fn parser;
    const uint8_t payload[] = {0x03, 0x00, 0xca, 0xfe};
    char error[128];

    check_not_null(schema);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            schema, "blob", TURBO_RUNTIME_BINARY_VALUE_BYTES16),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);

    compiler = turbo_runtime_binary_mir_compiler_create(schema);
    check_not_null(compiler);
    check_int_eq(
        turbo_runtime_binary_mir_compiler_build(
            compiler, turbo_runtime_data_bind_default_value_api(), error,
            sizeof(error)),
        TURBO_RUNTIME_BINARY_MIR_OK);
    parser = turbo_runtime_binary_mir_compiler_parser(compiler);
    check_not_null(parser);
    check_null(parser(payload, sizeof(payload), NULL));

    turbo_runtime_binary_mir_compiler_destroy(compiler);
    turbo_runtime_binary_schema_destroy(schema);
  }

  it("should parse repeated scalar fields through the repeated mir fast path") {
    turbo_runtime_binary_schema_t *schema =
        turbo_runtime_binary_schema_create("root");
    turbo_runtime_binary_mir_compiler_t *compiler;
    turbo_runtime_binary_mir_parser_fn parser;
    turbo_runtime_data_bind_value_t *value;
    const turbo_runtime_data_bind_value_t *scores;
    const turbo_runtime_data_bind_value_t *names;
    const uint8_t payload[] = {
        0x02, 0x00, 0x00, 0x00,
        0x05, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
        0x02, 0x00, 'o', 'k',
        0x03, 0x00, 'b', 'o', 'b'};
    char error[128];

    check_not_null(schema);
    check_int_eq(
        turbo_runtime_binary_schema_add_repeated_field(
            schema, "scores", TURBO_RUNTIME_BINARY_VALUE_I32),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_repeated_field(
            schema, "names", TURBO_RUNTIME_BINARY_VALUE_STRING16),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);

    compiler = turbo_runtime_binary_mir_compiler_create(schema);
    check_not_null(compiler);
    check_int_eq(
        turbo_runtime_binary_mir_compiler_build(
            compiler, turbo_runtime_data_bind_default_value_api(), error,
            sizeof(error)),
        TURBO_RUNTIME_BINARY_MIR_OK);
    parser = turbo_runtime_binary_mir_compiler_parser(compiler);
    check_not_null(parser);

    value = parser(payload, sizeof(payload), NULL);
    check_not_null(value);
    scores = turbo_runtime_data_bind_object_get(value, "scores");
    check_not_null(scores);
    check_size_eq(turbo_runtime_data_bind_value_size(scores), 2);
    check_int_eq(
        (int)turbo_runtime_data_bind_value_as_int64(
            turbo_runtime_data_bind_array_get(scores, 0), 0),
        5);
    check_int_eq(
        (int)turbo_runtime_data_bind_value_as_int64(
            turbo_runtime_data_bind_array_get(scores, 1), 0),
        8);
    names = turbo_runtime_data_bind_object_get(value, "names");
    check_not_null(names);
    check_size_eq(turbo_runtime_data_bind_value_size(names), 2);
    check_str_eq(
        turbo_runtime_data_bind_value_as_string(
            turbo_runtime_data_bind_array_get(names, 0)),
        "ok");
    check_str_eq(
        turbo_runtime_data_bind_value_as_string(
            turbo_runtime_data_bind_array_get(names, 1)),
        "bob");

    turbo_runtime_data_bind_default_value_api()->destroy_value(NULL, value);
    turbo_runtime_binary_mir_compiler_destroy(compiler);
    turbo_runtime_binary_schema_destroy(schema);
  }

  it("should reject truncated repeated scalar payloads on the repeated mir fast path") {
    turbo_runtime_binary_schema_t *schema =
        turbo_runtime_binary_schema_create("root");
    turbo_runtime_binary_mir_compiler_t *compiler;
    turbo_runtime_binary_mir_parser_fn parser;
    const uint8_t payload[] = {
        0x02, 0x00, 0x00, 0x00,
        0x05, 0x00, 0x00, 0x00};
    char error[128];

    check_not_null(schema);
    check_int_eq(
        turbo_runtime_binary_schema_add_repeated_field(
            schema, "scores", TURBO_RUNTIME_BINARY_VALUE_I32),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);

    compiler = turbo_runtime_binary_mir_compiler_create(schema);
    check_not_null(compiler);
    check_int_eq(
        turbo_runtime_binary_mir_compiler_build(
            compiler, turbo_runtime_data_bind_default_value_api(), error,
            sizeof(error)),
        TURBO_RUNTIME_BINARY_MIR_OK);
    parser = turbo_runtime_binary_mir_compiler_parser(compiler);
    check_not_null(parser);
    check_null(parser(payload, sizeof(payload), NULL));

    turbo_runtime_binary_mir_compiler_destroy(compiler);
    turbo_runtime_binary_schema_destroy(schema);
  }

  it("should reject trailing bytes on the repeated mir fast path") {
    turbo_runtime_binary_schema_t *schema =
        turbo_runtime_binary_schema_create("root");
    turbo_runtime_binary_mir_compiler_t *compiler;
    turbo_runtime_binary_mir_parser_fn parser;
    const uint8_t payload[] = {
        0x01, 0x00, 0x00, 0x00,
        0x05, 0x00, 0x00, 0x00,
        0xaa};
    char error[128];

    check_not_null(schema);
    check_int_eq(
        turbo_runtime_binary_schema_add_repeated_field(
            schema, "scores", TURBO_RUNTIME_BINARY_VALUE_I32),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);

    compiler = turbo_runtime_binary_mir_compiler_create(schema);
    check_not_null(compiler);
    check_int_eq(
        turbo_runtime_binary_mir_compiler_build(
            compiler, turbo_runtime_data_bind_default_value_api(), error,
            sizeof(error)),
        TURBO_RUNTIME_BINARY_MIR_OK);
    parser = turbo_runtime_binary_mir_compiler_parser(compiler);
    check_not_null(parser);
    check_null(parser(payload, sizeof(payload), NULL));

    turbo_runtime_binary_mir_compiler_destroy(compiler);
    turbo_runtime_binary_schema_destroy(schema);
  }

  it("should parse string-key map scalar fields through the map mir fast path") {
    turbo_runtime_binary_schema_t *schema =
        turbo_runtime_binary_schema_create("root");
    turbo_runtime_binary_mir_compiler_t *compiler;
    turbo_runtime_binary_mir_parser_fn parser;
    turbo_runtime_data_bind_value_t *value;
    const turbo_runtime_data_bind_value_t *scores;
    const turbo_runtime_data_bind_value_t *labels;
    const uint8_t payload[] = {
        0x02, 0x00, 0x00, 0x00,
        0x01, 0x00, 'a', 0x05, 0x00, 0x00, 0x00,
        0x01, 0x00, 'b', 0x08, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
        0x01, 0x00, 'x', 0x02, 0x00, 'o', 'k',
        0x01, 0x00, 'y', 0x03, 0x00, 'b', 'o', 'b'};
    char error[128];

    check_not_null(schema);
    check_int_eq(
        turbo_runtime_binary_schema_add_string_key_map_field(
            schema, "scores", TURBO_RUNTIME_BINARY_VALUE_I32),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_string_key_map_field(
            schema, "labels", TURBO_RUNTIME_BINARY_VALUE_STRING16),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);

    compiler = turbo_runtime_binary_mir_compiler_create(schema);
    check_not_null(compiler);
    check_int_eq(
        turbo_runtime_binary_mir_compiler_build(
            compiler, turbo_runtime_data_bind_default_value_api(), error,
            sizeof(error)),
        TURBO_RUNTIME_BINARY_MIR_OK);
    parser = turbo_runtime_binary_mir_compiler_parser(compiler);
    check_not_null(parser);

    value = parser(payload, sizeof(payload), NULL);
    check_not_null(value);
    scores = turbo_runtime_data_bind_object_get(value, "scores");
    check_not_null(scores);
    check_int_eq(
        (int)turbo_runtime_data_bind_value_as_int64(
            turbo_runtime_data_bind_object_get(scores, "a"), 0),
        5);
    check_int_eq(
        (int)turbo_runtime_data_bind_value_as_int64(
            turbo_runtime_data_bind_object_get(scores, "b"), 0),
        8);
    labels = turbo_runtime_data_bind_object_get(value, "labels");
    check_not_null(labels);
    check_str_eq(
        turbo_runtime_data_bind_value_as_string(
            turbo_runtime_data_bind_object_get(labels, "x")),
        "ok");
    check_str_eq(
        turbo_runtime_data_bind_value_as_string(
            turbo_runtime_data_bind_object_get(labels, "y")),
        "bob");

    turbo_runtime_data_bind_default_value_api()->destroy_value(NULL, value);
    turbo_runtime_binary_mir_compiler_destroy(compiler);
    turbo_runtime_binary_schema_destroy(schema);
  }

  it("should reject truncated map payloads on the map mir fast path") {
    turbo_runtime_binary_schema_t *schema =
        turbo_runtime_binary_schema_create("root");
    turbo_runtime_binary_mir_compiler_t *compiler;
    turbo_runtime_binary_mir_parser_fn parser;
    const uint8_t payload[] = {
        0x01, 0x00, 0x00, 0x00,
        0x03, 0x00, 'a'};
    char error[128];

    check_not_null(schema);
    check_int_eq(
        turbo_runtime_binary_schema_add_string_key_map_field(
            schema, "scores", TURBO_RUNTIME_BINARY_VALUE_I32),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);

    compiler = turbo_runtime_binary_mir_compiler_create(schema);
    check_not_null(compiler);
    check_int_eq(
        turbo_runtime_binary_mir_compiler_build(
            compiler, turbo_runtime_data_bind_default_value_api(), error,
            sizeof(error)),
        TURBO_RUNTIME_BINARY_MIR_OK);
    parser = turbo_runtime_binary_mir_compiler_parser(compiler);
    check_not_null(parser);
    check_null(parser(payload, sizeof(payload), NULL));

    turbo_runtime_binary_mir_compiler_destroy(compiler);
    turbo_runtime_binary_schema_destroy(schema);
  }
}
