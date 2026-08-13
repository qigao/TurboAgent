#include "tinytest.h"
#include "turbo_runtime_json.h"

#include <stdint.h>

spec("runtime json") {
  it("uses TurboParser as the owned value tree") {
    json_value_t *root = turbo_json_create_object();
    json_value_t *items = turbo_json_create_array();

    check_not_null(root);
    check_not_null(items);
    check_int_eq(turbo_runtime_json_array_append(items, turbo_json_create_string("one")),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_object_set(root, "items", items),
                 TURBO_RUNTIME_JSON_OK);
    check_size_eq(turbo_runtime_json_value_size(
                      turbo_json_object_get(root, "items")),
                  1);
    check_str_eq(turbo_runtime_json_value_as_string(turbo_json_array_get(
                     turbo_json_object_get(root, "items"), 0)),
                 "one");

    turbo_runtime_json_destroy(root);
  }

  it("replaces an existing object member") {
    json_value_t *root = turbo_json_create_object();

    check_int_eq(turbo_runtime_json_object_set(root, "value", turbo_json_create_int64(1)),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_object_set(root, "value", turbo_json_create_int64(2)),
                 TURBO_RUNTIME_JSON_OK);
    check_size_eq(turbo_json_object_size(root), 1);
    check_true(turbo_runtime_json_value_as_int64(
                   turbo_json_object_get(root, "value"), 0) == INT64_C(2));

    turbo_runtime_json_destroy(root);
  }

  it("reads exact int64 tokens without double precision loss") {
    json_value_t *value = turbo_json_create_int64(INT64_C(9007199254740993));

    check_not_null(value);
    check_true(turbo_runtime_json_value_as_int64(value, 0) ==
               INT64_C(9007199254740993));

    turbo_runtime_json_destroy(value);
  }

  it("uses TurboParser for YAML and XML formats") {
    static const uint8_t yaml[] = "name: agent\ncount: 2\n";
    static const uint8_t xml[] = "<root><name>agent</name></root>";
    turbo_yaml_doc_t *yaml_doc = NULL;
    turbo_xml_doc_t *xml_doc = NULL;
    json_value_t *yaml_json = NULL;
    turbo_xml_node_t *xml_root = NULL;
    char *xml_name = NULL;

    check_int_eq(turbo_parse_yaml(yaml, sizeof(yaml) - 1, &yaml_doc), 0);
    check_not_null(yaml_doc);
    yaml_json = turbo_yaml_to_json(yaml_doc);
    check_not_null(yaml_json);
    check_str_eq(turbo_runtime_json_value_as_string(
                     turbo_json_object_get(yaml_json, "name")),
                 "agent");
    check_true(turbo_runtime_json_value_as_int64(
                   turbo_json_object_get(yaml_json, "count"), 0) == INT64_C(2));

    check_int_eq(turbo_parse_xml(xml, sizeof(xml) - 1, &xml_doc), 0);
    check_not_null(xml_doc);
    xml_root = turbo_xml_root_element(xml_doc);
    check_not_null(xml_root);
    xml_name = turbo_xml_child_text_dup(xml_root, "name");
    check_str_eq(xml_name, "agent");

    turbo_xml_string_free(xml_name);
    turbo_free_xml(&xml_doc);
    turbo_runtime_json_destroy(yaml_json);
    turbo_free_yaml(&yaml_doc);
  }
}
