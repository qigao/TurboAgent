#ifndef TURBO_AGENT_HARNESS_H
#define TURBO_AGENT_HARNESS_H

#include <turbo_agent_api.h>
#include <salts/thread.h>

#include "turbo_agent_app.h"
#include "turbo_agent_execution.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_HARNESS_ABI_VERSION 1U

typedef struct turbo_agent_harness_s turbo_agent_harness_t;
typedef struct turbo_agent_harness_execution_s turbo_agent_harness_execution_t;

/**
 * Configuration for one embedded TurboAgent harness.
 *
 * The harness creates and owns one app from `app_config`. `executor` is
 * borrowed and must outlive the harness and every execution submitted through
 * it. The executor must have an explicit bounded queue capacity.
 */
typedef struct turbo_agent_harness_config_s {
  size_t struct_size;
  uint32_t abi_version;
  const turbo_agent_app_config_t *app_config;
  turbo_threadpool_t *executor;
} turbo_agent_harness_config_t;

/**
 * Options shared by start, resume, and fork submissions.
 *
 * `graph_options`, `session_options`, and their strings are borrowed only for
 * the submit call; the execution layer copies the values it needs. Event sink
 * user data is borrowed until the execution becomes terminal.
 */
typedef struct turbo_agent_harness_run_options_s {
  size_t struct_size;
  uint32_t abi_version;
  const turbo_graph_run_options_t *graph_options;
  const turbo_agent_session_exec_options_t *session_options;
  uint64_t deadline_mono_ms;
  turbo_event_sink_json_value_fn event_sink;
  void *event_sink_user_data;
} turbo_agent_harness_run_options_t;

CXX_C_API void turbo_agent_harness_config_init(turbo_agent_harness_config_t *config);
CXX_C_API void turbo_agent_harness_run_options_init(turbo_agent_harness_run_options_t *options);

CXX_C_API turbo_agent_harness_t *
turbo_agent_harness_create(const turbo_agent_harness_config_t *config);
CXX_C_API turbo_agent_harness_t *turbo_agent_harness_retain(turbo_agent_harness_t *harness);
CXX_C_API void turbo_agent_harness_release(turbo_agent_harness_t *harness);

/** @brief Return the app owned by the harness. The pointer is borrowed. */
CXX_C_API turbo_agent_app_t *turbo_agent_harness_app(const turbo_agent_harness_t *harness);

CXX_C_API int turbo_agent_harness_get_capabilities(const turbo_agent_harness_t *harness,
                                                   json_value_t **out_capabilities_json);
CXX_C_API int turbo_agent_harness_get_tool_schemas(const turbo_agent_harness_t *harness,
                                                   json_value_t **out_tool_schemas_json);
CXX_C_API int turbo_agent_harness_get_startup_diagnostics(const turbo_agent_harness_t *harness,
                                                          json_value_t **out_diagnostics_json);

/**
 * Start the harness default workflow from a TurboParser JSON-native state.
 * Only one non-terminal execution may be active per harness.
 */
CXX_C_API int turbo_agent_harness_start_state(turbo_agent_harness_t *harness,
                                              const json_value_t *state,
                                              const turbo_agent_harness_run_options_t *options,
                                              turbo_agent_harness_execution_t **out_execution);

/** @brief Start the harness default workflow from one user message. */
CXX_C_API int turbo_agent_harness_start_text(turbo_agent_harness_t *harness, const char *user_text,
                                             const turbo_agent_harness_run_options_t *options,
                                             turbo_agent_harness_execution_t **out_execution);

/**
 * Resume the harness default workflow.
 * `options->session_options` is required and selects checkpoint/thread scope
 * and override/patch/command input semantics.
 */
CXX_C_API int turbo_agent_harness_resume(turbo_agent_harness_t *harness, const json_value_t *input,
                                         const turbo_agent_harness_run_options_t *options,
                                         turbo_agent_harness_execution_t **out_execution);

/** @brief Fork the harness default workflow from a checkpoint or thread head. */
CXX_C_API int turbo_agent_harness_fork(turbo_agent_harness_t *harness, const json_value_t *input,
                                       const turbo_agent_harness_run_options_t *options,
                                       turbo_agent_harness_execution_t **out_execution);

CXX_C_API turbo_agent_harness_execution_t *
turbo_agent_harness_execution_retain(turbo_agent_harness_execution_t *execution);
CXX_C_API void turbo_agent_harness_execution_release(turbo_agent_harness_execution_t *execution);

CXX_C_API const char *
turbo_agent_harness_execution_id(const turbo_agent_harness_execution_t *execution);
CXX_C_API int turbo_agent_harness_execution_cancel(turbo_agent_harness_execution_t *execution,
                                                   turbo_cancel_reason_t reason);
CXX_C_API int turbo_agent_harness_execution_wait(turbo_agent_harness_execution_t *execution,
                                                 uint64_t timeout_ms);
CXX_C_API int
turbo_agent_harness_execution_get_status(const turbo_agent_harness_execution_t *execution,
                                         turbo_agent_execution_status_t *out_status);
CXX_C_API int
turbo_agent_harness_execution_result_code(const turbo_agent_harness_execution_t *execution,
                                          int *out_result_code);

/**
 * Move the terminal summary/state to the caller exactly once.
 * The returned TurboParser JSON values are owned by the caller.
 */
CXX_C_API int turbo_agent_harness_execution_take_result(turbo_agent_harness_execution_t *execution,
                                                        json_value_t **out_summary,
                                                        json_value_t **out_state);

#ifdef __cplusplus
}
#endif

#endif
