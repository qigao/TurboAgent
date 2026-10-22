#include "tinytest.h"
#include "turbo_agent_harness.h"
#include "turbo_agent_policy.h"
#include "turbo_agent_workspace.h"
#include "../../agent/examples/document_render_tool.h"

#include <cmeta_fs.h>
#include <stdlib.h>

static const document_render_source_t render_sources[] = {
    {"brief", "fixture-v1"}, {"notes", "fixture-v2"}};
static const char render_arguments[] =
    "{\"title\":\"Weekly <report>\",\"passages\":["
    "{\"text\":\"A [draft](link).\",\"source_id\":\"brief\"},"
    "{\"text\":\"中文摘要\",\"source_id\":\"notes\"}]}";
static const char render_plan[] =
    "{\"template_id\":\"inspect\",\"step_id\":\"render-1\",\"tool\":\"document_render\","
    "\"arguments_json\":\"{\\\"title\\\":\\\"Report\\\",\\\"passages\\\":["
    "{\\\"text\\\":\\\"Draft\\\",\\\"source_id\\\":\\\"brief\\\"}]}\"}";

typedef struct render_probe_s {
  document_render_catalog_t catalog;
  int calls;
  int invalid_result;
} render_probe_t;

static int render_probe_handler(const json_value_t *arguments, json_value_t **result, void *user) {
  render_probe_t *probe = (render_probe_t *)user;
  ++probe->calls;
  if (probe->invalid_result) {
    *result = json_create_string("invalid result");
    return *result ? 0 : -1;
  }
  return document_render_handler(arguments, result, &probe->catalog);
}

static int render_no_transport(const char *request, char **response, void *user) {
  (void)request;
  (void)response;
  (void)user;
  return -1;
}

static turbo_agent_harness_t *render_harness(cmeta_threadpool_t *pool,
                                             turbo_tool_registry_t *registry) {
  turbo_agent_session_config_t session = {0};
  turbo_agent_app_config_t app = {0};
  turbo_agent_harness_config_t config;
  session.thread_id = "document-render-test";
  session.runtime_store = turbo_agent_runtime_store_memory_create();
  session.agent_config.model = "offline-fixture";
  session.agent_config.transport_fn = render_no_transport;
  session.agent_config.tool_registry = registry;
  app.session_config = &session;
  turbo_agent_harness_config_init(&config);
  config.app_config = &app;
  config.executor = pool;
  return turbo_agent_harness_create(&config);
}

static int render_attempt(turbo_agent_harness_t *harness, const char *id,
                           json_value_t **result) {
  static const char *const allowed[] = {"runtime_tools"};
  turbo_agent_compiler_config_t config;
  turbo_agent_compile_diagnostic_t diagnostic = {0};
  turbo_tool_status_t tool_status = TURBO_TOOL_ERROR;
  json_value_t *certificate = NULL;
  int status;
  turbo_agent_compiler_config_init(&config);
  config.allowed_capabilities = allowed;
  config.allowed_capability_count = 1;
  status = turbo_agent_harness_execute_compiled_inspect_attempt_json(
      harness, &config, render_plan, sizeof(render_plan) - 1, NULL, id,
      &diagnostic, &tool_status, result, &certificate);
  json_free(certificate);
  return status;
}

