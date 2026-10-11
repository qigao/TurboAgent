#include "turbo_cli.h"
#include "turbo_agent_harness.h"
#include "turbo_agent_policy.h"
#include "turbo_agent_workspace.h"
#ifdef TURBO_CLI_CODING_TOOLS
#include "turbo_coding_tools.h"
#endif
#include <cmeta_fs.h>
#include <salts/clock.h>
#include <tstr.h>
#include <vstr.h>
#include <tlog.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

enum { CLI_MAX_PROMPT_BYTES = 64 * 1024, CLI_MAX_HISTORY_BYTES = 1024 * 1024,
       CLI_MAX_TURNS = 64, CLI_MAX_STEPS = 128, CLI_WAIT_MS = 50 };
#define CLI_EXECUTION_TIMEOUT_MS UINT64_C(600000)

/* State is extensible and may contain arbitrary provider text. Only fixed phase
 * names and checked numeric attempt fields may reach terminal diagnostics. */
static const char *cli_failure_phase(const json_value_t *state) {
  static const char *const phases[] = {"before_turn", "build_request", "middleware", "guardrail",
      "context_compaction", "transport", "parse_response", "usage", "stream_event",
      "append_model_event", "structured_output"};
  const char *phase = turbo_agent_state_model_error_phase(state);
  for (size_t i = 0; phase && i < sizeof(phases) / sizeof(phases[0]); ++i)
    if (!strcmp(phase, phases[i])) return phases[i];
  return "execution";
}

static const char *cli_execution_status_name(turbo_agent_execution_status_t status) {
  switch (status) {
    case TURBO_AGENT_EXECUTION_QUEUED: return "queued";
    case TURBO_AGENT_EXECUTION_RUNNING: return "running";
    case TURBO_AGENT_EXECUTION_COMPLETED: return "completed";
    case TURBO_AGENT_EXECUTION_INTERRUPTED: return "interrupted";
    case TURBO_AGENT_EXECUTION_CANCELLED: return "cancelled";
    case TURBO_AGENT_EXECUTION_TIMED_OUT: return "timed_out";
    case TURBO_AGENT_EXECUTION_FAILED: return "failed";
    default: return "unknown";
  }
}

static void cli_report_failure(FILE *diagnostics, const json_value_t *state,
                               int run_rc, turbo_agent_execution_status_t status,
                               int interrupted) {
  const char *phase = cli_failure_phase(state);
  const json_value_t *attempts = json_object_get(state, "provider_attempts");
  const json_value_t *last = json_type(attempts) == JSON_ARRAY && json_array_size(attempts)
      ? json_array_get(attempts, json_array_size(attempts) - 1) : NULL;
  int64_t http = turbo_runtime_json_value_as_int64(json_object_get(last, "http_status"), 0);
  int64_t transport = turbo_runtime_json_value_as_int64(json_object_get(last, "transport_status"), 0);
  const char *hint = "Check run limits and model configuration.";
  tlog_t *logger;
  cmeta_log_sink_t *sink;
  const tlog_config_t config = {.min_level = SALTS_LOG_LEVEL_ERROR};
  const cmeta_console_sink_opts_t sink_options = {
      .output = diagnostics, .use_colors = 0, .pattern = "{message}"};
  if (http < 100 || http > 599) http = 0;
  if (transport < INT_MIN || transport > INT_MAX) transport = 0;
  if (interrupted) hint = "Run interrupted by user.";
  else if (status == TURBO_AGENT_EXECUTION_CANCELLED) hint = "Run cancelled.";
  else if (status == TURBO_AGENT_EXECUTION_TIMED_OUT) hint = "Run deadline exceeded.";
  else if (status == TURBO_AGENT_EXECUTION_INTERRUPTED) hint = "Run paused before completion.";
  else if (!strcmp(phase, "transport")) {
    if (http == 401) hint = "Check OPENAI_API_KEY or the proxy client key.";
    else if (http == 403) hint = "Check model access and credential permissions.";
    else if (http == 404) hint = "Check OPENAI_BASE_URL and OPENAI_ENDPOINT_PATH.";
    else if (http == 429) hint = "Provider rate limit or quota exceeded.";
    else if (http >= 500) hint = "Provider or proxy returned a server error.";
    else if (transport == SALTS_ETIMEDOUT) hint = "Model request timed out.";
    else if (transport == SALTS_EPERM) hint = "Check the HTTP client owner thread.";
    else hint = "Check provider configuration and connectivity.";
  } else if (!strcmp(phase, "parse_response")) hint = "Provider response could not be parsed.";

  /* One owned logger at the terminal error boundary; do not change the host's
   * default logger. The caller's existing failure line survives logger OOM. */
  logger = tlog_create(&config);
  if (!logger) return;
  sink = cmeta_sink_console_create(&sink_options);
  if (!sink || tlog_add_sink(logger, sink) != 0) {
    cmeta_sink_destroy(sink);
    tlog_destroy(logger);
    return;
  }
  SALTS_LOG_ERRORF(logger, "turboagent.cli",
      "phase={} run_status={} execution_status={} http_status={} transport_status={}. {}",
      phase, run_rc, cli_execution_status_name(status), (int)http, (int)transport, hint);
  tlog_flush(logger);
  tlog_destroy(logger);
}

