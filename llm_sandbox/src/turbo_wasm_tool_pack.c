#include "turbo_wasm_tool_pack.h"

#include <stdlib.h>
#include <string.h>

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

  status = turbo_tool_runtime_add_to_registry(runtime, pack->registry, &config->execution_policy);
  turbo_tool_runtime_destroy(runtime);
  if (status != TURBO_TOOL_OK) {
    return status;
  }

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
