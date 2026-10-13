#include "tinytest.h"
#include "turbo_agent_http_test_support.h"
#include <http_client/rpc_service.h>
#include <http_server/rpc_service.h>
#include <cmeta_cmeta_fixed_width.h>
#include "turboagent_typed_rpc.rpc.h"
#include "turboagent_rpc_provider_dso.h"
#include <salts/plugin.h>
#include <stdatomic.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { TYPED_TIMEOUT_MS = 5000 };

/* Native binding is owned independently by this external TurboAgent consumer.
 * Only published CHttp.Native / Salts.Native / SaltsUtils.Native are linked. */
typedef struct AddRequest {
  uint32_t left;
  uint32_t right;
  uint32_t scale;
  uint8_t presence;
} AddRequest;

typedef struct AddResponse {
  uint32_t sum;
} AddResponse;

static const cmeta_type_identity ADD_REQUEST_ID =
    CMETA_TYPE_ID_ATOM_INIT("test.turboagent.rpc.AddRequest");
static const cmeta_type_identity ADD_RESPONSE_ID =
    CMETA_TYPE_ID_ATOM_INIT("test.turboagent.rpc.AddResponse");

static const cmeta_type_desc ADD_REQUEST_TYPE = {
    "AddRequest", sizeof(AddRequest), _Alignof(AddRequest),
    CMETA_T_OBJECT, NULL, NULL, &ADD_REQUEST_ID};
static const cmeta_type_desc ADD_RESPONSE_TYPE = {
    "AddResponse", sizeof(AddResponse), _Alignof(AddResponse),
    CMETA_T_OBJECT, NULL, NULL, &ADD_RESPONSE_ID};

static const cmeta_type_desc ADD_REQUEST_PTR_TYPE = {
    "const AddRequest *", sizeof(AddRequest *), _Alignof(AddRequest *),
    CMETA_T_POINTER, &ADD_REQUEST_TYPE, NULL, NULL};
static const cmeta_type_desc ADD_RESPONSE_PTR_TYPE = {
    "AddResponse *", sizeof(AddResponse *), _Alignof(AddResponse *),
    CMETA_T_POINTER, &ADD_RESPONSE_TYPE, NULL, NULL};

static const cmeta_field_desc ADD_REQUEST_LAYOUT_FIELDS[] = {
    {"left", "uint32_t", offsetof(AddRequest, left), sizeof(uint32_t),
     _Alignof(uint32_t), &cmeta_type_uint32, NULL},
    {"right", "uint32_t", offsetof(AddRequest, right), sizeof(uint32_t),
     _Alignof(uint32_t), &cmeta_type_uint32, NULL},
    {"scale", "uint32_t", offsetof(AddRequest, scale), sizeof(uint32_t),
     _Alignof(uint32_t), &cmeta_type_uint32, NULL}};
static const cmeta_struct_desc ADD_REQUEST_LAYOUT = {
    "AddRequest", sizeof(AddRequest), _Alignof(AddRequest),
    ADD_REQUEST_LAYOUT_FIELDS, 3u};
static const cmeta_data_field_desc ADD_REQUEST_FIELDS[] = {
    {"test.turboagent.rpc.AddRequest.left", "left", offsetof(AddRequest, left),
     &cmeta_data_uint32},
    {"test.turboagent.rpc.AddRequest.right", "right", offsetof(AddRequest, right),
     &cmeta_data_uint32},
    {"test.turboagent.rpc.AddRequest.scale", "scale", offsetof(AddRequest, scale),
     &cmeta_data_uint32}};
static const cmeta_data_struct_shape ADD_REQUEST_SHAPE = {
    &ADD_REQUEST_LAYOUT, ADD_REQUEST_FIELDS, 3u};
static const cmeta_data_desc ADD_REQUEST_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "test.turboagent.rpc.AddRequest.data",
    .display_name = "AddRequest",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &ADD_REQUEST_TYPE,
    .shape = &ADD_REQUEST_SHAPE};

static const cmeta_field_desc ADD_RESPONSE_LAYOUT_FIELDS[] = {
    {"sum", "uint32_t", offsetof(AddResponse, sum), sizeof(uint32_t),
     _Alignof(uint32_t), &cmeta_type_uint32, NULL}};
static const cmeta_struct_desc ADD_RESPONSE_LAYOUT = {
    "AddResponse", sizeof(AddResponse), _Alignof(AddResponse),
    ADD_RESPONSE_LAYOUT_FIELDS, 1u};
static const cmeta_data_field_desc ADD_RESPONSE_FIELDS[] = {
    {"test.turboagent.rpc.AddResponse.sum", "sum", offsetof(AddResponse, sum),
     &cmeta_data_uint32}};
static const cmeta_data_struct_shape ADD_RESPONSE_SHAPE = {
    &ADD_RESPONSE_LAYOUT, ADD_RESPONSE_FIELDS, 1u};
static const cmeta_data_desc ADD_RESPONSE_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "test.turboagent.rpc.AddResponse.data",
    .display_name = "AddResponse",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &ADD_RESPONSE_TYPE,
    .shape = &ADD_RESPONSE_SHAPE};

static const DataBindNativeStateBinding ADD_REQUEST_PRESENCE[] = {
    {sizeof(DataBindNativeStateBinding), "scale",
     offsetof(AddRequest, presence), 0u}};

static const DataBindNativeTypeBinding ADD_REQUEST_NATIVE = {
    .size = sizeof(DataBindNativeTypeBinding),
    .abi_version = DATA_BIND_NATIVE_BINDING_ABI_VERSION,
    .idl_type_name = "AddRequest",
    .data = &ADD_REQUEST_DATA,
    .presence = ADD_REQUEST_PRESENCE,
    .presence_count = 1u};

