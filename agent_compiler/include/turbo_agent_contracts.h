#ifndef TURBO_AGENT_CONTRACTS_H
#define TURBO_AGENT_CONTRACTS_H

#include <turbo_agent_api.h>

#include "turbo_tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum turbo_agent_contract_compatibility_e {
  /** Compatibility cannot be proven from the supported structural subset. */
  TURBO_AGENT_CONTRACT_UNKNOWN = 0,
  /** Producer result is provably admissible for the selected consumer slot. */
  TURBO_AGENT_CONTRACT_COMPATIBLE = 1,
  /** Producer result is provably incompatible with the selected consumer slot. */
  TURBO_AGENT_CONTRACT_INCOMPATIBLE = 2
} turbo_agent_contract_compatibility_t;

/**
 * Compare one producer RuntimeTool result contract to one named consumer input
 * property without executing either tool.
 *
 * Initial proof subset:
 * - producer and consumer slot must both publish one scalar JSON-Schema type;
 * - exact scalar type is compatible;
 * - integer producer is compatible with number consumer;
 * - different scalar types are incompatible;
 * - object/array, unions, missing schemas and unsupported shapes are UNKNOWN.
 *
 * This function intentionally does not claim general JSON-Schema implication.
 */
CXX_C_API turbo_agent_contract_compatibility_t
turbo_agent_tool_result_slot_compatibility(
    const turbo_tool_registry_t *registry,
    const char *producer_tool,
    const char *consumer_tool,
    const char *consumer_property);

/**
 * Prove one logical consumer input slot is compatible with a canonical native
 * CMeta scalar parameter type. The initial subset is deliberately narrow:
 * integer -> CMeta int/long, number -> float/double, boolean -> bool.
 */
CXX_C_API turbo_agent_contract_compatibility_t
turbo_agent_tool_input_slot_native_compatibility(
    const turbo_tool_registry_t *registry,
    const char *consumer_tool,
    const char *consumer_property,
    const cmeta_type_desc *native_type);

/**
 * Prove one canonical native CMeta scalar result type satisfies the tool's
 * logical RuntimeTools result contract. CMeta integer results may satisfy
 * integer or number; floating results satisfy number; bool satisfies boolean.
 */
CXX_C_API turbo_agent_contract_compatibility_t
turbo_agent_tool_result_native_compatibility(
    const turbo_tool_registry_t *registry,
    const char *tool,
    const cmeta_type_desc *native_type);

#ifdef __cplusplus
}
#endif

#endif
