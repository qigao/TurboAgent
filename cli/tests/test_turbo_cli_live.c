#include "tinytest.h"
#include "turbo_agent_harness.h"
#include "turbo_agent_session.h"
#include "turbo_runtime_store_sqlite.h"
#include "turbo_coding_tools.h"
#include <cmeta_fs.h>
#include <cmeta_process.h>
#include <fmt.h>
#include <salts/error_codes.h>
#include <stdlib.h>
#include <string.h>

/* This opt-in test uses the real executable, provider transport and tool pack.
 * Credentials stay in the source-directory .env / inherited environment. Child
 * stdout/stderr are bounded and deliberately not copied into test diagnostics. */
static const char live_source[] = "def inclusive_sum(start, end):\n"
                                  "    return sum(range(start, end))\n";

static const char live_tests[] =
    "import pathlib\n"
    "import unittest\n"
    "from ranges import inclusive_sum\n"
    "class RangeTests(unittest.TestCase):\n"
    "    def test_positive(self): self.assertEqual(inclusive_sum(1, 3), 6)\n"
    "    def test_single(self): self.assertEqual(inclusive_sum(4, 4), 4)\n"
    "    def test_negative(self): self.assertEqual(inclusive_sum(-3, -1), -6)\n"
    "    def test_empty(self): self.assertEqual(inclusive_sum(3, 1), 0)\n"
    "if __name__ == '__main__':\n"
    "    result = "
    "unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(RangeTests))\n"
    "    if result.wasSuccessful(): pathlib.Path('verified.txt').write_bytes(b'four tests "
    "passed\\n')\n"
    "    raise SystemExit(0 if result.wasSuccessful() else 1)\n";

static const char live_runbook[] =
    "# Orion release runbook\n"
    "The Orion release requires approval from Mira Chen before deployment.\n"
    "The canary observation window is 17 minutes.\n"
    "Rollback if the error rate exceeds 2% for 5 consecutive minutes.\n"
    "The rollback code is ORION-ROLLBACK-29. Stop the rollout, restore the previous\n"
    "version, and notify the release owner. This is a synthetic test runbook.\n";

static int live_write(const char *root, const char *name, const char *text) {
  char path[SALTS_FS_MAX_PATH];
  cmeta_fs_buf_t bytes = {(char *)text, strlen(text)};
  if (cmeta_fs_path_join(path, sizeof(path), root, name) != 0) return -1;
  return cmeta_fs_write_file(path, &bytes);
}

static const char *live_cli_failure_phase(cmeta_process_t *process) {
  char diagnostic[4096];
  size_t count = 0;
  /* Emit only known phase names; arbitrary model/transport text stays private. */
  (void)cmeta_process_read_stderr(process, diagnostic, sizeof(diagnostic) - 1, &count);
  diagnostic[count] = '\0';
  const char *details = strstr(diagnostic, "phase=");
  if (details) {
    char phase[48], execution[32];
    int run = 0, http = 0, transport = 0;
    if (sscanf(details, "phase=%47s run_status=%d execution_status=%31s http_status=%d transport_status=%d",
        phase, &run, execution, &http, &transport) == 5) {
      info("CLI failure codes run=%d http=%d transport=%d", run, http, transport);
      static const char *const known[] = {"build_request", "transport", "parse_response", "append_model_event", "execution"};
      for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i)
        if (!strcmp(phase, known[i])) return known[i];
    }
  }
  if (strstr(diagnostic, "Cannot initialize agent")) return "agent initialization";
  if (strstr(diagnostic, "Cannot initialize workspace tools")) return "tool registration";
  if (strstr(diagnostic, "Cannot prepare vector index")) return "vector import";
  if (strstr(diagnostic, "Cannot resolve workspace")) return "workspace path";
  if (strstr(diagnostic, "Agent run could not start")) return "run submission";
  if (strstr(diagnostic, "Agent run failed or was interrupted")) return "run execution";
  if (strstr(diagnostic, "Agent ended without a text answer")) return "final answer";
  return "unclassified";
}

/* Read only after the CLI exits and releases its exclusive SQLite ownership.
 * Keep full model/tool content private; callers report only verified facts. */
