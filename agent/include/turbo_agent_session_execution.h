#ifndef TURBO_AGENT_SESSION_EXECUTION_H
#define TURBO_AGENT_SESSION_EXECUTION_H

#include "turbo_agent_session.h"
#include "turbo_agent_execution.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_SESSION_SUBMIT_ABI_VERSION 1U

typedef enum turbo_agent_session_operation_e {
  TURBO_AGENT_SESSION_START = 0,
  TURBO_AGENT_SESSION_RESUME = 1,
  TURBO_AGENT_SESSION_FORK = 2
} turbo_agent_session_operation_t;

/**
 * Called once on the worker after result/status publication for an accepted
 * submission, including execution/completion failures. Not called on rejection.
 * execution is borrowed for the callback; retain it to keep a reference.
 * The callback may run before submit returns. It may release caller-owned
 * session/graph resources, which this operation no longer accesses. It must not
 * destroy/join the executor from its worker. Do not consume the result here if
 * another owner is responsible for take_result().
 */
typedef void (*turbo_agent_session_terminal_fn)(void *user_data,
                                                 turbo_agent_execution_t *execution);

typedef struct turbo_agent_session_submit_options_s {
  size_t struct_size;
  uint32_t abi_version;
  turbo_agent_session_operation_t operation;
  const turbo_graph_run_options_t *graph_options;
  /** Required for RESUME/FORK; must be NULL for START. */
  const turbo_agent_session_exec_options_t *session_options;
  uint64_t deadline_mono_ms;
  turbo_event_sink_json_value_fn event_sink;
  void *event_sink_user_data;
  turbo_agent_session_terminal_fn terminal;
  void *terminal_user_data;
} turbo_agent_session_submit_options_t;

/** Initialize versioned options to START, with no deadline or callbacks. */
CXX_C_API void turbo_agent_session_submit_options_init(
    turbo_agent_session_submit_options_t *options);

/**
 * Submit one Session operation through a caller-owned bounded executor.
 * Session owns summary synchronization, inbox claim commit, and follow-up runs;
 * their failures propagate to the execution result before terminal publication.
 * No extra thread/run/checkpoint state is created by this adapter.
 *
 * executor, session, graph, and graph callback data are borrowed. Keep them
 * alive through terminal publication and any terminal callback that uses them;
 * terminal_user_data must additionally live
 * until the terminal callback returns. wait() observes published state and does
 * not join that callback. Drain the pool or synchronize callback completion
 * before reclaiming terminal_user_data. Input JSON and option strings are copied
 * before return; graph/Session/event callback data are not copied or retained.
 * Event callback data must live through terminal publication. The caller must
 * serialize operations on a Session and obey its store's single-writer contract.
 *
 * options may be NULL for a default START. input is required for START and has
 * the existing Session override/patch/command meaning for RESUME/FORK.
 * Returns SALTS_OK with one owned execution reference on acceptance. Invalid
 * arguments/version return SALTS_EINVAL; allocation failures return SALTS_ENOMEM;
 * bounded admission errors (ENOBUFS/ESHUTDOWN) and other Execution errors propagate.
 * *out_execution is NULL on rejection; no terminal callback then runs.
 *
 * Example (resources remain live until the pool is drained):
 *   turbo_agent_execution_t *run = NULL;
 *   if (turbo_agent_session_submit(pool, session, graph, state, NULL, &run) == SALTS_OK) {
 *     turbo_agent_execution_wait(run, UINT64_MAX);
 *     turbo_agent_execution_release(run);
 *   }
 */
CXX_C_API int turbo_agent_session_submit(cmeta_threadpool_t *executor,
    turbo_agent_session_t *session, turbo_graph_t *graph, const json_value_t *input,
    const turbo_agent_session_submit_options_t *options,
    turbo_agent_execution_t **out_execution);

#ifdef __cplusplus
}
#endif
#endif
