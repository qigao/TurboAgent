#include "tinytest.h"

#include "turbo_agent_contracts.h"
#include "turbo_tool_registry.h"

#include <string.h>

static int contract_echo(
    const json_value_t *arguments,
    json_value_t **out_result,
    void *user_data) {
  (void)arguments;
  (void)user_data;
  if (!out_result) return -1;
  *out_result = json_create_string("value");
  return *out_result ? 0 : -1;
}

static int add_v5_tool(
    turbo_tool_registry_t *registry,
    const char *name,
    const char *parameters_json,
    const char *result_schema_json) {
  turbo_tool_definition_v5_t definition = {0};
  definition.struct_size = sizeof(definition);
  definition.abi_version = TURBO_TOOL_DEFINITION_V5_ABI_VERSION;
  definition.definition.name = name;
  definition.definition.description = name;
  definition.definition.parameters_json = parameters_json;
  definition.definition.strict = 1;
  definition.definition.json_value_handler = contract_echo;
  definition.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
  definition.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
  definition.result_schema_json = result_schema_json;
  definition.strict_result = result_schema_json ? 1 : 0;
  return turbo_tool_registry_add_v5(registry, &definition) == TURBO_TOOL_OK
             ? 0
             : -1;
}

spec("AgentCompiler RuntimeTools result contracts") {
  it("proves compatible and incompatible scalar producer-consumer edges") {
    turbo_tool_registry_t *registry = turbo_tool_registry_create();

    check_not_null(registry);
    check_equal(add_v5_tool(
                    registry,
                    "producer",
                    "{"type":"object"}",
                    "{"type":"string"}"),
                0);
    check_equal(add_v5_tool(
                    registry,
                    "consumer_string",
                    "{"type":"object","properties":{"value":{"type":"string"}},"required":["value"],"additionalProperties":false}",
                    NULL),
                0);
    check_equal(add_v5_tool(
                    registry,
                    "consumer_integer",
                    "{"type":"object","properties":{"value":{"type":"integer"}},"required":["value"],"additionalProperties":false}",
                    NULL),
                0);

    check_equal(
        turbo_agent_tool_result_slot_compatibility(
            registry, "producer", "consumer_string", "value"),
        TURBO_AGENT_CONTRACT_COMPATIBLE);
    check_equal(
        turbo_agent_tool_result_slot_compatibility(
            registry, "producer", "consumer_integer", "value"),
        TURBO_AGENT_CONTRACT_INCOMPATIBLE);

    turbo_tool_registry_destroy(registry);
  }

  it("treats missing and structurally complex contracts as opaque") {
    turbo_tool_registry_t *registry = turbo_tool_registry_create();

    check_not_null(registry);
    check_equal(add_v5_tool(
                    registry,
                    "opaque",
                    "{"type":"object"}",
                    NULL),
                0);
    check_equal(add_v5_tool(
                    registry,
                    "object_producer",
                    "{"type":"object"}",
                    "{"type":"object","properties":{"value":{"type":"string"}}}"),
                0);
    check_equal(add_v5_tool(
                    registry,
                    "consumer",
                    "{"type":"object","properties":{"value":{"type":"string"}},"required":["value"]}",
                    NULL),
                0);

    check_equal(
        turbo_agent_tool_result_slot_compatibility(
            registry, "opaque", "consumer", "value"),
        TURBO_AGENT_CONTRACT_UNKNOWN);
    check_equal(
        turbo_agent_tool_result_slot_compatibility(
            registry, "object_producer", "consumer", "value"),
        TURBO_AGENT_CONTRACT_UNKNOWN);
    check_equal(
        turbo_agent_tool_result_slot_compatibility(
            registry, "object_producer", "consumer", "missing"),
        TURBO_AGENT_CONTRACT_UNKNOWN);

    turbo_tool_registry_destroy(registry);
  }

  it("keeps constrained consumer slots opaque until implication is proven") {
    turbo_tool_registry_t *registry = turbo_tool_registry_create();

    check_not_null(registry);
    check_equal(add_v5_tool(
                    registry,
                    "string_producer",
                    "{\"type\":\"object\"}",
                    "{\"type\":\"string\"}"),
                0);
    check_equal(add_v5_tool(
                    registry,
                    "enum_consumer",
                    "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"string\",\"enum\":[\"only\"]}}}",
                    NULL),
                0);

    check_equal(
        turbo_agent_tool_result_slot_compatibility(
            registry, "string_producer", "enum_consumer", "value"),
        TURBO_AGENT_CONTRACT_UNKNOWN);

    turbo_tool_registry_destroy(registry);
  }

  it("keeps number-to-integer narrowing opaque without producer constraint proof") {
    turbo_tool_registry_t *registry = turbo_tool_registry_create();

    check_not_null(registry);
    check_equal(add_v5_tool(
                    registry,
                    "number_producer",
                    "{\"type\":\"object\"}",
                    "{\"type\":\"number\"}"),
                0);
    check_equal(add_v5_tool(
                    registry,
                    "integer_consumer",
                    "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"integer\"}}}",
                    NULL),
                0);

    check_equal(
        turbo_agent_tool_result_slot_compatibility(
            registry, "number_producer", "integer_consumer", "value"),
        TURBO_AGENT_CONTRACT_UNKNOWN);

    turbo_tool_registry_destroy(registry);
  }

  it("allows integer producer results to feed number slots") {
    turbo_tool_registry_t *registry = turbo_tool_registry_create();

    check_not_null(registry);
    check_equal(add_v5_tool(
                    registry,
                    "integer_producer",
                    "{"type":"object"}",
                    "{"type":"integer"}"),
                0);
    check_equal(add_v5_tool(
                    registry,
                    "number_consumer",
                    "{"type":"object","properties":{"value":{"type":"number"}}}",
                    NULL),
                0);

    check_equal(
        turbo_agent_tool_result_slot_compatibility(
            registry, "integer_producer", "number_consumer", "value"),
        TURBO_AGENT_CONTRACT_COMPATIBLE);

    turbo_tool_registry_destroy(registry);
  }
}
