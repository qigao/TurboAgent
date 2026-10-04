#include "turbo_tool_runtime.h"
#include "turbo_tool_registry.h"

#include <stdlib.h>
#include <string.h>

static const char *const turbo_tool_runtime_required_capabilities[] = {"runtime_tools"};

static char *turbo_tool_runtime_strdup(const char *src) {
  size_t len;
  char *copy;

  if (!src) {
    return NULL;
  }

  len = strlen(src);
  copy = (char *)malloc(len + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, src, len + 1);
  return copy;
}

struct turbo_tool_runtime_s {
  size_t ref_count;
  const turbo_tool_runtime_vtable_t *vtable;
  const turbo_tool_runtime_vtable_v2_t *vtable_v2;
  const turbo_tool_runtime_vtable_v3_t *vtable_v3;
  const turbo_tool_runtime_vtable_v4_t *vtable_v4;
  void *impl;
};

typedef struct turbo_tool_runtime_bridge_entry_s {
  turbo_tool_runtime_t *runtime;
  char *name;
} turbo_tool_runtime_bridge_entry_t;

typedef struct turbo_tool_runtime_native_impl_s {
  turbo_tool_registry_t *registry;
} turbo_tool_runtime_native_impl_t;

static int turbo_tool_runtime_bridge_handler(const char *arguments_json, char **out_output,
                                             void *user_data) {
  turbo_tool_runtime_bridge_entry_t *entry = (turbo_tool_runtime_bridge_entry_t *)user_data;
  turbo_tool_status_t status;

  if (!entry || !entry->runtime || !entry->name || !out_output) {
    return -1;
  }

  status = turbo_tool_runtime_invoke(entry->runtime, entry->name, arguments_json, out_output);
  return status == TURBO_TOOL_OK ? 0 : -1;
}

static int turbo_tool_runtime_bridge_json_value_handler(const json_value_t *arguments,
                                                        json_value_t **out_result,
                                                        void *user_data) {
  turbo_tool_runtime_bridge_entry_t *entry = (turbo_tool_runtime_bridge_entry_t *)user_data;
  turbo_tool_status_t status;

  if (!entry || !entry->runtime || !entry->name || !out_result) {
    return -1;
  }

  status = turbo_tool_runtime_invoke_json_value(entry->runtime, entry->name, arguments, out_result);
  return status == TURBO_TOOL_OK ? 0 : -1;
}

static turbo_tool_status_t turbo_tool_runtime_bridge_context_handler(
    const char *arguments_json, const turbo_tool_execution_context_t *context,
    char **out_output, void *user_data) {
  turbo_tool_runtime_bridge_entry_t *entry = (turbo_tool_runtime_bridge_entry_t *)user_data;
  if (!entry || !entry->runtime || !entry->name || !out_output) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  return turbo_tool_runtime_invoke_with_context(entry->runtime, entry->name, arguments_json,
                                                context, out_output);
}

static turbo_tool_status_t turbo_tool_runtime_bridge_json_value_context_handler(
    const json_value_t *arguments, const turbo_tool_execution_context_t *context,
    json_value_t **out_result, void *user_data) {
  turbo_tool_runtime_bridge_entry_t *entry = (turbo_tool_runtime_bridge_entry_t *)user_data;
  if (!entry || !entry->runtime || !entry->name || !out_result) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  return turbo_tool_runtime_invoke_json_value_with_context(entry->runtime, entry->name, arguments,
                                                           context, out_result);
}

static void turbo_tool_runtime_bridge_entry_destroy(void *user_data) {
  turbo_tool_runtime_bridge_entry_t *entry = (turbo_tool_runtime_bridge_entry_t *)user_data;

  if (!entry) {
    return;
  }

  turbo_tool_runtime_destroy(entry->runtime);
  free(entry->name);
  free(entry);
}

static void turbo_tool_runtime_native_destroy_impl(void *impl) {
  turbo_tool_runtime_native_impl_t *native_impl = (turbo_tool_runtime_native_impl_t *)impl;

  if (!native_impl) {
    return;
  }

  turbo_tool_registry_destroy(native_impl->registry);
  free(native_impl);
}

static size_t turbo_tool_runtime_native_tool_count(const void *impl) {
  const turbo_tool_runtime_native_impl_t *native_impl =
      (const turbo_tool_runtime_native_impl_t *)impl;

  if (!native_impl) {
    return 0;
  }

  return turbo_tool_registry_count(native_impl->registry);
}