int turbo_cli_complete_execution(turbo_agent_harness_execution_t *execution,
                                 int wait_status, int interrupted, FILE *output,
                                 FILE *diagnostics, json_value_t **out_state) {
  turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_FAILED;
  json_value_t *summary = NULL, *state = NULL;
  const char *answer;
  int run_rc = -1, status_rc, result_rc, take_rc;
  if (!out_state) return 1;
  *out_state = NULL;
  if (!execution || !output || !diagnostics) return 1;
  status_rc = turbo_agent_harness_execution_get_status(execution, &status);
  result_rc = turbo_agent_harness_execution_result_code(execution, &run_rc);
  take_rc = turbo_agent_harness_execution_take_result(execution, &summary, &state);
  json_free(summary);
  /* Runtime return 0 also describes persisted cancellation/timeout results.
   * Retained model text alone does not prove the turn completed. */
  if (wait_status != SALTS_OK || status_rc != SALTS_OK || result_rc != SALTS_OK ||
      take_rc != SALTS_OK || run_rc != 0 || !state || interrupted ||
      status != TURBO_AGENT_EXECUTION_COMPLETED) {
    fputs("Agent run failed or was interrupted.\n", diagnostics);
    cli_report_failure(diagnostics, state, run_rc, status, interrupted);
    json_free(state);
    return interrupted ? 130 : 1;
  }
  answer = turbo_agent_state_final_answer_text(state);
  if (!answer) answer = turbo_agent_state_last_output_text(state);
  if (!answer) {
    json_free(state);
    fputs("Agent ended without a text answer.\n", diagnostics);
    return 1;
  }
  fputs(answer, output);
  fputc('\n', output);
  fflush(output);
  *out_state = state;
  return 0;
}

/* Host-path conversion belongs to this platform adapter, not the tool pack. */
static tstr cli_absolute_path(const char *path) {
  tstr result = NULL;
#ifdef _WIN32
  wchar_t *input = NULL, *absolute = NULL;
  int wide_count, utf8_count;
  DWORD capacity, written;
  wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
  if (wide_count <= 0) return NULL;
  input = malloc((size_t)wide_count * sizeof(*input));
  if (!input || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, input, wide_count)) goto done;
  capacity = GetFullPathNameW(input, 0, NULL, NULL);
  if (!capacity || capacity > 32768) goto done;
  absolute = malloc((size_t)capacity * sizeof(*absolute));
  if (!absolute) goto done;
  written = GetFullPathNameW(input, capacity, absolute, NULL);
  if (!written || written >= capacity) goto done;
  utf8_count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, absolute, -1, NULL, 0, NULL, NULL);
  if (utf8_count <= 0) goto done;
  result = tstr_new_len(NULL, (size_t)utf8_count - 1);
  if (result && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, absolute, -1, result, utf8_count, NULL, NULL)) {
    tstr_free(result); result = NULL;
  }
done:
  free(absolute); free(input);
#else
  char *absolute = realpath(path, NULL);
  if (absolute) { result = tstr_dup(absolute); free(absolute); }
#endif
  return result;
}

static int cli_read_line(FILE *input, tstr *out) {
  int ch;
  tstr line = tstr_new();
  *out = NULL;
  if (!line) return -1;
  while ((ch = fgetc(input)) != EOF && ch != '\n') {
    char byte = (char)ch;
    tstr next;
    if (ch == 0 || tstr_len(line) >= CLI_MAX_PROMPT_BYTES) { tstr_free(line); return -1; }
    next = tstr_cat_len(line, &byte, 1);
    if (!next) { tstr_free(line); return -1; }
    line = next;
  }
  if (ferror(input)) { tstr_free(line); return -1; }
  if (ch == EOF && !tstr_len(line)) { tstr_free(line); return 0; }
  if (tstr_len(line) && line[tstr_len(line) - 1] == '\r') tstr_set_len(line, tstr_len(line) - 1);
  if (!vstr_utf8_valid(tstr_to_v(line))) { tstr_free(line); return -1; }
  *out = line;
  return 1;
}

