#include <turbo_agent.h>
#include <turbo_agent_app.h>
#include <turbo_agent_job.h>
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
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    check_equal(turbo_agent_harness_create_with_store(&harness_config, &harness), SALTS_OK);
    check_not_null(harness);
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