static const DataBindNativeTypeBinding ADD_RESPONSE_NATIVE = {
    .size = sizeof(DataBindNativeTypeBinding),
    .abi_version = DATA_BIND_NATIVE_BINDING_ABI_VERSION,
    .idl_type_name = "AddResponse",
    .data = &ADD_RESPONSE_DATA};

FunctionDeclAsAbiResult(
    value, int, &cmeta_type_int, CMETA_ABI_SCALAR,
    CMETA_RESULT_VALUE, turboagent_typed_add,
    (const AddRequest *, request,
     CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &ADD_REQUEST_PTR_TYPE, CMETA_ABI_OBJECT_POINTER),
    (AddResponse *, response,
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED,
     &ADD_RESPONSE_PTR_TYPE, CMETA_ABI_OBJECT_POINTER));

int turboagent_typed_add(
    const AddRequest *request, AddResponse *response) {
  if (request == NULL || response == NULL) return -1;
  if (request->left == 99u) return 7;
  response->sum = request->left + request->right * request->scale;
  return 0;
}

static bool DATA_BIND_NATIVE_CALL turboagent_typed_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  int result;
  (void)context;
  if (return_storage == NULL || params == NULL || param_count != 2u ||
      params[0] == NULL || params[1] == NULL)
    return false;
  result = turboagent_typed_add(
      (const AddRequest *)params[0], (AddResponse *)params[1]);
  *(int *)return_storage = result;
  return true;
}


static chttp_client_config typed_client_config(void) {
  const chttp_server_config server = agent_http_test_server_config();
  chttp_client_config config = {0};
  config.network = server.network;
  config.network.connection_capacity = 2u;
  config.request_capacity = 2u;
  config.max_start_line_bytes = 256u;
  config.max_header_count = 16u;
  config.max_header_bytes = 4096u;
  config.max_request_body_bytes = 8192u;
  config.max_response_body_bytes = 8192u;
  config.max_informational_responses = 2u;
  return config;
}

/* The invalid-request probe must inspect the bounded HTTP response payload;
 * do not treat the parsed DOM as an owned value or assume NUL termination. */
static int turboagent_rpc_body_contains(
    const chttp_response *response, const char *needle) {
  size_t i, n;
  if (response == NULL || response->body == NULL || needle == NULL) return 0;
  n = strlen(needle);
  if (n > response->body_size) return 0;
  for (i = 0u; i <= response->body_size - n; ++i)
    if (memcmp((const unsigned char *)response->body + i, needle, n) == 0)
      return 1;
  return 0;
}

typedef struct turboagent_typed_async_probe {
  int terminals;
  int status;
  chttp_client_rpc_outcome_kind kind;
  int64_t remote_code;
} turboagent_typed_async_probe;

static void turboagent_typed_async_complete(
    void *user, chttp_request handle,
    const chttp_client_rpc_outcome *outcome, const crpc_error *error) {
  turboagent_typed_async_probe *probe =
      (turboagent_typed_async_probe *)user;
  (void)handle;
  if (probe == NULL) return;
  ++probe->terminals;
  probe->status = error == NULL ? SALTS_OK : error->status;
  probe->kind = outcome == NULL ? CHTTP_CLIENT_RPC_NONE : outcome->kind;
  probe->remote_code = outcome == NULL ? 0 : outcome->remote_code;
}

/* Worker borrows only immutable MethodPlan/Binding while the test's Server
 * retains its Plugin generation lease; native workspace is per call. */
typedef struct turboagent_typed_call_thread {
  char uri[64];
  const DataBindRpcMethodPlan *plan;
  const DataBindServiceNativeBinding *binding;
  AddRequest request;
  AddResponse response;
  int status;
  chttp_client_rpc_outcome_kind kind;
  atomic_int finished;
} turboagent_typed_call_thread;

static void turboagent_typed_call_worker(void *user) {
  turboagent_typed_call_thread *args = (turboagent_typed_call_thread *)user;
  chttp_client client = {0};
  chttp_client_config config = typed_client_config();
  chttp_client_rpc_call_options options = CHTTP_CLIENT_RPC_CALL_OPTIONS_INIT;
  chttp_client_rpc_outcome outcome = CHTTP_CLIENT_RPC_OUTCOME_INIT;
  DataBindNativeOptions native = DATA_BIND_NATIVE_OPTIONS_INIT;
  unsigned char workspace[4096];
  crpc_error error = {0};
  int status;
  if (args == NULL) return;
  status = chttp_client_init(&client, &config);
  if (status == SALTS_OK) {
    native.workspace = workspace;
    native.workspace_bytes = sizeof(workspace);
    native.max_depth = 16u;
    native.max_items = 64u;
    native.max_owned_bytes = 1024u;
    options.connection_uri = args->uri;
    options.authority = "localhost";
    options.target = "/rpc";
    options.request_id = UINT64_C(611);
    options.deadline_ms = TYPED_TIMEOUT_MS;
    options.method_plan = args->plan;
    options.native_binding = args->binding;
    options.native_options = &native;
    options.request = &args->request;
    options.request_bytes = sizeof(args->request);
    options.response = &args->response;
    options.response_bytes = sizeof(args->response);
    status = chttp_client_rpc_call(&client, &options, &outcome, &error);
    args->kind = outcome.kind;
    {
      int teardown = chttp_client_destroy(&client, TYPED_TIMEOUT_MS);
      if (status == SALTS_OK && teardown != SALTS_OK) status = teardown;
    }
  }
  args->status = status;
  atomic_store_explicit(&args->finished, 1, memory_order_release);
}

