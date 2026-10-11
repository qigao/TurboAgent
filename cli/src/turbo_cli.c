#include "turbo_cli.h"
#include "turbo_cli_storage.h"
#include "turbo_cli_knowledge.h"
#include "turbo_cli_vector.h"
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
       CLI_MAX_STEPS = 128, CLI_MAX_REPLANS = 3, CLI_WAIT_MS = 50 };
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

static int cli_complete_execution(turbo_agent_harness_execution_t *execution,
                                 int wait_status, int interrupted, FILE *output,
                                 FILE *diagnostics, json_value_t **out_state,
                                 turbo_cli_storage_t *storage, turbo_agent_session_t *session) {
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
   * A stopped engineering graph can report completed after exhausting replans;
   * publishing its failure text must not commit an unfinished CLI turn. */
  if (wait_status != SALTS_OK || status_rc != SALTS_OK || result_rc != SALTS_OK ||
      take_rc != SALTS_OK || run_rc != 0 || !state || interrupted ||
      status != TURBO_AGENT_EXECUTION_COMPLETED ||
      (turbo_agent_state_plan(state) && !turbo_agent_state_plan_complete(state))) {
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
  if (storage && turbo_cli_storage_complete(storage, session) != SALTS_OK) {
    json_free(state);
    fputs("Cannot commit completed session; its unfinished marker is retained.\n", diagnostics);
    return 1;
  }
  fputs(answer, output);
  fputc('\n', output);
  fflush(output);
  *out_state = state;
  return 0;
}

int turbo_cli_complete_execution(turbo_agent_harness_execution_t *execution,
                                 int wait_status, int interrupted, FILE *output,
                                 FILE *diagnostics, json_value_t **out_state) {
  return cli_complete_execution(execution, wait_status, interrupted, output, diagnostics,
                                out_state, NULL, NULL);
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

/* Seed the existing JSON-native planner substate without adding planning
 * instructions to conversation history or to executor inputs. Each fresh turn
 * gets a new planner; completed-turn resume deliberately follows this path. */
static int cli_prepare_workflow(json_value_t *state,
                                turbo_agent_session_workflow_kind_t kind) {
  json_value_t *planner = NULL, *versions = NULL, *context = NULL;
  const json_value_t *memory;
  if (kind == TURBO_AGENT_SESSION_WORKFLOW_LOOP) return 0;
  planner = turbo_agent_session_create_input_messages_state_json_value(json_object_get(state, "input"));
  if (!planner || turbo_agent_state_add_user_message(planner,
      "Plan the latest user task. Return only a JSON object with a nonempty steps array "
      "of concise strings, for example {\"steps\":[\"Answer the question\"]}. "
      "Use at most 8 steps. Do not execute the plan yet. "
      "The final step must produce the complete user-facing answer using earlier results.") != 0) goto failed;
  memory = turbo_agent_state_memory_context(state);
  if (memory) {
    context = json_clone(memory);
    if (!context || turbo_runtime_json_object_set(planner, "memory_context", context) != 0) goto failed;
    context = NULL;
  }
  versions = json_create_array();
  if (!versions || turbo_runtime_json_array_append(versions, planner) != 0) goto failed;
  planner = NULL;
  if (turbo_runtime_json_object_set(state, "planner_state_versions", versions) != 0) goto failed;
  versions = NULL;
  return turbo_agent_state_set_replan_limit(state, CLI_MAX_REPLANS);
failed:
  json_free(context); json_free(planner); json_free(versions);
  return -1;
}

int turbo_cli_run(const turbo_cli_options_t *cli_options, FILE *input, FILE *output,
                  FILE *diagnostics, const turbo_agent_config_t *model_override,
                  const atomic_int *interrupted) {
  const char *workspace, *prompt, *model;
  int read_only, general, exit_code = 1, rc;
  tstr root = NULL, line = NULL, index_root = NULL;
  turbo_agent_knowledge_store_t *knowledge = NULL;
  turbo_cli_vector_t *vector = NULL;
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
  turbo_cli_storage_t storage = {0};
  if (!cli_options || !cli_options->workspace || !input || !output || !diagnostics) return 2;
  if (cli_options->diagram || cli_options->diagram_focus || cli_options->diagram_direction ||
      cli_options->diagram_depth || cli_options->diagram_journals || cli_options->diagram_origins ||
      cli_options->diagram_to || cli_options->diagram_plan_steps)
    return turbo_cli_export_diagram(cli_options, output, diagnostics);
  workspace = cli_options->workspace; prompt = cli_options->prompt; model = cli_options->model;
  read_only = cli_options->read_only;
  general = cli_options->mode && !strcmp(cli_options->mode, "general");
  if (!cli_options->workflow || !strcmp(cli_options->workflow, "loop"))
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
  else if (!strcmp(cli_options->workflow, "review"))
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_REVIEW;
  else if (!strcmp(cli_options->workflow, "engineering"))
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_ENGINEERING;
  else {
    fputs("Invalid workflow: use loop, review or engineering.\n", diagnostics);
    return 2;
  }
  if ((cli_options->mode && strcmp(cli_options->mode, "coding") && !general) ||
      (cli_options->instructions && (!cli_options->instructions[0] ||
        strlen(cli_options->instructions) > CLI_MAX_PROMPT_BYTES ||
        !vstr_utf8_valid(vstr_from_cstr(cli_options->instructions)))) ||
      (cli_options->knowledge_db && (!cli_options->knowledge_db[0] ||
        !strcmp(cli_options->knowledge_db, ":memory:") || !strncmp(cli_options->knowledge_db, "file:", 5) ||
        !vstr_utf8_valid(vstr_from_cstr(cli_options->knowledge_db)))) ||
      (cli_options->index_dir && (!cli_options->index_dir[0] ||
        (!cli_options->knowledge_db && !cli_options->vector_db))) ||
      turbo_cli_vector_validate(cli_options) != SALTS_OK) {
    fputs("Invalid agent options: mode is coding|general; --index-dir requires one knowledge/vector database; vector mode requires an embedding URL and model.\n", diagnostics);
    return 2;
  }
  if ((cli_options->resume && !cli_options->session_db) ||
      (cli_options->session_db && !cli_options->session_db[0])) {
    fputs("--resume requires a nonempty --session-db path.\n", diagnostics); return 2;
  }
  if ((prompt && (!prompt[0] || strlen(prompt) > CLI_MAX_PROMPT_BYTES || !vstr_utf8_valid(vstr_from_cstr(prompt)))) ||
      !workspace[0] || strlen(workspace) > CLI_MAX_PROMPT_BYTES) {
    fputs("Invalid prompt or workspace.\n", diagnostics); return 2;
  }
  root = cli_absolute_path(workspace);
  if (!root) { fputs("Cannot resolve workspace.\n", diagnostics); exit_code = 2; goto done; }
  if (cli_options->index_dir) {
    index_root = cli_absolute_path(cli_options->index_dir);
    if (!index_root) { fputs("Cannot resolve index directory.\n", diagnostics); exit_code = 2; goto done; }
  }
  if (cli_options->session_db) {
    rc = turbo_cli_storage_open(&storage, cli_options->session_db, root, cli_options->resume);
    if (rc != SALTS_OK) {
      if (rc == SALTS_EALREADY) fputs("Session already exists; use --resume or a new session file.\n", diagnostics);
      else if (rc == SALTS_EBUSY) fputs("Session is locked or contains unfinished work; automatic retry is refused.\n", diagnostics);
      else fputs("Cannot open session: check the file, workspace, format and storage limits.\n", diagnostics);
      goto done;
    }
    /* CLI owns the backend through worker drain, including construction failure.
     * Session receives a borrowed descriptor and never closes that connection. */
    session_config.runtime_store = storage.store;
    session_config.runtime_store.user_data_free = NULL;
    session_config.thread_id = json_get_string(storage.metadata, "thread_id");
  }
  if (cli_options->knowledge_db) {
    rc = turbo_cli_knowledge_open(cli_options->knowledge_db, index_root, interrupted, &knowledge);
    if (rc != SALTS_OK) {
      fputs("Cannot prepare knowledge index: check the dedicated database, FTS5, source text and import limits. Earlier per-document imports may remain.\n", diagnostics);
      if (interrupted && atomic_load_explicit(interrupted, memory_order_relaxed)) exit_code = 130;
      goto done;
    }
  }
  if (cli_options->vector_db) {
    rc = turbo_cli_vector_open(cli_options, index_root, interrupted, &vector);
    if (rc != SALTS_OK) {
      fputs("Cannot prepare vector index: check its dedicated file, lock, embedding identity/service and import limits. Earlier per-document imports may remain.\n", diagnostics);
      if (interrupted && atomic_load_explicit(interrupted, memory_order_relaxed)) exit_code = 130;
      goto done;
    }
  }
  registry = turbo_tool_registry_create();
  if (!registry) goto done;
#ifdef TURBO_CLI_CODING_TOOLS
  if (!general) {
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
  if (cli_options->instructions) session_config.agent_config.instructions = cli_options->instructions;
  if (!session_config.agent_config.instructions)
    session_config.agent_config.instructions = general
        ? "You are a general-purpose assistant. Answer the user's request clearly. "
          "Use supplied reference material as evidence, cite its source paths, and distinguish evidence from inference."
        : "You are a coding agent. Inspect relevant files before editing. "
        "Use workspace-relative file paths; use fs.list with path . for the root. "
        "Run relevant tests and report actual results. Shell commands have host privileges. "
        "Do not claim a change or test succeeded without checking its tool result.";
  app_config.session_config = &session_config;
  turbo_agent_harness_config_init(&harness_config);
  harness_config.app_config = &app_config; harness_config.executor = pool;
  harness = turbo_agent_harness_create(&harness_config);
  if (!harness) { fputs("Cannot initialize agent. Check model configuration.\n", diagnostics); goto done; }
  session = turbo_agent_app_session(turbo_agent_harness_app(harness));
  if (turbo_cli_storage_load(&storage, session, &previous) != SALTS_OK) {
    fputs("Cannot load the session's completed run.\n", diagnostics); goto done;
  }
  policy.workspace_root = root; policy.allow_patch = !general && !read_only; policy.allow_shell = !general && !read_only;
  if (turbo_agent_set_tool_policy(turbo_agent_session_agent(session), &policy) != 0) goto done;
  if (!prompt) fputs(general ? "TurboAgent (general). /quit exits.\n" :
                    read_only ? "TurboAgent (read-only). /quit exits.\n" :
                    "TurboAgent (workspace edits and host shell enabled). /quit exits.\n", diagnostics);
  for (size_t turn = storage.turns; turn < TURBO_CLI_MAX_TURNS; ) {
    json_value_t *state, *result = NULL;
    turbo_agent_harness_execution_t *execution = NULL;
    turbo_agent_harness_run_options_t options;
    turbo_graph_run_options_t graph_options = {.max_steps = CLI_MAX_STEPS};
    char *serialized;
    size_t history_bytes;
    int completion_rc;
    const char *text;
    if (interrupted && atomic_load_explicit(interrupted, memory_order_relaxed)) { exit_code = 130; goto done; }
    if (prompt) text = prompt;
    else {
      fputs("> ", diagnostics); fflush(diagnostics);
      tstr_free(line); line = NULL;
      rc = cli_read_line(input, &line);
      if (rc == 0) { exit_code = 0; goto done; }
      if (rc < 0) {
        if (interrupted && atomic_load_explicit(interrupted, memory_order_relaxed)) exit_code = 130;
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
    if (turbo_cli_knowledge_prepare(knowledge, text, state) != SALTS_OK ||
        turbo_cli_vector_prepare(vector, text, state) != SALTS_OK) {
      json_free(state);
      fputs("Cannot retrieve bounded knowledge context; no model call was submitted.\n", diagnostics);
      if (interrupted && atomic_load_explicit(interrupted, memory_order_relaxed)) exit_code = 130;
      goto done;
    }
    turbo_agent_harness_run_options_init(&options);
    if (cli_prepare_workflow(state, session_config.workflow_kind) != 0) {
      json_free(state);
      fputs("Cannot prepare workflow; no model call was submitted.\n", diagnostics); goto done;
    }
    options.graph_options = &graph_options;
    options.deadline_mono_ms = cmeta_monotonic_ms() + CLI_EXECUTION_TIMEOUT_MS;
    if (turbo_cli_storage_begin(&storage, session) != SALTS_OK) {
      json_free(state);
      fputs("Cannot commit session start; no work was submitted.\n", diagnostics); goto done;
    }
    rc = turbo_agent_harness_start_state(harness, state, &options, &execution);
    json_free(state);
    if (rc != SALTS_OK) { fputs("Agent run could not start.\n", diagnostics); goto done; }
    while ((rc = turbo_agent_harness_execution_wait(execution, CLI_WAIT_MS)) == SALTS_ETIMEDOUT) {
      if (interrupted && atomic_load_explicit(interrupted, memory_order_relaxed))
        turbo_agent_harness_execution_cancel(execution, TURBO_CANCEL_USER);
    }
    if (rc != SALTS_OK) {
      turbo_agent_harness_execution_cancel(execution, TURBO_CANCEL_SHUTDOWN);
      turbo_agent_harness_execution_wait(execution, UINT64_MAX);
    }
    /* Result publication can precede terminal callback cleanup. The next
     * Session projection requires quiescence, so drain this single-worker pool. */
    cmeta_threadpool_wait(pool);
    completion_rc = cli_complete_execution(execution, rc, interrupted && atomic_load_explicit(interrupted, memory_order_relaxed),
                                           output, diagnostics, &result, &storage, session);
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
  turbo_cli_storage_close(&storage);
  turbo_agent_workspace_destroy(workspace_context);
  turbo_tool_registry_destroy(registry);
  turbo_agent_knowledge_store_close(knowledge);
  if (turbo_cli_vector_close(vector) != SALTS_OK) {
    fputs("Cannot drain or close vector retrieval resources.\n", diagnostics);
    exit_code = 1;
  }
  tstr_free(root); tstr_free(line); tstr_free(index_root);
  return exit_code;
}
