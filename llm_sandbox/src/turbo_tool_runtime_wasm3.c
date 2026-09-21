#include "turbo_tool_runtime_wasm3.h"

#include <salts_fs.h>

#include <string.h>

turbo_tool_runtime_t *
turbo_tool_runtime_wasm3_create(const turbo_tool_runtime_wasm3_config_t *config) {
  turbo_tool_runtime_wasm_config_t wasm_config;
  turbo_wasm_execution_limits_t limits;
  turbo_wasm_policy_t *policy = NULL;
  turbo_tool_runtime_t *runtime = NULL;
  char module_root[SALTS_FS_MAX_PATH];
  char module_name[SALTS_FS_MAX_PATH];

  if (!config || !config->module_path || !config->module_path[0] || config->enable_turbonet_host ||
      config->enable_http_host || config->enable_redis_host ||
      salts_fs_path_dirname(config->module_path, module_root, sizeof(module_root)) != 0 ||
      salts_fs_path_basename(config->module_path, module_name, sizeof(module_name)) != 0)
    return NULL;
  policy = turbo_wasm_policy_create();
  if (!policy ||
      turbo_wasm_policy_set_capabilities(policy, TURBO_WASM_CAP_CORE | TURBO_WASM_CAP_APP) !=
          TURBO_WASM_OK ||
      turbo_wasm_policy_set_module_root(policy, module_root) != TURBO_WASM_OK ||
      turbo_wasm_policy_get_execution_limits(policy, &limits) != TURBO_WASM_OK)
    goto cleanup;
  if (config->stack_size) limits.stack_bytes = config->stack_size;
  if (turbo_wasm_policy_set_execution_limits(policy, &limits) != TURBO_WASM_OK) goto cleanup;
  turbo_tool_runtime_wasm_config_init(&wasm_config);
  wasm_config.module_path = module_name;
  wasm_config.policy = policy;
  runtime = turbo_tool_runtime_wasm_create(&wasm_config);

cleanup:
  turbo_wasm_policy_destroy(policy);
  return runtime;
}

turbo_tool_runtime_t *
turbo_tool_runtime_default_create(const turbo_tool_runtime_wasm3_config_t *config) {
  return turbo_tool_runtime_wasm3_create(config);
}
