#ifndef TURBO_TOOL_RUNTIME_H
#define TURBO_TOOL_RUNTIME_H

#include <platform.h>

#include "turbo_tool.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_tool_registry_s turbo_tool_registry_t;
typedef struct turbo_tool_runtime_s turbo_tool_runtime_t;

typedef struct turbo_tool_runtime_tool_s {
  const char *name;
  const char *description;
  const char *parameters_json;
  const turbo_runtime_data_bind_value_t *parameters_schema;
  int strict;
} turbo_tool_runtime_tool_t;

typedef struct turbo_tool_runtime_vtable_s {
  void (*destroy)(void *impl);
  size_t (*tool_count)(const void *impl);
  turbo_tool_status_t (*get_tool)(const void *impl, size_t index,
                                  turbo_tool_runtime_tool_t *out_tool);
  turbo_tool_status_t (*invoke)(void *impl, const char *name,
                                const char *arguments_json, char **out_output);
  turbo_tool_status_t (*invoke_bind)(void *impl, const char *name,
                                     const turbo_runtime_data_bind_value_t *arguments,
                                     turbo_runtime_data_bind_value_t **out_result);
} turbo_tool_runtime_vtable_t;

/**
 * @brief Create a generic tool runtime backed by a caller-provided vtable.
 * @param vtable Backend vtable. All entries must be non-NULL.
 * @param impl Backend implementation pointer owned by the runtime.
 * @return Runtime handle or NULL on allocation failure.
 */
CXX_C_API turbo_tool_runtime_t *
turbo_tool_runtime_create(const turbo_tool_runtime_vtable_t *vtable, void *impl);

/**
 * @brief Retain a runtime handle for shared ownership.
 * @param runtime Runtime handle.
 * @return Same runtime handle, or NULL.
 */
CXX_C_API turbo_tool_runtime_t *turbo_tool_runtime_retain(turbo_tool_runtime_t *runtime);

/**
 * @brief Release and destroy a runtime when the last reference drops.
 * @param runtime Runtime handle, may be NULL.
 */
CXX_C_API void turbo_tool_runtime_destroy(turbo_tool_runtime_t *runtime);

/**
 * @brief Return the number of exported tools in a runtime.
 * @param runtime Runtime handle.
 * @return Tool count.
 */
CXX_C_API size_t turbo_tool_runtime_count(const turbo_tool_runtime_t *runtime);

/**
 * @brief Read one exported tool descriptor by index.
 * @param runtime Runtime handle.
 * @param index Zero-based tool index.
 * @param out_tool Borrowed descriptor view populated on success.
 * @return Status code.
 */
CXX_C_API turbo_tool_status_t
turbo_tool_runtime_get_tool(const turbo_tool_runtime_t *runtime, size_t index,
                            turbo_tool_runtime_tool_t *out_tool);

/**
 * @brief Invoke one runtime tool by name with raw JSON arguments.
 * @param runtime Runtime handle.
 * @param name Tool name.
 * @param arguments_json JSON arguments string. NULL means `{}`.
 * @param out_output Output string allocated with malloc/free on success.
 * @return Status code.
 */
CXX_C_API turbo_tool_status_t
turbo_tool_runtime_invoke(turbo_tool_runtime_t *runtime, const char *name,
                          const char *arguments_json, char **out_output);

/**
 * @brief Invoke one runtime tool by name through the bind-native boundary.
 * @param runtime Runtime handle.
 * @param name Tool name.
 * @param arguments Runtime data-bind arguments tree. NULL means no arguments.
 * @param out_result Output runtime value owned by caller.
 * @return Status code.
 */
CXX_C_API turbo_tool_status_t
turbo_tool_runtime_invoke_bind(turbo_tool_runtime_t *runtime, const char *name,
                               const turbo_runtime_data_bind_value_t *arguments,
                               turbo_runtime_data_bind_value_t **out_result);

/**
 * @brief Build a `turbo_tool_registry_t` bridge over a runtime.
 *
 * The returned registry copies schema metadata and forwards execution back into
 * the runtime through retained references, so it can be passed into existing
 * agent code that still consumes `turbo_tool_registry_t`.
 *
 * @param runtime Runtime handle.
 * @return Registry owned by caller, or NULL on failure.
 */
CXX_C_API turbo_tool_registry_t *
turbo_tool_runtime_build_registry_bridge(turbo_tool_runtime_t *runtime);

/**
 * @brief Create an in-process native runtime backed by function tool callbacks.
 * @return Runtime handle or NULL on allocation failure.
 */
CXX_C_API turbo_tool_runtime_t *turbo_tool_runtime_native_create(void);

/**
 * @brief Add one native callback tool to a native runtime.
 * @param runtime Runtime from `turbo_tool_runtime_native_create()`.
 * @param definition Tool definition copied into the owned backend registry.
 * @return Status code.
 */
CXX_C_API turbo_tool_status_t
turbo_tool_runtime_native_add_tool(turbo_tool_runtime_t *runtime,
                                   const turbo_tool_definition_t *definition);

#ifdef __cplusplus
}
#endif

#endif