static turbo_tool_status_t turbo_tool_runtime_native_get_tool(const void *impl, size_t index,
                                                              turbo_tool_runtime_tool_t *out_tool) {
  const turbo_tool_runtime_native_impl_t *native_impl =
      (const turbo_tool_runtime_native_impl_t *)impl;
  turbo_tool_definition_t definition = {0};
  turbo_tool_status_t status;

  if (!native_impl || !out_tool) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  status = turbo_tool_registry_get_definition(native_impl->registry, index, &definition);
  if (status != TURBO_TOOL_OK) {
    return status;
  }

  memset(out_tool, 0, sizeof(*out_tool));
  out_tool->name = definition.name;
  out_tool->description = definition.description;
  out_tool->parameters_json = definition.parameters_json;
  out_tool->parameters_schema = definition.parameters_schema;
  out_tool->strict = definition.strict;
  return TURBO_TOOL_OK;
}

static turbo_tool_status_t turbo_tool_runtime_native_get_tool_v2(
    const void *impl, size_t index, turbo_tool_runtime_tool_v2_t *out_tool) {
  const turbo_tool_runtime_native_impl_t *native_impl =
      (const turbo_tool_runtime_native_impl_t *)impl;
  turbo_tool_definition_t definition = {0};
  const char *result_schema_json = NULL;
  const json_value_t *result_schema = NULL;
  int strict_result = 0;
  turbo_tool_status_t status;

  if (!native_impl || !out_tool) return TURBO_TOOL_INVALID_ARGUMENT;
  status = turbo_tool_registry_get_definition(
      native_impl->registry, index, &definition);
  if (status != TURBO_TOOL_OK) return status;
  status = turbo_tool_registry_get_result_contract(
      native_impl->registry, definition.name,
      &result_schema_json, &result_schema, &strict_result);
  if (status != TURBO_TOOL_OK) return status;
  (void)result_schema;

  memset(out_tool, 0, sizeof(*out_tool));
  out_tool->struct_size = sizeof(*out_tool);
  out_tool->abi_version = TURBO_TOOL_RUNTIME_TOOL_V2_ABI_VERSION;
  out_tool->base.name = definition.name;
  out_tool->base.description = definition.description;
  out_tool->base.parameters_json = definition.parameters_json;
  out_tool->base.parameters_schema = definition.parameters_schema;
  out_tool->base.strict = definition.strict;
  out_tool->result_schema_json = result_schema_json;
  out_tool->strict_result = strict_result;
  return TURBO_TOOL_OK;
}

static turbo_tool_status_t turbo_tool_runtime_native_get_tool_v3(
    const void *impl, size_t index, turbo_tool_runtime_tool_v3_t *out_tool) {
  const turbo_tool_runtime_native_impl_t *native_impl =
      (const turbo_tool_runtime_native_impl_t *)impl;
  turbo_tool_effect_flags_t effects = TURBO_TOOL_EFFECT_UNKNOWN;
  turbo_tool_status_t status;

  if (!native_impl || !out_tool) return TURBO_TOOL_INVALID_ARGUMENT;
  memset(out_tool, 0, sizeof(*out_tool));
  out_tool->struct_size = sizeof(*out_tool);
  out_tool->abi_version = TURBO_TOOL_RUNTIME_TOOL_V3_ABI_VERSION;
  status = turbo_tool_runtime_native_get_tool_v2(
      impl, index, &out_tool->base);
  if (status != TURBO_TOOL_OK) return status;
  status = turbo_tool_registry_get_effects(
      native_impl->registry, out_tool->base.base.name, &effects);
  if (status != TURBO_TOOL_OK) return status;
  out_tool->effect_flags = effects;
  return TURBO_TOOL_OK;
}

static turbo_tool_status_t turbo_tool_runtime_native_invoke(void *impl, const char *name,
                                                            const char *arguments_json,
                                                            char **out_output) {
  const turbo_tool_runtime_native_impl_t *native_impl =
      (const turbo_tool_runtime_native_impl_t *)impl;

  if (!native_impl) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  return turbo_tool_registry_execute(native_impl->registry, name, arguments_json, out_output);
}

