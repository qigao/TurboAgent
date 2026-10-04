#include "tinytest.h"

#include "turbo_wasm_tool_pack.h"
#include "turbo_agent_compiler.h"

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

static void test_pack_module_configure(
    turbo_wasm_tool_pack_module_config_t *config, const char *module_path) {
  turbo_wasm_tool_pack_module_config_init(config);
  config->runtime.module_path = module_path;
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
    turbo_tool_execution_policy_t observed = {0};
    const char *const *required_capabilities = NULL;
    size_t required_capability_count = 0;
    const json_value_t *execution_metadata = NULL;
    const char *result_schema_json = NULL;
    const json_value_t *result_schema = NULL;
    int strict_result = -1;
    char *output = NULL;

    turbo_wasm_tool_pack_config_init(&pack_config);
    pack = turbo_wasm_tool_pack_create(&pack_config);
    check_not_null(pack);
    test_pack_module_configure(&module_config, LLM_SANDBOX_WASM_TOOL_WASM_PATH);
    module_config.execution_policy.mode = TURBO_TOOL_EXECUTION_EXCLUSIVE;
    module_config.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
    check_equal(turbo_wasm_tool_pack_add_module(pack, &module_config), TURBO_TOOL_OK);
    check_equal(turbo_wasm_tool_pack_module_count(pack), 1);
    check_equal(turbo_wasm_tool_pack_tool_count(pack), 1);
    check_equal(turbo_tool_registry_get_execution_policy(
                    turbo_wasm_tool_pack_registry(pack), "echo_json", &observed),
                TURBO_TOOL_OK);
    check_equal(observed.mode, TURBO_TOOL_EXECUTION_EXCLUSIVE);
    check_equal(observed.idempotency, TURBO_TOOL_IDEMPOTENCY_READ_ONLY);
    check_equal(turbo_tool_registry_get_required_capabilities(
                    turbo_wasm_tool_pack_registry(pack), "echo_json",
                    &required_capabilities, &required_capability_count),
                TURBO_TOOL_OK);
    check_equal(required_capability_count, 1);
    check_equal(required_capabilities[0], "runtime_tools");
    check_equal(turbo_tool_registry_get_execution_metadata(
                    turbo_wasm_tool_pack_registry(pack), "echo_json",
                    &execution_metadata),
                TURBO_TOOL_OK);
    check_not_null(execution_metadata);
    check_equal(json_get_string(execution_metadata, "backend"), "turbowasm");
    check_true(strncmp(json_get_string(execution_metadata, "module_fingerprint"),
                       "fnv1a64:", 8) == 0);
    check_not_null(json_object_get(execution_metadata, "limits"));
    check_equal(turbo_tool_registry_get_result_contract(
                    turbo_wasm_tool_pack_registry(pack), "echo_json",
                    &result_schema_json, &result_schema, &strict_result),
                TURBO_TOOL_OK);
    check_not_null(result_schema_json);
    check_not_null(result_schema);
    check_equal(json_get_string(result_schema, "type"), "object");
    check_equal(strict_result, 0);

    check_equal(turbo_wasm_tool_pack_add_module(pack, &module_config),
                TURBO_TOOL_DUPLICATE);
    check_equal(turbo_wasm_tool_pack_module_count(pack), 1);
    check_equal(turbo_wasm_tool_pack_tool_count(pack), 1);

    check_equal(turbo_tool_registry_execute(
                    turbo_wasm_tool_pack_registry(pack), "echo_json",
                    "{\"pack\":true}", &output),
                TURBO_TOOL_OK);
    check_equal(output, "{\"pack\":true}");

    free(output);
    turbo_wasm_tool_pack_destroy(pack);
  }

  it("compiles an Inspect plan into the Wasm backend and certificates its fingerprint") {
    turbo_wasm_tool_pack_config_t pack_config;
    turbo_wasm_tool_pack_module_config_t module_config;
    turbo_wasm_tool_pack_t *pack = NULL;
    turbo_agent_compiler_config_t compiler_config;
    turbo_agent_typed_plan_t source;
    turbo_agent_executable_plan_t *plan = NULL;
    turbo_tool_execution_context_t context = {0};
    json_value_t *arguments = json_create_object();
    json_value_t *result = NULL;
    json_value_t *certificate = NULL;
    const json_value_t *execution_metadata = NULL;
    const json_value_t *limits = NULL;
    const char *allowed[] = {"runtime_tools"};

    check_not_null(arguments);
    turbo_wasm_tool_pack_config_init(&pack_config);
    pack = turbo_wasm_tool_pack_create(&pack_config);
    check_not_null(pack);

    test_pack_module_configure(&module_config, LLM_SANDBOX_WASM_TOOL_WASM_PATH);
    module_config.execution_policy.mode = TURBO_TOOL_EXECUTION_EXCLUSIVE;
    module_config.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
    check_equal(turbo_wasm_tool_pack_add_module(pack, &module_config),
                TURBO_TOOL_OK);

    turbo_agent_compiler_config_init(&compiler_config);
    compiler_config.allowed_capabilities = allowed;
    compiler_config.allowed_capability_count = 1;
    turbo_agent_typed_plan_init(&source);
    source.template_kind = TURBO_AGENT_TEMPLATE_INSPECT;
    source.step_id = "inspect-wasm";
    source.tool_name = "echo_json";
    source.arguments = arguments;

    check_equal(turbo_agent_compile_plan(
                    &compiler_config, turbo_wasm_tool_pack_registry(pack),
                    &source, &plan, NULL),
                TURBO_AGENT_COMPILE_OK);
    check_not_null(plan);

    certificate = turbo_agent_executable_plan_certificate_json_value(plan);
    check_not_null(certificate);
    execution_metadata = json_object_get(certificate, "execution_metadata");
    check_not_null(execution_metadata);
    check_equal(json_get_string(execution_metadata, "backend"), "turbowasm");
    check_true(strncmp(json_get_string(execution_metadata, "module_fingerprint"),
                       "fnv1a64:", 8) == 0);
    limits = json_object_get(execution_metadata, "limits");
    check_not_null(limits);
    check_not_null(json_get_string(limits, "fuel_per_call"));

    context.struct_size = sizeof(context);
    context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
    context.turn_id = "turn-wasm-phase2";
    context.tool_call_id = "call-wasm-phase2";
    check_equal(turbo_agent_execute_compiled_plan(plan, &context, &result),
                TURBO_TOOL_OK);
    check_not_null(result);
    check_equal(json_object_size(result), 0);

    turbo_runtime_json_destroy(result);
    turbo_runtime_json_destroy(certificate);
    turbo_agent_executable_plan_destroy(plan);
    turbo_runtime_json_destroy(arguments);
    turbo_wasm_tool_pack_destroy(pack);
  }

  it("enforces module and tool capacity without changing prior tools") {
    turbo_wasm_tool_pack_config_t pack_config;
    turbo_wasm_tool_pack_module_config_t first_module;
    turbo_wasm_tool_pack_module_config_t second_module;
    turbo_wasm_tool_pack_t *module_limited_pack;
    turbo_wasm_tool_pack_t *tool_limited_pack;

    test_pack_module_configure(&first_module, LLM_SANDBOX_WASM_TOOL_WASM_PATH);
    test_pack_module_configure(&second_module, LLM_SANDBOX_WASM_MEMORY_GROW_WASM_PATH);

    turbo_wasm_tool_pack_config_init(&pack_config);
    pack_config.max_modules = 1;
    module_limited_pack = turbo_wasm_tool_pack_create(&pack_config);
    check_not_null(module_limited_pack);
    check_equal(turbo_wasm_tool_pack_add_module(module_limited_pack, &first_module),
                TURBO_TOOL_OK);
    check_equal(turbo_wasm_tool_pack_add_module(module_limited_pack, &second_module),
                TURBO_TOOL_BACKPRESSURE);
    check_equal(turbo_wasm_tool_pack_tool_count(module_limited_pack), 1);

    turbo_wasm_tool_pack_config_init(&pack_config);
    pack_config.max_tools = 1;
    tool_limited_pack = turbo_wasm_tool_pack_create(&pack_config);
    check_not_null(tool_limited_pack);
    check_equal(turbo_wasm_tool_pack_add_module(tool_limited_pack, &first_module),
                TURBO_TOOL_OK);
    check_equal(turbo_wasm_tool_pack_add_module(tool_limited_pack, &second_module),
                TURBO_TOOL_BACKPRESSURE);
    check_equal(turbo_wasm_tool_pack_module_count(tool_limited_pack), 1);
    check_equal(turbo_wasm_tool_pack_tool_count(tool_limited_pack), 1);

    turbo_wasm_tool_pack_destroy(tool_limited_pack);
    turbo_wasm_tool_pack_destroy(module_limited_pack);
  }

  it("rejects empty catalogs and unsafe concurrent execution declarations") {
    turbo_wasm_tool_pack_config_t pack_config;
    turbo_wasm_tool_pack_module_config_t module_config;
    turbo_wasm_tool_pack_t *pack;

    turbo_wasm_tool_pack_config_init(&pack_config);
    pack = turbo_wasm_tool_pack_create(&pack_config);
    check_not_null(pack);
    test_pack_module_configure(&module_config, LLM_SANDBOX_WASM_ZERO_TOOLS_WASM_PATH);
    check_equal(turbo_wasm_tool_pack_add_module(pack, &module_config), TURBO_TOOL_ERROR);
    check_equal(turbo_wasm_tool_pack_module_count(pack), 0);
    check_equal(turbo_wasm_tool_pack_tool_count(pack), 0);

    module_config.execution_policy.mode = TURBO_TOOL_EXECUTION_PARALLEL_SAFE;
    check_equal(turbo_wasm_tool_pack_add_module(pack, &module_config),
                TURBO_TOOL_INVALID_ARGUMENT);
    check_equal(turbo_wasm_tool_pack_tool_count(pack), 0);

    turbo_wasm_tool_pack_destroy(pack);
  }
}
