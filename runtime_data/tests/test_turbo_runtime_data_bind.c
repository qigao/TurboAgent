#include "tinytest.h"
#include "turbo_runtime_data_bind.h"
#include "turbo_parser.h"

#include <string.h>

spec("turbo runtime data bind") {

  it("should build and inspect a default runtime value tree") {
    turbo_runtime_data_bind_value_t *root = NULL;
    turbo_runtime_data_bind_value_t *tags = NULL;
    turbo_runtime_data_bind_value_t *name = NULL;
    turbo_runtime_data_bind_value_t *count = NULL;
    turbo_runtime_data_bind_value_t *tag0 = NULL;
    turbo_runtime_data_bind_value_t *tag1 = NULL;
    uint8_t bytes[3] = {1, 2, 3};
    turbo_runtime_data_bind_value_t *payload = NULL;
    const turbo_runtime_data_bind_value_t *value = NULL;

    root = turbo_runtime_data_bind_value_create_object();
    tags = turbo_runtime_data_bind_value_create_array();
    name = turbo_runtime_data_bind_value_create_string("langchain");
    count = turbo_runtime_data_bind_value_create_int64(2);
    tag0 = turbo_runtime_data_bind_value_create_string("native");
    tag1 = turbo_runtime_data_bind_value_create_string("wasm3");
    payload = turbo_runtime_data_bind_value_create_bytes(bytes, sizeof(bytes));

    check_not_null(root);
    check_not_null(tags);
    check_not_null(name);
    check_not_null(count);
    check_not_null(tag0);
    check_not_null(tag1);
    check_not_null(payload);

    check_int_eq(turbo_runtime_data_bind_object_set(root, "name", name),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(root, "count", count),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_array_append(tags, tag0),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_array_append(tags, tag1),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(root, "tags", tags),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(root, "payload", payload),
                 TURBO_RUNTIME_DATA_BIND_OK);

    check_int_eq(turbo_runtime_data_bind_value_kind(root),
                 TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT);
    check_size_eq(turbo_runtime_data_bind_value_size(root), 4);
    check_str_eq(turbo_runtime_data_bind_object_key_at(root, 0), "name");
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(root, "name")),
                 "langchain");
    check_int_eq((int)turbo_runtime_data_bind_value_as_int64(
                     turbo_runtime_data_bind_object_get(root, "count"), -1),
                 2);
    value = turbo_runtime_data_bind_object_get(root, "tags");
    check_not_null(value);
    check_size_eq(turbo_runtime_data_bind_value_size(value), 2);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_array_get(value, 1)),
                 "wasm3");
    value = turbo_runtime_data_bind_object_get(root, "payload");
    check_not_null(value);
    check_size_eq(turbo_runtime_data_bind_value_data_size(value), 3);
    check_int_eq((int)turbo_runtime_data_bind_value_as_bytes(value)[2], 3);

    turbo_runtime_data_bind_value_destroy(root);
  }

  it("should expose a builder vtable for future MIR-backed codecs") {
    const turbo_runtime_data_bind_value_api_t *api =
        turbo_runtime_data_bind_default_value_api();
    turbo_runtime_data_bind_value_t *root = NULL;
    turbo_runtime_data_bind_value_t *enabled = NULL;

    check_not_null(api);
    check_not_null(api->create_object);
    check_not_null(api->create_bool);
    check_not_null(api->object_set);
    check_not_null(api->destroy_value);

    root = api->create_object(NULL);
    enabled = api->create_bool(NULL, 1);
    check_not_null(root);
    check_not_null(enabled);
    check_int_eq(api->object_set(NULL, root, "enabled", enabled),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_value_as_bool(
                     turbo_runtime_data_bind_object_get(root, "enabled"), 0),
                 1);

    api->destroy_value(NULL, root);
  }

  it("should convert between runtime values and json") {
    json_value_t *json_root = turbo_json_create_object();
    json_value_t *json_tags = turbo_json_create_array();
    turbo_runtime_data_bind_value_t *bound = NULL;
    json_value_t *roundtrip = NULL;

    check_not_null(json_root);
    check_not_null(json_tags);

    turbo_json_object_set_string(json_root, "name", "langchain");
    turbo_json_object_set_number(json_root, "count", 2.0);
    turbo_json_array_add(json_tags, turbo_json_create_string("native"));
    turbo_json_array_add(json_tags, turbo_json_create_string("json"));
    turbo_json_object_add(json_root, "tags", json_tags);

    bound = turbo_runtime_data_bind_value_from_json(json_root);
    check_not_null(bound);
    check_int_eq((int)turbo_runtime_data_bind_value_as_int64(
                     turbo_runtime_data_bind_object_get(bound, "count"), -1),
                 2);

    roundtrip = turbo_runtime_data_bind_value_to_json(bound);
    check_not_null(roundtrip);
    check_str_eq(turbo_json_get_string(roundtrip, "name"), "langchain");
    check_size_eq(turbo_json_array_size(turbo_json_object_get(roundtrip, "tags")), 2);

    turbo_runtime_data_bind_value_destroy(bound);
    turbo_free_json(&roundtrip);
    turbo_free_json(&json_root);
  }

  it("should expose a json builder adapter") {
    const turbo_runtime_data_bind_value_api_t *api =
        turbo_runtime_data_bind_json_value_api();
    turbo_runtime_data_bind_value_t *root = NULL;
    turbo_runtime_data_bind_value_t *items = NULL;
    turbo_runtime_data_bind_value_t *item = NULL;
    json_value_t *json_root = NULL;

    check_not_null(api);
    check_not_null(api->create_object);
    check_not_null(api->create_array);
    check_not_null(api->create_string);
    check_null(api->create_bytes);

    root = api->create_object(NULL);
    items = api->create_array(NULL);
    item = api->create_string(NULL, "tool");
    check_not_null(root);
    check_not_null(items);
    check_not_null(item);
    check_int_eq(api->array_append(NULL, items, item), TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(api->object_set(NULL, root, "items", items),
                 TURBO_RUNTIME_DATA_BIND_OK);

    json_root = (json_value_t *)root;
    check_size_eq(turbo_json_array_size(turbo_json_object_get(json_root, "items")), 1);
    check_str_eq(turbo_json_string(
                     turbo_json_array_get(turbo_json_object_get(json_root, "items"), 0)),
                 "tool");

    api->destroy_value(NULL, root);
  }
}