/* All catalog work stays on the input thread between quiescent runs. The
 * state layer copies selection text before its borrowed registry is released. */
static int cli_prepare_workspace(turbo_agent_workspace_t **workspace, const char *root,
                                 const turbo_agent_policy_t *policy, const char *task,
                                 const turbo_tool_registry_t *registry, json_value_t *state,
                                 FILE *diagnostics) {
  turbo_agent_workspace_selection_t *selection = NULL;
  turbo_agent_workspace_status_t status;
  const char *instructions;
  int rc = -1;
  if (!*workspace) {
    turbo_agent_workspace_config_t config;
    turbo_agent_workspace_config_init(&config);
    config.workspace_root = root;
    config.policy = policy;
    status = turbo_agent_workspace_create(&config, workspace);
  } else {
    status = turbo_agent_workspace_refresh(*workspace);
  }
  if (status != TURBO_AGENT_WORKSPACE_OK) goto failed;
  status = turbo_agent_workspace_prepare(*workspace, task, registry, NULL, &selection);
  if (status != TURBO_AGENT_WORKSPACE_OK) goto failed;
  instructions = turbo_agent_workspace_selection_instructions(selection);
  if (!instructions[0] ||
      turbo_agent_state_add_memory_context_layer(state, "project", root, instructions) == 0)
    rc = 0;
  else
    fputs("Cannot copy workspace instructions into the turn.\n", diagnostics);
  turbo_agent_workspace_selection_destroy(selection);
  return rc;
failed:
  fputs("Cannot prepare workspace instructions or skills", diagnostics);
  if (*workspace) {
    fputs(": ", diagnostics);
    fputs(turbo_agent_workspace_last_error(*workspace), diagnostics);
  }
  fputs(".\n", diagnostics);
  return -1;
}

