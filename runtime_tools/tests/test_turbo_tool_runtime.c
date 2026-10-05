#include "tinytest.h"
#include <json_parser.h>
#include "turbo_tool_registry.h"
#include "turbo_tool_runtime.h"

#include <stdlib.h>
#include <string.h>

static int test_echo_tool(const char *arguments_json, char **out_output, void *user_data) {
  size_t len;
  char *copy;
  (void)user_data;

  if (!out_output) {
    return -1;
  }

  len = strlen(arguments_json ? arguments_json : "{}") + 1;
  copy = (char *)malloc(len);
  if (!copy) {
    return -1;
  }

  memcpy(copy, arguments_json ? arguments_json : "{}", len);
  *out_output = copy;
  return 0;
}

static int test_echo_tool_json_value(const json_value_t *arguments, json_value_t **out_result,
                                     void *user_data) {
  json_value_t *json_value;
  json_value_t *result;
  (void)user_data;

  if (!arguments || !out_result) {
    return -1;
  }

  json_value = json_clone(arguments);
  if (!json_value) {
    return -1;
  }
  result = json_clone(json_value);
  json_free(json_value); json_value = NULL;
  if (!result) {
    return -1;
  }

  *out_result = result;
  return 0;
}

typedef struct test_tool_context_probe_s {
  const turbo_tool_execution_context_t *seen_context;
  const char *seen_turn_id;
  int calls;
} test_tool_context_probe_t;

FunctionDeclResult(
    value, int, CMETA_RESULT_VALUE, test_native_increment,
    (int, value, CMETA_PARAM_IN));

int test_native_increment(int value) {
  return value + 1;
}

static cmeta_callable test_native_increment_callable(void) {
  const cmeta_function_desc *function = FunctionMeta(test_native_increment);
  cmeta_callable callable = {0};
  callable.meta = CMETA_WRAP_TYPED_ANY(test_native_increment);
  callable.meta.effects =
      function ? function->effects : CMETA_EFFECT_UNKNOWN;
  callable.meta.properties =
      function ? function->properties : CMETA_PROP_NONE;
  callable.invoke = CMETA_TYPED_INVOKER_ANY(test_native_increment);
  callable.generate = NULL;
  callable.dispatch = CMETA_CALLABLE_DISPATCH_CANONICAL_RAW;
  callable.capture_size = 0u;
  return callable;
}

static turbo_tool_status_t test_context_echo_tool(
    const char *arguments_json, const turbo_tool_execution_context_t *context,
    char **out_output, void *user_data) {
  test_tool_context_probe_t *probe = (test_tool_context_probe_t *)user_data;
  size_t len;
  char *copy;
  if (!probe || !context || !out_output) return TURBO_TOOL_INVALID_ARGUMENT;
  ++probe->calls;
  probe->seen_context = context;
  probe->seen_turn_id = context->turn_id;
  len = strlen(arguments_json ? arguments_json : "{}") + 1;
  copy = (char *)malloc(len);
  if (!copy) return TURBO_TOOL_OUT_OF_MEMORY;
  memcpy(copy, arguments_json ? arguments_json : "{}", len);
  *out_output = copy;
  return TURBO_TOOL_OK;
}

