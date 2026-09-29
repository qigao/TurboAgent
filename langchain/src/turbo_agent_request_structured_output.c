#include "turbo_agent_request_structured_output_internal.h"

#include <json_parser.h>

#include <string.h>

CXX_C_API int turbo_agent_request_add_structured_output(const turbo_agent_t *agent,
                                                        json_value_t *request, int chat_mode,
                                                        int compatible_mode,
                                                        int anthropic_mode) {
  json_value_t *schema = NULL;
  json_value_t *response_format = NULL;
  json_value_t *json_schema = NULL;
  json_value_t *text = NULL;
  json_value_t *format = NULL;

  if (!agent || !request) {
    return -1;
  }

  if (!agent->structured_output_schema_json || agent->structured_output_schema_json[0] == '\0') {
    return 0;
  }

  if (anthropic_mode) {
    return 0;
  }

  schema = json_parse(agent->structured_output_schema_json,
                      strlen(agent->structured_output_schema_json));
  if (!schema || json_type(schema) != JSON_OBJECT) {
    json_free(schema); schema = NULL;
    return -1;
  }

  if (chat_mode) {
    response_format = json_create_object();
    json_schema = json_create_object();
    if (!response_format || !json_schema) {
      json_free(schema); schema = NULL;
      json_free(response_format); response_format = NULL;
      json_free(json_schema); json_schema = NULL;
      return -1;
    }

    json_object_set_string(response_format, "type", "json_schema");
    json_object_set_string(json_schema, "name",
                                 (agent->structured_output_name &&
                                  agent->structured_output_name[0] != '\0')
                                     ? agent->structured_output_name
                                     : "structured_output");
    json_object_add(json_schema, "schema", schema);
    json_object_set_bool(json_schema, "strict",
                               agent->structured_output_strict ? true : false);
    json_object_add(response_format, "json_schema", json_schema);
    json_object_add(request, compatible_mode ? "response_format" : "response_format",
                          response_format);
    return 0;
  }

  text = json_create_object();
  format = json_create_object();
  if (!text || !format) {
    json_free(schema); schema = NULL;
    json_free(text); text = NULL;
    json_free(format); format = NULL;
    return -1;
  }

  json_object_set_string(format, "type", "json_schema");
  json_object_set_string(format, "name",
                               (agent->structured_output_name &&
                                agent->structured_output_name[0] != '\0')
                                   ? agent->structured_output_name
                                   : "structured_output");
  json_object_add(format, "schema", schema);
  json_object_set_bool(format, "strict", agent->structured_output_strict ? true : false);
  json_object_add(text, "format", format);
  json_object_add(request, "text", text);
  return 0;
}
