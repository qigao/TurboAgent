#include "turbo_cli_approval.h"
#include "turbo_runtime_json.h"
#include <cmeta_uuid.h>
#include <fmt.h>
#include <salts/clock.h>
#include <stdlib.h>
#include <string.h>

enum { APPROVAL_MAX_BYTES = 64 * 1024, APPROVAL_MAX_REVIEWS = 32,
       APPROVAL_MAX_DEPTH = 64, APPROVAL_MAX_NODES = 65536 };

void turbo_cli_approval_clear(turbo_cli_approval_t *approval) {
  tstr_free(approval->approved_note);
  approval->approved_note = NULL;
}

static int approval_strings(const json_value_t *value, unsigned depth, size_t *remaining) {
  if (!value || depth > APPROVAL_MAX_DEPTH || !*remaining) return 0;
  --*remaining;
  if (json_type(value) == JSON_STRING) {
    const char *text = json_string(value);
    return text && strlen(text) == json_string_len(value);
  }
  if (json_type(value) == JSON_ARRAY) {
    for (size_t i = 0; i < json_array_size(value); ++i)
      if (!approval_strings(json_array_get(value, i), depth + 1, remaining)) return 0;
  } else if (json_type(value) == JSON_OBJECT) {
    for (size_t i = 0; i < json_object_size(value); ++i) {
      const char *key = json_object_key(value, i);
      if (!key || strlen(key) != json_object_key_len(value, i) ||
          !approval_strings(json_object_value(value, i), depth + 1, remaining)) return 0;
    }
  }
  return 1;
}

static int approval_after_model(turbo_agent_t *agent, const json_value_t *state,
    const char *text, const json_value_t *response, const char **reason, void *user) {
  size_t remaining = APPROVAL_MAX_NODES;
  (void)agent; (void)state; (void)user;
  if (!text || strlen(text) > APPROVAL_MAX_BYTES ||
      !approval_strings(response, 0, &remaining)) {
    *reason = "cli_approval_response_invalid";
    return -1;
  }
  return 0;
}

static int approval_before_model(turbo_agent_t *agent, const json_value_t *state,
    const char *request, const char **reason, void *user) {
  (void)agent; (void)state; (void)request; (void)reason;
  turbo_cli_approval_clear(user);
  return 0;
}

static int approval_before_tool(turbo_agent_t *agent, const json_value_t *state,
    const char *id, const char *name, const char *args, const char **reason, void *user) {
  turbo_cli_approval_t *approval = user;
  turbo_tool_effect_flags_t effects = 0;
  if (turbo_agent_tool_authority_snapshot_is_current(agent, &approval->authority) != 1) {
    *reason = "cli_authority_stale";
    return -1;
  }
  /* Shared ToolNode has one review slot. Do not pretend that it can approve
   * multiple effects atomically or dispatch part of a reviewed batch. */
  if (turbo_agent_state_pending_tool_calls(state) != 1) {
    *reason = "cli_approval_requires_single_tool_call";
    return -1;
  }
  if (turbo_tool_registry_get_effects(approval->registry, name, &effects) != TURBO_TOOL_OK ||
      !effects || (effects & (TURBO_TOOL_EFFECT_UNKNOWN | ~TURBO_TOOL_EFFECT_KNOWN_MASK))) {
    *reason = "cli_approval_unknown_effects";
    return -1;
  }
  if (!(effects & ~(TURBO_TOOL_EFFECT_READ | TURBO_TOOL_EFFECT_PURE))) return 0;
  if (approval->approved_note && approval->effects == effects &&
      turbo_agent_state_review_note(state) &&
      !strcmp(approval->approved_note, turbo_agent_state_review_note(state)) &&
      turbo_agent_tool_approval_review_matches(state, id, name, args)) return 0;
  *reason = "approval_required";
  return -1;
}

static int approval_after_tool(turbo_agent_t *agent, const json_value_t *state,
    const char *id, const char *name, const char *args, const char *output,
    turbo_tool_status_t status, const char **reason, void *user) {
  (void)agent; (void)state; (void)id; (void)name; (void)args;
  (void)output; (void)status; (void)reason;
  turbo_cli_approval_clear(user);
  return 0;
}

int turbo_cli_approval_attach(turbo_cli_approval_t *approval, turbo_agent_t *agent,
                              const turbo_tool_registry_t *registry) {
  turbo_agent_guardrail_t guard = {.before_model = approval_before_model,
      .after_model = approval_after_model,
      .before_tool = approval_before_tool, .after_tool = approval_after_tool,
      .user_data = approval};
  approval->agent = agent;
  approval->registry = registry;
  if (turbo_agent_tool_authority_snapshot_get(agent, &approval->authority) != 0) return -1;
  return turbo_agent_add_guardrail(agent, &guard);
}

