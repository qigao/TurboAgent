#ifndef TURBO_GRAPH_RUN_LOG_H
#define TURBO_GRAPH_RUN_LOG_H

#include <turbo_agent_api.h>

#include "turbo_event_log.h"
#include "turbo_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_graph_run_log_s turbo_graph_run_log_t;

/**
 * @brief Create one graph run log.
 */
CXX_C_API turbo_graph_run_log_t *turbo_graph_run_log_create(void);

/**
 * @brief Destroy one graph run log.
 */
CXX_C_API void turbo_graph_run_log_destroy(turbo_graph_run_log_t *log);

/**
 * @brief Reset one graph run log to empty state.
 */
CXX_C_API turbo_graph_exec_status_t turbo_graph_run_log_reset(turbo_graph_run_log_t *log);

/**
 * @brief Borrow the canonical event log captured for one run segment.
 */
CXX_C_API const turbo_event_log_t *turbo_graph_run_log_events(const turbo_graph_run_log_t *log);

/**
 * @brief Borrow the latest checkpoint captured for one run segment.
 */
CXX_C_API const turbo_graph_checkpoint_t *
turbo_graph_run_log_checkpoint(const turbo_graph_run_log_t *log);

/**
 * @brief Execute one TurboParser JSON-native graph run and capture canonical events plus latest checkpoint.
 */
CXX_C_API turbo_graph_exec_status_t turbo_graph_run_json_value_log(
    turbo_graph_t *graph, const json_value_t *state,
    const turbo_graph_run_options_t *options, turbo_graph_run_log_t *log,
    turbo_graph_run_result_t *out_result, json_value_t **out_state);

/**
 * @brief Resume one TurboParser JSON-native graph run from checkpoint and capture canonical events.
 */
CXX_C_API turbo_graph_exec_status_t turbo_graph_run_checkpoint_json_value_log(
    turbo_graph_t *graph, const turbo_graph_checkpoint_t *checkpoint,
    const turbo_graph_run_options_t *options, turbo_graph_run_log_t *log,
    turbo_graph_run_result_t *out_result, json_value_t **out_state);

#ifdef __cplusplus
}
#endif

#endif
