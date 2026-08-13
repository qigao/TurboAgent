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
CXX_C_API turbo_tool_status_t turbo_tool_registry_add(turbo_tool_registry_t *registry,
                                                      const turbo_tool_definition_t *definition);

/**
 * @brief Add a versioned tool definition with explicit execution policy.
 * @return INVALID_ARGUMENT for an unsupported ABI, size, mode, or idempotency.
 */
CXX_C_API turbo_tool_status_t turbo_tool_registry_add_v2(
    turbo_tool_registry_t *registry, const turbo_tool_definition_v2_t *definition);

/** @brief Remove one tool and release its registry-owned resources. */
CXX_C_API turbo_tool_status_t turbo_tool_registry_remove(turbo_tool_registry_t *registry,
                                                         const char *name);

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
CXX_C_API turbo_tool_status_t turbo_tool_registry_get_definition(
    const turbo_tool_registry_t *registry, size_t index, turbo_tool_definition_t *out_definition);

/** @brief Read execution policy by tool name. Returned value is copied. */
CXX_C_API turbo_tool_status_t
turbo_tool_registry_get_execution_policy(const turbo_tool_registry_t *registry, const char *name,
                                         turbo_tool_execution_policy_t *out_policy);

/**
 * @brief Build a non-owning projection containing only the named tools.
 *
 * Tool definitions and schemas are copied, while callback user data remains
 * borrowed from `source`. The source registry and every callback dependency
 * must outlive the returned projection, and projected source definitions must
 * not be removed while it is in use. Destroying the projection never calls the
 * source definitions' `user_data_free` callbacks.
 *
 * Names must be exact registry names and unique. An empty name list produces
 * an empty registry. On failure, `out_projection` is set to NULL.
 */
CXX_C_API turbo_tool_status_t turbo_tool_registry_project(
    const turbo_tool_registry_t *source, const char *const *names, size_t name_count,
    turbo_tool_registry_t **out_projection);

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
CXX_C_API turbo_tool_status_t turbo_tool_registry_execute(const turbo_tool_registry_t *registry,
                                                          const char *name,
                                                          const char *arguments_json,
                                                          char **out_output);

/**
 * @brief Execute a tool by name through the TurboParser JSON boundary.
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
turbo_tool_registry_execute_json_value(const turbo_tool_registry_t *registry, const char *name,
                                       const json_value_t *arguments, json_value_t **out_result);

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
