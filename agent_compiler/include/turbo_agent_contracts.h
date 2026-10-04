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

#ifdef __cplusplus
}
#endif

#endif