static turbo_tool_status_t turbo_tool_runtime_native_invoke_json_value(
    void *impl, const char *name, const json_value_t *arguments, json_value_t **out_result) {
  const turbo_tool_runtime_native_impl_t *native_impl =
      (const turbo_tool_runtime_native_impl_t *)impl;

  if (!native_impl) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  return turbo_tool_registry_execute_json_value(native_impl->registry, name, arguments, out_result);
}

static turbo_tool_status_t turbo_tool_runtime_native_invoke_with_context(
    void *impl, const char *name, const char *arguments_json,
    const turbo_tool_execution_context_t *context, char **out_output) {
  const turbo_tool_runtime_native_impl_t *native_impl =
      (const turbo_tool_runtime_native_impl_t *)impl;
  if (!native_impl) return TURBO_TOOL_INVALID_ARGUMENT;
  return turbo_tool_registry_execute_with_context(native_impl->registry, name, arguments_json,
                                                  context, out_output);
}

static turbo_tool_status_t turbo_tool_runtime_native_invoke_json_value_with_context(
    void *impl, const char *name, const json_value_t *arguments,
    const turbo_tool_execution_context_t *context, json_value_t **out_result) {
  const turbo_tool_runtime_native_impl_t *native_impl =
      (const turbo_tool_runtime_native_impl_t *)impl;
  if (!native_impl) return TURBO_TOOL_INVALID_ARGUMENT;
  return turbo_tool_registry_execute_json_value_with_context(native_impl->registry, name, arguments,
                                                             context, out_result);
}

static const turbo_tool_runtime_vtable_v4_t turbo_tool_runtime_native_vtable = {
    sizeof(turbo_tool_runtime_vtable_v4_t),
    TURBO_TOOL_RUNTIME_VTABLE_V4_ABI_VERSION,
    {sizeof(turbo_tool_runtime_vtable_v3_t),
     TURBO_TOOL_RUNTIME_VTABLE_V3_ABI_VERSION,
     {sizeof(turbo_tool_runtime_vtable_v2_t),
      TURBO_TOOL_RUNTIME_VTABLE_V2_ABI_VERSION,
      {turbo_tool_runtime_native_destroy_impl, turbo_tool_runtime_native_tool_count,
       turbo_tool_runtime_native_get_tool, turbo_tool_runtime_native_invoke,
       turbo_tool_runtime_native_invoke_json_value},
      turbo_tool_runtime_native_invoke_with_context,
      turbo_tool_runtime_native_invoke_json_value_with_context},
     turbo_tool_runtime_native_get_tool_v2},
    turbo_tool_runtime_native_get_tool_v3};

turbo_tool_runtime_t *turbo_tool_runtime_create(const turbo_tool_runtime_vtable_t *vtable,
                                                void *impl) {
  turbo_tool_runtime_t *runtime;

  if (!vtable || !vtable->destroy || !vtable->tool_count || !vtable->get_tool || !vtable->invoke ||
      !vtable->invoke_json_value) {
    return NULL;
  }

  runtime = (turbo_tool_runtime_t *)calloc(1, sizeof(*runtime));
  if (!runtime) {
    if (impl) {
      vtable->destroy(impl);
    }
    return NULL;
  }

  runtime->ref_count = 1;
  runtime->vtable = vtable;
  runtime->vtable_v2 = NULL;
  runtime->vtable_v3 = NULL;
  runtime->vtable_v4 = NULL;
  runtime->impl = impl;
  return runtime;
}

turbo_tool_runtime_t *turbo_tool_runtime_create_v2(const turbo_tool_runtime_vtable_v2_t *vtable,
                                                   void *impl) {
  turbo_tool_runtime_t *runtime;
  if (!vtable || vtable->struct_size < sizeof(*vtable) ||
      vtable->abi_version != TURBO_TOOL_RUNTIME_VTABLE_V2_ABI_VERSION ||
      !vtable->base.destroy || !vtable->base.tool_count || !vtable->base.get_tool ||
      !vtable->base.invoke || !vtable->base.invoke_json_value || !vtable->invoke_with_context ||
      !vtable->invoke_json_value_with_context) {
    return NULL;
  }
  runtime = (turbo_tool_runtime_t *)calloc(1, sizeof(*runtime));
  if (!runtime) {
    if (impl) vtable->base.destroy(impl);
    return NULL;
  }
  runtime->ref_count = 1;
  runtime->vtable = &vtable->base;
  runtime->vtable_v2 = vtable;
  runtime->vtable_v3 = NULL;
  runtime->vtable_v4 = NULL;
  runtime->impl = impl;
  return runtime;
}

