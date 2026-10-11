#include "tinytest.h"
#include "turbo_agent_harness.h"
#include "turbo_agent_session.h"
#include <cmeta_fs.h>
#include <cmeta_process.h>
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
  if (strstr(diagnostic, "Cannot initialize agent")) return "agent initialization";
  if (strstr(diagnostic, "Cannot initialize workspace tools")) return "tool registration";
  if (strstr(diagnostic, "Cannot resolve workspace")) return "workspace path";
  if (strstr(diagnostic, "Agent run could not start")) return "run submission";
  if (strstr(diagnostic, "Agent run failed or was interrupted")) return "run execution";
  if (strstr(diagnostic, "Agent ended without a text answer")) return "final answer";
  return "unclassified";
}

spec("Live coding CLI") {
  const char *enabled = getenv("TURBOAGENT_LIVE_TESTS");
  static char *root;
  static cmeta_process_t *process;
  static cmeta_fs_buf_t contents;
  static turbo_agent_session_t *session;
  static turbo_agent_harness_t *harness;
  static turbo_agent_harness_execution_t *execution;
  static cmeta_threadpool_t *pool;
  static json_value_t *summary, *state;

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

    it("repairs a program with a real model and verifies its tests independently") {
      char path[SALTS_FS_MAX_PATH];
      cmeta_process_options_t options;
      cmeta_process_result_t result;
      const char *python_args[] = {"-B", "test_ranges.py", NULL};
      const char *cli_args[] = {
          "-C", NULL, "-p",
          "Fix the off-by-one bug in ranges.py so inclusive_sum includes both endpoints, "
          "and an end smaller than start returns zero. Inspect the files, change only ranges.py "
          "using fs.edit, then run python -B test_ranges.py with shell.exec. Do not modify the "
          "tests or manually create verified.txt; the tests create it on success. Work only in "
          "this temporary workspace. Do not inspect environment variables, credentials, or "
          "files outside it. Report the actual test result concisely.",
          NULL};
      const char *child_env[] = {"PYTHONDONTWRITEBYTECODE=1", NULL};

      root = tt_make_temp_dir("turbo_cli_live");
      check_not_null(root);
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
    }
  }
}