spec("TurboAgent installed DataBind IDL typed RPC Owner") {
  it("compiles generated MethodPlan, mounts into HTTP Server, executes typed Client") {
    static const char contract_source[] =
        "message AddRequest { @Min(1) uint32 left; uint32 right;"
        " optional uint32 scale default 1; }"
        "message AddResponse { @Max(100) uint32 sum; }"
        "service Calc { Add: AddRequest -> AddResponse; }";
    DataBind *contract = NULL;
    DataBindError bind_error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic =
        DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    const DataBindRpcProjectionConfig *projection;
    DataBindRpcMethodPlan *plan = NULL;
    DataBindServiceNativeBinding binding =
        DATA_BIND_SERVICE_NATIVE_BINDING_INIT(
            FunctionMeta(turboagent_typed_add),
            &ADD_REQUEST_NATIVE, &ADD_RESPONSE_NATIVE);
    DataBindNativeExecution execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    chttp_server server = {0};
    chttp_server_config server_config = agent_http_test_server_config();
    chttp_server_rpc_config rpc_config = CHTTP_SERVER_RPC_CONFIG_INIT;
    chttp_server_rpc_mount_options mount = CHTTP_SERVER_RPC_MOUNT_OPTIONS_INIT;
    chttp_client_config client_config = typed_client_config();
    chttp_client client = {0};
    chttp_async_client async_client = {0};
    turboagent_typed_async_probe typed_async_probe = {0};
    chttp_request async_handle = {0};
    uint64_t async_deadline = 0u;
    chttp_client_rpc_call_options options = CHTTP_CLIENT_RPC_CALL_OPTIONS_INIT;
    chttp_client_rpc_outcome outcome = CHTTP_CLIENT_RPC_OUTCOME_INIT;
    DataBindNativeOptions native = DATA_BIND_NATIVE_OPTIONS_INIT;
    unsigned char workspace[4096];
    AddRequest request = {3u, 4u, 2u, 1u};
    AddResponse response = {0};
    crpc_error error = {0};
    char uri[64];
    uint16_t port = 0u;

    projection = data_bind_rpc_projection_artifact_find(
        &databind_turboagent_typed_rpc_rpc_projection, "Calc", "Add");
    check_not_null(projection);
    check_equal(projection->wire_method, "calc.add");
    check_null(data_bind_rpc_projection_artifact_find(
        &databind_turboagent_typed_rpc_rpc_projection, "Calc", "Missing"));
    check_equal(data_bind_create_from_text(
        contract_source, sizeof(contract_source) - 1u, &contract,
        &bind_error), DATA_BIND_OK);
    check_equal(data_bind_rpc_method_plan_compile_service(
        contract, "Calc", "Add", projection, &binding, &plan, &diagnostic),
        DATA_BIND_OK);
    check_not_null(plan);
    check_equal(data_bind_rpc_method_plan_wire_method(plan), "calc.add");

    execution.function = FunctionMeta(turboagent_typed_add);
    execution.abi = FunctionAbi(turboagent_typed_add);
    execution.invoke = turboagent_typed_invoke;
    rpc_config.method_capacity = 2u;
    rpc_config.max_inflight_calls = 2u;
    rpc_config.native_max_items = 64u;
    rpc_config.native_max_owned_bytes = 1024u;
    mount.target = "/rpc";
    mount.method_plan = plan;
    mount.native_binding = &binding;
    mount.execution = &execution;
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_rpc_configure(&server, &rpc_config), SALTS_OK);
    check_equal(chttp_server_rpc_mount(&server, &mount), SALTS_OK);
    check_equal(chttp_server_rpc_mount(&server, &mount), SALTS_EALREADY);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                        (unsigned int)port) > 0);

    native.workspace = workspace;
    native.workspace_bytes = sizeof(workspace);
    native.max_depth = 16u;
    native.max_items = 64u;
    native.max_owned_bytes = 1024u;
    options.connection_uri = uri;
    options.authority = "localhost";
    options.target = "/rpc";
    options.deadline_ms = TYPED_TIMEOUT_MS;
    options.method_plan = plan;
    options.native_binding = &binding;
    options.native_options = &native;
    options.request = &request;
    options.request_bytes = sizeof(request);
    options.response = &response;
    options.response_bytes = sizeof(response);

    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);
    options.request_id = UINT64_C(501);
    check_equal(chttp_client_rpc_call(&client, &options, &outcome, &error), SALTS_OK);
    check_equal(outcome.kind, CHTTP_CLIENT_RPC_SUCCESS);
    check_equal(response.sum, (uint32_t)11u);

    request = (AddRequest){2u, 5u, 3u, 1u};
    response = (AddResponse){0};
    outcome = (chttp_client_rpc_outcome)CHTTP_CLIENT_RPC_OUTCOME_INIT;
    options.request_id = UINT64_C(502);
    check_equal(chttp_client_rpc_call(&client, &options, &outcome, &error), SALTS_OK);
    check_equal(outcome.kind, CHTTP_CLIENT_RPC_SUCCESS);
    check_equal(response.sum, (uint32_t)17u);

    request = (AddRequest){99u, 1u, 1u, 1u};
    response = (AddResponse){0};
    outcome = (chttp_client_rpc_outcome)CHTTP_CLIENT_RPC_OUTCOME_INIT;
    options.request_id = UINT64_C(503);
    check_equal(chttp_client_rpc_call(&client, &options, &outcome, &error), SALTS_OK);
    check_equal(outcome.kind, CHTTP_CLIENT_RPC_REMOTE_ERROR);
    check_equal(outcome.remote_code, INT64_C(-32000));
    check_equal(chttp_client_destroy(&client, TYPED_TIMEOUT_MS), SALTS_OK);

    /* The same immutable generated MethodPlan must work on the existing
     * asynchronous HTTP Owner; caller borrows response storage until the
     * sole terminal callback, with no duplicate wire ID publication. */
    request = (AddRequest){3u, 4u, 2u, 1u};
    response = (AddResponse){0};
    options.request_id = UINT64_C(504);
    check_equal(chttp_async_client_init(&async_client, &client_config), SALTS_OK);
    check_equal(chttp_async_client_rpc_call(
        &async_client, &options, turboagent_typed_async_complete,
        &typed_async_probe, &async_handle), SALTS_OK);
    check_true(async_handle.slot != 0u && async_handle.generation != 0u);
    {
      AddResponse rejected = {.sum = UINT32_C(0xdecafbad)};
      chttp_client_rpc_call_options duplicate = options;
      chttp_request denied = {0};
      duplicate.response = &rejected;
      check_equal(chttp_async_client_rpc_call(
          &async_client, &duplicate, turboagent_typed_async_complete,
          &typed_async_probe, &denied), SALTS_EALREADY);
      check_equal(denied.slot, 0u);
      check_equal(rejected.sum, UINT32_C(0xdecafbad));
      check_equal(typed_async_probe.terminals, 0);
    }
    async_deadline = cmeta_monotonic_ms() + TYPED_TIMEOUT_MS;
    while (typed_async_probe.terminals == 0 &&
           cmeta_monotonic_ms() < async_deadline) {
      size_t completions = 0u;
      check_equal(chttp_async_client_poll(&async_client, 5u, &completions), SALTS_OK);
    }
    check_equal(typed_async_probe.terminals, 1);
    check_equal(typed_async_probe.status, SALTS_OK);
    check_equal(typed_async_probe.kind, CHTTP_CLIENT_RPC_SUCCESS);
    check_equal(response.sum, (uint32_t)11u);
    check_equal(chttp_async_request_cancel(&async_client, async_handle), SALTS_ENOENT);
    check_equal(chttp_async_client_stop(&async_client, TYPED_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&async_client), SALTS_OK);
    check_equal(typed_async_probe.terminals, 1);

    check_equal(chttp_server_stop(&server, TYPED_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    data_bind_rpc_method_plan_free(plan);
    data_bind_free(contract);
  }

  it("pins a real Plugin DSO Generation from typed mount through Server destruction") {
    static const char schema[] =
        "message AddRequest { @Min(1) uint32 left; uint32 right;"
        " optional uint32 scale default 1; }"
        "message AddResponse { @Max(100) uint32 sum; }"
        "service Calc { Add: AddRequest -> AddResponse; }";
    DataBind *contract = NULL;
    DataBindError parse_error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostics =
        DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    DataBindRpcMethodPlan *plan = NULL;
    const DataBindRpcProjectionConfig *projection = NULL;
    DataBindServiceNativeBinding binding =
        DATA_BIND_SERVICE_NATIVE_BINDING_INIT(
            FunctionMeta(turboagent_typed_add),
            &ADD_REQUEST_NATIVE, &ADD_RESPONSE_NATIVE);
    DataBindNativeExecution execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref ref = {0};
    cmeta_plugin_lease discovery = {0};
    cmeta_plugin_lifecycle_info lifecycle = {0};
    const cmeta_plugin_registry_config registry_config = {.capacity = 1u};
    const cmeta_plugin_manifest *manifest = NULL;
    const cmeta_plugin_export *entry = NULL;
    const turboagent_rpc_provider_dso_offer *offer = NULL;
    turboagent_rpc_provider_dso_probe probe = {0};
    bool quiescent = false;
    chttp_server server = {0};
    chttp_server_config server_config = agent_http_test_server_config();
    chttp_server_rpc_config rpc_config = CHTTP_SERVER_RPC_CONFIG_INIT;
    chttp_server_rpc_mount_options mount = CHTTP_SERVER_RPC_MOUNT_OPTIONS_INIT;
    chttp_client client = {0};
    chttp_client_config client_config = typed_client_config();
    chttp_client_rpc_call_options call = CHTTP_CLIENT_RPC_CALL_OPTIONS_INIT;
    chttp_client_rpc_outcome outcome = CHTTP_CLIENT_RPC_OUTCOME_INIT;
    DataBindNativeOptions native = DATA_BIND_NATIVE_OPTIONS_INIT;
    unsigned char workspace[4096];
    AddRequest request = {3u, 4u, 2u, 1u};
    AddResponse response = {0};
    crpc_error call_error = {0};
    uint16_t port = 0u;
    char uri[64];
    turboagent_typed_call_thread pending = {0};
    cmeta_thread_t blocked = {0};
    uint64_t deadline = 0u;
    int entered = 0;
    static const char bad_partial[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"calc.add\",\"params\":[3,\"not-uint\",2],\"id\":600}";
    static const chttp_header json_headers[] = {
        {"Content-Type", "application/json"}, {"Accept", "application/json"}};
    chttp_response invalid_response = {0};
    chttp_error invalid_error = {0};
    chttp_options invalid_post = {0};

    projection = data_bind_rpc_projection_artifact_find(
        &databind_turboagent_typed_rpc_rpc_projection, "Calc", "Add");
    check_not_null(projection);
    check_equal(data_bind_create_from_text(
        schema, sizeof(schema)-1u, &contract, &parse_error), DATA_BIND_OK);
    check_equal(data_bind_rpc_method_plan_compile_service(
        contract, "Calc", "Add", projection, &binding, &plan, &diagnostics), DATA_BIND_OK);

    /* Address of the native business callback is *only* acquired by the
     * installed Salts Plugin registry from a genuinely loaded shared library. */
    check_equal(cmeta_plugin_registry_init(&registry, &registry_config), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_load(
        &registry, TURBOAGENT_RPC_PROVIDER_DSO_PATH, &ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_start(&registry, ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_acquire(
        &registry, ref, &discovery, &manifest), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_manifest_find_export(
        manifest, TURBOAGENT_RPC_PROVIDER_DSO_EXPORT, &entry), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_export_require_function(
        entry, TURBOAGENT_RPC_PROVIDER_DSO_CONTRACT,
        TURBOAGENT_RPC_PROVIDER_DSO_VERSION, 0u), CMETA_PLUGIN_OK);
    check_true(cmeta_function_abi_contract_compatible(
        FunctionAbi(turboagent_rpc_provider_dso_get_offer),
        entry->value.function.abi));
    check_true(entry->value.function.invoke(
        entry->value.function.context, &offer, NULL, 0u));
    check_not_null(offer);
    check_true(offer->invoke != NULL);
    atomic_init(&probe.enabled, 0);
    atomic_init(&probe.entered, 0);
    atomic_init(&probe.release, 0);
    atomic_init(&probe.invoked, 0);
    execution.function = FunctionMeta(turboagent_typed_add);
    execution.abi = FunctionAbi(turboagent_typed_add);
    execution.invoke = offer->invoke;
    execution.context = &probe;

    rpc_config.method_capacity = 2u;
    rpc_config.max_inflight_calls = 2u;
    rpc_config.native_max_items = 64u;
    rpc_config.native_max_owned_bytes = 1024u;
    mount.target = "/rpc";
    mount.method_plan = plan;
    mount.native_binding = &binding;
    mount.execution = &execution;
    mount.plugin_registry = &registry;
    mount.plugin_ref = ref;
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_rpc_configure(&server, &rpc_config), SALTS_OK);
    check_equal(chttp_server_rpc_mount(&server, &mount), SALTS_OK);
    check_equal(cmeta_plugin_registry_release(
        &registry, &discovery), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, ref, &lifecycle), CMETA_PLUGIN_OK);
    check_equal(lifecycle.active_leases, (size_t)1u);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                        (unsigned int)port) > 0);

    native.workspace = workspace;
    native.workspace_bytes = sizeof(workspace);
    native.max_depth = 16u;
    native.max_items = 64u;
    native.max_owned_bytes = 1024u;
    call.connection_uri = uri;
    call.authority = "localhost";
    call.target = "/rpc";
    call.deadline_ms = TYPED_TIMEOUT_MS;
    call.method_plan = plan;
    call.native_binding = &binding;
    call.native_options = &native;
    call.request = &request;
    call.request_bytes = sizeof(request);
    call.response = &response;
    call.response_bytes = sizeof(response);
    call.request_id = UINT64_C(601);
    check_equal(chttp_client_init(&client, &client_config), SALTS_OK);
    /* Partial native ingress: "left" decoded before invalid "right".
     * The Provider must NOT execute, and the bounded slot must be reusable. */
    invalid_post.connection_uri = uri;
    invalid_post.authority = "localhost";
    invalid_post.target = "/rpc";
    invalid_post.headers = json_headers;
    invalid_post.header_count = 2u;
    invalid_post.body = bad_partial;
    invalid_post.body_size = sizeof(bad_partial) - 1u;
    invalid_post.timeout_ms = TYPED_TIMEOUT_MS;
    check_equal(chttp_post(&client, &invalid_post, &invalid_response, &invalid_error),
                SALTS_OK);
    check_equal(invalid_response.status_code, 200u);
    check_true(turboagent_rpc_body_contains(&invalid_response, "\"code\":-32602"));
    check_equal(atomic_load_explicit(&probe.invoked, memory_order_acquire), 0);
    chttp_response_destroy(&invalid_response);
    check_equal(chttp_client_rpc_call(&client, &call, &outcome, &call_error), SALTS_OK);
    check_equal(outcome.kind, CHTTP_CLIENT_RPC_SUCCESS);
    check_equal(response.sum, (uint32_t)11u);
    check_equal(atomic_load_explicit(
        &probe.invoked, memory_order_acquire), 1);
    check_equal(chttp_client_destroy(&client, TYPED_TIMEOUT_MS), SALTS_OK);

    /* A *typed asynchronous* RPC enters the actual dynamically loaded
     * Provider before its generation-safe CNet request is cancelled. The
     * Provider is held at a deterministic barrier, not simulated in-host. */
    {
      chttp_async_client async_client = {0};
      chttp_client_config async_config = typed_client_config();
      chttp_client_rpc_call_options async_options =
          CHTTP_CLIENT_RPC_CALL_OPTIONS_INIT;
      DataBindNativeOptions async_native = DATA_BIND_NATIVE_OPTIONS_INIT;
      unsigned char async_workspace[4096];
      AddRequest async_request = {3u, 4u, 2u, 1u};
      AddResponse async_result = {.sum = UINT32_C(0xdecafbad)};
      turboagent_typed_async_probe async_probe = {0};
      chttp_request async_handle = {0};
      uint64_t entered_deadline;
      uint64_t terminal_deadline;
      int async_entered = 0;
      int poll_status = SALTS_OK;
      int cancel_admission = SALTS_EINVAL;
      int duplicate_cancel = SALTS_EINVAL;
      int unload_while_executing = 0;

      async_native.workspace = async_workspace;
      async_native.workspace_bytes = sizeof(async_workspace);
      async_native.max_depth = 16u;
      async_native.max_items = 64u;
      async_native.max_owned_bytes = 1024u;
      async_options.connection_uri = uri;
      async_options.authority = "localhost";
      async_options.target = "/rpc";
      async_options.request_id = UINT64_C(609);
      async_options.deadline_ms = TYPED_TIMEOUT_MS;
      async_options.method_plan = plan;
      async_options.native_binding = &binding;
      async_options.native_options = &async_native;
      async_options.request = &async_request;
      async_options.request_bytes = sizeof(async_request);
      async_options.response = &async_result;
      async_options.response_bytes = sizeof(async_result);

      atomic_store_explicit(&probe.entered, 0, memory_order_release);
      atomic_store_explicit(&probe.release, 0, memory_order_release);
      atomic_store_explicit(&probe.enabled, 1, memory_order_release);
      check_equal(chttp_async_client_init(&async_client, &async_config), SALTS_OK);
      check_equal(chttp_async_client_rpc_call(
          &async_client, &async_options, turboagent_typed_async_complete,
          &async_probe, &async_handle), SALTS_OK);
      check_true(async_handle.slot != 0u && async_handle.generation != 0u);

      entered_deadline = cmeta_monotonic_ms() + 2000u;
      while (atomic_load_explicit(&probe.entered, memory_order_acquire) == 0 &&
             cmeta_monotonic_ms() < entered_deadline) {
        size_t completions = 0u;
        poll_status = chttp_async_client_poll(&async_client, 5u, &completions);
        if (poll_status != SALTS_OK) break;
      }
      async_entered = atomic_load_explicit(
          &probe.entered, memory_order_acquire);

      /* Don't leave an executing DSO blocked if a precondition fails; record
       * each return value and publish release before checking assertions. */
      if (async_entered != 0 && poll_status == SALTS_OK) {
        unload_while_executing =
            cmeta_plugin_registry_unload(&registry, ref);
        cancel_admission =
            chttp_async_request_cancel(&async_client, async_handle);
        duplicate_cancel =
            chttp_async_request_cancel(&async_client, async_handle);
      }
      atomic_store_explicit(&probe.release, 1, memory_order_release);
      atomic_store_explicit(&probe.enabled, 0, memory_order_release);
      check_equal(poll_status, SALTS_OK);
      check_equal(async_entered, 1);
      check_equal(unload_while_executing, CMETA_PLUGIN_BUSY);
      check_equal(cancel_admission, SALTS_OK);
      check_equal(duplicate_cancel, SALTS_EALREADY);

      terminal_deadline = cmeta_monotonic_ms() + TYPED_TIMEOUT_MS;
      while (async_probe.terminals == 0 &&
             cmeta_monotonic_ms() < terminal_deadline) {
        size_t completions = 0u;
        poll_status = chttp_async_client_poll(&async_client, 5u, &completions);
        if (poll_status != SALTS_OK) break;
      }
      check_equal(poll_status, SALTS_OK);
      check_equal(async_probe.terminals, 1);
      check_equal(async_probe.status, SALTS_ECANCELED);
      check_equal(async_result.sum, UINT32_C(0xdecafbad));
      check_equal(chttp_async_request_cancel(
          &async_client, async_handle), SALTS_ENOENT);
      {
        size_t completions = 0u;
        check_equal(chttp_async_client_poll(
            &async_client, 0u, &completions), SALTS_OK);
      }
      check_equal(async_probe.terminals, 1);
      check_equal(chttp_async_client_stop(
          &async_client, TYPED_TIMEOUT_MS), SALTS_OK);
      check_equal(chttp_async_client_destroy(&async_client), SALTS_OK);
      check_equal(async_probe.terminals, 1);
      check_equal(cmeta_plugin_registry_get_lifecycle(
          &registry, ref, &lifecycle), CMETA_PLUGIN_OK);
      check_equal(lifecycle.active_leases, (size_t)1u);
      check_equal(cmeta_plugin_registry_unload(
          &registry, ref), CMETA_PLUGIN_BUSY);
    }

    /* Deterministic concurrent Provider entry vs Server stop/unload:
     * the native invoke function physically resides inside this DSO, and the
     * test cannot release the old generation before all borrowed calls end. */
    pending.plan = plan;
    pending.binding = &binding;
    pending.request = (AddRequest){2u, 5u, 3u, 1u};
    check_true(snprintf(pending.uri, sizeof(pending.uri), "%s", uri) > 0);
    atomic_init(&pending.finished, 0);
    atomic_store_explicit(&probe.entered, 0, memory_order_release);
    atomic_store_explicit(&probe.release, 0, memory_order_release);
    atomic_store_explicit(&probe.enabled, 1, memory_order_release);
    check_equal(cmeta_thread_create(
        &blocked, turboagent_typed_call_worker, &pending), SALTS_OK);
    deadline = cmeta_monotonic_ms() + 2500u;
    while (atomic_load_explicit(&probe.entered, memory_order_acquire) == 0 &&
           cmeta_monotonic_ms() < deadline)
      cmeta_thread_yield();
    entered = atomic_load_explicit(&probe.entered, memory_order_acquire);
    if (entered != 0) {
      check_equal(cmeta_plugin_registry_request_stop(
          &registry, ref), CMETA_PLUGIN_OK);
      check_equal(cmeta_plugin_registry_unload(
          &registry, ref), CMETA_PLUGIN_BUSY);
      check_equal(chttp_server_stop(&server, 10u), SALTS_ETIMEDOUT);
      check_equal(chttp_server_destroy(&server), SALTS_EBUSY);
      check_equal(cmeta_plugin_registry_get_lifecycle(
          &registry, ref, &lifecycle), CMETA_PLUGIN_OK);
      check_equal(lifecycle.active_leases, (size_t)1u);
    }
    atomic_store_explicit(&probe.release, 1, memory_order_release);
    check_equal(cmeta_thread_join(&blocked), SALTS_OK);
    cmeta_thread_destroy(&blocked);
    check_equal(entered, 1);
    check_equal(atomic_load_explicit(&pending.finished, memory_order_acquire), 1);
    check_equal(pending.status, SALTS_OK);
    check_equal(pending.kind, CHTTP_CLIENT_RPC_SUCCESS);
    check_equal(pending.response.sum, (uint32_t)17u);

    /* Stop is not permission to unload the Provider callback while the
     * Server still retains its Generation pin. */
    check_equal(cmeta_plugin_registry_unload(
        &registry, ref), CMETA_PLUGIN_BUSY);
    check_equal(chttp_server_stop(&server, TYPED_TIMEOUT_MS), SALTS_OK);
    check_equal(cmeta_plugin_registry_unload(
        &registry, ref), CMETA_PLUGIN_BUSY);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, ref, &quiescent), CMETA_PLUGIN_OK);
    check_true(quiescent);
    check_equal(cmeta_plugin_registry_unload(
        &registry, ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
    data_bind_rpc_method_plan_free(plan);
    data_bind_free(contract);
  }
  it("holds a real Provider Generation through typed cancel, Server stop and DSO unload") {
    static const char schema[] =
        "message AddRequest { @Min(1) uint32 left; uint32 right;"
        " optional uint32 scale default 1; }"
        "message AddResponse { @Max(100) uint32 sum; }"
        "service Calc { Add: AddRequest -> AddResponse; }";
    DataBind *contract = NULL;
    DataBindError parse_error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic =
        DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    DataBindRpcMethodPlan *plan = NULL;
    const DataBindRpcProjectionConfig *projection;
    DataBindServiceNativeBinding binding =
        DATA_BIND_SERVICE_NATIVE_BINDING_INIT(
            FunctionMeta(turboagent_typed_add),
            &ADD_REQUEST_NATIVE, &ADD_RESPONSE_NATIVE);
    DataBindNativeExecution execution =
        (DataBindNativeExecution)DATA_BIND_NATIVE_EXECUTION_INIT;
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref ref = {0};
    cmeta_plugin_lease discovery = {0};
    cmeta_plugin_lifecycle_info lifecycle = {0};
    const cmeta_plugin_registry_config registry_config = {.capacity = 1u};
    const cmeta_plugin_manifest *manifest = NULL;
    const cmeta_plugin_export *entry = NULL;
    const turboagent_rpc_provider_dso_offer *offer = NULL;
    turboagent_rpc_provider_dso_probe provider = {0};
    chttp_server server = {0};
    chttp_server_config server_config = agent_http_test_server_config();
    chttp_server_rpc_config server_rpc = CHTTP_SERVER_RPC_CONFIG_INIT;
    chttp_server_rpc_mount_options mount = CHTTP_SERVER_RPC_MOUNT_OPTIONS_INIT;
    chttp_async_client client = {0};
    chttp_client_config client_config = typed_client_config();
    chttp_client_rpc_call_options call = CHTTP_CLIENT_RPC_CALL_OPTIONS_INIT;
    DataBindNativeOptions native = DATA_BIND_NATIVE_OPTIONS_INIT;
    unsigned char workspace[4096];
    AddRequest request = {3u, 4u, 2u, 1u};
    AddResponse response = {.sum = UINT32_C(0xdecafbad)};
    turboagent_typed_async_probe completed = {0};
    chttp_request handle = {0};
    uint16_t port = 0u;
    char uri[64];
    uint64_t deadline;
    int entered = 0;
    int poll_status = SALTS_OK;
    int cancel_status = SALTS_EINVAL;
    int duplicate_status = SALTS_EINVAL;
    cmeta_plugin_status stop_status = CMETA_PLUGIN_BUSY;
    cmeta_plugin_status unload_status = CMETA_PLUGIN_OK;
    cmeta_plugin_status lifecycle_status = CMETA_PLUGIN_BUSY;
    int short_stop = SALTS_OK;
    int early_destroy = SALTS_OK;
    size_t leases_during_stop = 0u;
    bool quiescent = false;

    projection = data_bind_rpc_projection_artifact_find(
        &databind_turboagent_typed_rpc_rpc_projection, "Calc", "Add");
    check_not_null(projection);
    check_equal(data_bind_create_from_text(
        schema, sizeof(schema)-1u, &contract, &parse_error), DATA_BIND_OK);
    check_equal(data_bind_rpc_method_plan_compile_service(
        contract, "Calc", "Add", projection, &binding, &plan, &diagnostic),
        DATA_BIND_OK);
    check_not_null(plan);

    /* An independent second plugin registry owns a new real dynamic DSO
     * generation. No callback address is linked into the host executable. */
    check_equal(cmeta_plugin_registry_init(&registry, &registry_config),
                CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_load(
        &registry, TURBOAGENT_RPC_PROVIDER_DSO_PATH, &ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_start(&registry, ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_acquire(
        &registry, ref, &discovery, &manifest), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_manifest_find_export(
        manifest, TURBOAGENT_RPC_PROVIDER_DSO_EXPORT, &entry), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_export_require_function(
        entry, TURBOAGENT_RPC_PROVIDER_DSO_CONTRACT,
        TURBOAGENT_RPC_PROVIDER_DSO_VERSION, 0u), CMETA_PLUGIN_OK);
    check_true(cmeta_function_abi_contract_compatible(
        FunctionAbi(turboagent_rpc_provider_dso_get_offer),
        entry->value.function.abi));
    check_true(entry->value.function.invoke(
        entry->value.function.context, &offer, NULL, 0u));
    check_not_null(offer);
    check_true(offer->invoke != NULL);
    atomic_init(&provider.enabled, 1);
    atomic_init(&provider.entered, 0);
    atomic_init(&provider.release, 0);
    atomic_init(&provider.invoked, 0);
    execution.function = FunctionMeta(turboagent_typed_add);
    execution.abi = FunctionAbi(turboagent_typed_add);
    execution.invoke = offer->invoke;
    execution.context = &provider;

    server_rpc.method_capacity = 2u;
    server_rpc.max_inflight_calls = 2u;
    server_rpc.native_max_items = 64u;
    server_rpc.native_max_owned_bytes = 1024u;
    mount.target = "/rpc";
    mount.method_plan = plan;
    mount.native_binding = &binding;
    mount.execution = &execution;
    mount.plugin_registry = &registry;
    mount.plugin_ref = ref;
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_rpc_configure(&server, &server_rpc), SALTS_OK);
    check_equal(chttp_server_rpc_mount(&server, &mount), SALTS_OK);
    check_equal(cmeta_plugin_registry_release(
        &registry, &discovery), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, ref, &lifecycle), CMETA_PLUGIN_OK);
    check_equal(lifecycle.active_leases, (size_t)1u);
    check_equal(chttp_server_start(&server), SALTS_OK);
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    check_true(snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u",
                        (unsigned int)port) > 0);

    native.workspace = workspace;
    native.workspace_bytes = sizeof(workspace);
    native.max_depth = 16u;
    native.max_items = 64u;
    native.max_owned_bytes = 1024u;
    call.connection_uri = uri;
    call.authority = "localhost";
    call.target = "/rpc";
    call.request_id = UINT64_C(701);
    call.deadline_ms = TYPED_TIMEOUT_MS;
    call.method_plan = plan;
    call.native_binding = &binding;
    call.native_options = &native;
    call.request = &request;
    call.request_bytes = sizeof(request);
    call.response = &response;
    call.response_bytes = sizeof(response);
    check_equal(chttp_async_client_init(&client, &client_config), SALTS_OK);
    check_equal(chttp_async_client_rpc_call(
        &client, &call, turboagent_typed_async_complete,
        &completed, &handle), SALTS_OK);
    check_true(handle.slot != 0u && handle.generation != 0u);

    /* The Provider's actual *dynamic library* code has entered and is
     * blocked before cancellation or Server stop is admitted. */
    deadline = cmeta_monotonic_ms() + 2000u;
    while (atomic_load_explicit(&provider.entered, memory_order_acquire) == 0 &&
           cmeta_monotonic_ms() < deadline) {
      size_t seen = 0u;
      poll_status = chttp_async_client_poll(&client, 5u, &seen);
      if (poll_status != SALTS_OK) break;
    }
    entered = atomic_load_explicit(&provider.entered, memory_order_acquire);
    if (entered != 0 && poll_status == SALTS_OK) {
      /* Cancellation is admitted on this active typed RPC; Server stop and
       * unload interleave with the same still-running Provider invocation. */
      cancel_status = chttp_async_request_cancel(&client, handle);
      duplicate_status = chttp_async_request_cancel(&client, handle);
      stop_status = cmeta_plugin_registry_request_stop(&registry, ref);
      unload_status = cmeta_plugin_registry_unload(&registry, ref);
      short_stop = chttp_server_stop(&server, 10u);
      early_destroy = chttp_server_destroy(&server);
      lifecycle_status = cmeta_plugin_registry_get_lifecycle(
          &registry, ref, &lifecycle);
      if (lifecycle_status == CMETA_PLUGIN_OK)
        leases_during_stop = lifecycle.active_leases;
    }
    /* Always unblock the DSO before evaluating test assertions, including
     * the negative-path preconditions: never strand a running callback. */
    atomic_store_explicit(&provider.release, 1, memory_order_release);
    check_equal(poll_status, SALTS_OK);
    check_equal(entered, 1);
    check_equal(cancel_status, SALTS_OK);
    check_equal(duplicate_status, SALTS_EALREADY);
    check_equal(stop_status, CMETA_PLUGIN_OK);
    check_equal(unload_status, CMETA_PLUGIN_BUSY);
    check_equal(short_stop, SALTS_ETIMEDOUT);
    check_equal(early_destroy, SALTS_EBUSY);
    check_equal(lifecycle_status, CMETA_PLUGIN_OK);
    check_equal(leases_during_stop, (size_t)1u);

    deadline = cmeta_monotonic_ms() + TYPED_TIMEOUT_MS;
    while (completed.terminals == 0 && cmeta_monotonic_ms() < deadline) {
      size_t seen = 0u;
      poll_status = chttp_async_client_poll(&client, 5u, &seen);
      if (poll_status != SALTS_OK) break;
    }
    check_equal(poll_status, SALTS_OK);
    check_equal(completed.terminals, 1);
    check_equal(completed.status, SALTS_ECANCELED);
    check_equal(response.sum, UINT32_C(0xdecafbad));
    check_equal(chttp_async_request_cancel(&client, handle), SALTS_ENOENT);
    {
      size_t seen = 0u;
      check_equal(chttp_async_client_poll(&client, 0u, &seen), SALTS_OK);
    }
    check_equal(completed.terminals, 1);
    check_equal(chttp_async_client_stop(&client, TYPED_TIMEOUT_MS), SALTS_OK);
    check_equal(chttp_async_client_destroy(&client), SALTS_OK);

    /* The old Provider still belongs to Server until final drain/destroy.
     * Only the final quiescent unload may call native dlclose/FreeLibrary. */
    check_equal(chttp_server_stop(&server, TYPED_TIMEOUT_MS), SALTS_OK);
    check_equal(cmeta_plugin_registry_unload(&registry, ref), CMETA_PLUGIN_BUSY);
    check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, ref, &quiescent), CMETA_PLUGIN_OK);
    check_true(quiescent);
    check_equal(cmeta_plugin_registry_unload(&registry, ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
    check_equal(completed.terminals, 1);
    data_bind_rpc_method_plan_free(plan);
    data_bind_free(contract);
  }

}
