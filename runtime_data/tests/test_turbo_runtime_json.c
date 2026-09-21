#include "tinytest.h"
#include "turbo_runtime_json.h"

#include <cyaml.h>
#include <cyaml_json_adapter.h>
#include <xml_parser/xml_parser.h>

#include <stdint.h>

spec("runtime json") {
  it("uses SaltsUtils JSON as the owned value tree") {
    json_value_t *root = json_create_object();
    json_value_t *items = json_create_array();

    check_not_null(root);
    check_not_null(items);
    check_int_eq(turbo_runtime_json_array_append(items, json_create_string("one")),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_object_set(root, "items", items),
                 TURBO_RUNTIME_JSON_OK);
    check_size_eq(turbo_runtime_json_value_size(
                      json_object_get(root, "items")),
                  1);
    check_str_eq(turbo_runtime_json_value_as_string(json_array_get(
                     json_object_get(root, "items"), 0)),
                 "one");

    turbo_runtime_json_destroy(root);
  }

  it("replaces an existing object member") {
    json_value_t *root = json_create_object();

    check_int_eq(turbo_runtime_json_object_set(root, "value", json_create_int64(1)),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_object_set(root, "value", json_create_int64(2)),
                 TURBO_RUNTIME_JSON_OK);
    check_size_eq(json_object_size(root), 1);
    check_true(turbo_runtime_json_value_as_int64(
                   json_object_get(root, "value"), 0) == INT64_C(2));

    turbo_runtime_json_destroy(root);
  }

  it("reads exact int64 tokens without double precision loss") {
    json_value_t *value = json_create_int64(INT64_C(9007199254740993));

    check_not_null(value);
    check_true(turbo_runtime_json_value_as_int64(value, 0) ==
               INT64_C(9007199254740993));

    turbo_runtime_json_destroy(value);
  }

  it("uses SaltsUtils for YAML and XML formats") {
    static const uint8_t yaml[] = "name: agent\ncount: 2\n";
    static const uint8_t xml[] = "<root><name>agent</name></root>";
    cyaml_doc_t *yaml_doc = NULL;
    salts_xml_document xml_doc = {0};
    json_value_t *yaml_json = NULL;
    salts_xml_node xml_root = {0};
    salts_xml_node xml_name_node = {0};
    char *xml_name = NULL;

    yaml_doc = cyaml_parse((const char *)yaml, sizeof(yaml) - 1, NULL, NULL);
    check_not_null(yaml_doc);
    yaml_json = json_value_from_cyaml(yaml_doc);
    check_not_null(yaml_json);
    check_str_eq(turbo_runtime_json_value_as_string(
                     json_object_get(yaml_json, "name")),
                 "agent");
    check_true(turbo_runtime_json_value_as_int64(
                   json_object_get(yaml_json, "count"), 0) == INT64_C(2));

    check_int_eq(salts_xml_parse(&xml_doc, (const char *)xml, sizeof(xml) - 1, NULL, NULL),
                 SALTS_XML_OK);
    xml_root = salts_xml_document_root(&xml_doc);
    check_not_null(xml_root.impl);
    xml_name_node = salts_xml_node_find(xml_root, "name");
    check_not_null(xml_name_node.impl);
    xml_name = salts_xml_node_text_dup(xml_name_node);
    check_str_eq(xml_name, "agent");

    salts_xml_owned_string_free(xml_name);
    salts_xml_document_destroy(&xml_doc);
    turbo_runtime_json_destroy(yaml_json);
    cyaml_free(yaml_doc);
  }
}
