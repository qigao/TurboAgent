#include "turbo_praktor_tool_pack.h"
#include "json_parser.h"
#include "turbo_runtime_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail(const char *message) {
  fprintf(stderr, "real Praktor integration failure: %s\n", message);
  return 1;
}

static int write_file(const char *path, const char *content) {
  FILE *file = fopen(path, "wb");
  if (!file) return -1;
  fputs(content, file);
  return fclose(file);
}

static int check_status(const char *text, const char *expected) {
  return text && expected && strstr(text, expected) != NULL;
}

int main(void) {
  char cwd[4096];
  char command_path[8192];
  char script_path[8192];
  turbo_praktor_tool_pack_config_t pack_config;
  turbo_praktor_workflow_config_t workflow_config;
  turbo_praktor_tool_pack_t *pack = NULL;
  turbo_tool_registry_t *registry = NULL;
  char *output = NULL;
  json_value_t *args = NULL;
  json_value_t *result = NULL;
  int rc = 1;

  if (!getcwd(cwd, sizeof(cwd))) return fail("getcwd");
  if (snprintf(command_path, sizeof(command_path), "%s/praktor_real_command.yml", cwd) < 0)
    return fail("command path");
  if (snprintf(script_path, sizeof(script_path), "%s/praktor_real_script.yml", cwd) < 0)
    return fail("script path");

  if (write_file(command_path,
                 "tasks:\n"
                 "  - name: command_ok\n"
                 "    command: \"printf turboagent-real-praktor\"\n") != 0)
    return fail("write command workflow");

  if (write_file(script_path,
                 "tasks:\n"
                 "  - name: script_rejected\n"
                 "    script: |\n"
                 "      ctx.output(\"unexpected\", true);\n") != 0)
    goto cleanup;

  turbo_praktor_tool_pack_config_init(&pack_config);
  pack = turbo_praktor_tool_pack_create(&pack_config);
  if (!pack) goto cleanup;

  registry = turbo_praktor_tool_pack_registry(pack);
  if (!registry) goto cleanup;

  turbo_praktor_workflow_config_init(&workflow_config);
  workflow_config.tool_name = "praktor_real_command";
  workflow_config.description = "Execute a real Praktor command workflow.";
  workflow_config.workflow_path = command_path;
  workflow_config.parameters_json =
      "{\"type\":\"object\",\"additionalProperties\":true}";
  if (turbo_praktor_tool_pack_add_workflow(pack, &workflow_config) != TURBO_TOOL_OK)
    goto cleanup;

  if (turbo_tool_registry_execute(
          registry, "praktor_real_command", "{}", &output) != TURBO_TOOL_OK ||
      !check_status(output, "\"workflow_status\":\"success\""))
    goto cleanup;

  free(output);
  output = NULL;

  args = json_create_object();
  if (!args ||
      turbo_tool_registry_execute_json_value(
          registry, "praktor_real_command", args, &result) != TURBO_TOOL_OK ||
      !result ||
      !json_get_string(result, "workflow_status") ||
      strcmp(json_get_string(result, "workflow_status"), "success") != 0)
    goto cleanup;

  turbo_runtime_json_destroy(args);
  args = NULL;
  turbo_runtime_json_destroy(result);
  result = NULL;

  turbo_praktor_workflow_config_init(&workflow_config);
  workflow_config.tool_name = "praktor_real_script";
  workflow_config.description = "Verify core-only Praktor rejects script workflows.";
  workflow_config.workflow_path = script_path;
  workflow_config.parameters_json =
      "{\"type\":\"object\",\"additionalProperties\":true}";
  if (turbo_praktor_tool_pack_add_workflow(pack, &workflow_config) != TURBO_TOOL_OK)
    goto cleanup;

  if (turbo_tool_registry_execute(
          registry, "praktor_real_script", "{}", &output) != TURBO_TOOL_OK ||
      !check_status(output, "\"workflow_status\":\"failed\"") ||
      !check_status(output, "ENABLE_SCRIPT_ENGINE=OFF"))
    goto cleanup;

  rc = 0;
  puts("TURBOAGENT_REAL_PRAKTOR_OK");

cleanup:
  free(output);
  turbo_runtime_json_destroy(args);
  turbo_runtime_json_destroy(result);
  turbo_praktor_tool_pack_destroy(pack);
  unlink(command_path);
  unlink(script_path);
  return rc;
}