turbo_tool_runtime_t *turbo_tool_runtime_create_v3(
    const turbo_tool_runtime_vtable_v3_t *vtable, void *impl) {
  turbo_tool_runtime_t *runtime;
  if (!vtable || vtable->struct_size < sizeof(*vtable) ||
      vtable->abi_version != TURBO_TOOL_RUNTIME_VTABLE_V3_ABI_VERSION ||
      vtable->base.struct_size < sizeof(vtable->base) ||
      vtable->base.abi_version != TURBO_TOOL_RUNTIME_VTABLE_V2_ABI_VERSION ||
      !vtable->base.base.destroy || !vtable->base.base.tool_count ||
      !vtable->base.base.get_tool || !vtable->base.base.invoke ||
      !vtable->base.base.invoke_json_value ||
      !vtable->base.invoke_with_context ||
      !vtable->base.invoke_json_value_with_context ||
      !vtable->get_tool_v2) {
    return NULL;
  }
  runtime = (turbo_tool_runtime_t *)calloc(1, sizeof(*runtime));
  if (!runtime) {
    if (impl) vtable->base.base.destroy(impl);
    return NULL;
  }
  runtime->ref_count = 1;
  runtime->vtable = &vtable->base.base;
  runtime->vtable_v2 = &vtable->base;
  runtime->vtable_v3 = vtable;
  runtime->vtable_v4 = NULL;
  runtime->impl = impl;
  return runtime;
}

turbo_tool_runtime_t *turbo_tool_runtime_create_v4(
    const turbo_tool_runtime_vtable_v4_t *vtable, void *impl) {
  turbo_tool_runtime_t *runtime;
  if (!vtable || vtable->struct_size < sizeof(*vtable) ||
      vtable->abi_version != TURBO_TOOL_RUNTIME_VTABLE_V4_ABI_VERSION ||
      vtable->base.struct_size < sizeof(vtable->base) ||
      vtable->base.abi_version != TURBO_TOOL_RUNTIME_VTABLE_V3_ABI_VERSION ||
      vtable->base.base.struct_size < sizeof(vtable->base.base) ||
      vtable->base.base.abi_version != TURBO_TOOL_RUNTIME_VTABLE_V2_ABI_VERSION ||
      !vtable->base.base.base.destroy || !vtable->base.base.base.tool_count ||
      !vtable->base.base.base.get_tool || !vtable->base.base.base.invoke ||
      !vtable->base.base.base.invoke_json_value ||
      !vtable->base.base.invoke_with_context ||
      !vtable->base.base.invoke_json_value_with_context ||
      !vtable->base.get_tool_v2 || !vtable->get_tool_v3) {
    return NULL;
  }
  runtime = (turbo_tool_runtime_t *)calloc(1, sizeof(*runtime));
  if (!runtime) {
    if (impl) vtable->base.base.base.destroy(impl);
    return NULL;
  }
  runtime->ref_count = 1;
  runtime->vtable = &vtable->base.base.base;
  runtime->vtable_v2 = &vtable->base.base;
  runtime->vtable_v3 = &vtable->base;
  runtime->vtable_v4 = vtable;
  runtime->impl = impl;
  return runtime;
}

turbo_tool_runtime_t *turbo_tool_runtime_retain(turbo_tool_runtime_t *runtime) {
  if (!runtime) {
    return NULL;
  }

  runtime->ref_count++;
  return runtime;
}

void turbo_tool_runtime_destroy(turbo_tool_runtime_t *runtime) {
  if (!runtime) {
    return;
  }

  if (runtime->ref_count > 1) {
    runtime->ref_count--;
    return;
  }

  runtime->vtable->destroy(runtime->impl);
  free(runtime);
}

size_t turbo_tool_runtime_count(const turbo_tool_runtime_t *runtime) {
  if (!runtime) {
    return 0;
  }

  return runtime->vtable->tool_count(runtime->impl);
}

turbo_tool_status_t turbo_tool_runtime_get_tool(const turbo_tool_runtime_t *runtime, size_t index,
                                                turbo_tool_runtime_tool_t *out_tool) {
  if (!runtime || !out_tool) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  return runtime->vtable->get_tool(runtime->impl, index, out_tool);
}