int turbo_cli_run(const turbo_cli_options_t *cli_options, FILE *input, FILE *output,
                  FILE *diagnostics, const turbo_agent_config_t *model_override,
                  volatile sig_atomic_t *interrupted) {
  const char *workspace, *prompt, *model;
  int read_only, exit_code = 1, rc;
  tstr root = NULL, line = NULL;
  turbo_tool_registry_t *registry = NULL;
  turbo_agent_workspace_t *workspace_context = NULL;
  cmeta_threadpool_t *pool = NULL;
  turbo_agent_harness_t *harness = NULL;
  turbo_agent_session_t *session;
  json_value_t *previous = NULL;
  turbo_agent_session_config_t session_config = {0};
  turbo_agent_app_config_t app_config = {0};
  turbo_agent_harness_config_t harness_config;
  cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
  turbo_agent_policy_t policy = turbo_agent_policy_default();
  if (!cli_options || !cli_options->workspace || !input || !output || !diagnostics) return 2;
  workspace = cli_options->workspace; prompt = cli_options->prompt; model = cli_options->model;
  read_only = cli_options->read_only;
  if ((prompt && (!prompt[0] || strlen(prompt) > CLI_MAX_PROMPT_BYTES || !vstr_utf8_valid(vstr_from_cstr(prompt)))) ||
      !workspace[0] || strlen(workspace) > CLI_MAX_PROMPT_BYTES) {
    fputs("Invalid prompt or workspace.\n", diagnostics); return 2;
  }
  root = cli_absolute_path(workspace);
  if (!root) { fputs("Cannot resolve workspace.\n", diagnostics); exit_code = 2; goto done; }
  registry = turbo_tool_registry_create();
  if (!registry) goto done;
#ifdef TURBO_CLI_CODING_TOOLS
  {
    turbo_coding_workspace_config_t tools;
    turbo_coding_workspace_config_init(&tools);
    tools.files.workspace_root = root;
    rc = read_only ? turbo_coding_tools_add_read_only(registry, &tools.files)
                   : turbo_coding_tools_add_workspace(registry, &tools);
    if (rc != TURBO_TOOL_OK) { fputs("Cannot initialize workspace tools.\n", diagnostics); goto done; }
  }
#endif
  pool = cmeta_threadpool_create_with_config(&pool_config);
  if (!pool) goto done;
  if (model_override) session_config.agent_config = *model_override;
  else (void)turbo_agent_config_apply_env(&session_config.agent_config, NULL, 0);
  if (model) session_config.agent_config.model = model;
  session_config.agent_config.tool_registry = registry;
  if (!session_config.agent_config.instructions)
    session_config.agent_config.instructions = "You are a coding agent. Inspect relevant files before editing. "
        "Use workspace-relative file paths; use fs.list with path . for the root. "
        "Run relevant tests and report actual results. Shell commands have host privileges. "
        "Do not claim a change or test succeeded without checking its tool result.";
  session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
  app_config.session_config = &session_config;
  turbo_agent_harness_config_init(&harness_config);
  harness_config.app_config = &app_config; harness_config.executor = pool;
  harness = turbo_agent_harness_create(&harness_config);
  if (!harness) { fputs("Cannot initialize agent. Check model configuration.\n", diagnostics); goto done; }
  session = turbo_agent_app_session(turbo_agent_harness_app(harness));
  policy.workspace_root = root; policy.allow_patch = !read_only; policy.allow_shell = !read_only;
  if (turbo_agent_set_tool_policy(turbo_agent_session_agent(session), &policy) != 0) goto done;
  if (!prompt) fputs(read_only ? "TurboAgent (read-only). /quit exits.\n" :
                    "TurboAgent (workspace edits and host shell enabled). /quit exits.\n", diagnostics);
  for (size_t turn = 0; turn < CLI_MAX_TURNS; ) {
    json_value_t *state, *result = NULL;
    turbo_agent_harness_execution_t *execution = NULL;
    turbo_agent_harness_run_options_t options;
    turbo_graph_run_options_t graph_options = {.max_steps = CLI_MAX_STEPS};
    char *serialized;
    size_t history_bytes;
    int completion_rc;
    const char *text;
    if (interrupted && *interrupted) { exit_code = 130; goto done; }
    if (prompt) text = prompt;
    else {
      fputs("> ", diagnostics); fflush(diagnostics);
      tstr_free(line); line = NULL;
      rc = cli_read_line(input, &line);
      if (rc == 0) { exit_code = 0; goto done; }
      if (rc < 0) {
        if (interrupted && *interrupted) exit_code = 130;
        else { fputs("Input must be bounded UTF-8 text.\n", diagnostics); exit_code = 2; }
        goto done;
      }
      if (!strcmp(line, "/quit")) { exit_code = 0; goto done; }
      if (!line[0]) continue;
      text = line;
    }
    state = previous ? turbo_agent_session_create_follow_up_state_json_value(session, previous, text)
                     : turbo_agent_session_create_input_state_json_value(text);
    if (!state) { fputs("Cannot prepare conversation.\n", diagnostics); goto done; }
    serialized = json_serialize(json_object_get(state, "input"), &history_bytes);
    if (!serialized || history_bytes > CLI_MAX_HISTORY_BYTES) {
      json_serialize_free(serialized); json_free(state);
      fputs("Conversation history limit reached; start a new session.\n", diagnostics); goto done;
    }
    json_serialize_free(serialized);
    if (cli_prepare_workspace(&workspace_context, root, &policy, text, registry, state,
                              diagnostics) != 0) {
      json_free(state); goto done;
    }
    turbo_agent_harness_run_options_init(&options);
    options.graph_options = &graph_options;
    options.deadline_mono_ms = cmeta_monotonic_ms() + CLI_EXECUTION_TIMEOUT_MS;
    rc = turbo_agent_harness_start_state(harness, state, &options, &execution);
    json_free(state);
    if (rc != SALTS_OK) { fputs("Agent run could not start.\n", diagnostics); goto done; }
    while ((rc = turbo_agent_harness_execution_wait(execution, CLI_WAIT_MS)) == SALTS_ETIMEDOUT) {
      if (interrupted && *interrupted) turbo_agent_harness_execution_cancel(execution, TURBO_CANCEL_USER);
    }
    if (rc != SALTS_OK) {
      turbo_agent_harness_execution_cancel(execution, TURBO_CANCEL_SHUTDOWN);
      turbo_agent_harness_execution_wait(execution, UINT64_MAX);
    }
    /* Result publication can precede terminal callback cleanup. The next
     * Session projection requires quiescence, so drain this single-worker pool. */
    cmeta_threadpool_wait(pool);
    completion_rc = turbo_cli_complete_execution(execution, rc, interrupted && *interrupted,
                                                  output, diagnostics, &result);
    turbo_agent_harness_execution_release(execution);
    if (completion_rc != 0) { exit_code = completion_rc; goto done; }
    json_free(previous); previous = result;
    ++turn;
    if (prompt) { exit_code = 0; goto done; }
  }
  fputs("Session turn limit reached; start a new session.\n", diagnostics);
done:
  json_free(previous);
  turbo_agent_harness_release(harness);
  if (pool) { cmeta_threadpool_wait(pool); cmeta_threadpool_destroy(pool); }
  turbo_agent_workspace_destroy(workspace_context);
  turbo_tool_registry_destroy(registry);
  tstr_free(root); tstr_free(line);
  return exit_code;
}
