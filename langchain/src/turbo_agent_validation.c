#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_state_core_internal.h"
#include "turbo_agent_state_output_internal.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_validation_internal.h"
#include "turbo_agent_util_internal.h"

#include <json_parser.h>
#include "turbo_prompt.h"

#include <fmt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *turbo_agent_schema_reason(const char *message, const char *field_name) {
  if (field_name && field_name[0] != '\0') {
    return (char *)tstr_cat_typed(tstr_new(), "{}: {}",
                                  message ? message : "schema validation failed", field_name);
  } else {
    return (char *)tstr_cat_typed(tstr_new(), "{}",
                                  message ? message : "schema validation failed");
  }
}

static int turbo_agent_json_schema_type_matches(const json_value_t *value,
                                                const char *type_name) {
  if (!type_name || type_name[0] == '\0') {
    return 1;
  }
  if (strcmp(type_name, "object") == 0) {
    return turbo_json_type(value) == TURBO_JSON_OBJECT;
  }
  if (strcmp(type_name, "array") == 0) {
    return turbo_json_type(value) == TURBO_JSON_ARRAY;
  }
  if (strcmp(type_name, "string") == 0) {
    return turbo_json_type(value) == TURBO_JSON_STRING;
  }
  if (strcmp(type_name, "number") == 0) {
    return turbo_json_type(value) == TURBO_JSON_NUMBER;
  }
  if (strcmp(type_name, "integer") == 0) {
    double num;
    return turbo_json_type(value) == TURBO_JSON_NUMBER &&
                   ((num = turbo_json_number(value)), num == (double)((long long)num))
               ? 1
               : 0;
  }
  if (strcmp(type_name, "boolean") == 0) {
    return turbo_json_type(value) == TURBO_JSON_BOOL;
  }
  if (strcmp(type_name, "null") == 0) {
    return turbo_json_type(value) == TURBO_JSON_NULL;
  }

  return 1;
}

static int turbo_agent_json_schema_validate_type(const json_value_t *value,
                                                 const json_value_t *type_schema,
                                                 char **out_reason) {
  size_t i;

  if (!type_schema) {
    return 1;
  }

  if (turbo_json_type(type_schema) == TURBO_JSON_STRING) {
    if (turbo_agent_json_schema_type_matches(value, turbo_json_string(type_schema))) {
      return 1;
    }
    if (out_reason) {
      *out_reason = turbo_agent_schema_reason("type mismatch", turbo_json_string(type_schema));
    }
    return 0;
  }

  if (turbo_json_type(type_schema) != TURBO_JSON_ARRAY) {
    return 1;
  }

  for (i = 0; i < turbo_json_array_size(type_schema); ++i) {
    const json_value_t *entry = turbo_json_array_get(type_schema, i);
    if (entry && turbo_json_type(entry) == TURBO_JSON_STRING &&
        turbo_agent_json_schema_type_matches(value, turbo_json_string(entry))) {
      return 1;
    }
  }

  if (out_reason) {
    *out_reason = turbo_agent_schema_reason("type mismatch", "allowed types");
  }
  return 0;
}