/* Render untrusted request bytes as printable ASCII. ANSI/control/Unicode
 * formatting cannot hide or overwrite another field on the Host terminal. */
static void approval_print(FILE *stream, const char *text) {
  for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
    if (*p >= 32 && *p < 127) fputc(*p, stream);
    else fprintf(stream, "\\x%02x", (unsigned)*p);
  }
}

int turbo_cli_approval_read(const json_value_t *request, FILE *input, FILE *diagnostics) {
  const char *challenge = json_get_string(request, "challenge");
  char *serialized = json_serialize(request, NULL);
  tstr expected = challenge ? tstr_format("approve {}", challenge) : NULL;
  tstr reply = tstr_new();
  int ch, result = 0;
  if (!serialized || !expected || !reply) goto done;
  fputs("Tool approval request (escaped bytes): ", diagnostics);
  approval_print(diagnostics, serialized);
  fputs("\nHost shell has full privileges; cwd is not a sandbox.\nType exactly: ", diagnostics);
  fputs(expected, diagnostics);
  fputs("\nAny other response or EOF rejects: ", diagnostics);
  if (fflush(diagnostics) != 0 || ferror(diagnostics)) goto done;
  while ((ch = fgetc(input)) != EOF && ch != '\n') {
    char byte = (char)ch;
    tstr next;
    if (ch == 0 || tstr_len(reply) >= 128) goto done;
    next = tstr_cat_len(reply, &byte, 1);
    if (!next) goto done;
    reply = next;
  }
  if (ch != '\n' || ferror(input)) goto done;
  if (tstr_len(reply) && reply[tstr_len(reply) - 1] == '\r')
    tstr_set_len(reply, tstr_len(reply) - 1);
  result = !strcmp(reply, expected);
done:
  json_serialize_free(serialized);
  tstr_free(expected); tstr_free(reply);
  return result;
}

static int approval_stopped(const atomic_int *interrupted, uint64_t deadline) {
  return (interrupted && atomic_load_explicit(interrupted, memory_order_relaxed)) ||
         cmeta_monotonic_ms() >= deadline;
}

