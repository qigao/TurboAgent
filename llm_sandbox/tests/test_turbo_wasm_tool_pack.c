#include "tinytest.h"
#include "turbo_fs.h"
#include "turbo_wasm_tool_pack.h"

#include <stdlib.h>

#ifndef LLM_SANDBOX_WASM_TOOL_WASM_PATH
  #error "LLM_SANDBOX_WASM_TOOL_WASM_PATH must be defined"
#endif
#ifndef LLM_SANDBOX_WASM_MEMORY_GROW_WASM_PATH
  #error "LLM_SANDBOX_WASM_MEMORY_GROW_WASM_PATH must be defined"
#endif
#ifndef LLM_SANDBOX_WASM_ZERO_TOOLS_WASM_PATH
  #error "LLM_SANDBOX_WASM_ZERO_TOOLS_WASM_PATH must be defined"
#endif

static turbo_wasm_policy_t *test_pack_policy_create(const char *module_path) {
  turbo_wasm_policy_t *policy = turbo_wasm_policy_create();
  char module_root[TURBO_FS_MAX_PATH];

  if (!policy || turbo_fs_path_dirname(module_path, module_root, sizeof(module_root)) != 0 ||
      turbo_wasm_policy_set_capabilities(policy, TURBO_WASM_CAP_CORE | TURBO_WASM_CAP_APP) !=
          TURBO_WASM_OK ||
      turbo_wasm_policy_set_module_root(policy, module_root) != TURBO_WASM_OK) {
    turbo_wasm_policy_destroy(policy);
    return NULL;
  }
  return policy;
}

static int test_pack_module_configure(turbo_wasm_tool_pack_module_config_t *config,
                                      const char *module_path, turbo_wasm_policy_t *policy,
                                      char *module_name, size_t module_name_size) {
  if (!config || !module_path || !policy || !module_name || !module_name_size ||
      turbo_fs_path_basename(module_path, module_name, module_name_size) != 0) {
    return -1;
  }
  turbo_wasm_tool_pack_module_config_init(config);
  config->runtime.module_path = module_name;
  config->runtime.policy = policy;
  return 0;
}

