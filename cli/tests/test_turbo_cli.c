#include "tinytest.h"
#include "turbo_cli.h"
#include "turbo_runtime_json.h"
#include <cmeta_fs.h>
#include <cmeta_process.h>
#include <cmeta_uuid.h>
#include <tstr.h>
#include <stdlib.h>
#include <string.h>

typedef struct cli_model_s {
  size_t calls;
  int chronological;
  int read_only;
  int model_overridden;
} cli_model_t;

static int cli_model(const char *request, char **out, void *data) {
  cli_model_t *model = data;
  const char *tools[] = {"fs.write", "fs.edit", "fs.read", "shell.exec"};
  const char *args[] = {"{\"path\":\"note.txt\",\"content\":\"alpha\"}",
                        "{\"path\":\"note.txt\",\"old_text\":\"alpha\",\"new_text\":\"beta\"}",
                        "{\"path\":\"note.txt\"}", "{\"command\":\"echo verified\"}"};
  json_value_t *response = json_create_object(), *outputs = json_create_array(), *item;
  model->model_overridden = strstr(request, "cli-model-override") != NULL;
  if (model->read_only) {
    if (!model->calls)
      model->chronological = strstr(request, "fs.read") && strstr(request, "fs.search") &&
                             !strstr(request, "fs.write") && !strstr(request, "shell.exec");
    else
      model->chronological = model->chronological && strstr(request, "needle_marker");
  } else if (model->calls == 5) {
    const char *initial = strstr(request, "initial_task");
    const char *answer = strstr(request, "work_completed");
    const char *follow_up = strstr(request, "followup_task");
    const char *tool_result = strstr(request, "beta");
    model->chronological = initial && answer && follow_up && tool_result &&
                          initial < answer && tool_result < answer && answer < follow_up;
  }
  if (model->read_only && model->calls == 0) {
    item = json_create_object();
    json_object_set_string(item, "type", "function_call");
    json_object_set_string(item, "name", "fs.search");
    json_object_set_string(item, "call_id", "search_call");
    json_object_set_string(item, "arguments", "{\"path\":\".\",\"pattern\":\"needle\"}");
  } else if (!model->read_only && model->calls < 4) {
    static const char *ids[] = {"write_call", "edit_call", "read_call", "exec_call"};
    item = json_create_object();
    json_object_set_string(item, "type", "function_call");
    json_object_set_string(item, "name", tools[model->calls]);
    json_object_set_string(item, "call_id", ids[model->calls]);
    json_object_set_string(item, "arguments", args[model->calls]);
  } else {
    const char message[] = "{\"type\":\"message\",\"role\":\"assistant\","
                           "\"content\":[{\"type\":\"output_text\",\"text\":\"work_completed\"}]}";
    item = json_parse(message, sizeof(message) - 1);
  }
  json_array_add(outputs, item);
  json_object_add(response, "output", outputs);
  json_object_set_string(response, "id", "response_cli");
  *out = json_serialize(response, NULL);
  json_free(response);
  ++model->calls;
  return *out ? 0 : -1;
}

static tstr cli_workspace(void) {
  char temp[SALTS_FS_MAX_PATH], path[SALTS_FS_MAX_PATH], uuid[SALTS_UUID_STRING_SIZE];
  cmeta_uuid_t id;
  if (cmeta_fs_get_tmpdir(temp, sizeof(temp)) != 0 || cmeta_uuid_v4_generate(&id) != 0 ||
      cmeta_uuid_format(&id, uuid, sizeof(uuid)) != 0 ||
      cmeta_fs_path_join(path, sizeof(path), temp, uuid) != 0 || cmeta_fs_mkdir(path, 0700) != 0) return NULL;
  return tstr_dup(path);
}