int turbo_cli_approval_resume(turbo_cli_approval_t *approval,
    turbo_agent_harness_t *harness, turbo_agent_session_t *session,
    const turbo_agent_harness_run_options_t *options,
    turbo_agent_harness_execution_t **execution, const turbo_cli_options_t *cli,
    FILE *input, FILE *diagnostics, const atomic_int *interrupted) {
  turbo_agent_execution_status_t status;
  turbo_agent_harness_execution_t *next = NULL;
  turbo_agent_session_exec_options_t resume = {.scope = TURBO_SESSION_SCOPE_CHECKPOINT,
      .input_kind = TURBO_SESSION_INPUT_COMMAND};
  turbo_agent_harness_run_options_t run = *options;
  json_value_t *summary = NULL, *state = NULL, *canonical = NULL, *note = NULL;
  json_value_t *request = NULL, *command = NULL;
  tstr checkpoint = NULL, generation = NULL, deadline = NULL;
  const char *text, *id, *name, *args, *session_checkpoint, *run_id, *thread_id;
  size_t remaining = APPROVAL_MAX_NODES;
  cmeta_uuid_t uuid;
  char challenge[SALTS_UUID_STRING_SIZE];
  turbo_tool_effect_flags_t effects = 0;
  int result = -1, run_rc = -1, decision;
  if (turbo_agent_harness_execution_get_status(*execution, &status) != SALTS_OK) return -1;
  if (status != TURBO_AGENT_EXECUTION_INTERRUPTED) return 0;
  turbo_cli_approval_clear(approval);
  if (++approval->reviews > APPROVAL_MAX_REVIEWS ||
      approval_stopped(interrupted, options->deadline_mono_ms) ||
      turbo_agent_tool_authority_snapshot_is_current(approval->agent, &approval->authority) != 1 ||
      turbo_agent_harness_execution_result_code(*execution, &run_rc) != SALTS_OK || run_rc != 0 ||
      turbo_agent_harness_execution_take_result(*execution, &summary, &state) != SALTS_OK) goto done;
  text = json_get_string(summary, "checkpoint_id");
  session_checkpoint = turbo_agent_session_last_checkpoint_id(session);
  run_id = json_get_string(summary, "run_id");
  thread_id = json_get_string(summary, "thread_id");
  if (!text || !text[0] || !session_checkpoint || strcmp(text, session_checkpoint) ||
      !run_id || !turbo_agent_session_last_run_id(session) ||
      strcmp(run_id, turbo_agent_session_last_run_id(session)) ||
      !thread_id || !turbo_agent_session_thread_id(session) ||
      strcmp(thread_id, turbo_agent_session_thread_id(session)) ||
      !turbo_agent_state_review_required(state) || turbo_agent_state_review_approved(state) ||
      !(checkpoint = tstr_dup(text)) ||
      turbo_agent_session_get_checkpoint_state_json_value(session, checkpoint, &canonical) != 0 ||
      !turbo_agent_state_review_required(canonical) || turbo_agent_state_review_approved(canonical) ||
      turbo_agent_state_pending_tool_calls(canonical) != 1) goto done;
  text = turbo_agent_state_review_note(canonical);
  if (!text || strlen(text) > APPROVAL_MAX_BYTES || !turbo_agent_state_review_note(state) ||
      strcmp(text, turbo_agent_state_review_note(state)) ||
      !(note = json_parse(text, strlen(text))) ||
      !approval_strings(note, 0, &remaining)) goto done;
  id = json_get_string(note, "call_id"); name = json_get_string(note, "tool_name");
  args = json_get_string(note, "arguments");
  if (!id || !id[0] || !name || !name[0] || !args || !json_get_string(note, "kind") ||
      strcmp(json_get_string(note, "kind"), "tool_approval") ||
      turbo_tool_registry_get_effects(approval->registry, name, &effects) != TURBO_TOOL_OK ||
      !effects || (effects & (TURBO_TOOL_EFFECT_UNKNOWN | ~TURBO_TOOL_EFFECT_KNOWN_MASK)) ||
      !(effects & ~(TURBO_TOOL_EFFECT_READ | TURBO_TOOL_EFFECT_PURE)) ||
      !(approval->approved_note = tstr_dup(text)) ||
      cmeta_uuid_v4_generate(&uuid) != 0 || cmeta_uuid_format(&uuid, challenge, sizeof(challenge)) != 0)
    goto done;
  approval->effects = effects;
  generation = tstr_format("{}", approval->authority.generation);
  deadline = tstr_format("{}", options->deadline_mono_ms);
  request = json_create_object(); command = json_create_object();
  if (!generation || !deadline || !request || !command ||
      turbo_runtime_json_object_set_string(request, "kind", "cli_tool_approval") ||
      turbo_runtime_json_object_set_string(request, "checkpoint_id", checkpoint) ||
      turbo_runtime_json_object_set_string(request, "run_id", run_id) ||
      turbo_runtime_json_object_set_string(request, "thread_id", thread_id) ||
      turbo_runtime_json_object_set_string(request, "call_id", id) ||
      turbo_runtime_json_object_set_string(request, "tool_name", name) ||
      turbo_runtime_json_object_set_string(request, "arguments", args) ||
      turbo_runtime_json_object_set_string(request, "authority_generation", generation) ||
      turbo_runtime_json_object_set_int64(request, "effects", (int64_t)effects) ||
      turbo_runtime_json_object_set_string(request, "deadline_mono_ms", deadline) ||
      turbo_runtime_json_object_set_string(request, "challenge", challenge) ||
      turbo_runtime_json_object_set_string(command, "kind", "approve_review")) goto done;
  decision = cli->approval ? cli->approval(request, cli->approval_user_data)
                           : turbo_cli_approval_read(request, input, diagnostics);
  if (decision != 1 || approval_stopped(interrupted, options->deadline_mono_ms) ||
      turbo_agent_tool_authority_snapshot_is_current(approval->agent, &approval->authority) != 1)
    goto done;
  resume.checkpoint_id = checkpoint;
  run.session_options = &resume; /* original deadline and cumulative step budget */
  if (turbo_agent_harness_resume(harness, command, &run, &next) != SALTS_OK) goto done;
  turbo_agent_harness_execution_release(*execution);
  *execution = next;
  result = 1;
done:
  if (result != 1) {
    turbo_cli_approval_clear(approval);
    fputs("Tool approval rejected, expired or unavailable; no resume was admitted.\n", diagnostics);
  }
  json_free(summary); json_free(state); json_free(canonical); json_free(note);
  json_free(request); json_free(command); tstr_free(checkpoint); tstr_free(generation); tstr_free(deadline);
  return result;
}
