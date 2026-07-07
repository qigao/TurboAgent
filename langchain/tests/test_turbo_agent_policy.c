#include "tinytest.h"
#include "turbo_agent_policy.h"

spec("turbo agent policy") {

  it("should allow read action inside workspace") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_action_tool_definition_t definition = {
        .name = "read_file",
        .description = "Read",
        .parameters_json = "{\"type\":\"object\"}",
        .kind = TURBO_ACTION_OBSERVE,
        .handler = (turbo_action_tool_handler_fn)1,
    };
    json_value_t *args = turbo_json_create_object();

    policy.workspace_root = "C:\\workspace";
    check_not_null(args);
    turbo_json_object_set_string(args, "path", "C:\\workspace\\src\\main.c");
    check_int_eq(turbo_agent_policy_check_action(&policy, &definition, args, NULL),
                 TURBO_AGENT_POLICY_ALLOW);

    turbo_free_json(&args);
  }

  it("should allow slash-normalized paths inside workspace") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_action_tool_definition_t definition = {
        .name = "read_file",
        .description = "Read",
        .parameters_json = "{\"type\":\"object\"}",
        .kind = TURBO_ACTION_OBSERVE,
        .handler = (turbo_action_tool_handler_fn)1,
    };
    json_value_t *args = turbo_json_create_object();

    policy.workspace_root = "C:\\workspace";
    check_not_null(args);
    turbo_json_object_set_string(args, "path", "C:/workspace/src/main.c");
    check_int_eq(turbo_agent_policy_check_action(&policy, &definition, args, NULL),
                 TURBO_AGENT_POLICY_ALLOW);

    turbo_free_json(&args);
  }

  it("should reject patch outside workspace") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_action_tool_definition_t definition = {
        .name = "apply_patch",
        .description = "Patch",
        .parameters_json = "{\"type\":\"object\"}",
        .kind = TURBO_ACTION_MUTATE,
        .handler = (turbo_action_tool_handler_fn)1,
    };
    json_value_t *args = turbo_json_create_object();
    const char *reason = NULL;

    policy.workspace_root = "C:\\workspace";
    check_not_null(args);
    turbo_json_object_set_string(args, "path", "D:\\other\\file.c");
    check_int_eq(turbo_agent_policy_check_action(&policy, &definition, args, &reason),
                 TURBO_AGENT_POLICY_DENY);
    check_str_eq(reason, "path_outside_workspace");

    turbo_free_json(&args);
  }

  it("should reject paths that escape workspace through parent segments") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_action_tool_definition_t definition = {
        .name = "apply_patch",
        .description = "Patch",
        .parameters_json = "{\"type\":\"object\"}",
        .kind = TURBO_ACTION_MUTATE,
        .handler = (turbo_action_tool_handler_fn)1,
    };
    json_value_t *args = turbo_json_create_object();
    const char *reason = NULL;

    policy.workspace_root = "C:\\workspace";
    check_not_null(args);
    turbo_json_object_set_string(args, "path", "C:\\workspace\\..\\outside\\file.c");
    check_int_eq(turbo_agent_policy_check_action(&policy, &definition, args, &reason),
                 TURBO_AGENT_POLICY_DENY);
    check_str_eq(reason, "path_outside_workspace");

    turbo_free_json(&args);
  }

  it("should allow paths normalized inside workspace") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_action_tool_definition_t definition = {
        .name = "read_file",
        .description = "Read",
        .parameters_json = "{\"type\":\"object\"}",
        .kind = TURBO_ACTION_OBSERVE,
        .handler = (turbo_action_tool_handler_fn)1,
    };
    json_value_t *args = turbo_json_create_object();

    policy.workspace_root = "C:\\workspace";
    check_not_null(args);
    turbo_json_object_set_string(args, "path", "C:\\workspace\\.\\src\\..\\src\\main.c");
    check_int_eq(turbo_agent_policy_check_action(&policy, &definition, args, NULL),
                 TURBO_AGENT_POLICY_ALLOW);

    turbo_free_json(&args);
  }

  it("should reject directory roots outside workspace") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_action_tool_definition_t definition = {
        .name = "agent.knowledge.index_directory",
        .description = "Index directory",
        .parameters_json = "{\"type\":\"object\"}",
        .kind = TURBO_ACTION_OBSERVE,
        .handler = (turbo_action_tool_handler_fn)1,
    };
    json_value_t *args = turbo_json_create_object();
    const char *reason = NULL;

    policy.workspace_root = "C:\\workspace";
    check_not_null(args);
    turbo_json_object_set_string(args, "root_dir", "D:\\other\\docs");
    check_int_eq(turbo_agent_policy_check_action(&policy, &definition, args, &reason),
                 TURBO_AGENT_POLICY_DENY);
    check_str_eq(reason, "path_outside_workspace");

    turbo_free_json(&args);
  }