turbo_tool_status_t turbo_tool_runtime_get_tool_v2(
    const turbo_tool_runtime_t *runtime, size_t index,
    turbo_tool_runtime_tool_v2_t *out_tool) {
  turbo_tool_status_t status;
  if (!runtime || !out_tool) return TURBO_TOOL_INVALID_ARGUMENT;
  memset(out_tool, 0, sizeof(*out_tool));
  out_tool->struct_size = sizeof(*out_tool);
  out_tool->abi_version = TURBO_TOOL_RUNTIME_TOOL_V2_ABI_VERSION;
  if (runtime->vtable_v3) {
    status = runtime->vtable_v3->get_tool_v2(runtime->impl, index, out_tool);
    if (status != TURBO_TOOL_OK) return status;
    if (out_tool->struct_size < sizeof(*out_tool) ||
        out_tool->abi_version != TURBO_TOOL_RUNTIME_TOOL_V2_ABI_VERSION) {
      memset(out_tool, 0, sizeof(*out_tool));
      return TURBO_TOOL_ERROR;
    }
    return TURBO_TOOL_OK;
  }
  status = runtime->vtable->get_tool(runtime->impl, index, &out_tool->base);
  if (status != TURBO_TOOL_OK) return status;
  out_tool->result_schema_json = NULL;
  out_tool->strict_result = 0;
  return TURBO_TOOL_OK;
}

turbo_tool_status_t turbo_tool_runtime_get_tool_v3(
    const turbo_tool_runtime_t *runtime, size_t index,
    turbo_tool_runtime_tool_v3_t *out_tool) {
  turbo_tool_status_t status;
  if (!runtime || !out_tool) return TURBO_TOOL_INVALID_ARGUMENT;
  memset(out_tool, 0, sizeof(*out_tool));
  out_tool->struct_size = sizeof(*out_tool);
  out_tool->abi_version = TURBO_TOOL_RUNTIME_TOOL_V3_ABI_VERSION;
  if (runtime->vtable_v4) {
    status = runtime->vtable_v4->get_tool_v3(runtime->impl, index, out_tool);
    if (status != TURBO_TOOL_OK) return status;
    if (out_tool->struct_size < sizeof(*out_tool) ||
        out_tool->abi_version != TURBO_TOOL_RUNTIME_TOOL_V3_ABI_VERSION) {
      memset(out_tool, 0, sizeof(*out_tool));
      return TURBO_TOOL_ERROR;
    }
    return TURBO_TOOL_OK;
  }
  status = turbo_tool_runtime_get_tool_v2(runtime, index, &out_tool->base);
  if (status != TURBO_TOOL_OK) return status;
  out_tool->effect_flags = TURBO_TOOL_EFFECT_UNKNOWN;
  return TURBO_TOOL_OK;
}

turbo_tool_status_t turbo_tool_runtime_invoke(turbo_tool_runtime_t *runtime, const char *name,
                                              const char *arguments_json, char **out_output) {
  if (!runtime || !name || !out_output) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  *out_output = NULL;
  return runtime->vtable->invoke(runtime->impl, name, arguments_json ? arguments_json : "{}",
                                 out_output);
}

turbo_tool_status_t turbo_tool_runtime_invoke_with_context(
    turbo_tool_runtime_t *runtime, const char *name, const char *arguments_json,
    const turbo_tool_execution_context_t *context, char **out_output) {
  if (!runtime || !name || !out_output) return TURBO_TOOL_INVALID_ARGUMENT;
  *out_output = NULL;
  if (runtime->vtable_v2) {
    return runtime->vtable_v2->invoke_with_context(
        runtime->impl, name, arguments_json ? arguments_json : "{}", context, out_output);
  }
  return runtime->vtable->invoke(runtime->impl, name, arguments_json ? arguments_json : "{}",
                                 out_output);
}

turbo_tool_status_t turbo_tool_runtime_invoke_json_value(turbo_tool_runtime_t *runtime,
                                                         const char *name,
                                                         const json_value_t *arguments,
                                                         json_value_t **out_result) {
  if (!runtime || !name || !out_result) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  *out_result = NULL;
  return runtime->vtable->invoke_json_value(runtime->impl, name, arguments, out_result);
}

