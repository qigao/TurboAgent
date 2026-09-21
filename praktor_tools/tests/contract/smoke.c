#include "turbo_praktor_tool_pack.h"
#include "json_parser.h"
#include "turbo_runtime_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail(const char *message) {
  fprintf(stderr, "contract failure: %s\n", message);
  return 1;
}

int main(void) {
  char cwd[4096];
  char workflow_path[8192];
  FILE *workflow;
  turbo_praktor_tool_pack_config_t pack_config;
  turbo_praktor_workflow_config_t workflow_config;
  turbo_praktor_tool_pack_t *pack;
  turbo_tool_registry_t *registry;
  const char *const *capabilities = NULL;
  size_t capability_count = 0;
  turbo_tool_execution_policy_t policy = {0};
  char *output = NULL;
  json_value_t *args = NULL;
  json_value_t *result = NULL;
  int rc = 1;

  if (!getcwd(cwd, sizeof(cwd))) return fail("getcwd");
  if (snprintf(workflow_path, sizeof(workflow_path), "%s/praktor_contract_workflow.yml", cwd) < 0)
    return fail("workflow path");
  workflow = fopen(workflow_path, "wb");
  if (!workflow) return fail("create workflow");
  fputs("tasks:\n  - name: noop\n    script: |\n      ctx.output(\"ok\", true);\n", workflow);
  fclose(workflow);

  turbo_praktor_tool_pack_config_init(&pack_config);
  pack = turbo_praktor_tool_pack_create(&pack_config);
  if (!pack) goto cleanup;
  registry = turbo_praktor_tool_pack_registry(pack);
  if (!registry) goto cleanup_pack;

  turbo_praktor_workflow_config_init(&workflow_config);
  workflow_config.tool_name = "praktor_contract";
  workflow_config.description = "Contract smoke workflow";
  workflow_config.workflow_path = workflow_path;
  workflow_config.parameters_json =
      "{\"type\":\"object\",\"additionalProperties\":true}";
  if (turbo_praktor_tool_pack_add_workflow(pack, &workflow_config) != TURBO_TOOL_OK)
    goto cleanup_pack;
  if (turbo_praktor_tool_pack_workflow_count(pack) != 1 ||
      turbo_tool_registry_count(registry) != 1)
    goto cleanup_pack;
  if (turbo_tool_registry_get_execution_policy(
          registry, "praktor_contract", &policy) != TURBO_TOOL_OK ||
      policy.mode != TURBO_TOOL_EXECUTION_EXCLUSIVE ||
      policy.idempotency != TURBO_TOOL_IDEMPOTENCY_NONE)
    goto cleanup_pack;
  if (turbo_tool_registry_get_required_capabilities(
          registry, "praktor_contract", &capabilities, &capability_count) != TURBO_TOOL_OK ||
      capability_count != 5 ||
      strcmp(capabilities[0], "runtime_tools") != 0 ||
      strcmp(capabilities[1], "network") != 0 ||
      strcmp(capabilities[2], "shell") != 0 ||
      strcmp(capabilities[3], "patch") != 0 ||
      strcmp(capabilities[4], "outside_workspace") != 0)
    goto cleanup_pack;

  if (turbo_tool_registry_execute(registry, "praktor_contract", "{}", &output) != TURBO_TOOL_OK ||
      !output || !strstr(output, "\"workflow_status\":\"success\""))
    goto cleanup_pack;

  args = json_create_object();
  if (!args ||
      turbo_tool_registry_execute_json_value(
          registry, "praktor_contract", args, &result) != TURBO_TOOL_OK ||
      !result ||
      !json_get_string(result, "workflow_status") ||
      strcmp(json_get_string(result, "workflow_status"), "success") != 0)
    goto cleanup_pack;

  rc = 0;
  puts("PRAKTOR_TOOLS_CONTRACT_OK");

cleanup_pack:
  free(output);
  turbo_runtime_json_destroy(args);
  turbo_runtime_json_destroy(result);
  turbo_praktor_tool_pack_destroy(pack);
cleanup:
  unlink(workflow_path);
  return rc;
}
