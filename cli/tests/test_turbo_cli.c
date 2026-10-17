#include "tinytest.h"
#include "turbo_cli.h"
#include "turbo_runtime_json.h"
#include <cmeta_fs.h>
#include <cmeta_process.h>
#include <cmeta_uuid.h>
#include <tstr.h>
#include <fmt.h>
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
      model->chronological = strstr(request, "fs_read") && strstr(request, "fs_search") &&
                             !strstr(request, "fs_write") && !strstr(request, "shell_exec");
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

spec("CLI discovery of executor-produced tool journals") {
  static char *directory;
  static tstr database;
  static FILE *input, *output, *diagnostics;
  static json_value_t *view;
  static cmeta_fs_buf_t file;
  before_each() {
    directory = tt_make_temp_dir("cli-journal-discovery"); check_not_null(directory);
    database = tstr_format("{}/session.db", directory); check_not_null(database);
    input = tmpfile(); output = tmpfile(); diagnostics = tmpfile();
    check_not_null(input); check_not_null(output); check_not_null(diagnostics);
  }
  after_each() {
    json_free(view); view = NULL;
    cmeta_fs_buf_free(&file); file = (cmeta_fs_buf_t){0};
    if (input) fclose(input);
    if (output) fclose(output);
    if (diagnostics) fclose(diagnostics);
    input = output = diagnostics = NULL;
    tstr_free(database); database = NULL;
    if (directory) tt_remove_tree(directory);
    free(directory); directory = NULL;
  }
  it("discovers write edit read and shell evidence without another model or tool run") {
    turbo_cli_options_t options = {.workspace = directory, .prompt = "initial_task",
                                   .session_db = database, .trust_profile = "trusted-local"};
    turbo_agent_config_t config = {0};
    cli_model_t model = {0};
    char buffer[32768], path[SALTS_FS_MAX_PATH];
    size_t bytes, count = 0;
    const json_value_t *nodes;
    config.model = "test-model"; config.transport_fn = cli_model; config.transport_user_data = &model;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_equal(model.calls, 5u);
    fclose(output); output = tmpfile(); check_not_null(output);
    options.prompt = NULL; options.diagram = "json"; options.diagram_journals = "all";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_equal(model.calls, 5u);
    rewind(output); bytes = fread(buffer, 1, sizeof(buffer) - 1, output); buffer[bytes] = 0;
    check_less(bytes, sizeof(buffer) - 1);
    view = json_parse(buffer, bytes); check_not_null(view);
    check_equal(json_get_int(view, "journal_count", -1), 4);
    check_equal(json_get_string(view, "session_phase"), "ready");
    check_equal(json_get_string(view, "journal_scope"), "published_run_records");
    nodes = json_object_get(view, "nodes");
    for (size_t i = 0; i < json_array_size(nodes); ++i) {
      const json_value_t *node = json_array_get(nodes, i);
      if (strcmp(json_get_string(node, "collection"), "agent_tool_journal")) continue;
      ++count;
      check_equal(json_get_string(node, "phase"), "committed");
      check_equal(json_get_int(node, "status", -1), 0);
      check_equal(json_get_string(node, "freshness"), "current_record");
      check_null(json_object_get(node, "arguments")); check_null(json_object_get(node, "output"));
      check_null(json_object_get(node, "detail"));
    }
    check_equal(count, 4u);
    check_contains(buffer, "write_call"); check_contains(buffer, "edit_call");
    check_contains(buffer, "read_call"); check_contains(buffer, "exec_call");
    check_equal(cmeta_fs_path_join(path, sizeof(path), directory, "note.txt"), 0);
    check_equal(cmeta_fs_read_file(path, &file), 0);
    check_equal(file.len, 4u); check_equal(memcmp(file.base, "beta", 4), 0);
  }
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
      {"--model", "requires a value"}, {"--workflow", "requires a value"}, {"--unknown", "unsupported option"},
      {"--session-db", "requires a value"}, {"--resume=true", "cannot take a value"},
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
      check_not_null(strstr(text, "--trust-profile"));
      check_equal(cmeta_process_read_stderr(process, text, sizeof(text) - 1, &count), SALTS_EOF);
      check_equal(count, 0);
      cmeta_process_destroy(process); process = NULL;
    }
  }

  it("should parse defaults and long equals options through CmdParser") {
    char *defaults[] = {"turboagent", NULL};
    char *arguments[] = {"turboagent", "--workspace=space directory", "--prompt=检查 tests",
                         "--model=cli-model", "--read-only", "--session-db=session.db", "--resume", NULL};
    turbo_cli_options_t options;
    turbo_cli_parse_options(1, defaults, &options);
    check_equal(options.workspace, "."); check_null(options.prompt);
    check_null(options.model); check_false(options.read_only); check_null(options.trust_profile);
    check_null(options.session_db); check_false(options.resume);
    check_null(options.diagram);
    check_null(options.diagram_focus); check_null(options.diagram_direction); check_null(options.diagram_depth);
    check_null(options.diagram_journals);
    check_false(options.diagram_origins);
    check_null(options.diagram_to);
    check_false(options.diagram_plan_steps);
    check_null(options.workflow);
    turbo_cli_parse_options(7, arguments, &options);
    check_equal(options.session_db, "session.db"); check_true(options.resume);
    check_equal(options.workspace, "space directory");
    check_equal(options.prompt, "检查 tests");
    check_equal(options.model, "cli-model"); check_true(options.read_only);
    {
      char *workflow_args[] = {"turboagent", "--mode=general", "--workflow=engineering",
                                "--trust-profile=trusted-local", NULL};
      turbo_cli_parse_options(4, workflow_args, &options);
      check_equal(options.mode, "general"); check_equal(options.workflow, "engineering");
      check_equal(options.trust_profile, "trusted-local");
    }
    {
      char *diagram_args[] = {"turboagent", "--session-db=session.db", "--diagram=json", NULL};
      turbo_cli_parse_options(3, diagram_args, &options);
      check_equal(options.diagram, "json"); check_equal(options.session_db, "session.db");
    }
    {
      char *path_args[] = {"turboagent", "--diagram=mermaid", "--diagram-focus=run:r",
          "--diagram-to=journal:j", "--diagram-depth=4", "--diagram-plan-steps", NULL};
      turbo_cli_parse_options(6, path_args, &options);
      check_true(options.diagram_plan_steps);
      check_equal(options.diagram_focus, "run:r"); check_equal(options.diagram_to, "journal:j");
      check_equal(options.diagram_depth, "4");
    }
    {
      char *focus_args[] = {"turboagent", "--session-db=session.db", "--diagram=mermaid",
          "--diagram-focus=checkpoint:c1", "--diagram-direction=incoming", "--diagram-depth=2",
          "--diagram-journals=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", NULL};
      turbo_cli_parse_options(7, focus_args, &options);
      check_equal(options.diagram_focus, "checkpoint:c1");
      check_equal(options.diagram_direction, "incoming"); check_equal(options.diagram_depth, "2");
      check_equal(options.diagram_journals, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    }
    {
      char *discover_args[] = {"turboagent", "--session-db=session.db", "--diagram=json", "--diagram-journals", "all", "--diagram-origins", NULL};
      turbo_cli_parse_options(6, discover_args, &options);
      check_true(options.diagram_origins);
      check_equal(options.diagram_journals, "all");
      check_equal(options.diagram, "json"); check_equal(options.session_db, "session.db");
    }
  }

  it("should run generated tools and preserve chronological conversation across prompts") {
    tstr root = cli_workspace();
    char *argv[] = {"turboagent", "-C", root, "--trust-profile=trusted-local", NULL};
    turbo_cli_options_t options;
    turbo_agent_config_t config = {0};
    cli_model_t model = {0};
    FILE *input = tmpfile(), *output = tmpfile(), *diagnostics = tmpfile();
    char path[SALTS_FS_MAX_PATH];
    cmeta_fs_buf_t file = {0};
    atomic_int interrupted = 0;
    check_not_null(root); check_not_null(input); check_not_null(output); check_not_null(diagnostics);
    config.model = "test-model"; config.transport_fn = cli_model; config.transport_user_data = &model;
    fputs("initial_task\nfollowup_task\n/quit\n", input); rewind(input);
    turbo_cli_parse_options(4, argv, &options);
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

  it("should reject implicit approval and contradictory trusted-local authority before any model call") {
    tstr root = cli_workspace();
    turbo_cli_options_t options = {.workspace = root, .prompt = "inspect"};
    turbo_agent_config_t config = {0};
    cli_model_t model = {0};
    FILE *input = tmpfile(), *output = tmpfile(), *diagnostics = tmpfile();
    check_not_null(root); check_not_null(input); check_not_null(output); check_not_null(diagnostics);
    config.model = "test-model"; config.transport_fn = cli_model; config.transport_user_data = &model;
    options.trust_profile = "approval";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 2);
    check_equal(model.calls, 0u);
    options.trust_profile = "trusted-local"; options.read_only = 1;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 2);
    check_equal(model.calls, 0u);
    options.trust_profile = "invalid"; options.read_only = 0;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 2);
    check_equal(model.calls, 0u);
    fclose(input); fclose(output); fclose(diagnostics);
    check_equal(cmeta_fs_rmdir(root), 0); tstr_free(root);
  }

  it("should apply parsed prompt workspace model and read-only options to a run") {
    tstr root = cli_workspace();
    char *argv[] = {"turboagent", "--workspace", root, "-p", "inspect",
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
    turbo_cli_parse_options(7, argv, &options);
    check_null(options.trust_profile); /* Omitted trust defaults to read-only inspection. */
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