turbo_tool_status_t turbo_tool_runtime_invoke_json_value_with_context(
    turbo_tool_runtime_t *runtime, const char *name, const json_value_t *arguments,
    const turbo_tool_execution_context_t *context, json_value_t **out_result) {
  if (!runtime || !name || !out_result) return TURBO_TOOL_INVALID_ARGUMENT;
  *out_result = NULL;
  if (runtime->vtable_v2) {
    return runtime->vtable_v2->invoke_json_value_with_context(runtime->impl, name, arguments,
                                                             context, out_result);
  }
  return runtime->vtable->invoke_json_value(runtime->impl, name, arguments, out_result);
}

static int turbo_tool_runtime_execution_policy_valid(const turbo_tool_execution_policy_t *policy) {
  if (!policy || policy->mode < TURBO_TOOL_EXECUTION_SEQUENTIAL ||
      policy->mode > TURBO_TOOL_EXECUTION_EXCLUSIVE) {
    return 0;
  }
  return policy->idempotency >= TURBO_TOOL_IDEMPOTENCY_NONE &&
         policy->idempotency <= TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
}

static void turbo_tool_runtime_rollback_registry(turbo_tool_runtime_t *runtime,
                                                 turbo_tool_registry_t *registry,
                                                 size_t added_count) {
  size_t index;

  for (index = 0; index < added_count; ++index) {
    turbo_tool_runtime_tool_v3_t tool = {0};
    if (turbo_tool_runtime_get_tool_v3(runtime, index, &tool) == TURBO_TOOL_OK && tool.base.base.name) {
      (void)turbo_tool_registry_remove(registry, tool.base.base.name);
    }
  }
}

turbo_tool_status_t
turbo_tool_runtime_add_to_registry(turbo_tool_runtime_t *runtime, turbo_tool_registry_t *registry,
                                   const turbo_tool_execution_policy_t *execution_policy) {
  size_t tool_count;
  size_t index;

  if (!runtime || !registry || !turbo_tool_runtime_execution_policy_valid(execution_policy)) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  tool_count = turbo_tool_runtime_count(runtime);
  for (index = 0; index < tool_count; ++index) {
    turbo_tool_runtime_tool_v3_t tool = {0};
    turbo_tool_execution_policy_t existing_policy;
    turbo_tool_status_t status = turbo_tool_runtime_get_tool_v3(runtime, index, &tool);
    if (status != TURBO_TOOL_OK || !tool.base.base.name || !tool.base.base.description ||
        (!tool.base.base.parameters_json && !tool.base.base.parameters_schema)) {
      return status == TURBO_TOOL_OK ? TURBO_TOOL_ERROR : status;
    }
    status = turbo_tool_registry_get_execution_policy(registry, tool.base.base.name, &existing_policy);
    if (status == TURBO_TOOL_OK) {
      return TURBO_TOOL_DUPLICATE;
    }
    if (status != TURBO_TOOL_NOT_FOUND) {
      return status;
    }
  }

  for (index = 0; index < tool_count; ++index) {
    turbo_tool_runtime_tool_v3_t tool = {0};
    turbo_tool_runtime_bridge_entry_t *entry;
    turbo_tool_definition_v6_t definition = {0};
    turbo_tool_status_t status = turbo_tool_runtime_get_tool_v3(runtime, index, &tool);
    if (status != TURBO_TOOL_OK) {
      turbo_tool_runtime_rollback_registry(runtime, registry, index);
      return status;
    }

    entry = (turbo_tool_runtime_bridge_entry_t *)calloc(1, sizeof(*entry));
    if (!entry) {
      turbo_tool_runtime_rollback_registry(runtime, registry, index);
      return TURBO_TOOL_OUT_OF_MEMORY;
    }
    entry->runtime = turbo_tool_runtime_retain(runtime);
    entry->name = turbo_tool_runtime_strdup(tool.base.base.name);
    if (!entry->runtime || !entry->name) {
      turbo_tool_runtime_bridge_entry_destroy(entry);
      turbo_tool_runtime_rollback_registry(runtime, registry, index);
      return TURBO_TOOL_OUT_OF_MEMORY;
    }

    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
    definition.definition.name = tool.base.base.name;
    definition.definition.description = tool.base.base.description;
    definition.definition.parameters_json = tool.base.base.parameters_json;
    definition.definition.parameters_schema = tool.base.base.parameters_schema;
    definition.definition.strict = tool.base.base.strict;
    definition.definition.handler = turbo_tool_runtime_bridge_handler;
    definition.definition.json_value_handler = turbo_tool_runtime_bridge_json_value_handler;
    definition.definition.user_data = entry;
    definition.definition.user_data_free = turbo_tool_runtime_bridge_entry_destroy;
    definition.context_handler = turbo_tool_runtime_bridge_context_handler;
    definition.json_value_context_handler = turbo_tool_runtime_bridge_json_value_context_handler;
    definition.execution_policy = *execution_policy;
    definition.required_capabilities = turbo_tool_runtime_required_capabilities;
    definition.required_capability_count = sizeof(turbo_tool_runtime_required_capabilities) /
                                           sizeof(turbo_tool_runtime_required_capabilities[0]);
    definition.result_schema_json = tool.base.result_schema_json;
    definition.strict_result = tool.base.strict_result;
    definition.effect_flags = tool.effect_flags;

    status = turbo_tool_registry_add_v6(registry, &definition);
    if (status != TURBO_TOOL_OK) {
      turbo_tool_runtime_bridge_entry_destroy(entry);
      turbo_tool_runtime_rollback_registry(runtime, registry, index);
      return status;
    }
  }

  return TURBO_TOOL_OK;
}

