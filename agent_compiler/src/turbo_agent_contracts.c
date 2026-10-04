#include "turbo_agent_contracts.h"

#include "turbo_tool_schema.h"

#include <string.h>

typedef enum turbo_agent_contract_type_e {
  TURBO_AGENT_CONTRACT_TYPE_UNKNOWN = 0,
  TURBO_AGENT_CONTRACT_TYPE_OBJECT,
  TURBO_AGENT_CONTRACT_TYPE_ARRAY,
  TURBO_AGENT_CONTRACT_TYPE_STRING,
  TURBO_AGENT_CONTRACT_TYPE_INTEGER,
  TURBO_AGENT_CONTRACT_TYPE_NUMBER,
  TURBO_AGENT_CONTRACT_TYPE_BOOLEAN,
  TURBO_AGENT_CONTRACT_TYPE_NULL
} turbo_agent_contract_type_t;

static turbo_agent_contract_type_t contract_type(
    const json_value_t *schema) {
  const json_value_t *type;
  const char *name;
  if (!schema || json_type(schema) != JSON_OBJECT) {
    return TURBO_AGENT_CONTRACT_TYPE_UNKNOWN;
  }
  type = json_object_get(schema, "type");
  if (!type || json_type(type) != JSON_STRING) {
    return TURBO_AGENT_CONTRACT_TYPE_UNKNOWN;
  }
  name = json_string(type);
  if (!name) return TURBO_AGENT_CONTRACT_TYPE_UNKNOWN;
  if (strcmp(name, "object") == 0) return TURBO_AGENT_CONTRACT_TYPE_OBJECT;
  if (strcmp(name, "array") == 0) return TURBO_AGENT_CONTRACT_TYPE_ARRAY;
  if (strcmp(name, "string") == 0) return TURBO_AGENT_CONTRACT_TYPE_STRING;
  if (strcmp(name, "integer") == 0) return TURBO_AGENT_CONTRACT_TYPE_INTEGER;
  if (strcmp(name, "number") == 0) return TURBO_AGENT_CONTRACT_TYPE_NUMBER;
  if (strcmp(name, "boolean") == 0) return TURBO_AGENT_CONTRACT_TYPE_BOOLEAN;
  if (strcmp(name, "null") == 0) return TURBO_AGENT_CONTRACT_TYPE_NULL;
  return TURBO_AGENT_CONTRACT_TYPE_UNKNOWN;
}

static int contract_scalar(turbo_agent_contract_type_t type) {
  return type == TURBO_AGENT_CONTRACT_TYPE_STRING ||
         type == TURBO_AGENT_CONTRACT_TYPE_INTEGER ||
         type == TURBO_AGENT_CONTRACT_TYPE_NUMBER ||
         type == TURBO_AGENT_CONTRACT_TYPE_BOOLEAN ||
         type == TURBO_AGENT_CONTRACT_TYPE_NULL;
}

static int contract_type_only_scalar(
    const json_value_t *schema,
    turbo_agent_contract_type_t *out_type) {
  turbo_agent_contract_type_t type;
  if (out_type) *out_type = TURBO_AGENT_CONTRACT_TYPE_UNKNOWN;
  if (!schema || json_type(schema) != JSON_OBJECT ||
      json_object_size(schema) != 1u ||
      !json_object_get(schema, "type")) {
    return 0;
  }
  type = contract_type(schema);
  if (!contract_scalar(type)) return 0;
  if (out_type) *out_type = type;
  return 1;
}

static int contract_definition_by_name(
    const turbo_tool_registry_t *registry,
    const char *name,
    turbo_tool_definition_t *out_definition) {
  const char *canonical = NULL;
  size_t index;
  if (!registry || !name || !out_definition) return 0;
  if (turbo_tool_registry_resolve_name(
          registry, name, &canonical) != TURBO_TOOL_OK ||
      !canonical) {
    return 0;
  }
  for (index = 0; index < turbo_tool_registry_count(registry); ++index) {
    turbo_tool_definition_t definition = {0};
    if (turbo_tool_registry_get_definition(
            registry, index, &definition) != TURBO_TOOL_OK) {
      return 0;
    }
    if (definition.name && strcmp(definition.name, canonical) == 0) {
      *out_definition = definition;
      return 1;
    }
  }
  return 0;
}

turbo_agent_contract_compatibility_t
turbo_agent_tool_result_slot_compatibility(
    const turbo_tool_registry_t *registry,
    const char *producer_tool,
    const char *consumer_tool,
    const char *consumer_property) {
  const char *result_schema_json = NULL;
  const json_value_t *result_schema = NULL;
  int strict_result = 0;
  turbo_tool_definition_t consumer = {0};
  json_value_t *owned_parameters = NULL;
  const json_value_t *parameters;
  const json_value_t *properties;
  const json_value_t *slot;
  turbo_agent_contract_type_t producer_type;
  turbo_agent_contract_type_t consumer_type;

  if (!registry || !producer_tool || !consumer_tool ||
      !consumer_property || !consumer_property[0]) {
    return TURBO_AGENT_CONTRACT_UNKNOWN;
  }

  if (turbo_tool_registry_get_result_contract(
          registry, producer_tool, &result_schema_json,
          &result_schema, &strict_result) != TURBO_TOOL_OK ||
      !result_schema_json || !result_schema) {
    return TURBO_AGENT_CONTRACT_UNKNOWN;
  }
  (void)strict_result;

  if (!contract_definition_by_name(registry, consumer_tool, &consumer)) {
    return TURBO_AGENT_CONTRACT_UNKNOWN;
  }
  if (consumer.parameters_schema) {
    parameters = consumer.parameters_schema;
  } else {
    owned_parameters = turbo_tool_schema_parse_parameters_json_value(
        consumer.parameters_json, consumer.strict);
    parameters = owned_parameters;
  }

  properties =
      parameters && json_type(parameters) == JSON_OBJECT
          ? json_object_get(parameters, "properties")
          : NULL;
  slot =
      properties && json_type(properties) == JSON_OBJECT
          ? json_object_get(properties, consumer_property)
          : NULL;

  producer_type = contract_type(result_schema);
  if (!contract_type_only_scalar(slot, &consumer_type)) {
    turbo_runtime_json_destroy(owned_parameters);
    return TURBO_AGENT_CONTRACT_UNKNOWN;
  }
  turbo_runtime_json_destroy(owned_parameters);

  if (!contract_scalar(producer_type)) {
    return TURBO_AGENT_CONTRACT_UNKNOWN;
  }
  if (producer_type == consumer_type ||
      (producer_type == TURBO_AGENT_CONTRACT_TYPE_INTEGER &&
       consumer_type == TURBO_AGENT_CONTRACT_TYPE_NUMBER)) {
    return TURBO_AGENT_CONTRACT_COMPATIBLE;
  }
  if ((producer_type == TURBO_AGENT_CONTRACT_TYPE_NUMBER &&
       consumer_type == TURBO_AGENT_CONTRACT_TYPE_INTEGER)) {
    return TURBO_AGENT_CONTRACT_UNKNOWN;
  }
  return TURBO_AGENT_CONTRACT_INCOMPATIBLE;
}