spec("coding agent CLI") {
  static cmeta_process_t *process;
  after_each() { cmeta_process_destroy(process); process = NULL; }

  it("should use CmdParser help and syntax errors at the process boundary") {
    static const struct {
      const char *argument;
      const char *message;
    } cases[] = {
      {"--help", "usage:"}, {"-h", "usage:"},
      {"--prompt", "requires a value"}, {"-p", "requires a value"},
      {"--workspace", "requires a value"}, {"-C", "requires a value"},
      {"--model", "requires a value"}, {"--unknown", "unsupported option"},
      {"unexpected", "unexpected positional argument"},
      {"--read-only=true", "cannot take a value"}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      const char *args[] = {cases[i].argument, NULL};
      cmeta_process_options_t options;
      cmeta_process_result_t result;
      char text[4096];
      size_t count = 0;
      cmeta_process_options_init(&options);
      options.program = TURBO_CLI_EXECUTABLE; options.args = args;
      options.timeout_ms = 5000; options.max_output_bytes = sizeof(text) - 1;
      check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
      check_equal(cmeta_process_wait(process, &result), SALTS_OK);
      check_equal(result.state, SALTS_PROCESS_EXITED);
      check_equal(result.exit_code, 1);
      check_equal(cmeta_process_read_stdout(process, text, sizeof(text) - 1, &count), SALTS_OK);
      text[count] = 0;
      check_not_null(strstr(text, cases[i].message));
      check_not_null(strstr(text, "-C, --workspace"));
      check_not_null(strstr(text, "-p, --prompt"));
      check_not_null(strstr(text, "--model"));
      check_not_null(strstr(text, "--read-only"));
      check_equal(cmeta_process_read_stderr(process, text, sizeof(text) - 1, &count), SALTS_EOF);
      check_equal(count, 0);
      cmeta_process_destroy(process); process = NULL;
    }
  }

  it("should parse defaults and long equals options through CmdParser") {
    char *defaults[] = {"turboagent", NULL};
    char *arguments[] = {"turboagent", "--workspace=space directory", "--prompt=检查 tests",
                         "--model=cli-model", "--read-only", NULL};
    turbo_cli_options_t options;
    turbo_cli_parse_options(1, defaults, &options);
    check_equal(options.workspace, "."); check_null(options.prompt);
    check_null(options.model); check_false(options.read_only);
    turbo_cli_parse_options(5, arguments, &options);
    check_equal(options.workspace, "space directory");
    check_equal(options.prompt, "检查 tests");
    check_equal(options.model, "cli-model"); check_true(options.read_only);
  }

  it("should run generated tools and preserve chronological conversation across prompts") {
    tstr root = cli_workspace();
    char *argv[] = {"turboagent", "-C", root, NULL};
    turbo_cli_options_t options;
    turbo_agent_config_t config = {0};
    cli_model_t model = {0};
    FILE *input = tmpfile(), *output = tmpfile(), *diagnostics = tmpfile();
    char path[SALTS_FS_MAX_PATH];
    cmeta_fs_buf_t file = {0};
    volatile sig_atomic_t interrupted = 0;
    check_not_null(root); check_not_null(input); check_not_null(output); check_not_null(diagnostics);
    config.model = "test-model"; config.transport_fn = cli_model; config.transport_user_data = &model;
    fputs("initial_task\nfollowup_task\n/quit\n", input); rewind(input);
    turbo_cli_parse_options(3, argv, &options);
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, &interrupted), 0);
    check_equal(model.calls, 6); check_true(model.chronological);
    check_equal(cmeta_fs_path_join(path, sizeof(path), root, "note.txt"), 0);
    check_equal(cmeta_fs_read_file(path, &file), 0);
    check_equal(file.len, 4); check_equal(memcmp(file.base, "beta", 4), 0);
    cmeta_fs_buf_free(&file);
    check_equal(cmeta_fs_unlink(path), 0);
    check_equal(cmeta_fs_rmdir(root), 0);
    fclose(input); fclose(output); fclose(diagnostics); tstr_free(root);
  }

  it("should apply parsed prompt workspace model and read-only options to a run") {
    tstr root = cli_workspace();
    char *argv[] = {"turboagent", "--workspace", root, "--read-only", "-p", "inspect",
                    "--model", "cli-model-override", NULL};
    turbo_cli_options_t options;
    turbo_agent_config_t config = {0};
    cli_model_t model = {.read_only = 1};
    FILE *input = tmpfile(), *output = tmpfile(), *diagnostics = tmpfile();
    char path[SALTS_FS_MAX_PATH];
    cmeta_fs_buf_t contents = cmeta_fs_buf_init("needle_marker\n", 14);
    check_equal(cmeta_fs_path_join(path, sizeof(path), root, "needle.txt"), 0);
    check_equal(cmeta_fs_write_file(path, &contents), 0);
    config.model = "test-model"; config.transport_fn = cli_model; config.transport_user_data = &model;
    turbo_cli_parse_options(8, argv, &options);
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_equal(model.calls, 2); check_true(model.chronological);
    check_true(model.model_overridden);
    options.prompt = "";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 2);
    check_equal(model.calls, 2);
    check_equal(cmeta_fs_unlink(path), 0);
    check_equal(cmeta_fs_rmdir(root), 0);
    fclose(input); fclose(output); fclose(diagnostics); tstr_free(root);
  }
}
