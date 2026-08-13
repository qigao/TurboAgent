#include "tinytest.h"
#include "turbo_agent_workspace.h"
#include "turbo_fs.h"
#include "turbo_wasm_tool_pack.h"

#include <stdlib.h>
#include <string.h>

#ifndef LLM_SANDBOX_WASM_TOOL_WASM_PATH
  #error "LLM_SANDBOX_WASM_TOOL_WASM_PATH must be defined"
#endif

static int pack_workspace_write_text(const char *path, const char *text) {
  turbo_fs_buf_t buffer = turbo_fs_buf_init((void *)text, strlen(text));
  return turbo_fs_write_file(path, &buffer);
}

static turbo_wasm_policy_t *pack_workspace_wasm_policy_create(char *module_name,
                                                              size_t module_name_size) {
  turbo_wasm_policy_t *policy = turbo_wasm_policy_create();
  char module_root[TURBO_FS_MAX_PATH];

  if (!policy ||
      turbo_fs_path_dirname(LLM_SANDBOX_WASM_TOOL_WASM_PATH, module_root, sizeof(module_root)) !=
          0 ||
      turbo_fs_path_basename(LLM_SANDBOX_WASM_TOOL_WASM_PATH, module_name, module_name_size) != 0 ||
      turbo_wasm_policy_set_capabilities(policy, TURBO_WASM_CAP_CORE | TURBO_WASM_CAP_APP) !=
          TURBO_WASM_OK ||
      turbo_wasm_policy_set_module_root(policy, module_root) != TURBO_WASM_OK) {
    turbo_wasm_policy_destroy(policy);
    return NULL;
  }
  return policy;
}

spec("TurboWasm tool pack workspace integration") {
  it("projects skill-declared Wasm tools only when Agent policy allows them") {
    static const char skill_text[] = "---\n"
                                     "name: wasm-echo\n"
                                     "description: Echo JSON through Wasm\n"
                                     "triggers: [wasm, echo]\n"
                                     "tools: [echo_json]\n"
                                     "capabilities: [wasm]\n"
                                     "---\n"
                                     "Use the registered sandboxed echo tool.\n";
    char *root = tt_make_temp_dir("turbo_wasm_pack_workspace");
    char skills[TURBO_FS_MAX_PATH];
    char skill_dir[TURBO_FS_MAX_PATH];
    char skill_path[TURBO_FS_MAX_PATH];
    char module_name[TURBO_FS_MAX_PATH];
    turbo_wasm_tool_pack_config_t pack_config;
    turbo_wasm_tool_pack_module_config_t module_config;
    turbo_wasm_tool_pack_t *pack = NULL;
    turbo_wasm_policy_t *wasm_policy =
        pack_workspace_wasm_policy_create(module_name, sizeof(module_name));
    turbo_agent_policy_t agent_policy = turbo_agent_policy_default();
    turbo_agent_workspace_tool_capability_t capability = {
        "echo_json", TURBO_AGENT_POLICY_CAPABILITY_RUNTIME_TOOLS};
    turbo_agent_workspace_config_t workspace_config;
    turbo_agent_workspace_t *workspace = NULL;
    turbo_agent_workspace_selection_t *selection = NULL;
    char *output = NULL;

    check_not_null(root);
    check_not_null(wasm_policy);
    check_int_eq(turbo_fs_path_join(skills, sizeof(skills), root, "skills"), 0);
    check_int_eq(turbo_fs_path_join(skill_dir, sizeof(skill_dir), skills, "wasm-echo"), 0);
    check_int_eq(turbo_fs_mkdir(skills, 0700), 0);
    check_int_eq(turbo_fs_mkdir(skill_dir, 0700), 0);
    check_int_eq(turbo_fs_path_join(skill_path, sizeof(skill_path), skill_dir, "SKILL.md"), 0);
    check_int_eq(pack_workspace_write_text(skill_path, skill_text), 0);

    turbo_wasm_tool_pack_config_init(&pack_config);
    pack = turbo_wasm_tool_pack_create(&pack_config);
    check_not_null(pack);
    turbo_wasm_tool_pack_module_config_init(&module_config);
    module_config.runtime.module_path = module_name;
    module_config.runtime.policy = wasm_policy;
    check_int_eq(turbo_wasm_tool_pack_add_module(pack, &module_config), TURBO_TOOL_OK);
    turbo_wasm_policy_destroy(wasm_policy);
    wasm_policy = NULL;

    agent_policy.allow_runtime_tools = 1;
    turbo_agent_workspace_config_init(&workspace_config);
    workspace_config.workspace_root = root;
    workspace_config.policy = &agent_policy;
    workspace_config.tool_capabilities = &capability;
    workspace_config.tool_capability_count = 1;
    check_int_eq(turbo_agent_workspace_create(&workspace_config, &workspace),
                 TURBO_AGENT_WORKSPACE_OK);
    check_int_eq(turbo_agent_workspace_prepare(workspace, "use wasm echo",
                                               turbo_wasm_tool_pack_registry(pack), NULL,
                                               &selection),
                 TURBO_AGENT_WORKSPACE_OK);
    check_not_null(selection);
    check_size_eq(turbo_agent_workspace_selection_tool_count(selection), 1);
    check_str_eq(turbo_agent_workspace_selection_tool_name(selection, 0), "echo_json");
    check_int_eq(turbo_tool_registry_execute(turbo_agent_workspace_selection_tools(selection),
                                             "echo_json", "{\"workspace\":true}", &output),
                 TURBO_TOOL_OK);
    check_str_eq(output, "{\"workspace\":true}");
    free(output);
    output = NULL;
    turbo_agent_workspace_selection_destroy(selection);
    selection = NULL;
    turbo_agent_workspace_destroy(workspace);
    workspace = NULL;

    agent_policy.allow_runtime_tools = 0;
    check_int_eq(turbo_agent_workspace_create(&workspace_config, &workspace),
                 TURBO_AGENT_WORKSPACE_OK);
    check_int_eq(turbo_agent_workspace_prepare(workspace, "use wasm echo",
                                               turbo_wasm_tool_pack_registry(pack), NULL,
                                               &selection),
                 TURBO_AGENT_WORKSPACE_CAPABILITY_DENIED);
    check_null(selection);

    turbo_agent_workspace_destroy(workspace);
    turbo_wasm_tool_pack_destroy(pack);
    check_int_eq(tt_remove_tree(root), 0);
    free(root);
  }
}
