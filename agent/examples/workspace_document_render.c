#include "document_render_tool.h"
#include "turbo_agent_workspace.h"

#include <stdio.h>

int main(int argc, char **argv) {
  static const document_render_source_t sources[] = {{"brief", "fixture-v1"}};
  document_render_catalog_t catalog = {sources, 1};
  turbo_agent_policy_t policy = turbo_agent_policy_default();
  turbo_agent_workspace_config_t config;
  turbo_agent_workspace_t *workspace = NULL;
  turbo_agent_workspace_selection_t *selection = NULL;
  turbo_tool_registry_t *registry = NULL;
  const turbo_tool_registry_t *selected;
  const char *reason = NULL;
  json_value_t *arguments = NULL;
  json_value_t *result = NULL;
  int status = 1;
  static const char input[] =
      "{\"title\":\"Fixture report\",\"passages\":[{\"text\":\"Draft for review.\",\"source_id\":\"brief\"}]}";

  if (argc != 2) {
    fprintf(stderr, "usage: workspace_document_render <absolute-workspace-with-document-report-skill>\n");
    return status;
  }
  registry = turbo_tool_registry_create();
  if (!registry || document_render_register(registry, &catalog) != TURBO_TOOL_OK) goto done;
  policy.allow_runtime_tools = 1; /* Explicit Host grant, before any dispatch. */
  turbo_agent_workspace_config_init(&config);
  config.workspace_root = argv[1];
  config.policy = &policy;
  if (turbo_agent_workspace_create(&config, &workspace) != TURBO_AGENT_WORKSPACE_OK ||
      turbo_agent_workspace_prepare(workspace, "$document-report", registry, NULL, &selection) !=
          TURBO_AGENT_WORKSPACE_OK ||
      turbo_agent_workspace_selection_skill_count(selection) != 1 ||
      strcmp(turbo_agent_workspace_selection_skill_name(selection, 0), "document-report") != 0 ||
      turbo_agent_workspace_selection_tool_count(selection) != 1) goto done;
  selected = turbo_agent_workspace_selection_tools(selection);
  arguments = json_parse(input, sizeof(input) - 1);
  /* Direct RuntimeTools is a Host API. Check Host policy explicitly here;
   * this example does not claim to be a compiled Harness execution. */
  if (!arguments || turbo_agent_policy_check_tool(&policy, selected, "document_render", &reason) !=
                        TURBO_AGENT_POLICY_ALLOW ||
      turbo_tool_registry_execute_json_value(selected, "document_render", arguments, &result) !=
          TURBO_TOOL_OK ||
      turbo_tool_schema_validate_result_json_value(selected, "document_render", result, NULL, 0) !=
          TURBO_TOOL_SCHEMA_VALID) goto done;
  puts(json_get_string(result, "markdown"));
  status = 0;
done:
  json_free(result);
  json_free(arguments);
  turbo_agent_workspace_selection_destroy(selection);
  turbo_agent_workspace_destroy(workspace);
  turbo_tool_registry_destroy(registry);
  if (status) fprintf(stderr, "document render example failed\n");
  return status;
}