#ifndef _WIN32
  it("should treat workspace paths as case-sensitive on non-Windows hosts") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_action_tool_definition_t definition = {
        .name = "read_file",
        .description = "Read",
        .parameters_json = "{\"type\":\"object\"}",
        .kind = TURBO_ACTION_OBSERVE,
        .handler = (turbo_action_tool_handler_fn)1,
    };
    json_value_t *args = turbo_json_create_object();
    const char *reason = NULL;

    policy.workspace_root = "/workspace";
    check_not_null(args);
    turbo_json_object_set_string(args, "path", "/Workspace/src/main.c");
    check_int_eq(turbo_agent_policy_check_action(&policy, &definition, args, &reason),
                 TURBO_AGENT_POLICY_DENY);
    check_str_eq(reason, "path_outside_workspace");

    turbo_free_json(&args);
  }
#endif

  it("should require approval for dangerous shell command") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_action_tool_definition_t definition = {
        .name = "run_command",
        .description = "Run",
        .parameters_json = "{\"type\":\"object\"}",
        .kind = TURBO_ACTION_DANGEROUS,
        .handler = (turbo_action_tool_handler_fn)1,
    };
    json_value_t *args = turbo_json_create_object();
    const char *reason = NULL;

    check_not_null(args);
    turbo_json_object_set_string(args, "command", "git reset --hard HEAD");
    check_int_eq(turbo_agent_policy_check_action(&policy, &definition, args, &reason),
                 TURBO_AGENT_POLICY_REQUIRE_APPROVAL);
    check_str_eq(reason, "dangerous_command");
    check_true(turbo_agent_policy_requires_approval(&policy, &definition, args));

    turbo_free_json(&args);
  }

  it("should match dangerous shell commands case-insensitively") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_action_tool_definition_t definition = {
        .name = "run_command",
        .description = "Run",
        .parameters_json = "{\"type\":\"object\"}",
        .kind = TURBO_ACTION_DANGEROUS,
        .handler = (turbo_action_tool_handler_fn)1,
    };
    json_value_t *args = turbo_json_create_object();
    const char *reason = NULL;

    check_not_null(args);
    turbo_json_object_set_string(args, "command", "RM -rf build");
    check_int_eq(turbo_agent_policy_check_action(&policy, &definition, args, &reason),
                 TURBO_AGENT_POLICY_REQUIRE_APPROVAL);
    check_str_eq(reason, "dangerous_command");

    turbo_free_json(&args);
  }

  it("should deny shell actions when disabled") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_action_tool_definition_t definition = {
        .name = "run_command",
        .description = "Run",
        .parameters_json = "{\"type\":\"object\"}",
        .kind = TURBO_ACTION_DANGEROUS,
        .handler = (turbo_action_tool_handler_fn)1,
    };

    policy.allow_shell = 0;
    check_int_eq(turbo_agent_policy_check_action(&policy, &definition, NULL, NULL),
                 TURBO_AGENT_POLICY_DENY);
  }

  it("should expose explicit capability checks with stable denial reasons") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    const char *reason = NULL;

    check_true(turbo_agent_policy_allows_capability(
        &policy, TURBO_AGENT_POLICY_CAPABILITY_CUSTOM_TOOLS, &reason));
    check_null(reason);
    check_true(turbo_agent_policy_allows_capability(
        &policy, TURBO_AGENT_POLICY_CAPABILITY_RUNTIME_TOOLS, &reason));
    check_true(turbo_agent_policy_allows_capability(
        &policy, TURBO_AGENT_POLICY_CAPABILITY_DELEGATE, &reason));
    check_true(turbo_agent_policy_allows_capability(
        &policy, TURBO_AGENT_POLICY_CAPABILITY_NETWORK, &reason));
    check_true(turbo_agent_policy_allows_capability(
        &policy, TURBO_AGENT_POLICY_CAPABILITY_SHELL, &reason));
    check_true(turbo_agent_policy_allows_capability(
        &policy, TURBO_AGENT_POLICY_CAPABILITY_PATCH, &reason));

    policy.allow_custom_tools = 0;
    reason = NULL;
    check_false(turbo_agent_policy_allows_capability(
        &policy, TURBO_AGENT_POLICY_CAPABILITY_CUSTOM_TOOLS, &reason));
    check_str_eq(reason, "custom_tools_disabled");

    policy.allow_runtime_tools = 0;
    reason = NULL;
    check_false(turbo_agent_policy_allows_capability(
        &policy, TURBO_AGENT_POLICY_CAPABILITY_RUNTIME_TOOLS, &reason));
    check_str_eq(reason, "runtime_tools_disabled");

    policy.allow_delegate = 0;
    reason = NULL;
    check_false(turbo_agent_policy_allows_capability(
        &policy, TURBO_AGENT_POLICY_CAPABILITY_DELEGATE, &reason));
    check_str_eq(reason, "delegate_disabled");

    policy.allow_network = 0;
    reason = NULL;
    check_false(turbo_agent_policy_allows_capability(
        &policy, TURBO_AGENT_POLICY_CAPABILITY_NETWORK, &reason));
    check_str_eq(reason, "network_disabled");
  }

  it("should reject unknown policy capabilities") {
    const char *reason = NULL;

    check_false(turbo_agent_policy_allows_capability(
        NULL, (turbo_agent_policy_capability_t)999, &reason));
    check_str_eq(reason, "unknown_capability");
  }
}
