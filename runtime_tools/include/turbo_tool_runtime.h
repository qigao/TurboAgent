#ifndef TURBO_TOOL_RUNTIME_H
#define TURBO_TOOL_RUNTIME_H

#include <stddef.h>

#include <turbo_agent_api.h>

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
  const json_value_t *parameters_schema;
  int strict;
} turbo_tool_runtime_tool_t;

#define TURBO_TOOL_RUNTIME_TOOL_V2_ABI_VERSION 2u

/**
 * Additive runtime catalog view with canonical result contract metadata.
 * Legacy runtimes are exposed through this view with result_schema_json=NULL.
 */
typedef struct turbo_tool_runtime_tool_v2_s {
  size_t struct_size;
  uint32_t abi_version;
  turbo_tool_runtime_tool_t base;
  const char *result_schema_json;
  int strict_result;
} turbo_tool_runtime_tool_v2_t;

#define TURBO_TOOL_RUNTIME_TOOL_V3_ABI_VERSION 3u

/** Additive runtime catalog view with canonical semantic effect facts. */
typedef struct turbo_tool_runtime_tool_v3_s {
  size_t struct_size;
  uint32_t abi_version;
  turbo_tool_runtime_tool_v2_t base;
  turbo_tool_effect_flags_t effect_flags;
} turbo_tool_runtime_tool_v3_t;

typedef struct turbo_tool_runtime_vtable_s {
  void (*destroy)(void *impl);
  size_t (*tool_count)(const void *impl);
  turbo_tool_status_t (*get_tool)(const void *impl, size_t index,
                                  turbo_tool_runtime_tool_t *out_tool);
  turbo_tool_status_t (*invoke)(void *impl, const char *name, const char *arguments_json,
                                char **out_output);
  turbo_tool_status_t (*invoke_json_value)(void *impl, const char *name,
                                           const json_value_t *arguments,
                                           json_value_t **out_result);
} turbo_tool_runtime_vtable_t;

#define TURBO_TOOL_RUNTIME_VTABLE_V2_ABI_VERSION 2u

/**
 * Additive runtime vtable for context-aware invocation.
 *
 * The embedded v1 vtable preserves every existing runtime. Registry bridges
 * prefer these callbacks when a caller supplies execution context.
 */
typedef struct turbo_tool_runtime_vtable_v2_s {
  size_t struct_size;
  uint32_t abi_version;
  turbo_tool_runtime_vtable_t base;
  turbo_tool_status_t (*invoke_with_context)(
      void *impl, const char *name, const char *arguments_json,
      const turbo_tool_execution_context_t *context, char **out_output);
  turbo_tool_status_t (*invoke_json_value_with_context)(
      void *impl, const char *name, const json_value_t *arguments,
      const turbo_tool_execution_context_t *context, json_value_t **out_result);
} turbo_tool_runtime_vtable_v2_t;

#define TURBO_TOOL_RUNTIME_VTABLE_V3_ABI_VERSION 3u

/**
 * Additive runtime vtable exposing result-aware catalog metadata.
 */
typedef struct turbo_tool_runtime_vtable_v3_s {
  size_t struct_size;
  uint32_t abi_version;
  turbo_tool_runtime_vtable_v2_t base;
  turbo_tool_status_t (*get_tool_v2)(
      const void *impl, size_t index, turbo_tool_runtime_tool_v2_t *out_tool);
} turbo_tool_runtime_vtable_v3_t;

#define TURBO_TOOL_RUNTIME_VTABLE_V4_ABI_VERSION 4u

/** Additive runtime vtable exposing semantic-effect-aware catalog metadata. */
typedef struct turbo_tool_runtime_vtable_v4_s {
  size_t struct_size;
  uint32_t abi_version;
  turbo_tool_runtime_vtable_v3_t base;
  turbo_tool_status_t (*get_tool_v3)(
      const void *impl, size_t index, turbo_tool_runtime_tool_v3_t *out_tool);
} turbo_tool_runtime_vtable_v4_t;

/**
 * @brief Create a generic tool runtime backed by a caller-provided vtable.
 * @param vtable Backend vtable. All entries must be non-NULL.
 * @param impl Backend implementation pointer owned by the runtime.
 * @return Runtime handle or NULL on allocation failure.
 */
CXX_C_API turbo_tool_runtime_t *turbo_tool_runtime_create(const turbo_tool_runtime_vtable_t *vtable,
                                                          void *impl);

/** Create a runtime that can receive borrowed invocation context. */
CXX_C_API turbo_tool_runtime_t *
turbo_tool_runtime_create_v2(const turbo_tool_runtime_vtable_v2_t *vtable, void *impl);

