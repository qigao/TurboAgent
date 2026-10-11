#include "tinytest.h"
#include "turbo_coding_tools.h"
#include "workspace_native.h"

#include <cmeta_fs.h>
#include <stdlib.h>
#include <string.h>

spec("generated workspace tool contract") {
  it("should generate and round trip the search path and literal pattern together") {
    DataBind *codec = NULL;
    DataBindError error = DATA_BIND_ERROR_INIT;
    WorkspaceSearch_t source, restored;
    char *wire = NULL;
    size_t length = 0;
    WorkspaceSearch_init(&source);
    WorkspaceSearch_init(&restored);
    check_equal(TurboWorkspace_codec_create(&codec, &error), DATA_BIND_OK);
    source.path = tstr_dup("src");
    source.pattern = tstr_dup("a.*b");
    check_not_null(source.path); check_not_null(source.pattern);
    check_equal(WorkspaceSearch_to_json(codec, &source, &wire, &length, &error), DATA_BIND_OK);
    check_equal(WorkspaceSearch_from_json(codec, &restored, wire, length, &error), DATA_BIND_OK);
    check_equal(restored.path, source.path);
    check_equal(restored.pattern, source.pattern);
    data_bind_serialized_free(wire);
    WorkspaceSearch_clear(&restored);
    WorkspaceSearch_clear(&source);
    data_bind_free(codec);
  }

  it("should round trip native path strings through the generated DataBind codec") {
    DataBind *codec = NULL;
    DataBindError error = DATA_BIND_ERROR_INIT;
    WorkspacePath_t source, restored;
    char *wire = NULL;
    size_t length = 0;
    WorkspacePath_init(&source);
    WorkspacePath_init(&restored);
    check_equal(TurboWorkspace_codec_create(&codec, &error), DATA_BIND_OK);
    source.path = tstr_dup("src/a file.c");
    check_not_null(source.path);
    check_equal(WorkspacePath_to_json(codec, &source, &wire, &length, &error), DATA_BIND_OK);
    check_equal(WorkspacePath_from_json(codec, &restored, wire, length, &error), DATA_BIND_OK);
    check_equal(strcmp(restored.path, source.path), 0);
    data_bind_serialized_free(wire);
    WorkspacePath_clear(&restored);
    WorkspacePath_clear(&source);
    data_bind_free(codec);
  }

  it("should expose the generated schema and reject malformed arguments before file access") {
    char workspace[SALTS_FS_MAX_PATH];
    turbo_coding_tools_config_t config;
    turbo_tool_definition_t definition = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    const json_value_t *properties;
    const json_value_t *required;
    const char *invalid[] = {"{}", "{\"path\":17}", "{\"path\":null}",
                            "{\"path\":\"x\",\"extra\":1}",
                            "{\"path\":\"x\\u0000suffix\"}"};
    size_t i;
    check_equal(cmeta_fs_get_tmpdir(workspace, sizeof(workspace)), 0);
    turbo_coding_tools_config_init(&config);
    config.workspace_root = workspace;
    check_equal(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_get_definition(registry, 0, &definition), TURBO_TOOL_OK);
    check_not_null(definition.parameters_schema);
    properties = json_object_get(definition.parameters_schema, "properties");
    required = json_object_get(definition.parameters_schema, "required");
    check_equal(json_object_size(properties), 1);
    check_equal(strcmp(json_get_string(json_object_get(properties, "path"), "type"), "string"), 0);
    check_equal(json_array_size(required), 1);
    check_equal(strcmp(json_string(json_array_get(required, 0)), "path"), 0);
    check_equal(json_get_bool(definition.parameters_schema, "additionalProperties", 1), 0);
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      char *output = NULL;
      json_value_t *result;
      check_equal(turbo_tool_registry_execute(registry, "fs.read", invalid[i], &output),
                  TURBO_TOOL_OK);
      check_not_null(output);
      result = json_parse(output, strlen(output));
      check_not_null(result);
      check_equal(strcmp(json_get_string(result, "status"), "rejected"), 0);
      check_equal(strcmp(json_get_string(result, "summary"), "invalid workspace path request"), 0);
      check_equal(json_get_bool(result, "ok", 1), 0);
      json_free(result);
      free(output);
    }
    {
      tstr large = tstr_new_len(NULL, 64u * 1024u + 1u);
      json_value_t *arguments = json_create_object();
      json_value_t *result = NULL;
      check_not_null(large);
      memset(large, 'a', tstr_len(large));
      check_equal(turbo_runtime_json_object_set_string(arguments, "path", large),
                  TURBO_RUNTIME_JSON_OK);
      check_equal(definition.json_value_handler(arguments, &result, definition.user_data), 0);
      check_not_null(result);
      check_equal(strcmp(json_get_string(result, "summary"), "invalid workspace path request"), 0);
      json_free(result);
      json_free(arguments);
      tstr_free(large);
    }
    turbo_tool_registry_destroy(registry);
  }
}