spec("document render Host example") {
  it("renders text with Host revisions and validates its canonical result") {
    document_render_catalog_t catalog = {render_sources, 2};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    json_value_t *arguments = json_parse(render_arguments, sizeof(render_arguments) - 1);
    json_value_t *result = NULL;
    check_not_null(registry);
    check_not_null(arguments);
    check_equal(document_render_register(registry, &catalog), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_execute_json_value(
                    registry, "document_render", arguments, &result), TURBO_TOOL_OK);
    check_equal(json_get_string(result, "markdown"),
                "# Weekly \\<report\\>\n\nA \\[draft\\]\\(link\\)\\.\n\n"
                "Source: brief @ fixture\\-v1\n\n中文摘要\n\nSource: notes @ fixture\\-v2\n\n");
    check_equal(json_get_string(result, "verification"), "reference_identity_only");
    check_equal(json_get_int(result, "passage_count", 0), 2);
    check_equal(turbo_tool_schema_validate_result_json_value(
                    registry, "document_render", result, NULL, 0), TURBO_TOOL_SCHEMA_VALID);
    json_free(result);
    json_free(arguments);
    turbo_tool_registry_destroy(registry);
  }

  it("rejects unknown references and malformed inputs without partial output") {
    static const char *const invalid[] = {
        "{\"title\":\"Report\",\"passages\":[{\"text\":\"Draft\",\"source_id\":\"missing\"}]}",
        "{\"title\":\"Report\",\"passages\":[{\"text\":\"Draft\",\"source_id\":\"brief\",\"revision\":\"forged\"}]}",
        "{\"title\":\"Report\",\"passages\":[]}",
        "{\"title\":\"Report\",\"passages\":[{\"text\":42,\"source_id\":\"brief\"}]}",
        "{\"title\":\"Bad\\nheading\",\"passages\":[{\"text\":\"Draft\",\"source_id\":\"brief\"}]}"};
    document_render_catalog_t catalog = {render_sources, 2};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    size_t i;
    check_not_null(registry);
    check_equal(document_render_register(registry, &catalog), TURBO_TOOL_OK);
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      json_value_t *arguments = json_parse(invalid[i], strlen(invalid[i]));
      json_value_t *result = NULL;
      check_not_null(arguments);
      check_not_equal(turbo_tool_registry_execute_json_value(
                          registry, "document_render", arguments, &result), TURBO_TOOL_OK);
      check_null(result);
      json_free(arguments);
    }
    {
      tstr oversized = tstr_new();
      json_value_t *arguments = json_parse(render_arguments, sizeof(render_arguments) - 1);
      json_value_t *result = NULL;
      check_not_null(oversized);
      check_not_null(arguments);
      for (i = 0; i <= DOCUMENT_RENDER_MAX_TITLE_BYTES; ++i) {
        tstr grown = tstr_cat(oversized, "x");
        check_not_null(grown);
        oversized = grown;
      }
      check_equal(turbo_runtime_json_object_set_string(arguments, "title", oversized),
                  TURBO_RUNTIME_JSON_OK);
      check_not_equal(turbo_tool_registry_execute_json_value(
                          registry, "document_render", arguments, &result), TURBO_TOOL_OK);
      check_null(result);
      tstr_free(oversized);
      json_free(arguments);
    }
    turbo_tool_registry_destroy(registry);
  }

  it("does not publish an ambiguous Host source catalog") {
    static const document_render_source_t duplicates[] = {{"brief", "v1"}, {"brief", "v2"}};
    document_render_catalog_t catalog = {duplicates, 2};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    check_not_null(registry);
    check_equal(document_render_register(registry, &catalog), TURBO_TOOL_INVALID_ARGUMENT);
    check_equal(turbo_tool_registry_count(registry), 0u);
    turbo_tool_registry_destroy(registry);
  }

  it("rejects embedded NUL without accepting a truncated source identity or draft") {
    static const char *const invalid[] = {
        "{\"title\":\"Report\\u0000hidden\",\"passages\":[{\"text\":\"Draft\",\"source_id\":\"brief\"}]}",
        "{\"title\":\"Report\",\"passages\":[{\"text\":\"Draft\\u0000hidden\",\"source_id\":\"brief\"}]}",
        "{\"title\":\"Report\",\"passages\":[{\"text\":\"Draft\",\"source_id\":\"brief\\u0000unknown\"}]}"};
    document_render_catalog_t catalog = {render_sources, 2};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    size_t i;
    check_not_null(registry);
    check_equal(document_render_register(registry, &catalog), TURBO_TOOL_OK);
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      json_value_t *arguments = json_parse(invalid[i], strlen(invalid[i]));
      json_value_t *result = NULL;
      /* A parser may reject this domain input itself. If it preserves NUL,
       * the tool must reject it before C-string matching or rendering. */
      if (arguments) {
        check_not_equal(turbo_tool_registry_execute_json_value(
                            registry, "document_render", arguments, &result), TURBO_TOOL_OK);
      }
      check_null(result);
      json_free(arguments);
    }
    turbo_tool_registry_destroy(registry);
  }

  it("accepts the documented byte and passage limits and rejects excess text") {
    static const char seed[] =
        "{\"title\":\"Report\",\"passages\":[{\"text\":\"Draft\",\"source_id\":\"brief\"}]}";
    document_render_catalog_t catalog = {render_sources, 2};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    json_value_t *arguments = json_parse(seed, sizeof(seed) - 1);
    json_value_t *result = NULL;
    json_value_t *passages;
    tstr title = tstr_new();
    tstr text = tstr_new();
    size_t i;
    check_not_null(registry);
    check_not_null(arguments);
    check_not_null(title);
    check_not_null(text);
    check_equal(document_render_register(registry, &catalog), TURBO_TOOL_OK);
    for (i = 0; i < 160; ++i) {
      tstr grown = tstr_cat(title, "x");
      check_not_null(grown);
      title = grown;
    }
    for (i = 0; i < 2048; ++i) {
      tstr grown = tstr_cat(text, "!");
      check_not_null(grown);
      text = grown;
    }
    passages = json_object_get(arguments, "passages");
    check_equal(turbo_runtime_json_object_set_string(arguments, "title", title), TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_runtime_json_object_set_string(json_array_get(passages, 0), "text", text),
                TURBO_RUNTIME_JSON_OK);
    for (i = 1; i < 8; ++i) {
      json_value_t *copy = json_clone(json_array_get(passages, 0));
      check_not_null(copy);
      check_equal(turbo_runtime_json_array_append(passages, copy), TURBO_RUNTIME_JSON_OK);
    }
    check_equal(turbo_tool_registry_execute_json_value(
                    registry, "document_render", arguments, &result), TURBO_TOOL_OK);
    check_equal(json_get_int(result, "passage_count", 0), 8);
    check_equal(turbo_tool_schema_validate_result_json_value(
                    registry, "document_render", result, NULL, 0), TURBO_TOOL_SCHEMA_VALID);
    json_free(result); result = NULL;
    {
      tstr grown = tstr_cat(text, "!");
      check_not_null(grown);
      text = grown;
    }
    check_equal(turbo_runtime_json_object_set_string(json_array_get(passages, 0), "text", text),
                TURBO_RUNTIME_JSON_OK);
    check_not_equal(turbo_tool_registry_execute_json_value(
                        registry, "document_render", arguments, &result), TURBO_TOOL_OK);
    check_null(result);
    tstr_free(text);
    tstr_free(title);
    json_free(arguments);
    turbo_tool_registry_destroy(registry);
  }

  it("projects the Skill tool only when Host runtime_tools is granted") {
    static const char skill[] =
        "---\nname: document-report\ndescription: Draft a cited report\n"
        "tools: [document_render]\ncapabilities: [runtime_tools]\n---\n"
        "Use Host-selected source IDs; reference identity is not factual verification.\n";
    char *root = tt_make_temp_dir("document_render_skill");
    char skills[SALTS_FS_MAX_PATH], folder[SALTS_FS_MAX_PATH], path[SALTS_FS_MAX_PATH];
    cmeta_fs_buf_t buffer = cmeta_fs_buf_init((void *)skill, sizeof(skill) - 1);
    document_render_catalog_t catalog = {render_sources, 2};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    int granted;
    check_not_null(root);
    check_not_null(registry);
    check_equal(document_render_register(registry, &catalog), TURBO_TOOL_OK);
    check_equal(cmeta_fs_path_join(skills, sizeof(skills), root, "skills"), 0);
    check_equal(cmeta_fs_path_join(folder, sizeof(folder), skills, "document-report"), 0);
    check_equal(cmeta_fs_path_join(path, sizeof(path), folder, "SKILL.md"), 0);
    check_equal(cmeta_fs_mkdir(skills, 0700), 0);
    check_equal(cmeta_fs_mkdir(folder, 0700), 0);
    check_equal(cmeta_fs_write_file(path, &buffer), 0);
    for (granted = 0; granted <= 1; ++granted) {
      turbo_agent_policy_t policy = turbo_agent_policy_default();
      turbo_agent_workspace_config_t config;
      turbo_agent_workspace_t *workspace = NULL;
      turbo_agent_workspace_selection_t *selection = NULL;
      policy.allow_runtime_tools = granted;
      turbo_agent_workspace_config_init(&config);
      config.workspace_root = root;
      config.policy = &policy;
      check_equal(turbo_agent_workspace_create(&config, &workspace), TURBO_AGENT_WORKSPACE_OK);
      check_equal(turbo_agent_workspace_prepare(workspace, "$document-report", registry, NULL,
                                                 &selection),
                  granted ? TURBO_AGENT_WORKSPACE_OK : TURBO_AGENT_WORKSPACE_CAPABILITY_DENIED);
      if (granted) {
        check_equal(turbo_agent_workspace_selection_tool_count(selection), 1u);
        check_equal(turbo_agent_workspace_selection_tool_name(selection, 0), "document_render");
      } else {
        check_null(selection);
      }
      turbo_agent_workspace_selection_destroy(selection);
      turbo_agent_workspace_destroy(workspace);
    }
    turbo_tool_registry_destroy(registry);
    check_equal(tt_remove_tree(root), 0);
    free(root);
  }

  it("checks authority before rendering and replay, with quiescent generation invalidation") {
    render_probe_t probe = {{render_sources, 2}, 0, 0};
    turbo_tool_definition_v6_t definition = document_render_definition(&probe.catalog);
    cmeta_threadpool_config_t config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&config);
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_harness_t *harness;
    turbo_agent_t *agent;
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    json_value_t *result = NULL;
    check_not_null(pool);
    check_not_null(registry);
    definition.definition.json_value_handler = render_probe_handler;
    definition.definition.user_data = &probe;
    check_equal(turbo_tool_registry_add_v6(registry, &definition), TURBO_TOOL_OK);
    harness = render_harness(pool, registry);
    check_not_null(harness);
    agent = turbo_agent_session_agent(turbo_agent_app_session(turbo_agent_harness_app(harness)));
    check_not_null(agent);
    check_not_equal(render_attempt(harness, "denied", &result), SALTS_OK);
    check_null(result);
    check_equal(probe.calls, 0);
    policy.allow_runtime_tools = 1;
    check_equal(turbo_agent_set_tool_policy(agent, &policy), 0);
    check_equal(render_attempt(harness, "render", &result), SALTS_OK);
    check_equal(json_get_string(result, "verification"), "reference_identity_only");
    check_equal(probe.calls, 1);
    json_free(result); result = NULL;
    check_equal(render_attempt(harness, "render", &result), SALTS_OK);
    check_equal(probe.calls, 1);
    json_free(result); result = NULL;
    /* All synchronous calls and callbacks returned before policy replacement. */
    check_equal(turbo_agent_set_tool_policy(agent, &policy), 0);
    check_not_equal(render_attempt(harness, "render", &result), SALTS_OK);
    check_null(result);
    check_equal(probe.calls, 1);
    check_equal(render_attempt(harness, "new-generation", &result), SALTS_OK);
    check_equal(probe.calls, 2);
    json_free(result); result = NULL;
    policy.allow_runtime_tools = 0;
    check_equal(turbo_agent_set_tool_policy(agent, &policy), 0);
    check_not_equal(render_attempt(harness, "new-generation", &result), SALTS_OK);
    check_null(result);
    check_equal(probe.calls, 2);
    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }

  it("withholds a malformed result instead of reporting a completed document") {
    render_probe_t probe = {{render_sources, 2}, 0, 1};
    turbo_tool_definition_v6_t definition = document_render_definition(&probe.catalog);
    cmeta_threadpool_config_t config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&config);
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_harness_t *harness;
    turbo_agent_t *agent;
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    json_value_t *result = NULL;
    check_not_null(pool);
    check_not_null(registry);
    definition.definition.json_value_handler = render_probe_handler;
    definition.definition.user_data = &probe;
    check_equal(turbo_tool_registry_add_v6(registry, &definition), TURBO_TOOL_OK);
    harness = render_harness(pool, registry);
    check_not_null(harness);
    agent = turbo_agent_session_agent(turbo_agent_app_session(turbo_agent_harness_app(harness)));
    policy.allow_runtime_tools = 1;
    check_equal(turbo_agent_set_tool_policy(agent, &policy), 0);
    check_not_equal(render_attempt(harness, "invalid-result", &result), SALTS_OK);
    check_null(result);
    check_equal(probe.calls, 1);
    turbo_agent_harness_release(harness);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }
}