spec("TurboWasm tool pack") {
  it("validates versioned pack configuration") {
    turbo_wasm_tool_pack_config_t pack_config;
    turbo_wasm_tool_pack_t *pack;

    turbo_wasm_tool_pack_config_init(&pack_config);
    pack_config.abi_version++;
    check_null(turbo_wasm_tool_pack_create(&pack_config));
    pack_config.abi_version = TURBO_WASM_TOOL_PACK_ABI_VERSION;
    pack_config.max_tools = 0;
    check_null(turbo_wasm_tool_pack_create(&pack_config));
    pack_config.max_tools = 1;
    pack = turbo_wasm_tool_pack_create(&pack_config);
    check_not_null(pack);
    turbo_wasm_tool_pack_destroy(pack);
  }

  it("loads a module atomically and exposes its owned registry") {
    turbo_wasm_tool_pack_config_t pack_config;
    turbo_wasm_tool_pack_module_config_t module_config;
    turbo_wasm_tool_pack_t *pack;
    turbo_wasm_policy_t *policy = test_pack_policy_create(LLM_SANDBOX_WASM_TOOL_WASM_PATH);
    turbo_tool_execution_policy_t observed = {0};
    const char *const *required_capabilities = NULL;
    size_t required_capability_count = 0;
    char module_name[TURBO_FS_MAX_PATH];
    char *output = NULL;

    turbo_wasm_tool_pack_config_init(&pack_config);
    pack = turbo_wasm_tool_pack_create(&pack_config);
    check_not_null(pack);
    check_not_null(policy);
    check_int_eq(test_pack_module_configure(&module_config, LLM_SANDBOX_WASM_TOOL_WASM_PATH, policy,
                                            module_name, sizeof(module_name)),
                 0);
    module_config.execution_policy.mode = TURBO_TOOL_EXECUTION_EXCLUSIVE;
    module_config.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
    check_int_eq(turbo_wasm_tool_pack_add_module(pack, &module_config), TURBO_TOOL_OK);
    check_size_eq(turbo_wasm_tool_pack_module_count(pack), 1);
    check_size_eq(turbo_wasm_tool_pack_tool_count(pack), 1);
    check_int_eq(turbo_tool_registry_get_execution_policy(turbo_wasm_tool_pack_registry(pack),
                                                          "echo_json", &observed),
                 TURBO_TOOL_OK);
    check_int_eq(observed.mode, TURBO_TOOL_EXECUTION_EXCLUSIVE);
    check_int_eq(observed.idempotency, TURBO_TOOL_IDEMPOTENCY_READ_ONLY);
    check_int_eq(turbo_tool_registry_get_required_capabilities(turbo_wasm_tool_pack_registry(pack),
                                                               "echo_json", &required_capabilities,
                                                               &required_capability_count),
                 TURBO_TOOL_OK);
    check_size_eq(required_capability_count, 1);
    check_str_eq(required_capabilities[0], "runtime_tools");

    check_int_eq(turbo_wasm_tool_pack_add_module(pack, &module_config), TURBO_TOOL_DUPLICATE);
    check_size_eq(turbo_wasm_tool_pack_module_count(pack), 1);
    check_size_eq(turbo_wasm_tool_pack_tool_count(pack), 1);
    turbo_wasm_policy_destroy(policy);
    policy = NULL;

    check_int_eq(turbo_tool_registry_execute(turbo_wasm_tool_pack_registry(pack), "echo_json",
                                             "{\"pack\":true}", &output),
                 TURBO_TOOL_OK);
    check_str_eq(output, "{\"pack\":true}");

    free(output);
    turbo_wasm_tool_pack_destroy(pack);
  }

  it("enforces module and tool capacity without changing prior tools") {
    turbo_wasm_tool_pack_config_t pack_config;
    turbo_wasm_tool_pack_module_config_t first_module;
    turbo_wasm_tool_pack_module_config_t second_module;
    turbo_wasm_tool_pack_t *module_limited_pack;
    turbo_wasm_tool_pack_t *tool_limited_pack;
    turbo_wasm_policy_t *policy = test_pack_policy_create(LLM_SANDBOX_WASM_TOOL_WASM_PATH);
    char first_name[TURBO_FS_MAX_PATH];
    char second_name[TURBO_FS_MAX_PATH];

    check_not_null(policy);
    check_int_eq(test_pack_module_configure(&first_module, LLM_SANDBOX_WASM_TOOL_WASM_PATH, policy,
                                            first_name, sizeof(first_name)),
                 0);
    check_int_eq(test_pack_module_configure(&second_module, LLM_SANDBOX_WASM_MEMORY_GROW_WASM_PATH,
                                            policy, second_name, sizeof(second_name)),
                 0);

    turbo_wasm_tool_pack_config_init(&pack_config);
    pack_config.max_modules = 1;
    module_limited_pack = turbo_wasm_tool_pack_create(&pack_config);
    check_not_null(module_limited_pack);
    check_int_eq(turbo_wasm_tool_pack_add_module(module_limited_pack, &first_module),
                 TURBO_TOOL_OK);
    check_int_eq(turbo_wasm_tool_pack_add_module(module_limited_pack, &second_module),
                 TURBO_TOOL_BACKPRESSURE);
    check_size_eq(turbo_wasm_tool_pack_tool_count(module_limited_pack), 1);

    turbo_wasm_tool_pack_config_init(&pack_config);
    pack_config.max_tools = 1;
    tool_limited_pack = turbo_wasm_tool_pack_create(&pack_config);
    check_not_null(tool_limited_pack);
    check_int_eq(turbo_wasm_tool_pack_add_module(tool_limited_pack, &first_module), TURBO_TOOL_OK);
    check_int_eq(turbo_wasm_tool_pack_add_module(tool_limited_pack, &second_module),
                 TURBO_TOOL_BACKPRESSURE);
    check_size_eq(turbo_wasm_tool_pack_module_count(tool_limited_pack), 1);
    check_size_eq(turbo_wasm_tool_pack_tool_count(tool_limited_pack), 1);

    turbo_wasm_tool_pack_destroy(tool_limited_pack);
    turbo_wasm_tool_pack_destroy(module_limited_pack);
    turbo_wasm_policy_destroy(policy);
  }

  it("rejects empty catalogs and unsafe concurrent execution declarations") {
    turbo_wasm_tool_pack_config_t pack_config;
    turbo_wasm_tool_pack_module_config_t module_config;
    turbo_wasm_tool_pack_t *pack;
    turbo_wasm_policy_t *policy = test_pack_policy_create(LLM_SANDBOX_WASM_ZERO_TOOLS_WASM_PATH);
    char module_name[TURBO_FS_MAX_PATH];

    turbo_wasm_tool_pack_config_init(&pack_config);
    pack = turbo_wasm_tool_pack_create(&pack_config);
    check_not_null(pack);
    check_not_null(policy);
    check_int_eq(test_pack_module_configure(&module_config, LLM_SANDBOX_WASM_ZERO_TOOLS_WASM_PATH,
                                            policy, module_name, sizeof(module_name)),
                 0);
    check_int_eq(turbo_wasm_tool_pack_add_module(pack, &module_config), TURBO_TOOL_ERROR);
    check_size_eq(turbo_wasm_tool_pack_module_count(pack), 0);
    check_size_eq(turbo_wasm_tool_pack_tool_count(pack), 0);

    module_config.execution_policy.mode = TURBO_TOOL_EXECUTION_PARALLEL_SAFE;
    check_int_eq(turbo_wasm_tool_pack_add_module(pack, &module_config),
                 TURBO_TOOL_INVALID_ARGUMENT);
    check_size_eq(turbo_wasm_tool_pack_tool_count(pack), 0);

    turbo_wasm_tool_pack_destroy(pack);
    turbo_wasm_policy_destroy(policy);
  }
}