/** Create a runtime whose catalog can publish canonical result contracts. */
CXX_C_API turbo_tool_runtime_t *
turbo_tool_runtime_create_v3(const turbo_tool_runtime_vtable_v3_t *vtable, void *impl);

/** Create a runtime whose catalog can publish canonical semantic effects. */
CXX_C_API turbo_tool_runtime_t *
turbo_tool_runtime_create_v4(const turbo_tool_runtime_vtable_v4_t *vtable, void *impl);

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
CXX_C_API turbo_tool_status_t turbo_tool_runtime_get_tool(const turbo_tool_runtime_t *runtime,
                                                          size_t index,
                                                          turbo_tool_runtime_tool_t *out_tool);

/**
 * Read the additive result-aware catalog view.
 * Legacy runtimes return the base descriptor with an opaque result contract.
 */
CXX_C_API turbo_tool_status_t turbo_tool_runtime_get_tool_v2(
    const turbo_tool_runtime_t *runtime, size_t index,
    turbo_tool_runtime_tool_v2_t *out_tool);

/**
 * Read the additive result/effect-aware catalog view.
 * Legacy runtimes surface TURBO_TOOL_EFFECT_UNKNOWN.
 */
CXX_C_API turbo_tool_status_t turbo_tool_runtime_get_tool_v3(
    const turbo_tool_runtime_t *runtime, size_t index,
    turbo_tool_runtime_tool_v3_t *out_tool);

/**
 * @brief Invoke one runtime tool by name with raw JSON arguments.
 * @param runtime Runtime handle.
 * @param name Tool name.
 * @param arguments_json JSON arguments string. NULL means `{}`.
 * @param out_output Output string allocated with malloc/free on success.
 * @return Status code.
 */
CXX_C_API turbo_tool_status_t turbo_tool_runtime_invoke(turbo_tool_runtime_t *runtime,
                                                        const char *name,
                                                        const char *arguments_json,
                                                        char **out_output);

/** Invoke with generic execution context; v1 runtimes fall back to invoke(). */
CXX_C_API turbo_tool_status_t turbo_tool_runtime_invoke_with_context(
    turbo_tool_runtime_t *runtime, const char *name, const char *arguments_json,
    const turbo_tool_execution_context_t *context, char **out_output);

/**
 * @brief Invoke one runtime tool by name through the TurboParser JSON-native boundary.
 * @param runtime Runtime handle.
 * @param name Tool name.
 * @param arguments TurboParser JSON arguments tree. NULL means no arguments.
 * @param out_result Output runtime value owned by caller.
 * @return Status code.
 */
CXX_C_API turbo_tool_status_t turbo_tool_runtime_invoke_json_value(turbo_tool_runtime_t *runtime,
                                                                   const char *name,
                                                                   const json_value_t *arguments,
                                                                   json_value_t **out_result);

/** JSON-native context-aware invoke; v1 runtimes fall back compatibly. */
CXX_C_API turbo_tool_status_t turbo_tool_runtime_invoke_json_value_with_context(
    turbo_tool_runtime_t *runtime, const char *name, const json_value_t *arguments,
    const turbo_tool_execution_context_t *context, json_value_t **out_result);

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
 * @brief Atomically append every runtime tool to an existing registry.
 *
 * Metadata is copied by the registry. Each successful binding retains the
 * runtime and releases it when removed or when the registry is destroyed.
 * On any error, bindings added by this call are removed and the registry's
 * previous contents remain intact. The runtime must not be mutated
 * concurrently while this control-plane operation runs.
 *
 * @param runtime Runtime whose immutable catalog is appended.
 * @param registry Destination registry owned by the caller.
 * @param execution_policy Policy applied to every tool in this runtime.
 * @return Status code; duplicate names return TURBO_TOOL_DUPLICATE.
 */
CXX_C_API turbo_tool_status_t
turbo_tool_runtime_add_to_registry(turbo_tool_runtime_t *runtime, turbo_tool_registry_t *registry,
                                   const turbo_tool_execution_policy_t *execution_policy);

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
CXX_C_API turbo_tool_status_t turbo_tool_runtime_native_add_tool(
    turbo_tool_runtime_t *runtime, const turbo_tool_definition_t *definition);

/** Add one native callback tool with the canonical v5 result contract surface. */
CXX_C_API turbo_tool_status_t turbo_tool_runtime_native_add_tool_v5(
    turbo_tool_runtime_t *runtime, const turbo_tool_definition_v5_t *definition);

/** Add one native callback tool with canonical result + semantic effects. */
CXX_C_API turbo_tool_status_t turbo_tool_runtime_native_add_tool_v6(
    turbo_tool_runtime_t *runtime, const turbo_tool_definition_v6_t *definition);

#ifdef __cplusplus
}
#endif

#endif
