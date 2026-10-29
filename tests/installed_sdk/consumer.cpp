#include <turbo_agent.h>
#include <turbo_agent_app.h>
#include <turbo_agent_job.h>
#include <turbo_agent_job_cancel.h>
#include <turbo_agent_job_execution.h>
#include <turbo_agent_job_observation.h>
#include <turbo_agent_job_admission.h>
#include <turbo_agent_job_queue_receipt.h>
#include <turbo_agent_job_acceptance.h>
#include <turbo_agent_job_delivery.h>
#include <turbo_agent_job_verifier.h>
#include <turbo_agent_job_file_verifier.h>
#include <turbo_agent_policy.h>
#include <turbo_agent_state.h>
#include <turbo_agent_harness.h>
#include <turbo_agent_harness_server.h>
#include <turbo_agent_harness_transport.h>
#include <turbo_agent_runtime_remote.h>
#include <turbo_agent_runtime_remote_client.h>
#include <turbo_agent_runtime_remote_chttp.h>
#include <turbo_graph.h>
#include <turbo_state_graph.h>
#include <tinytest.hpp>
#include <string>

static int reject_model_transport(const char *, char **response, void *) {
  if (response) *response = nullptr;
  return -1;
}

spec("installed C++17 SDK consumer") {
  it("constructs and releases exported runtime and Agent objects") {
    auto *graph = turbo_graph_create("installed-cpp17");
    auto *state = turbo_agent_state_create_json_value();
    turbo_agent_config_t config{};
    config.model = "installed-sdk-no-network";
    config.transport_fn = reject_model_transport;
    auto *agent = turbo_agent_create(&config);
    check_not_null(graph);
    check_not_null(state);
    check_not_null(agent);

    turbo_agent_tool_authority_snapshot_t before{};
    turbo_agent_tool_authority_snapshot_t after{};
    check_equal(turbo_agent_tool_authority_snapshot_get(agent, &before), SALTS_OK);
    check_equal(before.capability_mask, 0u);
    auto policy = turbo_agent_policy_default();
    policy.allow_custom_tools = 1;
    // No execution is active: policy replacement remains quiescent.
    check_equal(turbo_agent_set_tool_policy(agent, &policy), SALTS_OK);
    check_equal(turbo_agent_tool_authority_snapshot_is_current(agent, &before), 0);
    check_equal(turbo_agent_tool_authority_snapshot_get(agent, &after), SALTS_OK);
    check_equal(turbo_agent_tool_authority_snapshot_is_current(agent, &after), 1);
    check_true((after.capability_mask & TURBO_AGENT_TOOL_AUTHORITY_CAPABILITY_BIT(
        TURBO_AGENT_POLICY_CAPABILITY_CUSTOM_TOOLS)) != 0);

    turbo_agent_destroy(agent);
    json_free(state);
    turbo_graph_destroy(graph);
  }

  it("uses explicit-store construction through the installed C ABI") {
    turbo_agent_session_config_t config{};
    turbo_agent_session_t *session = nullptr;
    check_equal(turbo_agent_session_create_with_store(&config, &session), SALTS_EINVAL);
    check_null(session);
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    turbo_agent_app_config_t app_config{&config};
    turbo_agent_app_t *app = nullptr;
    check_equal(turbo_agent_app_create_with_store(&app_config, &app), SALTS_OK);
    check_not_null(app);
    turbo_agent_app_destroy(app);
    cmeta_threadpool_config_t pool_config{};
    pool_config.num_threads = 1;
    pool_config.queue_capacity = 1;
    auto *pool = cmeta_threadpool_create_with_config(&pool_config);
    check_not_null(pool);
    turbo_agent_harness_config_t harness_config{};
    turbo_agent_harness_config_init(&harness_config);
    harness_config.app_config = &app_config;
    harness_config.executor = pool;
    turbo_agent_harness_t *harness = nullptr;
    config.runtime_store = {};
    check_equal(turbo_agent_harness_create_with_store(&harness_config, &harness), SALTS_EINVAL);
    check_equal(turbo_agent_harness_create_verified_with_store(&harness_config, nullptr, &harness), SALTS_EINVAL);
    turbo_agent_job_verifier_t verifier{};
    turbo_agent_job_verifier_init(&verifier);
    check_equal(verifier.abi_version, TURBO_AGENT_JOB_VERIFIER_ABI_VERSION);
    check_equal(turbo_agent_harness_create_verified_with_store(&harness_config, &verifier, &harness), SALTS_EINVAL);
    turbo_agent_job_file_verifier_config_t files_config{};
    turbo_agent_job_file_verifier_config_init(&files_config);
    turbo_agent_job_file_verifier_t *files = nullptr;
    check_equal(turbo_agent_job_file_verifier_create(&files_config, &files), SALTS_EINVAL);
    check_null(files);
    char file_digest[65]{};
    check_equal(turbo_agent_job_file_verifier_digest_bytes(nullptr, 0, file_digest), SALTS_OK);
    check_equal(std::string(file_digest), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    check_equal(turbo_agent_harness_create_with_store(&harness_config, &harness), SALTS_OK);
    check_not_null(harness);
    turbo_agent_compiler_config_t compiler{};
    turbo_agent_compiler_config_init(&compiler);
    turbo_agent_compile_diagnostic_t diagnostic{};
    turbo_agent_job_execution_status_t job_status{};
    turbo_agent_job_execution_status_init(&job_status);
    check_equal(job_status.abi_version, TURBO_AGENT_JOB_EXECUTION_ABI_VERSION);
    turbo_agent_job_execution_t *job_execution = nullptr;
    check_equal(turbo_agent_harness_submit_job_inspect(harness, "../bad", 2, &compiler, 0, &job_execution), SALTS_EINVAL);
    check_null(job_execution);
    check_equal(turbo_agent_harness_submit_recorded_job_inspect(harness, "../bad", 2,
        &compiler, 0, "observation", &job_execution), SALTS_EINVAL);
    int observation_status = 0;
    int rejected_receipt = 0;
    check_equal(turbo_agent_harness_submit_receipted_job_inspect(harness, "../bad", 2,
        &compiler, 0, "admission", &rejected_receipt, &job_execution), SALTS_EINVAL);
    check_equal(rejected_receipt, SALTS_ENOENT); check_null(job_execution);
    check_equal(turbo_agent_job_execution_queue_receipt_status(nullptr, &rejected_receipt), SALTS_EINVAL);
    check_equal(turbo_agent_harness_submit_admitted_job_inspect(harness, "../bad", 2,
        &compiler, 0, "admission", &job_execution), SALTS_EINVAL);
    check_null(job_execution);
    check_equal(turbo_agent_job_execution_observation_status(nullptr, &observation_status), SALTS_EINVAL);
    turbo_tool_status_t tool_status = TURBO_TOOL_OK;
    json_value_t *result = nullptr, *certificate = nullptr, *reference = nullptr;
    check_equal(turbo_agent_harness_execute_job_inspect_json(harness, "../bad", 2,
        &compiler, nullptr, &diagnostic, &tool_status, &result, &certificate, &reference), SALTS_EINVAL);
    check_null(result); check_null(certificate); check_null(reference);
    check_equal(turbo_agent_harness_get_job_acceptance_json(harness, "../bad", 2,
        &compiler, &diagnostic, &reference), SALTS_EINVAL);
    check_null(reference);
    check_equal(turbo_agent_harness_prepare_job_delivery_json(harness, "../bad", 2,
        &compiler, "delivery-1", "destination", "payload", &diagnostic, &reference), SALTS_EINVAL);
    check_null(reference);
    check_equal(turbo_agent_harness_get_job_delivery_json(harness, "../bad", 2,
        &compiler, &diagnostic, &reference), SALTS_EINVAL);
    check_null(reference);
    check_equal(turbo_agent_harness_record_job_delivery_receipt_json(harness, "../bad", 2,
        &compiler, "delivery-1", "receipt", &diagnostic, &reference), SALTS_EINVAL);
    check_null(reference);
    check_equal(turbo_agent_harness_record_job_acceptance_json(harness, "../bad", 2,
        &compiler, nullptr, TURBO_AGENT_JOB_ACCEPTED, "evidence", &diagnostic, &reference), SALTS_EINVAL);
    check_null(reference);
    turbo_agent_harness_release(harness);
    cmeta_threadpool_destroy(pool);
  }

  it("uses the Job proposal ABI without creating an execution service") {
    auto store = turbo_agent_runtime_store_memory_create();
    auto *runtime = turbo_agent_runtime_create(&store);
    turbo_agent_job_t *job = nullptr;
    json_value_t *snapshot = nullptr;
    check_not_null(runtime);
    check_equal(turbo_agent_job_create(runtime, "cpp-job", "goal", "criteria", &job), SALTS_OK);
    check_equal(turbo_agent_job_propose(job, 0, "{}", 2, "source-v1"), SALTS_OK);
    check_equal(turbo_agent_job_bind_compiled_inspect(job, 1, "thread", "attempt"), SALTS_OK);
    check_equal(turbo_agent_job_get_snapshot(job, &snapshot), SALTS_OK);
    check_equal(json_get_string(snapshot, "state"), "attempt_bound");
    json_free(snapshot); snapshot = nullptr;
    check_equal(turbo_agent_job_admission_get(runtime, "cpp-job", "admission", &snapshot), SALTS_ENOENT);
    check_null(snapshot);
    check_equal(turbo_agent_job_queue_receipt_get(runtime, "cpp-job", "admission", &snapshot), SALTS_ENOENT);
    check_null(snapshot);
    check_equal(turbo_agent_job_request_cancel(job, 2, "cpp-stop", "host-reason"), SALTS_OK);
    check_equal(turbo_agent_job_get_cancellation(job, &snapshot), SALTS_OK);
    check_equal(json_get_string(snapshot, "request_id"), "cpp-stop");
    turbo_agent_job_destroy(job);
    json_free(snapshot);
    turbo_agent_runtime_destroy(runtime);
  }

  it("receives the Harness configuration ABI across the C++ boundary") {
    turbo_agent_harness_config_t config{};
    turbo_agent_harness_run_options_t options{};
    turbo_agent_harness_config_init(&config);
    turbo_agent_harness_run_options_init(&options);
    check_equal(config.struct_size, sizeof(config));
    check_equal(options.struct_size, sizeof(options));
    check_equal(config.abi_version, TURBO_AGENT_HARNESS_ABI_VERSION);
    check_equal(options.abi_version, TURBO_AGENT_HARNESS_ABI_VERSION);
  }
}
