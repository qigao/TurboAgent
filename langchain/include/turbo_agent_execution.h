#ifndef TURBO_AGENT_EXECUTION_H
#define TURBO_AGENT_EXECUTION_H

#include <turbo_agent_api.h>
#include <turbo_thread.h>

#include "turbo_agent_runtime.h"
#include "turbo_runtime_control.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_EXECUTION_ABI_VERSION 1U

typedef struct turbo_agent_execution_s turbo_agent_execution_t;

typedef enum turbo_agent_execution_status_e {
  TURBO_AGENT_EXECUTION_QUEUED = 0,
  TURBO_AGENT_EXECUTION_RUNNING = 1,
  TURBO_AGENT_EXECUTION_COMPLETED = 2,
  TURBO_AGENT_EXECUTION_INTERRUPTED = 3,
  TURBO_AGENT_EXECUTION_CANCELLED = 4,
  TURBO_AGENT_EXECUTION_TIMED_OUT = 5,
  TURBO_AGENT_EXECUTION_FAILED = 6
} turbo_agent_execution_status_t;

typedef struct turbo_agent_execution_options_s {
  uint32_t struct_size;
  uint32_t abi_version;
  /** Absolute monotonic deadline. Zero disables deadline cancellation. */
  uint64_t deadline_mono_ms;
} turbo_agent_execution_options_t;

/**
 * Submit a durable runtime operation to a caller-owned bounded thread pool.
 *
 * `executor`, `runtime`, `graph`, graph callback user data, checkpoint/event
 * callback user data, and the runtime store must outlive the terminal execution
 * state. Input JSON and option strings are copied before this function returns.
 * The executor should use an explicit queue capacity; queue rejection returns
 * SALTS_EBUSY without publishing an execution handle.
 *
 * A runtime/store may be used concurrently only when its store contract permits
 * it. The built-in memory and file stores should be treated as single-writer.
 */
CXX_C_API int turbo_agent_execution_start(turbo_threadpool_t *executor,
                                          turbo_agent_runtime_t *runtime, turbo_graph_t *graph,
                                          const json_value_t *state,
                                          const turbo_graph_run_options_t *graph_options,
                                          const turbo_agent_runtime_exec_options_t *runtime_options,
                                          const turbo_agent_execution_options_t *execution_options,
                                          turbo_agent_execution_t **out_execution);

CXX_C_API int
turbo_agent_execution_resume(turbo_threadpool_t *executor, turbo_agent_runtime_t *runtime,
                             turbo_graph_t *graph, const json_value_t *input,
                             const turbo_graph_run_options_t *graph_options,
                             const turbo_agent_runtime_exec_options_t *runtime_options,
                             const turbo_agent_execution_options_t *execution_options,
                             turbo_agent_execution_t **out_execution);

CXX_C_API int turbo_agent_execution_fork(turbo_threadpool_t *executor,
                                         turbo_agent_runtime_t *runtime, turbo_graph_t *graph,
                                         const json_value_t *input,
                                         const turbo_graph_run_options_t *graph_options,
                                         const turbo_agent_runtime_exec_options_t *runtime_options,
                                         const turbo_agent_execution_options_t *execution_options,
                                         turbo_agent_execution_t **out_execution);

CXX_C_API turbo_agent_execution_t *turbo_agent_execution_retain(turbo_agent_execution_t *execution);
CXX_C_API void turbo_agent_execution_release(turbo_agent_execution_t *execution);

/** @brief Return the stable UUID execution identifier owned by the handle. */
CXX_C_API const char *turbo_agent_execution_id(const turbo_agent_execution_t *execution);

/** @brief Request cancellation. The first cancellation reason wins. */
CXX_C_API int turbo_agent_execution_cancel(turbo_agent_execution_t *execution,
                                           turbo_cancel_reason_t reason);

/**
 * Wait for a terminal state. timeout_ms is relative; UINT64_MAX waits forever.
 * Returns SALTS_OK at a terminal state or SALTS_ETIMEDOUT for caller wait expiry.
 */
CXX_C_API int turbo_agent_execution_wait(turbo_agent_execution_t *execution, uint64_t timeout_ms);

CXX_C_API int turbo_agent_execution_get_status(const turbo_agent_execution_t *execution,
                                               turbo_agent_execution_status_t *out_status);

/** @brief Return the underlying synchronous runtime return code after terminal. */
CXX_C_API int turbo_agent_execution_result_code(const turbo_agent_execution_t *execution,
                                                int *out_result_code);

/**
 * Move the final summary/state to the caller exactly once.
 * Returns SALTS_EBUSY before terminal and SALTS_EALREADY after a prior take.
 */
CXX_C_API int turbo_agent_execution_take_result(turbo_agent_execution_t *execution,
                                                json_value_t **out_summary,
                                                json_value_t **out_state);

#ifdef __cplusplus
}
#endif

#endif