static int turbo_agent_json_schema_validate(const json_value_t *value,
                                            const json_value_t *schema,
                                            char **out_reason) {
  const json_value_t *type_schema;
  const json_value_t *required;
  const json_value_t *properties;
  const json_value_t *items;
  size_t i;

  if (out_reason) {
    *out_reason = NULL;
  }

  if (!schema || turbo_json_type(schema) != TURBO_JSON_OBJECT) {
    return 1;
  }

  type_schema = turbo_json_object_get(schema, "type");
  if (!turbo_agent_json_schema_validate_type(value, type_schema, out_reason)) {
    return 0;
  }

  required = turbo_json_object_get(schema, "required");
  if (required && turbo_json_type(required) == TURBO_JSON_ARRAY &&
      turbo_json_type(value) == TURBO_JSON_OBJECT) {
    for (i = 0; i < turbo_json_array_size(required); ++i) {
      const json_value_t *required_entry = turbo_json_array_get(required, i);
      const char *required_name;
      if (!required_entry || turbo_json_type(required_entry) != TURBO_JSON_STRING) {
        continue;
      }
      required_name = turbo_json_string(required_entry);
      if (!turbo_json_object_get(value, required_name)) {
        if (out_reason) {
          *out_reason = turbo_agent_schema_reason("missing required property", required_name);
        }
        return 0;
      }
    }
  }

  properties = turbo_json_object_get(schema, "properties");
  if (properties && turbo_json_type(properties) == TURBO_JSON_OBJECT &&
      turbo_json_type(value) == TURBO_JSON_OBJECT) {
    for (i = 0; i < turbo_json_object_size(properties); ++i) {
      const char *key = turbo_json_object_key(properties, i);
      const json_value_t *property_schema = turbo_json_object_value(properties, i);
      const json_value_t *property_value = key ? turbo_json_object_get(value, key) : NULL;
      char *child_reason = NULL;

      if (!key || !property_value) {
        continue;
      }
      if (!turbo_agent_json_schema_validate(property_value, property_schema, &child_reason)) {
        if (out_reason) {
          *out_reason = child_reason ? child_reason
                                     : turbo_agent_schema_reason("invalid property", key);
        } else {
          tstr_free(child_reason);
        }
        return 0;
      }
    }
  }

  if (turbo_json_type(value) == TURBO_JSON_OBJECT &&
      !turbo_json_get_bool(schema, "additionalProperties", true)) {
    for (i = 0; i < turbo_json_object_size(value); ++i) {
      const char *key = turbo_json_object_key(value, i);
      if (key && (!properties || turbo_json_type(properties) != TURBO_JSON_OBJECT ||
                  !turbo_json_object_get(properties, key))) {
        if (out_reason) {
          *out_reason = turbo_agent_schema_reason("unexpected property", key);
        }
        return 0;
      }
    }
  }

  items = turbo_json_object_get(schema, "items");
  if (items && turbo_json_type(value) == TURBO_JSON_ARRAY) {
    for (i = 0; i < turbo_json_array_size(value); ++i) {
      const json_value_t *item = turbo_json_array_get(value, i);
      char *child_reason = NULL;
      if (!turbo_agent_json_schema_validate(item, items, &child_reason)) {
        if (out_reason) {
          *out_reason = child_reason ? child_reason
                                     : turbo_agent_schema_reason("invalid array item", NULL);
        } else {
          tstr_free(child_reason);
        }
        return 0;
      }
    }
  }

  return 1;
}

CXX_C_API int turbo_agent_structured_output_valid_for_state(const turbo_agent_t *agent,
                                                            const json_value_t *state,
                                                            char **out_reason) {
  json_value_t *parsed = NULL;
  json_value_t *schema = NULL;
  int valid = 0;

  if (!agent) {
    return 0;
  }

  if (!agent->structured_output_schema_json || agent->structured_output_schema_json[0] == '\0') {
    if (out_reason) {
      *out_reason = NULL;
    }
    return 1;
  }

  if (turbo_agent_state_parse_final_output_json(state, &parsed) != 0) {
    if (out_reason) {
      *out_reason = turbo_agent_util_strdup("response was not valid JSON");
    }
    return 0;
  }

  if (turbo_parse_json((const uint8_t *)agent->structured_output_schema_json,
                       strlen(agent->structured_output_schema_json), &schema) != 0) {
    turbo_free_json(&parsed);
    if (out_reason) {
      *out_reason = turbo_agent_util_strdup(
          "failed to parse structured output schema");
    }
    return 0;
  }

  valid = turbo_agent_json_schema_validate(parsed, schema, out_reason);
  turbo_free_json(&schema);
  turbo_free_json(&parsed);
  return valid;
}

CXX_C_API json_value_t *turbo_agent_build_structured_retry_state(const json_value_t *state,
                                                                 size_t attempt_index,
                                                                 const char *reason) {
  static const char *retry_prefix =
      "Your previous response did not satisfy the requested JSON schema. Return only valid JSON "
      "that matches the requested schema. Do not include markdown fences or extra prose.";
  json_value_t *retry_state = NULL;
  json_value_t *input = NULL;
  json_value_t *message = NULL;
  char *prompt = NULL;
  int needed;

  if (!state) {
    return NULL;
  }

  if (turbo_agent_clone_json(state, &retry_state) != TURBO_GRAPH_EXEC_OK) {
    return NULL;
  }

  input = turbo_agent_state_get_array(retry_state, "input");
  if (!input) {
    turbo_free_json(&retry_state);
    return NULL;
  }

  if (reason && reason[0] != '\0') {
    prompt = (char *)tstr_cat_typed(tstr_new(), "{} Reason: {}. Retry attempt {}.", retry_prefix,
                                    reason, (unsigned long)(attempt_index + 1));
  } else {
    prompt = (char *)tstr_cat_typed(tstr_new(), "{} Retry attempt {}.", retry_prefix,
                                    (unsigned long)(attempt_index + 1));
  }
  if (!prompt) {
    turbo_free_json(&retry_state);
    return NULL;
  }

  message = turbo_prompt_message_create("user", prompt);
  tstr_free(prompt);
  if (!message) {
    turbo_free_json(&retry_state);
    return NULL;
  }

  turbo_json_array_add(input, message);
  return retry_state;
}