spec("turbo tool runtime") {
  it("should publish and preserve a borrowed canonical CMeta native projection") {
    turbo_tool_registry_t *source = turbo_tool_registry_create();
    turbo_tool_registry_t *projected = NULL;
    turbo_tool_registry_t *composite = NULL;
    const turbo_tool_registry_t *sources[1];
    const char *names[] = {"native_increment"};
    turbo_tool_definition_v6_t definition = {0};
    turbo_tool_native_projection_t native = {0};
    turbo_tool_native_projection_t observed = {0};

    check_not_null(source);
    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
    definition.definition.name = "native_increment";
    definition.definition.description = "Reflected native increment";
    definition.definition.parameters_json =
        "{\"type\":\"object\",\"additionalProperties\":true}";
    definition.definition.json_value_handler = test_echo_tool_json_value;
    definition.execution_policy.mode = TURBO_TOOL_EXECUTION_PARALLEL_SAFE;
    definition.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
    definition.result_schema_json = "{\"type\":\"integer\"}";
    definition.effect_flags = TURBO_TOOL_EFFECT_PURE;
    check_equal(turbo_tool_registry_add_v6(source, &definition), TURBO_TOOL_OK);

    native.struct_size = sizeof(native);
    native.abi_version = TURBO_TOOL_NATIVE_PROJECTION_ABI_VERSION;
    native.function = FunctionMeta(test_native_increment);
    native.abi = FunctionAbi(test_native_increment);
    native.callable = test_native_increment_callable();

    check_equal(turbo_tool_registry_publish_native_projection(
                    source, "native_increment", &native),
                TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_publish_native_projection(
                    source, "native_increment", &native),
                TURBO_TOOL_DUPLICATE);
    check_equal(turbo_tool_registry_get_native_projection(
                    source, "native_increment", &observed),
                TURBO_TOOL_OK);
    check_equal(observed.function, native.function);
    check_equal(observed.abi, native.abi);
    check_true(cmeta_callable_same(observed.callable, native.callable));

    check_equal(turbo_tool_registry_project(
                    source, names, 1u, &projected),
                TURBO_TOOL_OK);
    memset(&observed, 0, sizeof(observed));
    check_equal(turbo_tool_registry_get_native_projection(
                    projected, "native_increment", &observed),
                TURBO_TOOL_OK);
    check_equal(observed.function, native.function);
    check_true(cmeta_callable_same(observed.callable, native.callable));

    sources[0] = projected;
    check_equal(turbo_tool_registry_compose(sources, 1u, &composite),
                TURBO_TOOL_OK);
    memset(&observed, 0, sizeof(observed));
    check_equal(turbo_tool_registry_get_native_projection(
                    composite, "native_increment", &observed),
                TURBO_TOOL_OK);
    check_equal(observed.abi, native.abi);
    check_true(cmeta_callable_same(observed.callable, native.callable));

    turbo_tool_registry_destroy(composite);
    turbo_tool_registry_destroy(projected);
    turbo_tool_registry_destroy(source);
  }

  it("should fail closed for missing or inconsistent native projection authority") {
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_v6_t definition = {0};
    turbo_tool_native_projection_t native = {0};
    turbo_tool_native_projection_t observed = {0};

    check_not_null(registry);
    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
    definition.definition.name = "legacy_json_only";
    definition.definition.description = "No native authority";
    definition.definition.parameters_json = "{\"type\":\"object\"}";
    definition.definition.json_value_handler = test_echo_tool_json_value;
    definition.execution_policy.mode = TURBO_TOOL_EXECUTION_PARALLEL_SAFE;
    definition.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
    definition.effect_flags = TURBO_TOOL_EFFECT_PURE;
    check_equal(turbo_tool_registry_add_v6(registry, &definition), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_get_native_projection(
                    registry, "legacy_json_only", &observed),
                TURBO_TOOL_NOT_FOUND);

    native.struct_size = sizeof(native);
    native.abi_version = TURBO_TOOL_NATIVE_PROJECTION_ABI_VERSION;
    native.function = FunctionMeta(test_native_increment);
    native.abi = NULL;
    native.callable = test_native_increment_callable();
    check_equal(turbo_tool_registry_publish_native_projection(
                    registry, "legacy_json_only", &native),
                TURBO_TOOL_INVALID_ARGUMENT);

    native.abi = FunctionAbi(test_native_increment);
    native.callable.meta.effects = CMETA_EFFECT_STATEFUL;
    check_equal(turbo_tool_registry_publish_native_projection(
                    registry, "legacy_json_only", &native),
                TURBO_TOOL_INVALID_ARGUMENT);

    turbo_tool_registry_destroy(registry);
  }

  it("should preserve native projection through the native runtime registry bridge") {
    turbo_tool_runtime_t *runtime = turbo_tool_runtime_native_create();
    turbo_tool_registry_t *bridge = NULL;
    turbo_tool_definition_v6_t definition = {0};
    turbo_tool_native_projection_t native = {0};
    turbo_tool_native_projection_t observed = {0};

    check_not_null(runtime);
    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
    definition.definition.name = "native_runtime_increment";
    definition.definition.description = "Runtime reflected native increment";
    definition.definition.parameters_json = "{\"type\":\"object\"}";
    definition.definition.json_value_handler = test_echo_tool_json_value;
    definition.execution_policy.mode = TURBO_TOOL_EXECUTION_PARALLEL_SAFE;
    definition.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
    definition.result_schema_json = "{\"type\":\"integer\"}";
    definition.effect_flags = TURBO_TOOL_EFFECT_PURE;
    check_equal(turbo_tool_runtime_native_add_tool_v6(runtime, &definition),
                TURBO_TOOL_OK);

    native.struct_size = sizeof(native);
    native.abi_version = TURBO_TOOL_NATIVE_PROJECTION_ABI_VERSION;
    native.function = FunctionMeta(test_native_increment);
    native.abi = FunctionAbi(test_native_increment);
    native.callable = test_native_increment_callable();
    check_equal(turbo_tool_runtime_native_publish_native_projection(
                    runtime, "native_runtime_increment", &native),
                TURBO_TOOL_OK);
    check_equal(turbo_tool_runtime_native_get_native_projection(
                    runtime, "native_runtime_increment", &observed),
                TURBO_TOOL_OK);
    check_true(cmeta_callable_same(observed.callable, native.callable));

    bridge = turbo_tool_runtime_build_registry_bridge(runtime);
    check_not_null(bridge);
    memset(&observed, 0, sizeof(observed));
    check_equal(turbo_tool_registry_get_native_projection(
                    bridge, "native_runtime_increment", &observed),
                TURBO_TOOL_OK);
    check_equal(observed.function, native.function);
    check_equal(observed.abi, native.abi);
    check_true(cmeta_callable_same(observed.callable, native.callable));

    turbo_tool_registry_destroy(bridge);
    turbo_tool_runtime_destroy(runtime);
  }

  it("should own and preserve execution metadata through projection and composition") {
    turbo_tool_registry_t *source = turbo_tool_registry_create();
    turbo_tool_registry_t *projection = NULL;
    turbo_tool_registry_t *composite = NULL;
    const turbo_tool_registry_t *sources[1];
    turbo_tool_definition_t definition = {
        "meta", "metadata", "{\"type\":\"object\"}", NULL, 1,
        test_echo_tool, test_echo_tool_json_value, NULL, NULL};
    json_value_t *metadata = json_create_object();
    json_value_t *limits = json_create_object();
    const json_value_t *observed = NULL;
    const char *names[] = {"meta"};

    check_not_null(source);
    check_not_null(metadata);
    check_not_null(limits);
    check_equal(turbo_tool_registry_add(source, &definition), TURBO_TOOL_OK);
    check_equal(turbo_runtime_json_object_set(
                    metadata, "backend", json_create_string("probe")),
                TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_runtime_json_object_set(
                    limits, "fuel", json_create_int64(7)),
                TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_runtime_json_object_set(metadata, "limits", limits),
                TURBO_RUNTIME_JSON_OK);
    limits = NULL;

    check_equal(turbo_tool_registry_set_execution_metadata(
                    source, "meta", metadata),
                TURBO_TOOL_OK);
    check_equal(turbo_runtime_json_object_set(
                    metadata, "backend", json_create_string("mutated")),
                TURBO_RUNTIME_JSON_OK);

    check_equal(turbo_tool_registry_get_execution_metadata(
                    source, "meta", &observed),
                TURBO_TOOL_OK);
    check_not_null(observed);
    check_equal(json_get_string(observed, "backend"), "probe");

    check_equal(turbo_tool_registry_project(source, names, 1, &projection),
                TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_get_execution_metadata(
                    projection, "meta", &observed),
                TURBO_TOOL_OK);
    check_equal(json_get_string(observed, "backend"), "probe");

    sources[0] = source;
    check_equal(turbo_tool_registry_compose(sources, 1, &composite),
                TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_get_execution_metadata(
                    composite, "meta", &observed),
                TURBO_TOOL_OK);
    check_equal(json_get_string(observed, "backend"), "probe");

    turbo_runtime_json_destroy(metadata);
    turbo_tool_registry_destroy(composite);
    turbo_tool_registry_destroy(projection);
    turbo_tool_registry_destroy(source);
  }


  it("should propagate v4 execution context through projection and composition") {
    turbo_tool_registry_t *source = turbo_tool_registry_create();
    turbo_tool_registry_t *projection = NULL;
    turbo_tool_registry_t *composite = NULL;
    const turbo_tool_registry_t *sources[1];
    const char *names[] = {"context_echo"};
    test_tool_context_probe_t probe = {0};
    turbo_tool_definition_v4_t definition = {0};
    turbo_tool_execution_context_t context = {0};
    char *output = NULL;

    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
    definition.definition.name = "context_echo";
    definition.definition.description = "Context echo";
    definition.definition.parameters_json = "{\"type\":\"object\"}";
    definition.definition.strict = 1;
    definition.definition.user_data = &probe;
    definition.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
    definition.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_NONE;
    definition.context_handler = test_context_echo_tool;
    check_equal(turbo_tool_registry_add_v4(source, &definition), TURBO_TOOL_OK);

    check_equal(turbo_tool_registry_project(source, names, 1, &projection), TURBO_TOOL_OK);
    sources[0] = projection;
    check_equal(turbo_tool_registry_compose(sources, 1, &composite), TURBO_TOOL_OK);

    context.struct_size = sizeof(context);
    context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
    context.turn_id = "turn-7";
    context.tool_call_id = "call-9";
    check_equal(turbo_tool_registry_execute_with_context(
                    composite, "context_echo", "{\"ok\":true}", &context, &output),
                TURBO_TOOL_OK);
    check_equal(output, "{\"ok\":true}");
    check_equal(probe.calls, 1);
    check_equal(probe.seen_turn_id, "turn-7");

    free(output);
    output = NULL;
    memset(&context, 0, sizeof(context));
    context.struct_size = TURBO_TOOL_EXECUTION_CONTEXT_V1_SIZE;
    context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION_V1;
    context.turn_id = "legacy-turn";
    check_equal(turbo_tool_registry_execute_with_context(
                    composite, "context_echo", "{}", &context, &output),
                TURBO_TOOL_OK);
    check_equal(probe.calls, 2);
    check_equal(probe.seen_turn_id, "legacy-turn");

    free(output);
    turbo_tool_registry_destroy(composite);
    turbo_tool_registry_destroy(projection);
    turbo_tool_registry_destroy(source);
  }

  it("should preserve legacy policy and expose validated v2 policy") {
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_execution_policy_t policy = {0};
    turbo_tool_definition_t legacy = {"legacy", "Legacy",       "{\"type\":\"object\"}",   NULL,
                                      1,        test_echo_tool, test_echo_tool_json_value, NULL,
                                      NULL};
    turbo_tool_definition_v2_t v2 = {
        sizeof(turbo_tool_definition_v2_t),
        TURBO_TOOL_DEFINITION_V2_ABI_VERSION,
        {"parallel", "Parallel", "{\"type\":\"object\"}", NULL, 1, test_echo_tool,
         test_echo_tool_json_value, NULL, NULL},
        {TURBO_TOOL_EXECUTION_PARALLEL_SAFE, TURBO_TOOL_IDEMPOTENCY_READ_ONLY}};

    check_not_null(registry);
    check_equal(turbo_tool_registry_add(registry, &legacy), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_get_execution_policy(registry, "legacy", &policy),
                 TURBO_TOOL_OK);
    check_equal(policy.mode, TURBO_TOOL_EXECUTION_SEQUENTIAL);
    check_equal(policy.idempotency, TURBO_TOOL_IDEMPOTENCY_NONE);
    check_equal(turbo_tool_registry_add_v2(registry, &v2), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_get_execution_policy(registry, "parallel", &policy),
                 TURBO_TOOL_OK);
    check_equal(policy.mode, TURBO_TOOL_EXECUTION_PARALLEL_SAFE);
    check_equal(policy.idempotency, TURBO_TOOL_IDEMPOTENCY_READ_ONLY);
    v2.abi_version++;
    check_equal(turbo_tool_registry_add_v2(registry, &v2), TURBO_TOOL_INVALID_ARGUMENT);
    turbo_tool_registry_destroy(registry);
  }

  it("should preserve required capabilities across projection and composition") {
    turbo_tool_registry_t *source = turbo_tool_registry_create();
    turbo_tool_registry_t *projection = NULL;
    turbo_tool_registry_t *composite = NULL;
    const turbo_tool_registry_t *sources[1];
    const char *names[] = {"remote"};
    const char *required[] = {"runtime_tools", "network"};
    const char *const *observed = NULL;
    size_t observed_count = 0;
    turbo_tool_definition_v3_t definition = {
        sizeof(turbo_tool_definition_v3_t),
        TURBO_TOOL_DEFINITION_V3_ABI_VERSION,
        {"remote", "Remote", "{\"type\":\"object\"}", NULL, 1, test_echo_tool,
         test_echo_tool_json_value, NULL, NULL},
        {TURBO_TOOL_EXECUTION_SEQUENTIAL, TURBO_TOOL_IDEMPOTENCY_NONE},
        required,
        2};

    check_not_null(source);
    check_equal(turbo_tool_registry_add_v3(source, &definition), TURBO_TOOL_OK);
    check_equal(
        turbo_tool_registry_get_required_capabilities(source, "remote", &observed, &observed_count),
        TURBO_TOOL_OK);
    check_equal(observed_count, 2);
    check_equal(observed[0], "runtime_tools");
    check_equal(observed[1], "network");
    check_equal(turbo_tool_registry_require_capability(source, "remote", "network"),
                 TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_project(source, names, 1, &projection), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_get_required_capabilities(projection, "remote", &observed,
                                                               &observed_count),
                 TURBO_TOOL_OK);
    check_equal(observed_count, 2);
    sources[0] = projection;
    check_equal(turbo_tool_registry_compose(sources, 1, &composite), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_get_required_capabilities(composite, "remote", &observed,
                                                               &observed_count),
                 TURBO_TOOL_OK);
    check_equal(observed_count, 2);

    turbo_tool_registry_destroy(composite);
    turbo_tool_registry_destroy(projection);
    turbo_tool_registry_destroy(source);
  }

  it("should resolve unique compatible names to canonical registry identity") {
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t definition = {
        "repo.inspect.meta", "Inspect metadata", "{\"type\":\"object\"}", NULL, 1,
        test_echo_tool, test_echo_tool_json_value, NULL, NULL};
    const char *canonical = NULL;

    check_not_null(registry);
    check_equal(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_resolve_name(
                    registry, "repo.inspect.meta", &canonical),
                TURBO_TOOL_OK);
    check_equal(canonical, "repo.inspect.meta");
    canonical = NULL;
    check_equal(turbo_tool_registry_resolve_name(
                    registry, "repo_inspect_meta", &canonical),
                TURBO_TOOL_OK);
    check_equal(canonical, "repo.inspect.meta");

    turbo_tool_registry_destroy(registry);
  }

  it("should build an ordered non-owning registry projection") {
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_registry_t *projection = NULL;
    turbo_tool_definition_t first = {"first", "First",        "{\"type\":\"object\"}",   NULL,
                                     1,       test_echo_tool, test_echo_tool_json_value, NULL,
                                     NULL};
    turbo_tool_definition_t second = {"second", "Second",       "{\"type\":\"object\"}",   NULL,
                                      1,        test_echo_tool, test_echo_tool_json_value, NULL,
                                      NULL};
    turbo_tool_definition_t view = {0};
    const char *names[] = {"second"};
    const char *missing[] = {"missing"};

    check_equal(turbo_tool_registry_add(registry, &first), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_add(registry, &second), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_project(registry, names, 1, &projection), TURBO_TOOL_OK);
    check_not_null(projection);
    check_equal(turbo_tool_registry_count(projection), 1);
    check_equal(turbo_tool_registry_get_definition(projection, 0, &view), TURBO_TOOL_OK);
    check_equal(view.name, "second");
    turbo_tool_registry_destroy(projection);
    projection = NULL;
    check_equal(turbo_tool_registry_project(registry, missing, 1, &projection),
                 TURBO_TOOL_NOT_FOUND);
    check_null(projection);
    turbo_tool_registry_destroy(registry);
  }

  it("should compose ordered borrowed registries and reject name collisions") {
    turbo_tool_registry_t *first = turbo_tool_registry_create();
    turbo_tool_registry_t *second = turbo_tool_registry_create();
    turbo_tool_registry_t *composite = NULL;
    const turbo_tool_registry_t *sources[] = {first, second};
    turbo_tool_definition_t first_tool = {
        "first", "First", "{\"type\":\"object\"}", NULL, 1, test_echo_tool, NULL, NULL, NULL};
    turbo_tool_definition_t second_tool = {
        "second", "Second", "{\"type\":\"object\"}", NULL, 1, test_echo_tool, NULL, NULL, NULL};

    check_not_null(first);
    check_not_null(second);
    check_equal(turbo_tool_registry_add(first, &first_tool), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_add(second, &second_tool), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_compose(sources, 2, &composite), TURBO_TOOL_OK);
    check_not_null(composite);
    check_equal(turbo_tool_registry_count(composite), 2);
    turbo_tool_registry_destroy(composite);
    composite = NULL;

    second_tool.name = "first";
    check_equal(turbo_tool_registry_remove(second, "second"), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_add(second, &second_tool), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_compose(sources, 2, &composite), TURBO_TOOL_DUPLICATE);
    check_null(composite);

    turbo_tool_registry_destroy(second);
    turbo_tool_registry_destroy(first);
  }

  it("should bridge native callback tools through a runtime") {
    turbo_tool_runtime_t *runtime = NULL;
    turbo_tool_registry_t *registry = NULL;
    turbo_tool_runtime_tool_t tool = {0};
    turbo_tool_definition_t definition = {"echo_json",
                                          "Echo JSON back to the caller.",
                                          "{\"type\":\"object\"}",
                                          NULL,
                                          1,
                                          test_echo_tool,
                                          test_echo_tool_json_value,
                                          NULL,
                                          NULL};
    char *output = NULL;
    json_value_t *json_value_args = NULL;
    json_value_t *json_value_result = NULL;

    runtime = turbo_tool_runtime_native_create();
    check_not_null(runtime);
    check_equal(turbo_tool_runtime_native_add_tool(runtime, &definition), TURBO_TOOL_OK);
    check_equal(turbo_tool_runtime_count(runtime), 1);
    check_equal(turbo_tool_runtime_get_tool(runtime, 0, &tool), TURBO_TOOL_OK);
    check_equal(tool.name, "echo_json");
    check_equal(turbo_tool_runtime_invoke(runtime, "echo_json", "{\"ok\":true}", &output),
                 TURBO_TOOL_OK);
    check_equal(output, "{\"ok\":true}");
    free(output);
    output = NULL;

    json_value_args = json_create_object();
    check_not_null(json_value_args);
    check_equal(
        turbo_runtime_json_object_set(json_value_args, "bridge", json_create_int64(1)),
        TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_tool_runtime_invoke_json_value(runtime, "echo_json", json_value_args,
                                                      &json_value_result),
                 TURBO_TOOL_OK);
    check_not_null(json_value_result);
    check_equal((int)turbo_runtime_json_value_as_int64(
                     json_object_get(json_value_result, "bridge"), 0),
                 1);
    turbo_runtime_json_destroy(json_value_result);
    json_value_result = NULL;

    registry = turbo_tool_runtime_build_registry_bridge(runtime);
    check_not_null(registry);
    check_equal(turbo_tool_registry_execute(registry, "echo_json", "{\"bridge\":1}", &output),
                 TURBO_TOOL_OK);
    check_equal(output, "{\"bridge\":1}");

    check_equal(turbo_tool_registry_execute_json_value(registry, "echo_json", json_value_args,
                                                        &json_value_result),
                 TURBO_TOOL_OK);
    check_not_null(json_value_result);
    check_equal((int)turbo_runtime_json_value_as_int64(
                     json_object_get(json_value_result, "bridge"), 0),
                 1);

    turbo_runtime_json_destroy(json_value_result);
    turbo_runtime_json_destroy(json_value_args);
    free(output);
    turbo_tool_registry_destroy(registry);
    turbo_tool_runtime_destroy(runtime);
  }

  it("should append runtimes with v2 policy and retain their lifetime") {
    turbo_tool_runtime_t *first_runtime = turbo_tool_runtime_native_create();
    turbo_tool_runtime_t *second_runtime = turbo_tool_runtime_native_create();
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t first = {"first_runtime_tool",
                                     "First runtime tool",
                                     "{\"type\":\"object\"}",
                                     NULL,
                                     1,
                                     test_echo_tool,
                                     test_echo_tool_json_value,
                                     NULL,
                                     NULL};
    turbo_tool_definition_t second = {"second_runtime_tool",
                                      "Second runtime tool",
                                      "{\"type\":\"object\"}",
                                      NULL,
                                      1,
                                      test_echo_tool,
                                      test_echo_tool_json_value,
                                      NULL,
                                      NULL};
    turbo_tool_execution_policy_t first_policy = {TURBO_TOOL_EXECUTION_EXCLUSIVE,
                                                  TURBO_TOOL_IDEMPOTENCY_KEYED};
    turbo_tool_execution_policy_t second_policy = {TURBO_TOOL_EXECUTION_SEQUENTIAL,
                                                   TURBO_TOOL_IDEMPOTENCY_READ_ONLY};
    turbo_tool_execution_policy_t observed = {0};
    const char *const *required_capabilities = NULL;
    size_t required_capability_count = 0;
    char *output = NULL;

    check_not_null(first_runtime);
    check_not_null(second_runtime);
    check_not_null(registry);
    check_equal(turbo_tool_runtime_native_add_tool(first_runtime, &first), TURBO_TOOL_OK);
    check_equal(turbo_tool_runtime_native_add_tool(second_runtime, &second), TURBO_TOOL_OK);
    check_equal(turbo_tool_runtime_add_to_registry(first_runtime, registry, &first_policy),
                 TURBO_TOOL_OK);
    check_equal(turbo_tool_runtime_add_to_registry(second_runtime, registry, &second_policy),
                 TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_count(registry), 2);
    turbo_tool_runtime_destroy(first_runtime);
    turbo_tool_runtime_destroy(second_runtime);

    check_equal(
        turbo_tool_registry_get_execution_policy(registry, "first_runtime_tool", &observed),
        TURBO_TOOL_OK);
    check_equal(observed.mode, TURBO_TOOL_EXECUTION_EXCLUSIVE);
    check_equal(observed.idempotency, TURBO_TOOL_IDEMPOTENCY_KEYED);
    check_equal(turbo_tool_registry_get_required_capabilities(registry, "first_runtime_tool",
                                                               &required_capabilities,
                                                               &required_capability_count),
                 TURBO_TOOL_OK);
    check_equal(required_capability_count, 1);
    check_equal(required_capabilities[0], "runtime_tools");
    check_equal(
        turbo_tool_registry_execute(registry, "second_runtime_tool", "{\"ok\":2}", &output),
        TURBO_TOOL_OK);
    check_equal(output, "{\"ok\":2}");

    free(output);
    turbo_tool_registry_destroy(registry);
  }

  it("should leave the destination unchanged when a runtime name conflicts") {
    turbo_tool_runtime_t *runtime = turbo_tool_runtime_native_create();
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t existing = {
        "conflict", "Existing tool", "{\"type\":\"object\"}",   NULL,
        1,          test_echo_tool,  test_echo_tool_json_value, NULL,
        NULL};
    turbo_tool_definition_t first = {"would_be_added",
                                     "First runtime tool",
                                     "{\"type\":\"object\"}",
                                     NULL,
                                     1,
                                     test_echo_tool,
                                     test_echo_tool_json_value,
                                     NULL,
                                     NULL};
    turbo_tool_definition_t conflict = {"conflict",
                                        "Conflicting runtime tool",
                                        "{\"type\":\"object\"}",
                                        NULL,
                                        1,
                                        test_echo_tool,
                                        test_echo_tool_json_value,
                                        NULL,
                                        NULL};
    turbo_tool_execution_policy_t policy = {TURBO_TOOL_EXECUTION_SEQUENTIAL,
                                            TURBO_TOOL_IDEMPOTENCY_NONE};

    check_not_null(runtime);
    check_not_null(registry);
    check_equal(turbo_tool_registry_add(registry, &existing), TURBO_TOOL_OK);
    check_equal(turbo_tool_runtime_native_add_tool(runtime, &first), TURBO_TOOL_OK);
    check_equal(turbo_tool_runtime_native_add_tool(runtime, &conflict), TURBO_TOOL_OK);
    check_equal(turbo_tool_runtime_add_to_registry(runtime, registry, &policy),
                 TURBO_TOOL_DUPLICATE);
    check_equal(turbo_tool_registry_count(registry), 1);
    check_equal(turbo_tool_registry_get_execution_policy(registry, "would_be_added", &policy),
                 TURBO_TOOL_NOT_FOUND);

    turbo_tool_registry_destroy(registry);
    turbo_tool_runtime_destroy(runtime);
  }

  it("should own v5 result contracts and preserve them through projections") {
    turbo_tool_registry_t *source = turbo_tool_registry_create();
    turbo_tool_registry_t *projection = NULL;
    turbo_tool_registry_t *composite = NULL;
    const turbo_tool_registry_t *sources[1];
    const char *names[] = {"typed_result"};
    turbo_tool_definition_v5_t definition = {0};
    char result_schema[64] = "{\"type\":\"string\"}";
    const char *observed_json = NULL;
    const json_value_t *observed_schema = NULL;
    int strict_result = 0;

    check_not_null(source);
    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V5_ABI_VERSION;
    definition.definition.name = "typed_result";
    definition.definition.description = "typed result";
    definition.definition.parameters_json = "{\"type\":\"object\"}";
    definition.definition.strict = 1;
    definition.definition.json_value_handler = test_echo_tool_json_value;
    definition.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
    definition.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
    definition.result_schema_json = result_schema;
    definition.strict_result = 1;

    check_equal(turbo_tool_registry_add_v5(source, &definition), TURBO_TOOL_OK);
    strcpy(result_schema, "{\"type\":\"number\"}");

    check_equal(turbo_tool_registry_get_result_contract(
                    source, "typed_result", &observed_json,
                    &observed_schema, &strict_result),
                TURBO_TOOL_OK);
    check_equal(observed_json, "{\"type\":\"string\"}");
    check_not_null(observed_schema);
    check_equal(json_get_string(observed_schema, "type"), "string");
    check_equal(strict_result, 1);

    check_equal(turbo_tool_registry_project(
                    source, names, 1, &projection),
                TURBO_TOOL_OK);
    observed_json = NULL;
    observed_schema = NULL;
    strict_result = 0;
    check_equal(turbo_tool_registry_get_result_contract(
                    projection, "typed_result", &observed_json,
                    &observed_schema, &strict_result),
                TURBO_TOOL_OK);
    check_equal(observed_json, "{\"type\":\"string\"}");
    check_equal(json_get_string(observed_schema, "type"), "string");
    check_equal(strict_result, 1);

    sources[0] = projection;
    check_equal(turbo_tool_registry_compose(sources, 1, &composite),
                TURBO_TOOL_OK);
    observed_json = NULL;
    observed_schema = NULL;
    strict_result = 0;
    check_equal(turbo_tool_registry_get_result_contract(
                    composite, "typed_result", &observed_json,
                    &observed_schema, &strict_result),
                TURBO_TOOL_OK);
    check_equal(observed_json, "{\"type\":\"string\"}");
    check_equal(strict_result, 1);

    turbo_tool_registry_destroy(composite);
    turbo_tool_registry_destroy(projection);
    turbo_tool_registry_destroy(source);
  }

  it("should keep legacy result contracts explicitly opaque and reject invalid v5 metadata") {
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_v4_t legacy = {0};
    turbo_tool_definition_v5_t invalid = {0};
    const char *schema_json = (const char *)1;
    const json_value_t *schema = (const json_value_t *)1;
    int strict_result = 7;

    check_not_null(registry);
    legacy.struct_size = sizeof(legacy);
    legacy.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
    legacy.definition.name = "legacy_result";
    legacy.definition.description = "legacy result";
    legacy.definition.parameters_json = "{\"type\":\"object\"}";
    legacy.definition.json_value_handler = test_echo_tool_json_value;
    legacy.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
    legacy.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_NONE;
    check_equal(turbo_tool_registry_add_v4(registry, &legacy), TURBO_TOOL_OK);

    check_equal(turbo_tool_registry_get_result_contract(
                    registry, "legacy_result", &schema_json,
                    &schema, &strict_result),
                TURBO_TOOL_OK);
    check_null(schema_json);
    check_null(schema);
    check_equal(strict_result, 0);

    invalid.struct_size = sizeof(invalid);
    invalid.abi_version = TURBO_TOOL_DEFINITION_V5_ABI_VERSION;
    invalid.definition.name = "invalid_result";
    invalid.definition.description = "invalid result";
    invalid.definition.parameters_json = "{\"type\":\"object\"}";
    invalid.definition.json_value_handler = test_echo_tool_json_value;
    invalid.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
    invalid.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_NONE;
    invalid.result_schema_json = "{bad";
    check_equal(turbo_tool_registry_add_v5(registry, &invalid),
                TURBO_TOOL_INVALID_ARGUMENT);
    invalid.result_schema_json = "{\"type\":\"string\"}";
    invalid.strict_result = 2;
    check_equal(turbo_tool_registry_add_v5(registry, &invalid),
                TURBO_TOOL_INVALID_ARGUMENT);
    invalid.result_schema_json = NULL;
    invalid.strict_result = 1;
    check_equal(turbo_tool_registry_add_v5(registry, &invalid),
                TURBO_TOOL_INVALID_ARGUMENT);

    turbo_tool_registry_destroy(registry);
  }

  it("should preserve native runtime v5 result contracts through registry bridge") {
    turbo_tool_runtime_t *runtime = turbo_tool_runtime_native_create();
    turbo_tool_registry_t *bridge = NULL;
    turbo_tool_definition_v5_t definition = {0};
    turbo_tool_runtime_tool_v2_t tool = {0};
    turbo_tool_runtime_tool_v3_t effect_tool = {0};
    const char *schema_json = NULL;
    const json_value_t *schema = NULL;
    int strict_result = 0;

    check_not_null(runtime);
    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V5_ABI_VERSION;
    definition.definition.name = "native_typed";
    definition.definition.description = "native typed";
    definition.definition.parameters_json = "{\"type\":\"object\"}";
    definition.definition.json_value_handler = test_echo_tool_json_value;
    definition.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
    definition.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
    definition.result_schema_json = "{\"type\":\"string\"}";
    definition.strict_result = 1;

    check_equal(turbo_tool_runtime_native_add_tool_v5(
                    runtime, &definition),
                TURBO_TOOL_OK);
    check_equal(turbo_tool_runtime_get_tool_v2(runtime, 0, &tool),
                TURBO_TOOL_OK);
    check_equal(tool.abi_version, TURBO_TOOL_RUNTIME_TOOL_V2_ABI_VERSION);
    check_equal(tool.base.name, "native_typed");
    check_equal(tool.result_schema_json, "{\"type\":\"string\"}");
    check_equal(tool.strict_result, 1);
    check_equal(turbo_tool_runtime_get_tool_v3(runtime, 0, &effect_tool),
                TURBO_TOOL_OK);
    check_equal(effect_tool.effect_flags, TURBO_TOOL_EFFECT_UNKNOWN);

    bridge = turbo_tool_runtime_build_registry_bridge(runtime);
    check_not_null(bridge);
    check_equal(turbo_tool_registry_get_result_contract(
                    bridge, "native_typed", &schema_json,
                    &schema, &strict_result),
                TURBO_TOOL_OK);
    check_equal(schema_json, "{\"type\":\"string\"}");
    check_not_null(schema);
    check_equal(json_get_string(schema, "type"), "string");
    check_equal(strict_result, 1);

    turbo_tool_registry_destroy(bridge);
    turbo_tool_runtime_destroy(runtime);
  }


  it("should preserve canonical v6 effects through projection and composition") {
    turbo_tool_registry_t *source = turbo_tool_registry_create();
    turbo_tool_registry_t *projection = NULL;
    turbo_tool_registry_t *composite = NULL;
    const turbo_tool_registry_t *sources[1];
    const char *names[] = {"pure_read"};
    const char *required[] = {"runtime_tools", "network"};
    turbo_tool_definition_v6_t definition = {0};
    turbo_tool_effect_flags_t effects = 0;
    const char *const *caps = NULL;
    size_t cap_count = 0;

    check_not_null(source);
    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
    definition.definition.name = "pure_read";
    definition.definition.description = "effect contract";
    definition.definition.parameters_json = "{\"type\":\"object\"}";
    definition.definition.json_value_handler = test_echo_tool_json_value;
    definition.execution_policy.mode = TURBO_TOOL_EXECUTION_PARALLEL_SAFE;
    definition.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
    definition.required_capabilities = required;
    definition.required_capability_count = 2u;
    definition.result_schema_json = "{\"type\":\"object\"}";
    definition.effect_flags = TURBO_TOOL_EFFECT_PURE;

    check_equal(turbo_tool_registry_add_v6(source, &definition), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_get_effects(source, "pure_read", &effects),
                TURBO_TOOL_OK);
    check_equal(effects, TURBO_TOOL_EFFECT_PURE);
    check_equal(turbo_tool_registry_get_required_capabilities(
                    source, "pure_read", &caps, &cap_count),
                TURBO_TOOL_OK);
    check_equal(cap_count, 2u);
    check_equal(caps[1], "network");

    check_equal(turbo_tool_registry_project(source, names, 1u, &projection),
                TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_get_effects(projection, "pure_read", &effects),
                TURBO_TOOL_OK);
    check_equal(effects, TURBO_TOOL_EFFECT_PURE);

    sources[0] = projection;
    check_equal(turbo_tool_registry_compose(sources, 1u, &composite),
                TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_get_effects(composite, "pure_read", &effects),
                TURBO_TOOL_OK);
    check_equal(effects, TURBO_TOOL_EFFECT_PURE);
    check_equal(turbo_tool_registry_get_required_capabilities(
                    composite, "pure_read", &caps, &cap_count),
                TURBO_TOOL_OK);
    check_equal(cap_count, 2u);
    check_equal(caps[1], "network");

    turbo_tool_registry_destroy(composite);
    turbo_tool_registry_destroy(projection);
    turbo_tool_registry_destroy(source);
  }

  it("should keep legacy effects UNKNOWN and reject invalid effect combinations") {
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_v5_t legacy = {0};
    turbo_tool_definition_v6_t invalid = {0};
    turbo_tool_effect_flags_t effects = 0;

    check_not_null(registry);
    legacy.struct_size = sizeof(legacy);
    legacy.abi_version = TURBO_TOOL_DEFINITION_V5_ABI_VERSION;
    legacy.definition.name = "legacy_effect";
    legacy.definition.description = "legacy";
    legacy.definition.parameters_json = "{\"type\":\"object\"}";
    legacy.definition.json_value_handler = test_echo_tool_json_value;
    legacy.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
    legacy.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_NONE;
    check_equal(turbo_tool_registry_add_v5(registry, &legacy), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_get_effects(
                    registry, "legacy_effect", &effects),
                TURBO_TOOL_OK);
    check_equal(effects, TURBO_TOOL_EFFECT_UNKNOWN);

    invalid.struct_size = sizeof(invalid);
    invalid.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
    invalid.definition.name = "invalid_effect";
    invalid.definition.description = "invalid";
    invalid.definition.parameters_json = "{\"type\":\"object\"}";
    invalid.definition.json_value_handler = test_echo_tool_json_value;
    invalid.execution_policy.mode = TURBO_TOOL_EXECUTION_PARALLEL_SAFE;
    invalid.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;

    invalid.effect_flags = TURBO_TOOL_EFFECT_PURE | TURBO_TOOL_EFFECT_WRITE;
    check_equal(turbo_tool_registry_add_v6(registry, &invalid),
                TURBO_TOOL_INVALID_ARGUMENT);
    invalid.effect_flags = TURBO_TOOL_EFFECT_UNKNOWN | TURBO_TOOL_EFFECT_READ;
    check_equal(turbo_tool_registry_add_v6(registry, &invalid),
                TURBO_TOOL_INVALID_ARGUMENT);
    invalid.effect_flags = UINT64_C(1) << 63;
    check_equal(turbo_tool_registry_add_v6(registry, &invalid),
                TURBO_TOOL_INVALID_ARGUMENT);

    invalid.effect_flags = 0;
    check_equal(turbo_tool_registry_add_v6(registry, &invalid), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_get_effects(
                    registry, "invalid_effect", &effects),
                TURBO_TOOL_OK);
    check_equal(effects, TURBO_TOOL_EFFECT_UNKNOWN);

    turbo_tool_registry_destroy(registry);
  }

  it("should preserve native v6 effects through runtime catalog and registry bridge") {
    turbo_tool_runtime_t *runtime = turbo_tool_runtime_native_create();
    turbo_tool_registry_t *bridge = NULL;
    turbo_tool_definition_v6_t definition = {0};
    turbo_tool_runtime_tool_v3_t tool = {0};
    turbo_tool_effect_flags_t effects = 0;

    check_not_null(runtime);
    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
    definition.definition.name = "native_effect";
    definition.definition.description = "native effect";
    definition.definition.parameters_json = "{\"type\":\"object\"}";
    definition.definition.json_value_handler = test_echo_tool_json_value;
    definition.execution_policy.mode = TURBO_TOOL_EXECUTION_PARALLEL_SAFE;
    definition.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
    definition.result_schema_json = "{\"type\":\"object\"}";
    definition.effect_flags = TURBO_TOOL_EFFECT_READ | TURBO_TOOL_EFFECT_NETWORK;

    check_equal(turbo_tool_runtime_native_add_tool_v6(runtime, &definition),
                TURBO_TOOL_OK);
    check_equal(turbo_tool_runtime_get_tool_v3(runtime, 0u, &tool),
                TURBO_TOOL_OK);
    check_equal(tool.abi_version, TURBO_TOOL_RUNTIME_TOOL_V3_ABI_VERSION);
    check_equal(tool.base.base.name, "native_effect");
    check_equal(tool.effect_flags,
                TURBO_TOOL_EFFECT_READ | TURBO_TOOL_EFFECT_NETWORK);

    bridge = turbo_tool_runtime_build_registry_bridge(runtime);
    check_not_null(bridge);
    check_equal(turbo_tool_registry_get_effects(
                    bridge, "native_effect", &effects),
                TURBO_TOOL_OK);
    check_equal(effects,
                TURBO_TOOL_EFFECT_READ | TURBO_TOOL_EFFECT_NETWORK);

    turbo_tool_registry_destroy(bridge);
    turbo_tool_runtime_destroy(runtime);
  }

}
