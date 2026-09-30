#include "turbo_wasm_tool_pack.h"

#include <salts_fs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TURBO_WASM_TOOL_PACK_FNV_OFFSET UINT64_C(14695981039346656037)
#define TURBO_WASM_TOOL_PACK_FNV_PRIME UINT64_C(1099511628211)

enum {
  TURBO_WASM_TOOL_PACK_DEFAULT_MAX_MODULES = 16,
  TURBO_WASM_TOOL_PACK_DEFAULT_MAX_TOOLS = 128
};

struct turbo_wasm_tool_pack_s {
  turbo_tool_registry_t *registry;
  size_t module_count;
  size_t max_modules;
  size_t max_tools;
};

static uint64_t turbo_wasm_tool_pack_module_hash(
    const void *data, size_t size) {
  const unsigned char *bytes = (const unsigned char *)data;
  uint64_t hash = TURBO_WASM_TOOL_PACK_FNV_OFFSET;
  size_t i;
  for (i = 0; i < size; ++i) {
    hash ^= (uint64_t)bytes[i];
    hash *= TURBO_WASM_TOOL_PACK_FNV_PRIME;
  }
  return hash;
}

static json_value_t *turbo_wasm_tool_pack_execution_metadata(
    const turbo_tool_runtime_wasm_config_t *config) {
  salts_fs_buf_t bytes = {0};
  json_value_t *root = NULL;
  json_value_t *limits = NULL;
  json_value_t *field = NULL;
  char identity[32];
  uint64_t hash;

  if (!config || !config->module_path ||
      salts_fs_read_file(config->module_path, &bytes) != 0)
    return NULL;
  hash = turbo_wasm_tool_pack_module_hash(bytes.base, bytes.len);
  salts_fs_buf_free(&bytes);

  root = json_create_object();
  limits = json_create_object();
  if (!root || !limits) goto fail;

  field = json_create_string("turbowasm");
  if (!field || turbo_runtime_json_object_set(root, "backend", field) !=
                    TURBO_RUNTIME_JSON_OK)
    goto fail;
  field = NULL;

  snprintf(identity, sizeof(identity), "fnv1a64:%016llx",
           (unsigned long long)hash);
  field = json_create_string(identity);
  if (!field ||
      turbo_runtime_json_object_set(root, "module_identity", field) !=
          TURBO_RUNTIME_JSON_OK)
    goto fail;
  field = NULL;

#define TURBO_WASM_LIMIT(name, value)                                      \
  do {                                                                     \
    field = json_create_int64((int64_t)(value));                            \
    if (!field ||                                                          \
        turbo_runtime_json_object_set(limits, (name), field) !=             \
            TURBO_RUNTIME_JSON_OK)                                         \
      goto fail;                                                           \
    field = NULL;                                                          \
  } while (0)

  TURBO_WASM_LIMIT("max_module_bytes", config->max_module_bytes);
  TURBO_WASM_LIMIT("max_allocation_bytes", config->max_allocation_bytes);
  TURBO_WASM_LIMIT("max_linear_memory_bytes", config->max_linear_memory_bytes);
  TURBO_WASM_LIMIT("max_table_elements", config->max_table_elements);
  TURBO_WASM_LIMIT("max_input_bytes", config->max_input_bytes);
  TURBO_WASM_LIMIT("max_output_bytes", config->max_output_bytes);
  TURBO_WASM_LIMIT("fuel_per_call", config->fuel_per_call);
#undef TURBO_WASM_LIMIT

  if (turbo_runtime_json_object_set(root, "limits", limits) !=
      TURBO_RUNTIME_JSON_OK)
    goto fail;
  limits = NULL;
  return root;

fail:
  turbo_runtime_json_destroy(field);
  turbo_runtime_json_destroy(limits);
  turbo_runtime_json_destroy(root);
  return NULL;
}

static void turbo_wasm_tool_pack_remove_runtime_tools(
    turbo_wasm_tool_pack_t *pack, turbo_tool_runtime_t *runtime) {
  size_t i;
  if (!pack || !runtime) return;
  for (i = 0; i < turbo_tool_runtime_count(runtime); ++i) {
    turbo_tool_runtime_tool_t tool = {0};
    if (turbo_tool_runtime_get_tool(runtime, i, &tool) == TURBO_TOOL_OK &&
        tool.name)
      (void)turbo_tool_registry_remove(pack->registry, tool.name);
  }
}

static int
turbo_wasm_tool_pack_execution_policy_valid(const turbo_tool_execution_policy_t *policy) {
  if (!policy || policy->mode < TURBO_TOOL_EXECUTION_SEQUENTIAL ||
      policy->mode > TURBO_TOOL_EXECUTION_EXCLUSIVE ||
      policy->mode == TURBO_TOOL_EXECUTION_PARALLEL_SAFE) {
    return 0;
  }
  return policy->idempotency >= TURBO_TOOL_IDEMPOTENCY_NONE &&
         policy->idempotency <= TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
}