static int live_load_state(const char *path, int turns, json_value_t **out) {
  turbo_runtime_sqlite_config_t config;
  turbo_agent_runtime_store_t store = {0};
  json_value_t *metadata = NULL, *head = NULL;
  char *text = NULL;
  int rc;
  *out = NULL;
  turbo_runtime_sqlite_config_init(&config);
  config.path = path; config.create_if_missing = 0;
  rc = turbo_runtime_store_sqlite_open(&config, &store);
  if (rc != SALTS_OK) return rc;
  rc = store.get(store.user_data, "cli_sessions", "current", &text);
  if (rc != SALTS_OK) goto done;
  metadata = json_parse(text, strlen(text)); free(text); text = NULL;
  rc = SALTS_EPROTO;
  const char *phase = json_get_string(metadata, "phase");
  const char *thread = json_get_string(metadata, "thread_id");
  const char *run_id = json_get_string(metadata, "last_run_id");
  if (!phase || strcmp(phase, "ready") || !thread || !run_id ||
      json_get_int(metadata, "turn_count", -1) != turns) goto done;
  rc = store.get(store.user_data, "thread_heads", thread, &text);
  if (rc != SALTS_OK) goto done;
  head = json_parse(text, strlen(text));
  const json_value_t *run = json_object_get(head, "run_snapshot");
  const char *status = json_get_string(run, "status"), *id = json_get_string(run, "id");
  const json_value_t *snapshot = json_object_get(run, "state_snapshot");
  rc = SALTS_EPROTO;
  if (!status || strcmp(status, "completed") || !id || strcmp(id, run_id) ||
      json_type(snapshot) != JSON_OBJECT) goto done;
  *out = json_clone(snapshot);
  rc = *out ? SALTS_OK : SALTS_ENOMEM;
done:
  free(text); json_free(metadata); json_free(head);
  store.user_data_free(store.user_data);
  return rc;
}

static size_t live_tool_calls(const json_value_t *state,
    const turbo_tool_registry_t *registry, const char *name) {
  size_t count = 0;
  for (size_t i = 0; i < turbo_agent_state_event_count(state); ++i) {
    const json_value_t *calls = json_object_get(turbo_agent_state_event_at(state, i), "tool_calls");
    for (size_t j = 0; j < json_array_size(calls); ++j) {
      const json_value_t *call = json_array_get(calls, j);
      const char *actual = json_get_string(call, "name");
      const char *canonical = NULL;
      /* Compatible providers use wire aliases such as fs_read. Resolve with
       * the same public registry contract used by actual tool execution. */
      if (!name || (actual && turbo_tool_registry_resolve_name(registry, actual, &canonical) == TURBO_TOOL_OK &&
          !strcmp(name, canonical))) ++count;
    }
  }
  return count;
}

