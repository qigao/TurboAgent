#include "tinytest.h"
#include "turbo_agent_session.h"
#include "turbo_agent_state.h"
#include "turbo_agent_test_support.h"

static int summary_node(turbo_graph_exec_ctx_t *ctx, void *data) {
  (void)data;
  return turbo_agent_state_set_final_answer(ctx->json_value_state, "finished");
}

spec("Agent runtime summary boundary") {
  static turbo_agent_session_t *session;
  static turbo_graph_t *graph;
  static json_value_t *input, *summary, *output, *record;

  before_each() {
    turbo_agent_session_config_t config = {.thread_id = "summary-thread"};
    session = turbo_agent_session_create(&config);
    graph = turbo_graph_create("summary-graph");
    input = turbo_agent_session_create_input_state_json_value("hello");
    check_not_null(session);
    check_not_null(graph);
    check_not_null(input);
    check_equal(turbo_graph_add_json_value_node(graph, "work", summary_node, NULL), 0);
    check_equal(turbo_graph_set_entry(graph, "work"), 0);
  }

  after_each() {
    json_free(input);
    json_free(summary);
    json_free(output);
    json_free(record);
    input = summary = output = record = NULL;
    turbo_graph_destroy(graph);
    graph = NULL;
    turbo_agent_session_destroy(session);
    session = NULL;
  }

  it("preserves every configured parent link and returns independently owned metadata") {
    turbo_agent_session_config_t config = {.thread_id = "linked-summary",
                                           .parent_agent_run_id = "parent-agent-run",
                                           .parent_tool_call_id = "parent-tool-call",
                                           .parent_tool_name = "parent-tool",
                                           .parent_graph_run_id = "parent-graph-run",
                                           .call_frame_id = "parent-frame"};
    turbo_agent_session_destroy(session);
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_equal(turbo_agent_state_set_active_agent(input, "planner"), 0);
    check_equal(turbo_agent_state_request_handoff(input, "executor", "delegate"), 0);
    check_equal(
        turbo_agent_session_start_graph(session, graph, input, NULL, NULL, NULL, &summary, &output),
        0);
    check_equal(json_get_string(summary, "thread_id"), "linked-summary");
    check_equal(json_get_string(summary, "parent_agent_run_id"), "parent-agent-run");
    check_equal(json_get_string(summary, "parent_tool_call_id"), "parent-tool-call");
    check_equal(json_get_string(summary, "parent_tool_name"), "parent-tool");
    check_equal(json_get_string(summary, "parent_graph_run_id"), "parent-graph-run");
    check_equal(json_get_string(summary, "call_frame_id"), "parent-frame");
    check_equal(json_get_string(summary, "active_agent"), "planner");
    check_equal(json_get_string(summary, "handoff_target_agent"), "executor");
    check_equal(json_get_string(summary, "handoff_reason"), "delegate");
    check_equal(turbo_runtime_json_object_set_string(summary, "parent_tool_name", "caller-edit"),
                0);
    check_equal(turbo_agent_runtime_get_run(turbo_agent_session_runtime(session),
                                            json_get_string(summary, "run_id"), &record),
                0);
    check_equal(json_get_string(record, "parent_tool_name"), "parent-tool");
    check_equal(turbo_agent_state_set_active_agent(output, "caller-agent"), 0);
    json_free(input);
    input = NULL;
    turbo_graph_destroy(graph);
    graph = NULL;
    turbo_agent_session_destroy(session);
    session = NULL;
    check_equal(json_get_string(summary, "active_agent"), "planner");
    check_equal(json_get_string(summary, "status"), "completed");
    check_equal(json_type(json_object_get(summary, "result")), JSON_OBJECT);
    check_equal(turbo_agent_state_final_answer_text(output), "finished");
  }

  it("keeps absent summary fields explicit null and completed command arrays empty") {
    static const char *null_fields[] = {
        "checkpoint_id",       "parent_agent_run_id", "parent_tool_call_id", "parent_tool_name",
        "parent_graph_run_id", "call_frame_id",       "active_agent",        "handoff_target_agent",
        "handoff_reason",      "pending_node",        "interrupt_reason",    "pending_action"};
    size_t i;
    check_equal(
        turbo_agent_session_start_graph(session, graph, input, NULL, NULL, NULL, &summary, &output),
        0);
    for (i = 0; i < sizeof(null_fields) / sizeof(null_fields[0]); ++i) {
      const json_value_t *field = json_object_get(summary, null_fields[i]);
      check_not_null(field);
      check_equal(json_type(field), JSON_NULL);
    }
    check_equal(json_type(json_object_get(summary, "available_commands")), JSON_ARRAY);
    check_equal(json_array_size(json_object_get(summary, "available_commands")), 0);
    check_equal(json_type(json_object_get(summary, "available_command_descriptors")), JSON_ARRAY);
    check_equal(json_array_size(json_object_get(summary, "available_command_descriptors")), 0);
  }

  it("preserves explicit empty parent strings supplied directly to Runtime") {
    turbo_agent_runtime_parent_link_t parent = {.parent_agent_run_id = "",
                                                .parent_tool_call_id = "",
                                                .parent_tool_name = "",
                                                .parent_graph_run_id = "",
                                                .call_frame_id = ""};
    turbo_agent_runtime_exec_options_t options = {.thread_id = "empty-parent-summary",
                                                  .parent_link = &parent};
    check_equal(turbo_agent_runtime_exec_start(turbo_agent_session_runtime(session), graph, input,
                                               NULL, &options, &summary, &output),
                0);
    check_equal(json_get_string(summary, "parent_agent_run_id"), "");
    check_equal(json_get_string(summary, "parent_tool_call_id"), "");
    check_equal(json_get_string(summary, "parent_tool_name"), "");
    check_equal(json_get_string(summary, "parent_graph_run_id"), "");
    check_equal(json_get_string(summary, "call_frame_id"), "");
  }

  it("keeps interrupt priority and complete ordered command descriptors across state changes") {
    static const char *nodes[] = {"work"};
    /* Existing executor-failure projection returns "step failed" even without
     * an executor result; preserve that baseline classification here. */
    static const char *reasons[] = {"failure",          "failure",
                                    "model_error",      "guardrail_rejection",
                                    "replan_requested", "review_required"};
    static const char *actions[] = {"host_intervention",
                                    "host_intervention",
                                    "host_intervention",
                                    "host_intervention",
                                    "replan",
                                    "review"};
    static const size_t counts[] = {4, 4, 4, 4, 3, 6};
    turbo_graph_run_options_t options = {.interrupt_before_nodes = nodes,
                                         .interrupt_before_count = 1};
    size_t i, j;
    for (i = 0; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
      const json_value_t *commands, *descriptors;
      if (i == 1) check_equal(turbo_agent_state_set_failure(input, "tool_result", "failed"), 0);
      if (i == 2) check_equal(turbo_agent_state_set_model_error(input, "transport", "failed"), 0);
      if (i == 3)
        check_equal(turbo_agent_state_set_guardrail_rejection(input, "before_model", "rejected"),
                    0);
      if (i == 4) check_equal(turbo_agent_state_request_replan(input, "new plan"), 0);
      if (i == 5) check_equal(turbo_agent_state_request_review(input, "review this"), 0);
      check_equal(turbo_agent_session_start_graph(session, graph, input, &options, NULL, NULL,
                                                  &summary, &output),
                  0);
      check_equal(json_get_string(summary, "status"), "interrupted");
      check_equal(json_get_string(summary, "interrupt_reason"), reasons[i]);
      check_equal(json_get_string(summary, "pending_action"), actions[i]);
      check_equal(json_get_string(summary, "pending_node"), "work");
      check_not_null(json_get_string(summary, "checkpoint_id"));
      commands = json_object_get(summary, "available_commands");
      descriptors = json_object_get(summary, "available_command_descriptors");
      check_equal(json_array_size(commands), counts[i]);
      check_equal(json_array_size(descriptors), counts[i]);
      for (j = 0; j < counts[i]; ++j) {
        const json_value_t *descriptor = json_array_get(descriptors, j);
        const json_value_t *schema = json_object_get(descriptor, "args_schema");
        check_equal(json_get_string(descriptor, "name"),
                    turbo_runtime_json_value_as_string(json_array_get(commands, j)));
        check_equal(json_get_string(schema, "type"), "object");
        check_equal(json_type(json_object_get(schema, "properties")), JSON_OBJECT);
        check_equal(json_type(json_object_get(schema, "required")), JSON_ARRAY);
        check_equal(json_type(json_object_get(descriptor, "accepted_keys")), JSON_ARRAY);
        check_equal(json_type(json_object_get(descriptor, "example_payload")), JSON_OBJECT);
      }
      if (i == 5) {
        turbo_agent_test_check_command_descriptor_fixture(
            json_array_get(descriptors, 2), "command_descriptors.golden.json", "append_feedback");
        turbo_agent_test_check_command_descriptor_fixture(
            json_array_get(descriptors, 4), "command_descriptors.golden.json", "request_replan");
        turbo_agent_test_check_command_descriptor_fixture(json_array_get(descriptors, 5),
                                                          "command_descriptors.golden.json",
                                                          "override_final_output");
      }
      json_free(summary);
      json_free(output);
      summary = output = NULL;
    }
  }
}
