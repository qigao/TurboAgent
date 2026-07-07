#ifndef TURBO_RUNNABLE_H
#define TURBO_RUNNABLE_H

#include <platform.h>

#include "turbo_chain.h"
#include "turbo_event_log.h"
#include "turbo_graph.h"
#include "turbo_runtime_data_bind.h"
#include "turbo_state_graph.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "turbo_event.h"

typedef struct turbo_runnable_s turbo_runnable_t;
typedef struct turbo_agent_app_s turbo_agent_app_t;
typedef struct turbo_agent_session_s turbo_agent_session_t;

typedef int (*turbo_runnable_invoke_bind_fn)(
    const turbo_runtime_data_bind_value_t *input,
    turbo_runtime_data_bind_value_t **out_output, void *user_data);
typedef int (*turbo_runnable_invoke_bind_stream_fn)(
    const turbo_runtime_data_bind_value_t *input, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data, turbo_runtime_data_bind_value_t **out_output, void *user_data);
typedef int (*turbo_runnable_batch_bind_fn)(
    const turbo_runtime_data_bind_value_t *inputs,
    turbo_runtime_data_bind_value_t **out_outputs, void *user_data);
typedef void (*turbo_runnable_user_data_free_fn)(void *user_data);
typedef int (*turbo_runnable_before_bind_fn)(
    const turbo_runtime_data_bind_value_t *input,
    turbo_runtime_data_bind_value_t **out_input, void *user_data);
typedef int (*turbo_runnable_after_bind_fn)(
    const turbo_runtime_data_bind_value_t *input,
    const turbo_runtime_data_bind_value_t *output,
    turbo_runtime_data_bind_value_t **out_output, void *user_data);

typedef struct turbo_runnable_config_s {
  turbo_runnable_invoke_bind_fn invoke_bind;
  turbo_runnable_invoke_bind_stream_fn invoke_bind_stream;
  turbo_runnable_batch_bind_fn batch_bind;
  void *user_data;
  turbo_runnable_user_data_free_fn user_data_free;
} turbo_runnable_config_t;

typedef struct turbo_runnable_wrap_config_s {
  turbo_runnable_before_bind_fn before_invoke;
  turbo_runnable_after_bind_fn after_invoke;
  void *user_data;
  turbo_runnable_user_data_free_fn user_data_free;
} turbo_runnable_wrap_config_t;

/**
 * @brief Create one bind-native runnable.
 */
CXX_C_API turbo_runnable_t *turbo_runnable_create(const turbo_runnable_config_t *config);

/**
 * @brief Destroy one runnable handle.
 */
CXX_C_API void turbo_runnable_destroy(turbo_runnable_t *runnable);

/**
 * @brief Invoke one runnable against bind-native input.
 */
CXX_C_API int turbo_runnable_invoke_bind(const turbo_runnable_t *runnable,
                                         const turbo_runtime_data_bind_value_t *input,
                                         turbo_runtime_data_bind_value_t **out_output);

/**
 * @brief Invoke one runnable and stream canonical trace events to one sink.
 */
CXX_C_API int turbo_runnable_invoke_bind_stream(
    const turbo_runnable_t *runnable, const turbo_runtime_data_bind_value_t *input,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    turbo_runtime_data_bind_value_t **out_output);

/**
 * @brief Invoke one runnable and capture canonical events into one log.
 */
CXX_C_API int turbo_runnable_invoke_bind_log(
    const turbo_runnable_t *runnable, const turbo_runtime_data_bind_value_t *input,
    turbo_event_log_t *log, turbo_runtime_data_bind_value_t **out_output);

/**
 * @brief Invoke one runnable for each element in a bind-native input array.
 */
CXX_C_API int turbo_runnable_batch_bind(
    const turbo_runnable_t *runnable, const turbo_runtime_data_bind_value_t *inputs,
    turbo_runtime_data_bind_value_t **out_outputs);

/**
 * @brief Compose two runnables left-to-right.
 */
CXX_C_API turbo_runnable_t *turbo_runnable_pipe(const turbo_runnable_t *first,
                                                const turbo_runnable_t *second);

/**
 * @brief Wrap one runnable with before/after bind-native hooks.
 *
 * The inner runnable is borrowed and must outlive the wrapper. Hook replacement
 * values transfer ownership to the wrapper; NULL means "use the original".
 */
CXX_C_API turbo_runnable_t *turbo_runnable_wrap_bind(
    const turbo_runnable_t *inner, const turbo_runnable_wrap_config_t *config);

/**
 * @brief Wrap one chain as a runnable. The chain must outlive the runnable.
 */
CXX_C_API turbo_runnable_t *turbo_runnable_from_chain(turbo_chain_t *chain);

/**
 * @brief Wrap one graph as a runnable. The graph must outlive the runnable.
 */
CXX_C_API turbo_runnable_t *turbo_runnable_from_graph(
    turbo_graph_t *graph, const turbo_graph_run_options_t *options,
    turbo_graph_run_result_t *result_sink);

/**
 * @brief Wrap one state graph as a runnable. The graph must outlive the runnable.
 */
CXX_C_API turbo_runnable_t *turbo_runnable_from_state_graph(
    turbo_state_graph_t *graph, const char *thread_id,
    const turbo_state_graph_run_options_t *options,
    turbo_state_graph_run_result_t *result_sink);

/**
 * @brief Wrap one agent session as a runnable. The session must outlive the runnable.
 *
 * String inputs are treated as user text. Object inputs may contain string
 * `input`, `text`, or `user_text`, or a canonical prompt-message array under
 * `messages`.
 */
CXX_C_API turbo_runnable_t *turbo_runnable_from_agent_session(
    turbo_agent_session_t *session, const turbo_graph_run_options_t *options);

/**
 * @brief Wrap one agent app as a runnable. The app must outlive the runnable.
 *
 * Uses the same input and output shape as `turbo_runnable_from_agent_session`.
 */
CXX_C_API turbo_runnable_t *turbo_runnable_from_agent_app(
    turbo_agent_app_t *app, const turbo_graph_run_options_t *options);

#ifdef __cplusplus
}
#endif

#endif
