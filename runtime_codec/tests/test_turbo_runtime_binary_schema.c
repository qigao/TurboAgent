#include "tinytest.h"
#include "turbo_runtime_binary_schema.h"
#include <turbo_parser.h>
#include <string.h>

spec("turbo runtime codec binary schema") {

  it("should collect abi requirements from nested binary schemas") {
    turbo_runtime_binary_schema_t *child =
        turbo_runtime_binary_schema_create("child");
    turbo_runtime_binary_schema_t *root =
        turbo_runtime_binary_schema_create("root");
    turbo_runtime_binary_abi_requirements_t requirements;
    turbo_runtime_binary_field_descriptor_t field = {0};

    check_not_null(child);
    check_not_null(root);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            child, "name", TURBO_RUNTIME_BINARY_VALUE_STRING16),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            child, "blob", TURBO_RUNTIME_BINARY_VALUE_BYTES16),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            root, "flag", TURBO_RUNTIME_BINARY_VALUE_BOOL),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_repeated_field(
            root, "scores", TURBO_RUNTIME_BINARY_VALUE_I32),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_string_key_map_field(
            root, "labels", TURBO_RUNTIME_BINARY_VALUE_F64),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_object_field(root, "child", child),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);

    check_size_eq(turbo_runtime_binary_schema_field_count(root), 4);
    check_int_eq(
        turbo_runtime_binary_schema_get_field(root, 3, &field),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_str_eq(field.name, "child");
    check_int_eq(field.shape, TURBO_RUNTIME_BINARY_FIELD_OBJECT);
    check_not_null(field.object_schema);

    memset(&requirements, 0, sizeof(requirements));
    check_int_eq(
        turbo_runtime_binary_schema_collect_abi_requirements(root, &requirements),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_true(requirements.create_bool);
    check_true(requirements.create_int64);
    check_true(requirements.create_double);
    check_true(requirements.create_string);
    check_true(requirements.create_bytes);
    check_true(requirements.create_object);
    check_true(requirements.create_array);
    check_true(requirements.object_set);
    check_true(requirements.array_append);
    check_true(requirements.destroy_value);

    turbo_runtime_binary_schema_destroy(root);
    turbo_runtime_binary_schema_destroy(child);
  }

  it("should validate a value api against schema requirements") {
    turbo_runtime_binary_schema_t *root =
        turbo_runtime_binary_schema_create("root");
    char error[128];

    check_not_null(root);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            root, "blob", TURBO_RUNTIME_BINARY_VALUE_BYTES16),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);

    check_int_eq(
        turbo_runtime_binary_schema_validate_value_api(
            root, turbo_runtime_data_bind_default_value_api(), error,
            sizeof(error)),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_validate_value_api(
            root, turbo_runtime_data_bind_json_value_api(), error,
            sizeof(error)),
        TURBO_RUNTIME_BINARY_SCHEMA_UNSUPPORTED);
    check_true(strstr(error, "create_bytes") != NULL);

    turbo_runtime_binary_schema_destroy(root);
  }

  it("should validate canonical binary payloads") {
    turbo_runtime_binary_schema_t *child =
        turbo_runtime_binary_schema_create("child");
    turbo_runtime_binary_schema_t *root =
        turbo_runtime_binary_schema_create("root");
    const uint8_t payload[] = {
        0x01,
        0x02, 0x00, 0x00, 0x00,
        0x0a, 0x00, 0x00, 0x00,
        0x14, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x03, 0x00, 'p', 'i', ' ',
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x40,
        0x03, 0x00, 'b', 'o', 'b',
        0x02, 0x00, 0xde, 0xad};
    const uint8_t truncated[] = {
        0x01,
        0x01, 0x00, 0x00, 0x00,
        0x0a, 0x00};
    char error[128];

    check_not_null(child);
    check_not_null(root);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            child, "name", TURBO_RUNTIME_BINARY_VALUE_STRING16),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            child, "blob", TURBO_RUNTIME_BINARY_VALUE_BYTES16),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            root, "flag", TURBO_RUNTIME_BINARY_VALUE_BOOL),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_repeated_field(
            root, "scores", TURBO_RUNTIME_BINARY_VALUE_I32),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_string_key_map_field(
            root, "labels", TURBO_RUNTIME_BINARY_VALUE_F64),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_object_field(root, "child", child),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);

    check_int_eq(
        turbo_runtime_binary_schema_validate_payload(root, payload,
                                                    sizeof(payload), error,
                                                    sizeof(error)),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_validate_payload(root, truncated,
                                                    sizeof(truncated), error,
                                                    sizeof(error)),
        TURBO_RUNTIME_BINARY_SCHEMA_TRUNCATED);
    check_true(strstr(error, "scores") != NULL);

    turbo_runtime_binary_schema_destroy(root);
    turbo_runtime_binary_schema_destroy(child);
  }

  it("should parse canonical binary payloads into runtime values") {
    turbo_runtime_binary_schema_t *child =
        turbo_runtime_binary_schema_create("child");
    turbo_runtime_binary_schema_t *root =
        turbo_runtime_binary_schema_create("root");
    const uint8_t payload[] = {
        0x01,
        0x02, 0x00, 0x00, 0x00,
        0x0a, 0x00, 0x00, 0x00,
        0x14, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x03, 0x00, 'p', 'i', ' ',
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x40,
        0x03, 0x00, 'b', 'o', 'b',
        0x02, 0x00, 0xde, 0xad};
    turbo_runtime_data_bind_value_t *value;
    const turbo_runtime_data_bind_value_t *scores;
    const turbo_runtime_data_bind_value_t *labels;
    const turbo_runtime_data_bind_value_t *child_value;
    const turbo_runtime_data_bind_value_t *blob;
    char error[128];

    check_not_null(child);
    check_not_null(root);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            child, "name", TURBO_RUNTIME_BINARY_VALUE_STRING16),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            child, "blob", TURBO_RUNTIME_BINARY_VALUE_BYTES16),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            root, "flag", TURBO_RUNTIME_BINARY_VALUE_BOOL),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_repeated_field(
            root, "scores", TURBO_RUNTIME_BINARY_VALUE_I32),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_string_key_map_field(
            root, "labels", TURBO_RUNTIME_BINARY_VALUE_F64),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_object_field(root, "child", child),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);

    value = turbo_runtime_binary_schema_parse(
        root, payload, sizeof(payload), turbo_runtime_data_bind_default_value_api(),
        NULL, error, sizeof(error));
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
        10);
    check_int_eq(
        (int)turbo_runtime_data_bind_value_as_int64(
            turbo_runtime_data_bind_array_get(scores, 1), 0),
        20);

    labels = turbo_runtime_data_bind_object_get(value, "labels");
    check_not_null(labels);
    check_double_eq(
        turbo_runtime_data_bind_value_as_double(
            turbo_runtime_data_bind_object_get(labels, "pi "), 0.0),
        4.5, 0.000001);

    child_value = turbo_runtime_data_bind_object_get(value, "child");
    check_not_null(child_value);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(child_value, "name")),
                 "bob");
    blob = turbo_runtime_data_bind_object_get(child_value, "blob");
    check_not_null(blob);
    check_size_eq(turbo_runtime_data_bind_value_data_size(blob), 2);
    check_int_eq((int)turbo_runtime_data_bind_value_as_bytes(blob)[0], 0xde);
    check_int_eq((int)turbo_runtime_data_bind_value_as_bytes(blob)[1], 0xad);

    turbo_runtime_data_bind_default_value_api()->destroy_value(NULL, value);
    turbo_runtime_binary_schema_destroy(root);
    turbo_runtime_binary_schema_destroy(child);
  }

  it("should parse compatible payloads into the json value api") {
    turbo_runtime_binary_schema_t *root =
        turbo_runtime_binary_schema_create("root");
    const uint8_t payload[] = {
        0x01, 0x00, 'x',
        0x02, 0x00, 0x00, 0x00,
        0x07, 0x00, 0x00, 0x00,
        0x09, 0x00, 0x00, 0x00};
    turbo_runtime_data_bind_value_t *value;
    json_value_t *json_value;
    char error[128];

    check_not_null(root);
    check_int_eq(
        turbo_runtime_binary_schema_add_scalar_field(
            root, "name", TURBO_RUNTIME_BINARY_VALUE_STRING16),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);
    check_int_eq(
        turbo_runtime_binary_schema_add_repeated_field(
            root, "scores", TURBO_RUNTIME_BINARY_VALUE_I32),
        TURBO_RUNTIME_BINARY_SCHEMA_OK);

    value = turbo_runtime_binary_schema_parse(
        root, payload, sizeof(payload), turbo_runtime_data_bind_json_value_api(),
        NULL, error, sizeof(error));
    check_not_null(value);
    json_value = (json_value_t *)value;
    check_str_eq(turbo_json_get_string(json_value, "name"), "x");
    check_int_eq((int)turbo_json_number(
                     turbo_json_array_get(turbo_json_object_get(json_value, "scores"), 0)),
                 7);
    check_int_eq((int)turbo_json_number(
                     turbo_json_array_get(turbo_json_object_get(json_value, "scores"), 1)),
                 9);

    turbo_runtime_data_bind_json_value_api()->destroy_value(NULL, value);
    turbo_runtime_binary_schema_destroy(root);
  }
}
