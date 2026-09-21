#ifndef TURBO_TOOL_SCHEMA_H
#define TURBO_TOOL_SCHEMA_H

#include <platform.h>
#include <json_parser.h>

#include "turbo_runtime_json.h"
#include "turbo_tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Serialize registry tools into OpenAI Responses API shape.
 * @param registry Tool registry.
 * @return JSON array owned by caller, or NULL on failure.
 */
CXX_C_API json_value_t *
turbo_tool_schema_build_openai_tools(const turbo_tool_registry_t *registry);

/**
 * @brief Parse one tool parameter schema into a SaltsUtils JSON tree.
 * @param parameters_json JSON schema string for parameters.
 * @param strict Whether to enforce `additionalProperties=false` when absent.
 * @return SaltsUtils JSON tree owned by caller, or NULL on failure.
 */
CXX_C_API json_value_t *
turbo_tool_schema_parse_parameters_json_value(const char *parameters_json, int strict);

/**
 * @brief Export registry tool definitions into one SaltsUtils JSON-native schema array.
 * @param registry Tool registry.
 * @return SaltsUtils JSON array owned by caller, or NULL on failure.
 */
CXX_C_API json_value_t *
turbo_tool_schema_build_registry_json_value(const turbo_tool_registry_t *registry);

/**
 * @brief Build one OpenAI Chat tool object from raw definition fields.
 * @param name Tool name.
 * @param description Tool description.
 * @param parameters_json JSON schema string for function parameters.
 * @param strict Whether to emit strict mode and add `additionalProperties=false`.
 * @param compatible_mode When non-zero, omit OpenAI-only `function.strict`.
 * @return JSON object owned by caller, or NULL on failure.
 */
CXX_C_API json_value_t *turbo_tool_schema_build_openai_chat_tool_definition(
    const char *name, const char *description, const char *parameters_json, int strict,
    int compatible_mode);

/**
 * @brief Serialize registry tools into OpenAI Chat Completions API shape.
 * @param registry Tool registry.
 * @return JSON array owned by caller, or NULL on failure.
 */
CXX_C_API json_value_t *
turbo_tool_schema_build_openai_chat_tools(const turbo_tool_registry_t *registry);

/**
 * @brief Serialize registry tools into conservative OpenAI-compatible Chat shape.
 * @param registry Tool registry.
 * @return JSON array owned by caller, or NULL on failure.
 */
CXX_C_API json_value_t *
turbo_tool_schema_build_openai_compatible_chat_tools(const turbo_tool_registry_t *registry);

/**
 * @brief Serialize registry tools into Anthropic Messages API shape.
 * @param registry Tool registry.
 * @return JSON array owned by caller, or NULL on failure.
 */
CXX_C_API json_value_t *
turbo_tool_schema_build_anthropic_tools(const turbo_tool_registry_t *registry);

#ifdef __cplusplus
}
#endif

#endif
