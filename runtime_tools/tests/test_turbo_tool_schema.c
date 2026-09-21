#include "tinytest.h"
#include "turbo_tool_registry.h"
#include "turbo_tool_schema.h"

static int fake_tool_handler(const char *arguments_json, char **out_output, void *user_data) {
  (void)arguments_json;
  (void)out_output;
  (void)user_data;
  return 0;
}

static int fake_tool_json_value_handler(const json_value_t *arguments,
                                  json_value_t **out_result,
                                  void *user_data) {
  json_value_t *result;

  (void)arguments;
  (void)user_data;
  if (!out_result) {
    return -1;
  }

  result = json_create_object();
  if (!result) {
    return -1;
  }
  if (turbo_runtime_json_object_set(
          result, "ok", json_create_bool(1)) !=
      TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(result);
    return -1;
  }
  *out_result = result;
  return 0;
}

spec("turbo tool schema helpers") {

  describe("registry definition views") {

    it("should expose borrowed tool definitions by index") {
      turbo_tool_registry_t *registry = turbo_tool_registry_create();
      turbo_tool_definition_t definition = {
          .name = "sum",
          .description = "add two numbers",
          .parameters_json = "{\"type\":\"object\"}",
          .parameters_schema = NULL,
          .strict = 1,
          .handler = fake_tool_handler,
          .json_value_handler = NULL,
          .user_data = NULL,
          .user_data_free = NULL,
      };
      turbo_tool_definition_t view = {0};

      check_not_null(registry);
      check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
      check_int_eq(turbo_tool_registry_get_definition(registry, 0, &view), TURBO_TOOL_OK);
      check_str_eq(view.name, "sum");
      check_str_eq(view.description, "add two numbers");
      check_str_eq(view.parameters_json, "{\"type\":\"object\"}");
      check_ptr_eq(view.parameters_schema, NULL);
      check_int_eq(view.strict, 1);
      check_ptr_eq(view.handler, fake_tool_handler);
      check_ptr_eq(view.json_value_handler, NULL);

      turbo_tool_registry_destroy(registry);
    }

    it("should execute SaltsUtils JSON-native tools through the registry") {
      turbo_tool_registry_t *registry = turbo_tool_registry_create();
      turbo_tool_definition_t definition = {
          .name = "sum",
          .description = "add two numbers",
          .parameters_json = "{\"type\":\"object\"}",
          .parameters_schema = NULL,
          .strict = 1,
          .handler = NULL,
          .json_value_handler = fake_tool_json_value_handler,
          .user_data = NULL,
          .user_data_free = NULL,
      };
      json_value_t *args = json_create_object();
      json_value_t *result = NULL;

      check_not_null(registry);
      check_not_null(args);
      check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
      check_int_eq(turbo_tool_registry_execute_json_value(registry, "sum", args, &result),
                   TURBO_TOOL_OK);
      check_not_null(result);
      check_true(turbo_runtime_json_value_as_bool(
          json_object_get(result, "ok"), 0));

      turbo_runtime_json_destroy(result);
      turbo_runtime_json_destroy(args);
      turbo_tool_registry_destroy(registry);
    }

    it("should execute tools by their OpenAI-compatible sanitized name") {
      turbo_tool_registry_t *registry = turbo_tool_registry_create();
      turbo_tool_definition_t definition = {
          .name = "codex.files.read",
          .description = "read a file",
          .parameters_json = "{\"type\":\"object\"}",
          .parameters_schema = NULL,
          .strict = 1,
          .handler = fake_tool_handler,
          .json_value_handler = NULL,
          .user_data = NULL,
          .user_data_free = NULL,
      };
      char *output = NULL;

      check_not_null(registry);
      check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
      check_int_eq(turbo_tool_registry_execute(registry, "codex_files_read", "{}",
                                               &output),
                   TURBO_TOOL_OK);
      check_null(output);

      turbo_tool_registry_destroy(registry);
    }

    it("should reject ambiguous OpenAI-compatible sanitized execution names") {
      turbo_tool_registry_t *registry = turbo_tool_registry_create();
      turbo_tool_definition_t dotted = {
          .name = "codex.files",
          .description = "dotted",
          .parameters_json = "{\"type\":\"object\"}",
          .parameters_schema = NULL,
          .strict = 1,
          .handler = fake_tool_handler,
          .json_value_handler = NULL,
          .user_data = NULL,
          .user_data_free = NULL,
      };
      turbo_tool_definition_t slashed = dotted;
      char *output = NULL;

      slashed.name = "codex/files";
      slashed.description = "slashed";

      check_not_null(registry);
      check_int_eq(turbo_tool_registry_add(registry, &dotted), TURBO_TOOL_OK);
      check_int_eq(turbo_tool_registry_add(registry, &slashed), TURBO_TOOL_OK);
      check_int_eq(turbo_tool_registry_execute(registry, "codex_files", "{}",
                                               &output),
                   TURBO_TOOL_NOT_FOUND);
      check_null(output);

      turbo_tool_registry_destroy(registry);
    }

    it("should accept schema-native tool definitions without parameters_json") {
      turbo_tool_registry_t *registry = turbo_tool_registry_create();
      json_value_t *schema =
          json_create_object();
      turbo_tool_definition_t definition = {
          .name = "sum",
          .description = "add two numbers",
          .parameters_json = NULL,
          .parameters_schema = schema,
          .strict = 1,
          .handler = NULL,
          .json_value_handler = fake_tool_json_value_handler,
          .user_data = NULL,
          .user_data_free = NULL,
      };
      turbo_tool_definition_t view = {0};

      check_not_null(registry);
      check_not_null(schema);
      check_int_eq(turbo_runtime_json_object_set(
                       schema, "type",
                       json_create_string("object")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
      check_int_eq(turbo_tool_registry_get_definition(registry, 0, &view), TURBO_TOOL_OK);
      check_not_null(view.parameters_json);
      check_not_null(view.parameters_schema);
      check_str_eq(turbo_runtime_json_value_as_string(
                       json_object_get(view.parameters_schema, "type")),
                   "object");

      turbo_runtime_json_destroy(schema);
      turbo_tool_registry_destroy(registry);
    }
  }

  describe("provider schema builders") {

    it("should build openai chat tool payloads from registry definitions") {
      turbo_tool_registry_t *registry = turbo_tool_registry_create();
      turbo_tool_definition_t definition = {
          .name = "sum",
          .description = "add two numbers",
          .parameters_json = "{\"type\":\"object\"}",
          .parameters_schema = NULL,
          .strict = 1,
          .handler = fake_tool_handler,
          .json_value_handler = NULL,
          .user_data = NULL,
          .user_data_free = NULL,
      };
      json_value_t *tools;
      json_value_t *tool;
      json_value_t *function_object;

      check_not_null(registry);
      check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);

      tools = turbo_tool_schema_build_openai_chat_tools(registry);
      check_not_null(tools);
      check_size_eq(json_array_size(tools), 1);

      tool = json_array_get(tools, 0);
      function_object = json_object_get(tool, "function");
      check_str_eq(json_get_string(tool, "type"), "function");
      check_str_eq(json_get_string(function_object, "name"), "sum");
      check_true(json_get_bool(function_object, "strict", false));

      json_free(tools);
      turbo_tool_registry_destroy(registry);
    }

    it("should build openai chat tool payloads from schema-native definitions") {
      turbo_tool_registry_t *registry = turbo_tool_registry_create();
      json_value_t *schema =
          json_create_object();
      turbo_tool_definition_t definition = {
          .name = "sum",
          .description = "add two numbers",
          .parameters_json = NULL,
          .parameters_schema = schema,
          .strict = 1,
          .handler = fake_tool_handler,
          .json_value_handler = NULL,
          .user_data = NULL,
          .user_data_free = NULL,
      };
      json_value_t *tools;
      json_value_t *function_object;
      json_value_t *parameters;

      check_not_null(registry);
      check_not_null(schema);
      check_int_eq(turbo_runtime_json_object_set(
                       schema, "type",
                       json_create_string("object")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);

      tools = turbo_tool_schema_build_openai_chat_tools(registry);
      check_not_null(tools);
      function_object = json_object_get(json_array_get(tools, 0), "function");
      check_str_eq(json_get_string(function_object, "name"), "sum");
      parameters = json_object_get(function_object, "parameters");
      check_not_null(parameters);
      check_str_eq(json_get_string(parameters, "type"), "object");

      json_free(tools);
      turbo_runtime_json_destroy(schema);
      turbo_tool_registry_destroy(registry);
    }

    it("should omit function.strict in compatible chat mode") {
      json_value_t *tool = turbo_tool_schema_build_openai_chat_tool_definition(
          "sum", "add two numbers", "{\"type\":\"object\"}", 1, 1);
      json_value_t *function_object;

      check_not_null(tool);
      function_object = json_object_get(tool, "function");
      check_not_null(function_object);
      check_false(json_get_bool(function_object, "strict", false));
      check_not_null(json_object_get(function_object, "parameters"));

      json_free(tool);
    }

    it("should sanitize compatible chat tool names") {
      turbo_tool_registry_t *registry = turbo_tool_registry_create();
      turbo_tool_definition_t definition = {
          .name = "codex.files.read",
          .description = "read a file",
          .parameters_json = "{\"type\":\"object\"}",
          .parameters_schema = NULL,
          .strict = 1,
          .handler = fake_tool_handler,
          .json_value_handler = NULL,
          .user_data = NULL,
          .user_data_free = NULL,
      };
      json_value_t *tools;
      json_value_t *tool;
      json_value_t *function_object;

      check_not_null(registry);
      check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);

      tools = turbo_tool_schema_build_openai_compatible_chat_tools(registry);
      check_not_null(tools);
      check_size_eq(json_array_size(tools), 1);
      tool = json_array_get(tools, 0);
      function_object = json_object_get(tool, "function");
      check_str_eq(json_get_string(function_object, "name"), "codex_files_read");
      check_false(json_get_bool(function_object, "strict", false));

      json_free(tools);
      turbo_tool_registry_destroy(registry);
    }

    it("should build compatible chat tool payloads from schema-native definitions") {
      turbo_tool_registry_t *registry = turbo_tool_registry_create();
      json_value_t *schema =
          json_create_object();
      turbo_tool_definition_t definition = {
          .name = "codex.files.read",
          .description = "read a file",
          .parameters_json = NULL,
          .parameters_schema = schema,
          .strict = 1,
          .handler = fake_tool_handler,
          .json_value_handler = NULL,
          .user_data = NULL,
          .user_data_free = NULL,
      };
      json_value_t *tools;
      json_value_t *function_object;
      json_value_t *parameters;

      check_not_null(registry);
      check_not_null(schema);
      check_int_eq(turbo_runtime_json_object_set(
                       schema, "type",
                       json_create_string("object")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);

      tools = turbo_tool_schema_build_openai_compatible_chat_tools(registry);
      check_not_null(tools);
      function_object = json_object_get(json_array_get(tools, 0), "function");
      check_str_eq(json_get_string(function_object, "name"), "codex_files_read");
      parameters = json_object_get(function_object, "parameters");
      check_not_null(parameters);
      check_str_eq(json_get_string(parameters, "type"), "object");

      json_free(tools);
      turbo_runtime_json_destroy(schema);
      turbo_tool_registry_destroy(registry);
    }

    it("should reject compatible chat tool names that collide after sanitizing") {
      turbo_tool_registry_t *registry = turbo_tool_registry_create();
      turbo_tool_definition_t dotted = {
          .name = "codex.files",
          .description = "dotted",
          .parameters_json = "{\"type\":\"object\"}",
          .parameters_schema = NULL,
          .strict = 1,
          .handler = fake_tool_handler,
          .json_value_handler = NULL,
          .user_data = NULL,
          .user_data_free = NULL,
      };
      turbo_tool_definition_t slashed = dotted;
      json_value_t *tools;

      slashed.name = "codex/files";
      slashed.description = "slashed";

      check_not_null(registry);
      check_int_eq(turbo_tool_registry_add(registry, &dotted), TURBO_TOOL_OK);
      check_int_eq(turbo_tool_registry_add(registry, &slashed), TURBO_TOOL_OK);
      tools = turbo_tool_schema_build_openai_compatible_chat_tools(registry);
      check_null(tools);

      turbo_tool_registry_destroy(registry);
    }

    it("should parse parameter schema into a bind tree") {
      json_value_t *schema =
          turbo_tool_schema_parse_parameters_json_value("{\"type\":\"object\"}", 1);

      check_not_null(schema);
      check_str_eq(turbo_runtime_json_value_as_string(
                       json_object_get(schema, "type")),
                   "object");
      check_true(turbo_runtime_json_value_as_bool(
          json_object_get(schema, "additionalProperties"), 1) == 0);

      turbo_runtime_json_destroy(schema);
    }

    it("should export registry definitions into a SaltsUtils JSON-native schema surface") {
      turbo_tool_registry_t *registry = turbo_tool_registry_create();
      json_value_t *schema =
          json_create_object();
      turbo_tool_definition_t definition = {
          .name = "sum",
          .description = "add two numbers",
          .parameters_json = NULL,
          .parameters_schema = schema,
          .strict = 1,
          .handler = fake_tool_handler,
          .json_value_handler = NULL,
          .user_data = NULL,
          .user_data_free = NULL,
      };
      json_value_t *tools = NULL;
      const json_value_t *tool = NULL;
      const json_value_t *parameters = NULL;

      check_not_null(registry);
      check_not_null(schema);
      check_int_eq(turbo_runtime_json_object_set(
                       schema, "type",
                       json_create_string("object")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);

      tools = turbo_tool_schema_build_registry_json_value(registry);
      check_not_null(tools);
      check_size_eq(turbo_runtime_json_value_size(tools), 1);
      tool = json_array_get(tools, 0);
      check_str_eq(turbo_runtime_json_value_as_string(
                       json_object_get(tool, "name")),
                   "sum");
      parameters = json_object_get(tool, "parameters");
      check_not_null(parameters);
      check_str_eq(turbo_runtime_json_value_as_string(
                       json_object_get(parameters, "type")),
                   "object");

      turbo_runtime_json_destroy(schema);
      turbo_runtime_json_destroy(tools);
      turbo_tool_registry_destroy(registry);
    }
  }
}