spec("Live coding and general CLI") {
  const char *enabled = getenv("TURBOAGENT_LIVE_TESTS");
  const char *embedding_url = getenv("TURBOAGENT_LIVE_EMBEDDING_URL");
  const char *embedding_model = getenv("TURBOAGENT_LIVE_EMBEDDING_MODEL");
  static char *root;
  static cmeta_process_t *process;
  static cmeta_fs_buf_t contents;
  static turbo_agent_session_t *session;
  static turbo_agent_harness_t *harness;
  static turbo_agent_harness_execution_t *execution;
  static cmeta_threadpool_t *pool;
  static json_value_t *summary, *state;
  static tstr database, knowledge, docs;
  static turbo_tool_registry_t *registry;

  after_each() {
    if (execution) {
      turbo_agent_harness_execution_cancel(execution, TURBO_CANCEL_SHUTDOWN);
      turbo_agent_harness_execution_wait(execution, UINT64_MAX);
      turbo_agent_harness_execution_release(execution);
      execution = NULL;
    }
    if (pool) cmeta_threadpool_wait(pool);
    turbo_agent_harness_release(harness);
    harness = NULL;
    cmeta_threadpool_destroy(pool);
    pool = NULL;
    turbo_agent_session_destroy(session);
    session = NULL;
    json_free(summary);
    json_free(state);
    summary = state = NULL;
    cmeta_process_destroy(process);
    process = NULL;
    cmeta_fs_buf_free(&contents);
    turbo_tool_registry_destroy(registry); registry = NULL;
    tstr_free(database); database = NULL;
    tstr_free(knowledge); knowledge = NULL;
    tstr_free(docs); docs = NULL;
    if (root) {
      /* root comes only from TinyTest's fresh absolute temporary directory. */
      tt_remove_tree(root);
      free(root);
      root = NULL;
    }
  }

  if (!enabled || strcmp(enabled, "1") != 0) {
    it_skip("requires TURBOAGENT_LIVE_TESTS=1") {}
  } else {
    const char *skill_modes[] = {"general", "coding"};
    const char *skill_workflows[] = {"loop", "review", "engineering"};
    for (size_t m = 0; m < 2; ++m) {
      for (size_t w = 0; w < 3; ++w) {
        it("%s %s follows a workspace skill with the real provider", skill_modes[m], skill_workflows[w]) {
          cmeta_process_options_t options;
          cmeta_process_result_t result;
          char answer[4096]; size_t count = 0;
          const char *args[] = {"-C", NULL, "--mode", skill_modes[m], "--workflow", skill_workflows[w],
              "--read-only", "--session-db", NULL, "-p",
              "Use release-summary to answer: we processed six batches with seven items each. "
              "How many items? Include the skill's verification label in the final answer. "
              "No file or shell operations are needed.", NULL};
          root = tt_make_temp_dir("turbo_cli_live_skill"); check_not_null(root);
          database = tstr_format("{}/session.db", root); check_not_null(database);
          docs = tstr_format("{}/skills", root); check_not_null(docs);
          check_equal(cmeta_fs_mkdir(docs, 0700), 0);
          check_equal(live_write(root, "AGENTS.md", "Answer concisely using the selected skill.\n"), 0);
          check_equal(live_write(root, "skills/release-summary.md",
              "---\nname: release-summary\ndescription: Summarize a release count\n"
              "triggers: [release-summary]\n---\n"
              "The verification label is ORION-SKILL-5738. Include it with the computed count "
              "in the final user-facing answer. During planning obey the requested JSON format.\n"), 0);
          args[1] = root; args[8] = database;
          cmeta_process_options_init(&options);
          options.program = TURBO_CLI_EXECUTABLE; options.args = args;
          options.timeout_ms = 120000; options.max_output_bytes = 16384;
          check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
          check_equal(cmeta_process_wait(process, &result), SALTS_OK);
          if (result.exit_code != 0) info("CLI failure phase: %s", live_cli_failure_phase(process));
          check_equal(result.state, SALTS_PROCESS_EXITED); check_equal(result.exit_code, 0);
          check_equal(cmeta_process_read_stdout(process, answer, sizeof(answer) - 1, &count), SALTS_OK);
          answer[count] = 0;
          check_contains(answer, "42"); check_contains(answer, "ORION-SKILL-5738");
          cmeta_process_destroy(process); process = NULL;
          check_equal(live_load_state(database, 1, &state), SALTS_OK);
          check_greater(turbo_agent_state_memory_layer_count(state), 0u);
          if (w) {
            check_greater(turbo_agent_state_plan_step_count(state), 0u);
            check_true(turbo_agent_state_plan_complete(state));
            check_greater(turbo_agent_state_executor_event_version_count(state), 0u);
          } else check_equal(turbo_agent_state_plan_step_count(state), 0u);
          if (!m) check_equal(live_tool_calls(state, NULL, NULL), 0u);
        }
      }
    }

    it("answers a general task through the configured live provider") {
      turbo_agent_session_config_t config = {.load_env = 1};
      turbo_graph_run_options_t limits = {.max_steps = 4};
      int rc;
      config.agent_config.instructions = "Answer concisely. Follow the requested output format.";
      session = turbo_agent_session_create(&config);
      check_not_null(session);
      rc = turbo_agent_session_start_text(session, "What is 19 plus 23? Reply only 42.", &limits,
                                          &summary, &state);
      if (rc != 0) {
        const char *detail = turbo_agent_state_model_error_detail(state);
        int call = 0, status = 0, native = 0;
        unsigned int http = 0;
        if (detail)
          (void)sscanf(
              detail,
              "http transport failed: call_status=%d http_status=%u status=%d native_status=%d",
              &call, &http, &status, &native);
        info("model failure rc=%d http=%u call=%d status=%d native=%d", rc, http, call, status,
             native);
      }
      check_equal(rc, 0);
      check_equal(turbo_agent_state_final_answer_text(state), "42");
    }

    it("answers a general task on the harness worker through the live provider") {
      turbo_agent_session_config_t config = {.load_env = 1};
      turbo_agent_app_config_t app_config = {.session_config = &config};
      turbo_agent_harness_config_t harness_config;
      cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
      int rc = -1;
      config.agent_config.instructions = "Answer concisely. Follow the requested output format.";
      config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
      pool = cmeta_threadpool_create_with_config(&pool_config);
      check_not_null(pool);
      turbo_agent_harness_config_init(&harness_config);
      harness_config.app_config = &app_config;
      harness_config.executor = pool;
      harness = turbo_agent_harness_create(&harness_config);
      check_not_null(harness);
      check_equal(turbo_agent_harness_start_text(harness, "What is 19 plus 23? Reply only 42.",
                                                 NULL, &execution),
                  SALTS_OK);
      check_equal(turbo_agent_harness_execution_wait(execution, 90000), SALTS_OK);
      check_equal(turbo_agent_harness_execution_result_code(execution, &rc), SALTS_OK);
      check_equal(turbo_agent_harness_execution_take_result(execution, &summary, &state), SALTS_OK);
      if (rc != 0) {
        const char *detail = turbo_agent_state_model_error_detail(state);
        int call = 0, status = 0, native = 0;
        unsigned int http = 0;
        if (detail)
          (void)sscanf(
              detail,
              "http transport failed: call_status=%d http_status=%u status=%d native_status=%d",
              &call, &http, &status, &native);
        info("worker failure rc=%d http=%u call=%d status=%d native=%d", rc, http, call, status,
             native);
      }
      check_equal(rc, 0);
      check_equal(turbo_agent_state_final_answer_text(state), "42");
    }

    for (int retrieval_case = 0; retrieval_case < 3; ++retrieval_case) {
      const char *mode = retrieval_case == 2 ? "coding" : "general";
      int vector = retrieval_case != 0;
      if (vector && !embedding_url && !embedding_model) {
        it_skip("%s vector case requires an explicit live embedding URL and model", mode) {}
      } else {
        it("%s answers a release case with %s evidence and resumes it in a new CLI process",
            mode, vector ? "live vector" : "FTS") {
          cmeta_process_options_t options;
          cmeta_process_result_t result;
          char answer[16384];
          size_t count = 0;
          if (vector) {
            check_not_null(embedding_url); check_not_null(embedding_model);
            check_not_equal(embedding_url[0], '\0'); check_not_equal(embedding_model[0], '\0');
          }
          root = tt_make_temp_dir("turbo_cli_general_live"); check_not_null(root);
          database = tstr_format("{}/session.db", root);
          knowledge = tstr_format("{}/knowledge.db", root);
          docs = tstr_format("{}/docs", root);
          check_not_null(database); check_not_null(knowledge); check_not_null(docs);
          check_equal(cmeta_fs_mkdir(docs, 0700), 0);
          check_equal(live_write(docs, "orion-release.md", live_runbook), 0);
          const char *initial[] = {"--mode", mode, "-C", root,
              vector ? "--vector-db" : "--knowledge-db", knowledge,
              "--index-dir", docs, "--session-db", database,
              "-p", "Orion release: who approves deployment and how long is the canary observation "
              "window? Use the indexed runbook and cite its filename. Answer briefly in English with "
              "digits for durations. Remember my ticket ORION-TICKET-73 for our next turn.",
              vector ? "--embedding-url" : NULL, embedding_url, "--embedding-model", embedding_model, NULL};
          cmeta_process_options_init(&options);
          options.program = TURBO_CLI_EXECUTABLE; options.args = initial;
          options.timeout_ms = 90000; options.max_output_bytes = 65536;
          check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
          check_equal(cmeta_process_wait(process, &result), SALTS_OK);
          info("%s %s initial process state=%d exit=%d", mode, vector ? "vector" : "FTS",
              (int)result.state, result.exit_code);
          if (result.exit_code != 0) info("CLI failure phase: %s", live_cli_failure_phase(process));
          check_equal(result.state, SALTS_PROCESS_EXITED); check_equal(result.exit_code, 0);
          check_equal(cmeta_process_read_stdout(process, answer, sizeof(answer) - 1, &count), SALTS_OK);
          check_less(count, sizeof(answer) - 1); answer[count] = 0;
          check_true(strstr(answer, "Mira Chen") != NULL);
          check_true(strstr(answer, "17") != NULL);
          check_true(strstr(answer, "orion-release.md") != NULL);
          check_equal(live_load_state(database, 1, &state), SALTS_OK);
          if (!strcmp(mode, "general")) check_equal(live_tool_calls(state, NULL, NULL), 0u);
          check_equal(json_array_size(turbo_agent_state_memory_layers(state)), 2u);
          check_equal(json_get_string(json_array_get(turbo_agent_state_memory_layers(state), 0), "text"), live_runbook);
          json_free(state); state = NULL;
          cmeta_process_destroy(process); process = NULL;

          /* The second process cannot get the facts by importing the original file.
           * Its fresh retrieval layer must come from the persisted retrieval database. */
          char source_path[SALTS_FS_MAX_PATH];
          check_equal(cmeta_fs_path_join(source_path, sizeof(source_path), docs, "orion-release.md"), 0);
          check_equal(cmeta_fs_unlink(source_path), 0);
          const char *followup[] = {"--mode", mode, "-C", root,
              vector ? "--vector-db" : "--knowledge-db", knowledge, "--session-db", database, "--resume", "-p",
              "Orion release rollback: the observed error rate is 4% for 6 consecutive minutes. "
              "Choose ROLLBACK or CONTINUE from the indexed policy, include the exact rollback code "
              "from the runbook, recall my ticket from the previous turn, and cite the source filename. "
              "Answer briefly in English. Treat this as a decision based on the indexed snapshot.",
              vector ? "--embedding-url" : NULL, embedding_url, "--embedding-model", embedding_model, NULL};
          options.args = followup;
          check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
          check_equal(cmeta_process_wait(process, &result), SALTS_OK);
          info("%s %s resumed process state=%d exit=%d", mode, vector ? "vector" : "FTS",
              (int)result.state, result.exit_code);
          if (result.exit_code != 0) info("CLI failure phase: %s", live_cli_failure_phase(process));
          check_equal(result.state, SALTS_PROCESS_EXITED); check_equal(result.exit_code, 0);
          check_equal(cmeta_process_read_stdout(process, answer, sizeof(answer) - 1, &count), SALTS_OK);
          check_less(count, sizeof(answer) - 1); answer[count] = 0;
          check_true(strstr(answer, "ROLLBACK") != NULL);
          check_true(strstr(answer, "ORION-ROLLBACK-29") != NULL);
          check_true(strstr(answer, "ORION-TICKET-73") != NULL);
          check_true(strstr(answer, "orion-release.md") != NULL);
          check_equal(live_load_state(database, 2, &state), SALTS_OK);
          if (!strcmp(mode, "general")) check_equal(live_tool_calls(state, NULL, NULL), 0u);
          check_equal(json_array_size(turbo_agent_state_memory_layers(state)), 2u);
          check_equal(json_get_string(json_array_get(turbo_agent_state_memory_layers(state), 0), "text"), live_runbook);
          info("Verified release owner, 17-minute canary, rollback decision/code, source citation, "
              "ticket memory, two persisted turns and fresh retrieval evidence.");
        }
      }
    }

    it("reads workspace evidence through a real provider tool call") {
      turbo_agent_session_config_t config = {.load_env = 1};
      turbo_graph_run_options_t limits = {.max_steps = 6};
      turbo_coding_tools_config_t tools_config;
      int rc;
      root = tt_make_temp_dir("turbo_cli_tool_live"); check_not_null(root);
      check_equal(live_write(root, "proof.txt", "ORION-PROOF-8462\n"), 0);
      registry = turbo_tool_registry_create(); check_not_null(registry);
      turbo_coding_tools_config_init(&tools_config);
      tools_config.workspace_root = root;
      check_equal(turbo_coding_tools_add_read_only(registry, &tools_config), TURBO_TOOL_OK);
      config.agent_config.tool_registry = registry;
      config.agent_config.instructions = "Read the requested workspace file with the provided tool. "
          "Reply with only its contents. Do not guess its contents.";
      session = turbo_agent_session_create(&config); check_not_null(session);
      rc = turbo_agent_session_start_text(session, "Read proof.txt using the file reading tool.",
          &limits, &summary, &state);
      if (rc != 0) {
        const char *detail = turbo_agent_state_model_error_detail(state);
        /* Classify known protocol failures without disclosing provider text. */
        static const char *const fields[] = {"name", "strict", "required", "stream",
            "previous_response_id", "reasoning", "function_call", "input", "not found",
            "store", "encrypted", "unsupported"};
        info("tool request failure rc=%d observed_calls=%zu", rc,
            live_tool_calls(state, registry, "fs.read"));
        for (size_t i = 0; detail && i < sizeof(fields) / sizeof(fields[0]); ++i)
          if (strstr(detail, fields[i])) info("provider error mentions %s", fields[i]);
      }
      check_equal(rc, 0);
      check_equal(turbo_agent_state_final_answer_text(state), "ORION-PROOF-8462");
      check_greater(live_tool_calls(state, registry, "fs.read"), 0u);
    }

    const char *repair_workflows[] = {"loop", "engineering"};
    for (size_t w = 0; w < 2; ++w) {
      it("%s repairs a program with a real model and verifies its tests independently", repair_workflows[w]) {
        char path[SALTS_FS_MAX_PATH];
        cmeta_process_options_t options;
        cmeta_process_result_t result;
        const char *python_args[] = {"-B", "test_ranges.py", NULL};
        const char *cli_args[] = {
            "-C", NULL, "-p",
            "Fix the off-by-one bug in ranges.py so inclusive_sum includes both endpoints, "
            "and an end smaller than start returns zero. Inspect both files using fs.read, change only ranges.py "
            "using fs.edit, then run python -B test_ranges.py with shell.exec. Do not modify the "
            "tests or manually create verified.txt; the tests create it on success. Work only in "
            "this temporary workspace. Do not inspect environment variables, credentials, or "
            "files outside it. Report the actual test result concisely.",
            "--session-db", NULL, "--workflow", repair_workflows[w], NULL};
        const char *child_env[] = {"PYTHONDONTWRITEBYTECODE=1", NULL};

        root = tt_make_temp_dir("turbo_cli_live");
        check_not_null(root);
        database = tstr_format("{}/session.db", root); check_not_null(database);
        check_equal(live_write(root, "ranges.py", live_source), 0);
        check_equal(live_write(root, "test_ranges.py", live_tests), 0);

        cmeta_process_options_init(&options);
        options.program = "python";
        options.args = python_args;
        options.cwd = root;
        options.timeout_ms = 10000;
        options.max_output_bytes = 16384;
        check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
        check_equal(cmeta_process_wait(process, &result), SALTS_OK);
        check_equal(result.state, SALTS_PROCESS_EXITED);
        check_equal(result.exit_code, 1);
        cmeta_process_destroy(process);
        process = NULL;

        cli_args[1] = root;
        cli_args[5] = database;
        options.program = TURBO_CLI_EXECUTABLE;
        options.args = cli_args;
        options.env = child_env;
        options.cwd = NULL; /* CTest selects the source directory containing .env. */
        options.timeout_ms = 180000;
        options.max_output_bytes = 65536;
        check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
        check_equal(cmeta_process_wait(process, &result), SALTS_OK);
        info("CLI process state=%d exit=%d", (int)result.state, result.exit_code);
        if (result.exit_code != 0) info("CLI failure phase: %s", live_cli_failure_phase(process));
        check_equal(result.state, SALTS_PROCESS_EXITED);
        check_equal(result.exit_code, 0);
        cmeta_process_destroy(process);
        process = NULL;

        check_equal(live_load_state(database, 1, &state), SALTS_OK);
        if (w) {
          check_greater(turbo_agent_state_plan_step_count(state), 0u);
          check_true(turbo_agent_state_plan_complete(state));
        }
        turbo_coding_workspace_config_t tools;
        turbo_coding_workspace_config_init(&tools); tools.files.workspace_root = root;
        registry = turbo_tool_registry_create(); check_not_null(registry);
        check_equal(turbo_coding_tools_add_workspace(registry, &tools), TURBO_TOOL_OK);

        check_equal(cmeta_fs_path_join(path, sizeof(path), root, "test_ranges.py"), 0);
        check_equal(cmeta_fs_read_file(path, &contents), 0);
        check_equal(contents.len, sizeof(live_tests) - 1);
        check_equal(memcmp(contents.base, live_tests, contents.len), 0);
        cmeta_fs_buf_free(&contents);
        check_equal(cmeta_fs_path_join(path, sizeof(path), root, "verified.txt"), 0);
        check_equal(cmeta_fs_read_file(path, &contents), 0);
        check_equal(contents.len, strlen("four tests passed\n"));
        check_equal(memcmp(contents.base, "four tests passed\n", contents.len), 0);
        cmeta_fs_buf_free(&contents);

        /* Run the unchanged tests ourselves after the Agent reports completion. */
        options.program = "python";
        options.args = python_args;
        options.cwd = root;
        options.timeout_ms = 10000;
        check_equal(cmeta_process_spawn(&options, &process), SALTS_OK);
        check_equal(cmeta_process_wait(process, &result), SALTS_OK);
        check_equal(result.state, SALTS_PROCESS_EXITED);
        check_equal(result.exit_code, 0);
        check_greater(live_tool_calls(state, registry, "fs.read"), 0u);
        check_greater(live_tool_calls(state, registry, "fs.edit"), 0u);
        check_greater(live_tool_calls(state, registry, "shell.exec"), 0u);
        info("Verified recorded fs.read/fs.edit/shell.exec calls, unchanged tests, "
            "agent-created success artifact and independent four-test pass.");
      }
    }
  }
}
