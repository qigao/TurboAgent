#include "turbo_tool_runtime.h"
#include "turbo_tool_registry.h"

#include <stdlib.h>
#include <string.h>

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

static const turbo_tool_runtime_vtable_t turbo_tool_runtime_native_vtable = {
    turbo_tool_runtime_native_destroy_impl, turbo_tool_runtime_native_tool_count,
    turbo_tool_runtime_native_get_tool, turbo_tool_runtime_native_invoke,
    turbo_tool_runtime_native_invoke_json_value};

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

turbo_tool_status_t turbo_tool_runtime_invoke(turbo_tool_runtime_t *runtime, const char *name,
                                              const char *arguments_json, char **out_output) {
  if (!runtime || !name || !out_output) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  *out_output = NULL;
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
    turbo_tool_runtime_tool_t tool = {0};
    if (turbo_tool_runtime_get_tool(runtime, index, &tool) == TURBO_TOOL_OK && tool.name) {
      (void)turbo_tool_registry_remove(registry, tool.name);
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
    turbo_tool_runtime_tool_t tool = {0};
    turbo_tool_execution_policy_t existing_policy;
    turbo_tool_status_t status = turbo_tool_runtime_get_tool(runtime, index, &tool);
    if (status != TURBO_TOOL_OK || !tool.name || !tool.description ||
        (!tool.parameters_json && !tool.parameters_schema)) {
      return status == TURBO_TOOL_OK ? TURBO_TOOL_ERROR : status;
    }
    status = turbo_tool_registry_get_execution_policy(registry, tool.name, &existing_policy);
    if (status == TURBO_TOOL_OK) {
      return TURBO_TOOL_DUPLICATE;
    }
    if (status != TURBO_TOOL_NOT_FOUND) {
      return status;
    }
  }

  for (index = 0; index < tool_count; ++index) {
    turbo_tool_runtime_tool_t tool = {0};
    turbo_tool_runtime_bridge_entry_t *entry;
    turbo_tool_definition_v2_t definition = {0};
    turbo_tool_status_t status = turbo_tool_runtime_get_tool(runtime, index, &tool);
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
    entry->name = turbo_tool_runtime_strdup(tool.name);
    if (!entry->runtime || !entry->name) {
      turbo_tool_runtime_bridge_entry_destroy(entry);
      turbo_tool_runtime_rollback_registry(runtime, registry, index);
      return TURBO_TOOL_OUT_OF_MEMORY;
    }

    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V2_ABI_VERSION;
    definition.definition.name = tool.name;
    definition.definition.description = tool.description;
    definition.definition.parameters_json = tool.parameters_json;
    definition.definition.parameters_schema = tool.parameters_schema;
    definition.definition.strict = tool.strict;
    definition.definition.handler = turbo_tool_runtime_bridge_handler;
    definition.definition.json_value_handler = turbo_tool_runtime_bridge_json_value_handler;
    definition.definition.user_data = entry;
    definition.definition.user_data_free = turbo_tool_runtime_bridge_entry_destroy;
    definition.execution_policy = *execution_policy;

    status = turbo_tool_registry_add_v2(registry, &definition);
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

  return turbo_tool_runtime_create(&turbo_tool_runtime_native_vtable, impl);
}

turbo_tool_status_t turbo_tool_runtime_native_add_tool(turbo_tool_runtime_t *runtime,
                                                       const turbo_tool_definition_t *definition) {
  turbo_tool_runtime_native_impl_t *impl;

  if (!runtime || !definition || runtime->vtable != &turbo_tool_runtime_native_vtable) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  impl = (turbo_tool_runtime_native_impl_t *)runtime->impl;
  if (!impl) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  return turbo_tool_registry_add(impl->registry, definition);
}
