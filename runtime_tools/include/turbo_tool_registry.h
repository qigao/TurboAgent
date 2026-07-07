#ifndef TURBO_TOOL_REGISTRY_H
#define TURBO_TOOL_REGISTRY_H

#include <platform.h>
#include <turbo_parser.h>

#include "turbo_tool.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create an empty tool registry.
 * @return Registry handle or NULL on allocation failure.
 */
CXX_C_API turbo_tool_registry_t *turbo_tool_registry_create(void);

/**
 * @brief Destroy a tool registry.
 * @param registry Registry handle, may be NULL.
 */
CXX_C_API void turbo_tool_registry_destroy(turbo_tool_registry_t *registry);

/**
 * @brief Add a function tool definition.
 * @param registry Registry handle.
 * @param definition Tool definition.
 * @return Status code.
 */
CXX_C_API turbo_tool_status_t
turbo_tool_registry_add(turbo_tool_registry_t *registry,
                        const turbo_tool_definition_t *definition);

/**
 * @brief Get the number of tools in the registry.
 * @param registry Registry handle.
 * @return Tool count.
 */
CXX_C_API size_t turbo_tool_registry_count(const turbo_tool_registry_t *registry);

/**
 * @brief Read a stored tool definition by index.
 * @param registry Registry handle.
 * @param index Zero-based tool index.
 * @param out_definition Output view populated with borrowed pointers.
 * @return Status code.
 */
CXX_C_API turbo_tool_status_t
turbo_tool_registry_get_definition(const turbo_tool_registry_t *registry, size_t index,
                                   turbo_tool_definition_t *out_definition);

/**
 * @brief Execute a tool by name.
 *
 * This is direct registry execution. It does not invoke agent middleware,
 * guardrails, review gates, or policy hooks; policy-aware callers should route
 * execution through the agent workflow or invoke those checks explicitly.
 *
 * @param registry Registry handle.
 * @param name Tool name.
 * @param arguments_json Raw JSON string from the model.
 * @param out_output Output string allocated with malloc/free.
 * @return Status code.
 */
CXX_C_API turbo_tool_status_t
turbo_tool_registry_execute(const turbo_tool_registry_t *registry, const char *name,
                            const char *arguments_json, char **out_output);

/**
 * @brief Execute a tool by name through the runtime data-bind boundary.
 *
 * This is direct registry execution. It does not invoke agent middleware,
 * guardrails, review gates, or policy hooks; policy-aware callers should route
 * execution through the agent workflow or invoke those checks explicitly.
 *
 * @param registry Registry handle.
 * @param name Tool name.
 * @param arguments Bound arguments tree. NULL means no arguments.
 * @param out_result Output value owned by caller.
 * @return Status code.
 */
CXX_C_API turbo_tool_status_t
turbo_tool_registry_execute_bind(const turbo_tool_registry_t *registry, const char *name,
                                 const turbo_runtime_data_bind_value_t *arguments,
                                 turbo_runtime_data_bind_value_t **out_result);

/**
 * @brief Serialize registry tools into OpenAI Responses API shape.
 * @param registry Registry handle.
 * @return JSON array owned by caller, or NULL on failure.
 */
CXX_C_API json_value_t *
turbo_tool_registry_build_openai_tools(const turbo_tool_registry_t *registry);

/**
 * @brief Serialize registry tools into OpenAI Chat Completions API shape.
 * @param registry Registry handle.
 * @return JSON array owned by caller, or NULL on failure.
 */
CXX_C_API json_value_t *
turbo_tool_registry_build_openai_chat_tools(const turbo_tool_registry_t *registry);

/**
 * @brief Serialize registry tools into conservative OpenAI-compatible Chat shape.
 * @param registry Registry handle.
 * @return JSON array owned by caller, or NULL on failure.
 *
 * This omits OpenAI-only extensions such as `function.strict`.
 */
CXX_C_API json_value_t *
turbo_tool_registry_build_openai_compatible_chat_tools(const turbo_tool_registry_t *registry);

/**
 * @brief Serialize registry tools into Anthropic Messages API shape.
 * @param registry Registry handle.
 * @return JSON array owned by caller, or NULL on failure.
 */
CXX_C_API json_value_t *
turbo_tool_registry_build_anthropic_tools(const turbo_tool_registry_t *registry);

#ifdef __cplusplus
}
#endif

#endif