void turbo_wasm_tool_pack_config_init(turbo_wasm_tool_pack_config_t *config) {
  if (!config) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_WASM_TOOL_PACK_ABI_VERSION;
  config->max_modules = TURBO_WASM_TOOL_PACK_DEFAULT_MAX_MODULES;
  config->max_tools = TURBO_WASM_TOOL_PACK_DEFAULT_MAX_TOOLS;
}

void turbo_wasm_tool_pack_module_config_init(turbo_wasm_tool_pack_module_config_t *config) {
  if (!config) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_WASM_TOOL_PACK_ABI_VERSION;
  turbo_tool_runtime_wasm_config_init(&config->runtime);
  config->execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
  config->execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_NONE;
}

turbo_wasm_tool_pack_t *turbo_wasm_tool_pack_create(const turbo_wasm_tool_pack_config_t *config) {
  turbo_wasm_tool_pack_t *pack;

  if (!config || config->struct_size < sizeof(*config) ||
      config->abi_version != TURBO_WASM_TOOL_PACK_ABI_VERSION || !config->max_modules ||
      !config->max_tools) {
    return NULL;
  }

  pack = (turbo_wasm_tool_pack_t *)calloc(1, sizeof(*pack));
  if (!pack) {
    return NULL;
  }
  pack->registry = turbo_tool_registry_create();
  if (!pack->registry) {
    free(pack);
    return NULL;
  }
  pack->max_modules = config->max_modules;
  pack->max_tools = config->max_tools;
  return pack;
}

void turbo_wasm_tool_pack_destroy(turbo_wasm_tool_pack_t *pack) {
  if (!pack) {
    return;
  }
  turbo_tool_registry_destroy(pack->registry);
  free(pack);
}

turbo_tool_status_t
turbo_wasm_tool_pack_add_module(turbo_wasm_tool_pack_t *pack,
                                const turbo_wasm_tool_pack_module_config_t *config) {
  turbo_tool_runtime_t *runtime;
  turbo_tool_status_t status;
  json_value_t *execution_metadata = NULL;
  size_t module_tool_count;
  size_t current_tool_count;

  if (!pack || !config || config->struct_size < sizeof(*config) ||
      config->abi_version != TURBO_WASM_TOOL_PACK_ABI_VERSION ||
      !turbo_wasm_tool_pack_execution_policy_valid(&config->execution_policy)) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  if (pack->module_count >= pack->max_modules) {
    return TURBO_TOOL_BACKPRESSURE;
  }

  runtime = turbo_tool_runtime_wasm_create(&config->runtime);
  if (!runtime) {
    return TURBO_TOOL_ERROR;
  }
  module_tool_count = turbo_tool_runtime_count(runtime);
  current_tool_count = turbo_tool_registry_count(pack->registry);
  if (!module_tool_count) {
    turbo_tool_runtime_destroy(runtime);
    return TURBO_TOOL_ERROR;
  }
  if (current_tool_count > pack->max_tools ||
      module_tool_count > pack->max_tools - current_tool_count) {
    turbo_tool_runtime_destroy(runtime);
    return TURBO_TOOL_BACKPRESSURE;
  }

  execution_metadata =
      turbo_wasm_tool_pack_execution_metadata(&config->runtime);
  if (!execution_metadata) {
    turbo_tool_runtime_destroy(runtime);
    return TURBO_TOOL_ERROR;
  }

  status = turbo_tool_runtime_add_to_registry(
      runtime, pack->registry, &config->execution_policy);
  if (status == TURBO_TOOL_OK) {
    size_t tool_index;
    for (tool_index = 0; tool_index < module_tool_count; ++tool_index) {
      turbo_tool_runtime_tool_t tool = {0};
      status = turbo_tool_runtime_get_tool(runtime, tool_index, &tool);
      if (status != TURBO_TOOL_OK || !tool.name) {
        status = TURBO_TOOL_ERROR;
        break;
      }
      status = turbo_tool_registry_set_execution_metadata(
          pack->registry, tool.name, execution_metadata);
      if (status != TURBO_TOOL_OK) break;
    }
  }
  if (status != TURBO_TOOL_OK) {
    turbo_wasm_tool_pack_remove_runtime_tools(pack, runtime);
    turbo_runtime_json_destroy(execution_metadata);
    turbo_tool_runtime_destroy(runtime);
    return status;
  }

  turbo_runtime_json_destroy(execution_metadata);
  turbo_tool_runtime_destroy(runtime);
  pack->module_count++;
  return TURBO_TOOL_OK;
}

turbo_tool_registry_t *turbo_wasm_tool_pack_registry(turbo_wasm_tool_pack_t *pack) {
  return pack ? pack->registry : NULL;
}

size_t turbo_wasm_tool_pack_module_count(const turbo_wasm_tool_pack_t *pack) {
  return pack ? pack->module_count : 0;
}

size_t turbo_wasm_tool_pack_tool_count(const turbo_wasm_tool_pack_t *pack) {
  return pack ? turbo_tool_registry_count(pack->registry) : 0;
}