turbo_tool_registry_t *turbo_tool_runtime_build_registry_bridge(turbo_tool_runtime_t *runtime) {
  turbo_tool_registry_t *registry;
  const turbo_tool_execution_policy_t legacy_policy = {TURBO_TOOL_EXECUTION_SEQUENTIAL,
                                                       TURBO_TOOL_IDEMPOTENCY_NONE};

  if (!runtime) {
    return NULL;
  }

  registry = turbo_tool_registry_create();
  if (!registry) {
    return NULL;
  }
  if (turbo_tool_runtime_add_to_registry(runtime, registry, &legacy_policy) != TURBO_TOOL_OK) {
    turbo_tool_registry_destroy(registry);
    return NULL;
  }
  return registry;
}

turbo_tool_runtime_t *turbo_tool_runtime_native_create(void) {
  turbo_tool_runtime_native_impl_t *impl;

  impl = (turbo_tool_runtime_native_impl_t *)calloc(1, sizeof(*impl));
  if (!impl) {
    return NULL;
  }

  impl->registry = turbo_tool_registry_create();
  if (!impl->registry) {
    free(impl);
    return NULL;
  }

  return turbo_tool_runtime_create_v4(&turbo_tool_runtime_native_vtable, impl);
}

turbo_tool_status_t turbo_tool_runtime_native_add_tool(turbo_tool_runtime_t *runtime,
                                                       const turbo_tool_definition_t *definition) {
  turbo_tool_runtime_native_impl_t *impl;

  if (!runtime || !definition || runtime->vtable_v4 != &turbo_tool_runtime_native_vtable) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  impl = (turbo_tool_runtime_native_impl_t *)runtime->impl;
  if (!impl) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  return turbo_tool_registry_add(impl->registry, definition);
}

turbo_tool_status_t turbo_tool_runtime_native_add_tool_v5(
    turbo_tool_runtime_t *runtime, const turbo_tool_definition_v5_t *definition) {
  turbo_tool_runtime_native_impl_t *impl;
  if (!runtime || !definition ||
      runtime->vtable_v4 != &turbo_tool_runtime_native_vtable) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  impl = (turbo_tool_runtime_native_impl_t *)runtime->impl;
  if (!impl) return TURBO_TOOL_INVALID_ARGUMENT;
  return turbo_tool_registry_add_v5(impl->registry, definition);
}

turbo_tool_status_t turbo_tool_runtime_native_add_tool_v6(
    turbo_tool_runtime_t *runtime, const turbo_tool_definition_v6_t *definition) {
  turbo_tool_runtime_native_impl_t *impl;
  if (!runtime || !definition ||
      runtime->vtable_v4 != &turbo_tool_runtime_native_vtable) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  impl = (turbo_tool_runtime_native_impl_t *)runtime->impl;
  if (!impl) return TURBO_TOOL_INVALID_ARGUMENT;
  return turbo_tool_registry_add_v6(impl->registry, definition);
}
