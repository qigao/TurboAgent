#include "tinytest.h"
#include "turbo_cli.h"
#include "turbo_runtime_json.h"
#include <cmeta_fs.h>
#include <tstr.h>
#include <stdlib.h>
#include <string.h>

static const char review_skill[] =
    "---\nname: repo-review\ndescription: Review code\ntriggers: [review]\n"
    "tools: [fs.read]\ncapabilities: [custom_tools]\n---\nREVIEW_MARKER\n";
static const char build_skill[] =
    "---\nname: repo-build\ndescription: Build code\ntriggers: [build]\n"
    "tools: [shell.exec]\ncapabilities: [shell]\n---\nBUILD_MARKER\n";

typedef struct workspace_model_s {
  const char *root;
  const char *change_path;
  const char *change_text;
  size_t calls;
  int inspect_context;
  int read_only;
  int valid;
} workspace_model_t;

static int workspace_write(const char *root, const char *relative, const char *text, size_t size) {
  char path[SALTS_FS_MAX_PATH];
  if (cmeta_fs_path_join(path, sizeof(path), root, relative) != 0) return -1;
  return tt_write_file(path, text, size);
}

static int workspace_model(const char *request, char **out, void *data) {
  static const char response[] =
      "{\"id\":\"workspace-response\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"completed\"}]}]}";
  workspace_model_t *model = data;
  if (model->inspect_context) {
    json_value_t *wire = json_parse(request, strlen(request));
    const char *instructions = wire ? json_get_string(wire, "instructions") : NULL;
    const char *root_marker = model->calls ? "ROOT_NEW" : "ROOT_OLD";
    const char *skill_marker = model->calls ? "BUILD_MARKER" : "REVIEW_MARKER";
    const char *other_marker = model->calls ? "REVIEW_MARKER" : "BUILD_MARKER";
    const char *match = instructions ? strstr(instructions, root_marker) : NULL;
    model->valid = model->valid && match && !strstr(match + strlen(root_marker), root_marker) &&
                   strstr(instructions, "BASE_MARKER") && strstr(instructions, skill_marker) &&
                   !strstr(instructions, other_marker) && !strstr(instructions, "NESTED_MARKER") &&
                   (!model->calls || !strstr(instructions, "ROOT_OLD")) &&
                   strstr(request, "fs.read") &&
                   (model->read_only ? !strstr(request, "fs.write") && !strstr(request, "shell.exec")
                                     : strstr(request, "fs.write") && strstr(request, "shell.exec"));
    json_free(wire);
  }
  if (!model->calls && model->change_path &&
      workspace_write(model->root, model->change_path, model->change_text,
                      strlen(model->change_text)) != 0) return -1;
  ++model->calls;
  *out = malloc(sizeof(response));
  if (!*out) return -1;
  memcpy(*out, response, sizeof(response));
  return 0;
}

spec("CLI workspace instructions") {
  static char *root;
  static FILE *input, *output, *diagnostics;
  static turbo_cli_options_t options;
  static turbo_agent_config_t config;
  static workspace_model_t model;

  before_each() {
    char path[SALTS_FS_MAX_PATH];
    root = tt_make_temp_dir("turbo_cli_workspace");
    input = tmpfile(); output = tmpfile(); diagnostics = tmpfile();
    check_not_null(root); check_not_null(input); check_not_null(output); check_not_null(diagnostics);
    check_equal(cmeta_fs_path_join(path, sizeof(path), root, "skills"), 0);
    check_equal(cmeta_fs_mkdir(path, 0700), 0);
    options = (turbo_cli_options_t){.workspace = root, .prompt = "review"};
    model = (workspace_model_t){.root = root, .valid = 1};
    config = (turbo_agent_config_t){.model = "workspace-test", .instructions = "BASE_MARKER",
                                   .transport_fn = workspace_model, .transport_user_data = &model};
  }
  after_each() {
    if (input) fclose(input);
    if (output) fclose(output);
    if (diagnostics) fclose(diagnostics);
    input = output = diagnostics = NULL;
    if (root) { tt_remove_tree(root); free(root); root = NULL; }
  }

  it("should refresh root instructions and reselect skills without duplicating old context") {
    char nested[SALTS_FS_MAX_PATH];
    check_equal(workspace_write(root, "AGENTS.md", "ROOT_OLD", 8), 0);
    check_equal(workspace_write(root, "skills/review.md", review_skill, sizeof(review_skill) - 1), 0);
    check_equal(workspace_write(root, "skills/build.md", build_skill, sizeof(build_skill) - 1), 0);
    check_equal(cmeta_fs_path_join(nested, sizeof(nested), root, "nested"), 0);
    check_equal(cmeta_fs_mkdir(nested, 0700), 0);
    check_equal(workspace_write(root, "nested/AGENTS.md", "NESTED_MARKER", 13), 0);
    model.inspect_context = 1;
    model.change_path = "AGENTS.md"; model.change_text = "ROOT_NEW";
    options.prompt = NULL;
    fputs("review\nbuild\n/quit\n", input); rewind(input);
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_equal(model.calls, 2); check_true(model.valid);
  }

  it("should allow a read-only skill without exposing write or shell tools") {
    check_equal(workspace_write(root, "AGENTS.md", "ROOT_OLD", 8), 0);
    check_equal(workspace_write(root, "skills/review.md", review_skill, sizeof(review_skill) - 1), 0);
    options.read_only = 1;
    model.inspect_context = 1; model.read_only = 1;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_equal(model.calls, 1); check_true(model.valid);
  }

  it("should deny shell and patch skills in read-only mode before making a model request") {
    static const char patch_skill[] =
        "---\nname: patch\ndescription: Edit code\ntriggers: [build]\n"
        "tools: [fs.edit]\ncapabilities: [patch]\n---\nEdit.\n";
    const char *skills[] = {build_skill, patch_skill};
    options.prompt = "build"; options.read_only = 1;
    for (size_t i = 0; i < sizeof(skills) / sizeof(skills[0]); ++i) {
      check_equal(workspace_write(root, "skills/build.md", skills[i], strlen(skills[i])), 0);
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 1);
      check_equal(model.calls, 0);
    }
  }

  it("should reject a selected skill whose tool is unavailable") {
    static const char skill[] = "---\nname: missing\ndescription: Missing tool\n"
                                "triggers: [review]\ntools: [missing.tool]\n---\nInspect.\n";
    check_equal(workspace_write(root, "skills/missing.md", skill, sizeof(skill) - 1), 0);
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 1);
    check_equal(model.calls, 0);
  }

  it("should reject malformed skill metadata before making a model request") {
    static const char malformed[] = "---\nname: unfinished\n";
    check_equal(workspace_write(root, "skills/broken.md", malformed, sizeof(malformed) - 1), 0);
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 1);
    check_equal(model.calls, 0);
  }

  it("should reject oversized root instructions before making a model request") {
    const size_t size = 256 * 1024 + 1;
    tstr text = tstr_new_len(NULL, size);
    int rc;
    check_not_null(text);
    memset(text, 'x', size);
    rc = workspace_write(root, "AGENTS.md", text, size);
    tstr_free(text);
    check_equal(rc, 0);
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 1);
    check_equal(model.calls, 0);
  }

  it("should stop on refresh failure instead of running with stale instructions") {
    check_equal(workspace_write(root, "skills/review.md", review_skill, sizeof(review_skill) - 1), 0);
    model.change_path = "skills/review.md";
    model.change_text = "---\nname: unfinished\n";
    options.prompt = NULL;
    fputs("review\nreview\n/quit\n", input); rewind(input);
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 1);
    check_equal(model.calls, 1);
  }
}
