#include "turbo_praktor_approved_template.h"

#include "turbo_runtime_json.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct turbo_praktor_template_session_run_s {
  atomic_size_t refs;
  turbo_agent_session_t *session; /* borrowed: caller drains before destruction */
  const turbo_agent_template_plan_t *plan; /* borrowed frozen projection */
  turbo_graph_t *graph;
  turbo_agent_execution_t *execution;
  char *thread_id;
  char *attempt_id;
  char *approval_id;
  char plan_hash[17];
};

static char *praktor_session_copy_id(const char *source, size_t max_length) {
  size_t i;
  char *copy;
  if (!source || !source[0]) return NULL;
  for (i = 0; source[i]; ++i) {
    const char c = source[i];
    if (i >= max_length ||
        !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return NULL;
  }
  copy = (char *)malloc(i + 1);
  if (copy) memcpy(copy, source, i + 1);
  return copy;
}

void turbo_praktor_template_session_run_release(
    turbo_praktor_template_session_run_t *run) {
  if (!run) return;
  if (atomic_fetch_sub_explicit(&run->refs, 1, memory_order_acq_rel) != 1)
    return;
  atomic_thread_fence(memory_order_acquire);
  turbo_agent_execution_release(run->execution);
  turbo_graph_destroy(run->graph);
  free(run->thread_id);
  free(run->attempt_id);
  free(run->approval_id);
  free(run);
}

static void praktor_session_terminal(
    void *user, turbo_agent_execution_t *execution) {
  (void)execution;
  turbo_praktor_template_session_run_release(
      (turbo_praktor_template_session_run_t *)user);
}

static int praktor_session_node(turbo_graph_exec_ctx_t *ctx, void *user) {
  turbo_praktor_template_session_run_t *run =
      (turbo_praktor_template_session_run_t *)user;
  turbo_agent_template_outcome_t outcome = TURBO_AGENT_TEMPLATE_OUTCOME_INVALID_ARGUMENT;
  turbo_tool_execution_context_t tool_context = {0};
  turbo_tool_status_t status;
  json_value_t *result = NULL;
  const char *saved_hash, *saved_thread, *saved_attempt, *saved_approval;
  int replayed = 0;
  int accepted;

  if (!run || !ctx || !ctx->json_value_state ||
      json_type(ctx->json_value_state) != JSON_OBJECT)
    return -1;
  saved_hash = json_get_string(ctx->json_value_state, "praktor_plan_hash");
  saved_thread = json_get_string(ctx->json_value_state, "praktor_thread_id");
  saved_attempt = json_get_string(ctx->json_value_state, "praktor_attempt_id");
  saved_approval = json_get_string(ctx->json_value_state, "praktor_approval_id");
  if (!saved_hash || strcmp(saved_hash, run->plan_hash) != 0 ||
      !saved_thread || strcmp(saved_thread, run->thread_id) != 0 ||
      !saved_attempt || strcmp(saved_attempt, run->attempt_id) != 0 ||
      !saved_approval || strcmp(saved_approval, run->approval_id) != 0)
    return -1; /* Reject forged/stale resumed identity before all callbacks. */

  /* The controlled Runtime Graph owns this token until the synchronous node
   * returns. Praktor and each HostTool borrow it only inside this call.
   * No second cancel source or Plugin-generation owner is created here. */
  tool_context.struct_size = sizeof(tool_context);
  tool_context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
  tool_context.cancel_token = ctx->cancel_token;
  tool_context.deadline_mono_ms = ctx->cancel_token
      ? turbo_cancel_token_deadline_mono_ms(ctx->cancel_token) : 0;
  tool_context.thread_id = run->thread_id;
  status = turbo_praktor_template_execute_approved(
      turbo_agent_session_runtime(run->session), run->plan,
      run->thread_id, run->attempt_id, run->approval_id,
      &tool_context, &replayed, &outcome, &result);
  accepted = turbo_runtime_json_object_set_int64(
                 ctx->json_value_state, "praktor_tool_status", (int64_t)status) ==
                 TURBO_RUNTIME_JSON_OK &&
             turbo_runtime_json_object_set_int64(
                 ctx->json_value_state, "praktor_outcome", (int64_t)outcome) ==
                 TURBO_RUNTIME_JSON_OK &&
             turbo_runtime_json_object_set(
                 ctx->json_value_state, "praktor_replayed", json_create_bool(replayed)) ==
                 TURBO_RUNTIME_JSON_OK &&
             (!result || turbo_runtime_json_object_set_clone(
                 ctx->json_value_state, "praktor_result", result) ==
                 TURBO_RUNTIME_JSON_OK);
  turbo_runtime_json_destroy(result);
  return accepted && status == TURBO_TOOL_OK ? 0 : -1;
}

int turbo_praktor_template_session_submit(
    cmeta_threadpool_t *executor, turbo_agent_session_t *session,
    const turbo_agent_template_plan_t *plan, const char *attempt_id,
    const char *host_approval_id, const char *checkpoint_id,
    int interrupt_before_workflow, uint64_t deadline_mono_ms,
    turbo_praktor_template_session_run_t **out_run) {
  turbo_praktor_template_session_run_t *run = NULL;
  turbo_agent_session_submit_options_t submit_options;
  turbo_agent_session_exec_options_t session_options = {0};
  turbo_graph_run_options_t graph_options = {0};
  const char *interrupts[] = {"praktor_execute"};
  const char *thread_id;
  const char *checkpoint_thread;
  char semantic_id[64];
  json_value_t *initial_state = NULL, *checkpoint_record = NULL;
  int rc = SALTS_EINVAL;
  if (!out_run) return SALTS_EINVAL;
  *out_run = NULL;
  if (!executor || !session || !plan ||
      !turbo_agent_template_praktor_execution_preflight(plan) ||
      (checkpoint_id && (!checkpoint_id[0] || interrupt_before_workflow)) ||
      cmeta_threadpool_capacity(executor) == 0 ||
      !cmeta_threadpool_is_accepting(executor))
    return SALTS_EINVAL;

  thread_id = turbo_agent_session_thread_id(session);
  run = (turbo_praktor_template_session_run_t *)calloc(1, sizeof(*run));
  if (!run) return SALTS_ENOMEM;
  atomic_init(&run->refs, 2);
  run->session = session;
  run->plan = plan;
  run->thread_id = praktor_session_copy_id(thread_id, 80);
  run->attempt_id = praktor_session_copy_id(attempt_id, 64);
  run->approval_id = praktor_session_copy_id(host_approval_id, 64);
  if (!run->thread_id || !run->attempt_id || !run->approval_id) goto fail;
  snprintf(run->plan_hash, sizeof(run->plan_hash), "%016llx",
           (unsigned long long)turbo_agent_template_plan_hash(plan));
  if (snprintf(semantic_id, sizeof(semantic_id), "praktor-approved-v1-%s",
               run->plan_hash) < 0) goto fail;

  if (checkpoint_id) {
    if (turbo_agent_session_get_checkpoint(
            session, checkpoint_id, &checkpoint_record) != SALTS_OK)
      goto fail;
    checkpoint_thread = json_get_string(checkpoint_record, "thread_id");
    if (!checkpoint_thread ||
        strcmp(checkpoint_thread, run->thread_id) != 0)
      goto fail; /* Never borrow another Session's checkpoint. */
    session_options.scope = TURBO_SESSION_SCOPE_CHECKPOINT;
    session_options.input_kind = TURBO_SESSION_INPUT_OVERRIDE;
    session_options.checkpoint_id = checkpoint_id;
    graph_options.skip_initial_interrupt = 1;
  } else {
    initial_state = json_create_object();
    if (!initial_state ||
        turbo_runtime_json_object_set_string(
            initial_state, "praktor_plan_hash", run->plan_hash) !=
            TURBO_RUNTIME_JSON_OK ||
        turbo_runtime_json_object_set_string(
            initial_state, "praktor_thread_id", run->thread_id) !=
            TURBO_RUNTIME_JSON_OK ||
        turbo_runtime_json_object_set_string(
            initial_state, "praktor_attempt_id", run->attempt_id) !=
            TURBO_RUNTIME_JSON_OK ||
        turbo_runtime_json_object_set_string(
            initial_state, "praktor_approval_id", run->approval_id) !=
            TURBO_RUNTIME_JSON_OK) goto fail;
    if (interrupt_before_workflow) {
      graph_options.interrupt_before_nodes = interrupts;
      graph_options.interrupt_before_count = 1;
    }
  }

  run->graph = turbo_graph_create("praktor-approved-template-run");
  if (!run->graph ||
      turbo_graph_add_json_value_node_ex(
          run->graph, "praktor_execute", semantic_id,
          praktor_session_node, run) != TURBO_GRAPH_EXEC_OK ||
      turbo_graph_set_entry(run->graph, "praktor_execute") !=
          TURBO_GRAPH_EXEC_OK) goto fail;

  turbo_agent_session_submit_options_init(&submit_options);
  submit_options.operation = checkpoint_id ?
      TURBO_AGENT_SESSION_RESUME : TURBO_AGENT_SESSION_START;
  submit_options.graph_options = &graph_options;
  submit_options.session_options = checkpoint_id ? &session_options : NULL;
  submit_options.deadline_mono_ms = deadline_mono_ms;
  submit_options.terminal = praktor_session_terminal;
  submit_options.terminal_user_data = run;

  rc = turbo_agent_session_submit_compiled(
      executor, session, run->graph, initial_state,
      &submit_options, &run->execution);
  if (rc != SALTS_OK) goto fail;
  turbo_runtime_json_destroy(initial_state);
  turbo_runtime_json_destroy(checkpoint_record);
  *out_run = run;
  return SALTS_OK;

fail:
  turbo_runtime_json_destroy(initial_state);
  turbo_runtime_json_destroy(checkpoint_record);
  turbo_praktor_template_session_run_release(run);
  turbo_praktor_template_session_run_release(run);
  return rc == SALTS_EINVAL ? SALTS_EPROTO : rc;
}

int turbo_praktor_template_session_run_wait(
    turbo_praktor_template_session_run_t *run, uint64_t timeout_ms) {
  return run && run->execution ?
      turbo_agent_execution_wait(run->execution, timeout_ms) : SALTS_EINVAL;
}

int turbo_praktor_template_session_run_cancel(
    turbo_praktor_template_session_run_t *run, turbo_cancel_reason_t reason) {
  return run && run->execution ?
      turbo_agent_execution_cancel(run->execution, reason) : SALTS_EINVAL;
}

int turbo_praktor_template_session_run_status(
    const turbo_praktor_template_session_run_t *run,
    turbo_agent_execution_status_t *out_status) {
  return run && run->execution ?
      turbo_agent_execution_get_status(run->execution, out_status) : SALTS_EINVAL;
}

int turbo_praktor_template_session_run_take_result(
    turbo_praktor_template_session_run_t *run,
    json_value_t **out_summary, json_value_t **out_state) {
  return run && run->execution ?
      turbo_agent_execution_take_result(run->execution, out_summary, out_state) :
      SALTS_EINVAL;
}
