#include "tinytest.h"
#include "turbo_agent_workspace.h"

#include <turbo_fs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int workspace_echo_tool(const char *arguments_json, char **out_output, void *user_data) {
  size_t len;
  (void)user_data;
  if (!out_output) return -1;
  len = strlen(arguments_json ? arguments_json : "{}") + 1;
  *out_output = (char *)malloc(len);
  if (!*out_output) return -1;
  memcpy(*out_output, arguments_json ? arguments_json : "{}", len);
  return 0;
}

static int workspace_transport(const char *request_json, char **out_response_json,
                               void *user_data) {
  (void)request_json;
  (void)out_response_json;
  (void)user_data;
  return -1;
}

static int workspace_write_text(const char *path, const char *text) {
  turbo_fs_buf_t buffer = turbo_fs_buf_init((void *)text, strlen(text));
  return turbo_fs_write_file(path, &buffer);
}

static int workspace_join(char *out, size_t out_size, const char *base, const char *path) {
  return turbo_fs_path_join(out, out_size, base, path);
}

spec("turbo agent workspace") {

  it("should compose hierarchical instructions and project selected skill tools") {
    static const char skill_text[] =
        "---\n"
        "name: repo-review\n"
        "description: Review repository code\n"
        "triggers: [review, 审查]\n"
        "tools: [fs.read]\n"
        "capabilities: [custom_tools]\n"
        "---\n"
        "Inspect relevant implementation and tests before reporting findings.\n";
    char *root = tt_make_temp_dir("turbo_workspace");
    char src[TURBO_FS_MAX_PATH];
    char skills[TURBO_FS_MAX_PATH];
    char skill_dir[TURBO_FS_MAX_PATH];
    char root_agents[TURBO_FS_MAX_PATH];
    char nested_agents[TURBO_FS_MAX_PATH];
    char skill_path[TURBO_FS_MAX_PATH];
    turbo_agent_workspace_config_t config;
    turbo_agent_workspace_t *workspace = NULL;
    turbo_agent_workspace_selection_t *selection = NULL;
    turbo_agent_t *agent = NULL;
    turbo_agent_config_t agent_config = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t read_tool = {
        "fs.read", "Read", "{\"type\":\"object\"}", NULL, 1, workspace_echo_tool, NULL, NULL, NULL};
    const char *instructions;

    check_not_null(root);
    check_int_eq(workspace_join(src, sizeof(src), root, "src"), 0);
    check_int_eq(workspace_join(skills, sizeof(skills), root, "skills"), 0);
    check_int_eq(workspace_join(skill_dir, sizeof(skill_dir), skills, "repo-review"), 0);
    check_int_eq(turbo_fs_mkdir(src, 0700), 0);
    check_int_eq(turbo_fs_mkdir(skills, 0700), 0);
    check_int_eq(turbo_fs_mkdir(skill_dir, 0700), 0);
    check_int_eq(workspace_join(root_agents, sizeof(root_agents), root, "AGENTS.md"), 0);
    check_int_eq(workspace_join(nested_agents, sizeof(nested_agents), src, "AGENTS.md"), 0);
    check_int_eq(workspace_join(skill_path, sizeof(skill_path), skill_dir, "SKILL.md"), 0);
    check_int_eq(workspace_write_text(root_agents, "Root instructions."), 0);
    check_int_eq(workspace_write_text(nested_agents, "Nested instructions."), 0);
    check_int_eq(workspace_write_text(skill_path, skill_text), 0);
    check_int_eq(turbo_tool_registry_add(registry, &read_tool), TURBO_TOOL_OK);

    turbo_agent_workspace_config_init(&config);
    config.workspace_root = root;
    config.working_directory = "src";
    check_int_eq(turbo_agent_workspace_create(&config, &workspace), TURBO_AGENT_WORKSPACE_OK);
    check_not_null(workspace);
    check_size_eq(turbo_agent_workspace_skill_count(workspace), 1);
    check_int_eq(turbo_agent_workspace_prepare(workspace, "review this repo", registry,
                                               "Base instructions.", &selection),
                 TURBO_AGENT_WORKSPACE_OK);
    check_not_null(selection);
    check_size_eq(turbo_agent_workspace_selection_skill_count(selection), 1);
    check_str_eq(turbo_agent_workspace_selection_skill_name(selection, 0), "repo-review");
    check_size_eq(turbo_agent_workspace_selection_tool_count(selection), 1);
    check_str_eq(turbo_agent_workspace_selection_tool_name(selection, 0), "fs.read");
    check_size_eq(turbo_tool_registry_count(turbo_agent_workspace_selection_tools(selection)), 1);
    instructions = turbo_agent_workspace_selection_instructions(selection);
    check_not_null(strstr(instructions, "Base instructions."));
    check_not_null(strstr(instructions, "Root instructions."));
    check_not_null(strstr(instructions, "Nested instructions."));
    check_not_null(strstr(instructions, "Inspect relevant implementation"));

    turbo_agent_workspace_selection_destroy(selection);
    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = workspace_transport;
    agent_config.tool_registry = registry;
    agent = turbo_agent_create_for_workspace(&agent_config, workspace, "review this repo");
    check_not_null(agent);
    check_size_eq(turbo_agent_tool_count(agent), 1);
    turbo_agent_destroy(agent);
    turbo_agent_workspace_destroy(workspace);
    turbo_tool_registry_destroy(registry);
    check_int_eq(tt_remove_tree(root), 0);
    free(root);
  }

  it("should fail fast when a selected skill tool is unavailable") {
    static const char skill_text[] = "---\n"
                                     "name: build-helper\n"
                                     "description: Build helper\n"
                                     "triggers: [build]\n"
                                     "tools: [build.run]\n"
                                     "capabilities: [shell]\n"
                                     "---\n"
                                     "Run the configured build.\n";
    char *root = tt_make_temp_dir("turbo_workspace_missing");
    char skills[TURBO_FS_MAX_PATH];
    char skill_dir[TURBO_FS_MAX_PATH];
    char skill_path[TURBO_FS_MAX_PATH];
    turbo_agent_workspace_config_t config;
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_agent_workspace_tool_capability_t capability = {"build.run",
                                                          TURBO_AGENT_POLICY_CAPABILITY_SHELL};
    turbo_agent_workspace_t *workspace = NULL;
    turbo_agent_workspace_selection_t *selection = NULL;
    turbo_tool_registry_t *registry = turbo_tool_registry_create();

    check_not_null(root);
    check_int_eq(workspace_join(skills, sizeof(skills), root, "skills"), 0);
    check_int_eq(workspace_join(skill_dir, sizeof(skill_dir), skills, "build-helper"), 0);
    check_int_eq(turbo_fs_mkdir(skills, 0700), 0);
    check_int_eq(turbo_fs_mkdir(skill_dir, 0700), 0);
    check_int_eq(workspace_join(skill_path, sizeof(skill_path), skill_dir, "SKILL.md"), 0);
    check_int_eq(workspace_write_text(skill_path, skill_text), 0);
    policy.allow_custom_tools = 0;
    turbo_agent_workspace_config_init(&config);
    config.workspace_root = root;
    config.policy = &policy;
    config.tool_capabilities = &capability;
    config.tool_capability_count = 1;
    check_int_eq(turbo_agent_workspace_create(&config, &workspace), TURBO_AGENT_WORKSPACE_OK);
    check_int_eq(
        turbo_agent_workspace_prepare(workspace, "build the project", registry, NULL, &selection),
        TURBO_AGENT_WORKSPACE_TOOL_NOT_FOUND);
    check_null(selection);
    check_not_null(turbo_agent_workspace_last_error(workspace));

    turbo_agent_workspace_destroy(workspace);
    turbo_tool_registry_destroy(registry);
    check_int_eq(tt_remove_tree(root), 0);
    free(root);
  }

  it("should reject a selected skill capability denied by policy") {
    static const char skill_text[] = "---\n"
                                     "name: network-helper\n"
                                     "description: Network helper\n"
                                     "triggers: [download]\n"
                                     "capabilities: [network]\n"
                                     "---\n"
                                     "Fetch the requested resource.\n";
    char *root = tt_make_temp_dir("turbo_workspace_policy");
    char skills[TURBO_FS_MAX_PATH];
    char skill_dir[TURBO_FS_MAX_PATH];
    char skill_path[TURBO_FS_MAX_PATH];
    turbo_agent_workspace_config_t config;
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_agent_workspace_t *workspace = NULL;
    turbo_agent_workspace_selection_t *selection = NULL;

    check_not_null(root);
    check_int_eq(workspace_join(skills, sizeof(skills), root, "skills"), 0);
    check_int_eq(workspace_join(skill_dir, sizeof(skill_dir), skills, "network-helper"), 0);
    check_int_eq(turbo_fs_mkdir(skills, 0700), 0);
    check_int_eq(turbo_fs_mkdir(skill_dir, 0700), 0);
    check_int_eq(workspace_join(skill_path, sizeof(skill_path), skill_dir, "SKILL.md"), 0);
    check_int_eq(workspace_write_text(skill_path, skill_text), 0);
    policy.allow_network = 0;
    turbo_agent_workspace_config_init(&config);
    config.workspace_root = root;
    config.policy = &policy;
    check_int_eq(turbo_agent_workspace_create(&config, &workspace), TURBO_AGENT_WORKSPACE_OK);
    check_int_eq(
        turbo_agent_workspace_prepare(workspace, "download documentation", NULL, NULL, &selection),
        TURBO_AGENT_WORKSPACE_CAPABILITY_DENIED);
    check_null(selection);

    turbo_agent_workspace_destroy(workspace);
    check_int_eq(tt_remove_tree(root), 0);
    free(root);
  }
}
