#ifndef TURBO_AGENT_EXTENSIONS_H
#define TURBO_AGENT_EXTENSIONS_H

#include <platform.h>

#include "turbo_event.h"
#include "turbo_tool.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_s turbo_agent_t;
typedef struct json_value_s json_value_t;
typedef struct turbo_agent_policy_s turbo_agent_policy_t;
typedef struct turbo_action_tool_registry_s turbo_action_tool_registry_t;
typedef struct turbo_agent_runnable_s turbo_agent_runnable_t;

typedef int (*turbo_agent_middleware_before_model_fn)(
    turbo_agent_t *agent, json_value_t *state, char **inout_request_json,
    void *user_data);
typedef int (*turbo_agent_middleware_after_model_fn)(
    turbo_agent_t *agent, json_value_t *state, const char *request_json,
    char **inout_response_json, int transport_status, void *user_data);
typedef int (*turbo_agent_middleware_before_tool_fn)(
    turbo_agent_t *agent, json_value_t *state, const char *call_id,
    const char *tool_name, char **inout_arguments_json, void *user_data);
typedef int (*turbo_agent_middleware_after_tool_fn)(
    turbo_agent_t *agent, json_value_t *state, const char *call_id,
    const char *tool_name, const char *arguments_json, char **inout_output,
    turbo_tool_status_t tool_status, void *user_data);
typedef void (*turbo_agent_middleware_user_data_free_fn)(void *user_data);

typedef struct turbo_agent_middleware_s {
  turbo_agent_middleware_before_model_fn before_model;
  turbo_agent_middleware_after_model_fn after_model;
  turbo_agent_middleware_before_tool_fn before_tool;
  turbo_agent_middleware_after_tool_fn after_tool;
  void *user_data;
  turbo_agent_middleware_user_data_free_fn user_data_free;
} turbo_agent_middleware_t;

typedef enum {
  TURBO_AGENT_TRACE_MODEL_REQUEST = 0,
  TURBO_AGENT_TRACE_MODEL_RESPONSE = 1,
  TURBO_AGENT_TRACE_TOOL_DISPATCH = 2,
  TURBO_AGENT_TRACE_TOOL_RESULT = 3,
  TURBO_AGENT_TRACE_STRUCTURED_RETRY = 4,
  TURBO_AGENT_TRACE_REPLAN_REQUESTED = 5,
  TURBO_AGENT_TRACE_REVIEW_REQUIRED = 6,
  TURBO_AGENT_TRACE_REVIEW_APPROVED = 7,
  TURBO_AGENT_TRACE_GUARDRAIL_REJECTED = 8,
  TURBO_AGENT_TRACE_MEMORY_LOAD = 9,
  TURBO_AGENT_TRACE_MEMORY_SAVE = 10
} turbo_agent_trace_event_kind_t;

typedef void (*turbo_agent_trace_fn)(
    turbo_agent_t *agent, json_value_t *state,
    turbo_agent_trace_event_kind_t kind, const char *name, const char *detail,
    const char *payload, int status, void *user_data);
typedef void (*turbo_agent_trace_user_data_free_fn)(void *user_data);

typedef struct turbo_agent_trace_sink_s {
  turbo_agent_trace_fn callback;
  void *user_data;
  turbo_agent_trace_user_data_free_fn user_data_free;
} turbo_agent_trace_sink_t;

typedef void (*turbo_agent_trace_json_value_fn)(
    turbo_agent_t *agent, const json_value_t *event, void *user_data);

typedef struct turbo_agent_trace_json_value_sink_s {
  turbo_agent_trace_json_value_fn callback;
  void *user_data;
  turbo_agent_trace_user_data_free_fn user_data_free;
} turbo_agent_trace_json_value_sink_t;

typedef int (*turbo_agent_store_get_fn)(void *user_data, const char *key,
                                               char **out_value_json);
typedef int (*turbo_agent_store_put_fn)(void *user_data, const char *key,
                                               const char *value_json);
typedef int (*turbo_agent_store_delete_fn)(void *user_data, const char *key);
typedef void (*turbo_agent_store_user_data_free_fn)(void *user_data);

typedef struct turbo_agent_store_s {
  turbo_agent_store_get_fn get;
  turbo_agent_store_put_fn put;
  turbo_agent_store_delete_fn remove;
  void *user_data;
  turbo_agent_store_user_data_free_fn user_data_free;
} turbo_agent_store_t;

typedef int (*turbo_agent_runnable_fn)(const json_value_t *input, json_value_t **out_output,
                                        void *user_data);
typedef void (*turbo_agent_runnable_user_data_free_fn)(void *user_data);

typedef struct turbo_agent_runnable_config_s {
  turbo_agent_runnable_fn invoke;
  void *user_data;
  turbo_agent_runnable_user_data_free_fn user_data_free;
} turbo_agent_runnable_config_t;

typedef int (*turbo_agent_guardrail_before_model_fn)(
    turbo_agent_t *agent, const json_value_t *state, const char *request_json,
    const char **out_reason, void *user_data);
typedef int (*turbo_agent_guardrail_after_model_fn)(
    turbo_agent_t *agent, const json_value_t *state, const char *response_json,
    const json_value_t *response, const char **out_reason, void *user_data);
typedef int (*turbo_agent_guardrail_before_tool_fn)(
    turbo_agent_t *agent, const json_value_t *state, const char *call_id,
    const char *tool_name, const char *arguments_json, const char **out_reason,
    void *user_data);
typedef int (*turbo_agent_guardrail_after_tool_fn)(
    turbo_agent_t *agent, const json_value_t *state, const char *call_id,
    const char *tool_name, const char *arguments_json, const char *output,
    turbo_tool_status_t tool_status, const char **out_reason, void *user_data);
typedef void (*turbo_agent_guardrail_user_data_free_fn)(void *user_data);

typedef struct turbo_agent_guardrail_s {
  turbo_agent_guardrail_before_model_fn before_model;
  turbo_agent_guardrail_after_model_fn after_model;
  turbo_agent_guardrail_before_tool_fn before_tool;
  turbo_agent_guardrail_after_tool_fn after_tool;
  void *user_data;
  turbo_agent_guardrail_user_data_free_fn user_data_free;
} turbo_agent_guardrail_t;

/**
 * @brief Register a middleware hook set on an agent.
 *
 * Middleware runs in registration order. Hooks may rewrite request JSON,
 * response JSON, tool arguments, or tool output by replacing the pointed-to
 * string with a newly allocated buffer owned by the agent runtime for the
 * duration of the current call.
 *
 * @param agent Agent handle.
 * @param middleware Hook set copied by value into the agent.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_add_middleware(
    turbo_agent_t *agent, const turbo_agent_middleware_t *middleware);

/**
 * @brief Register a trace sink for runtime agent events.
 *
 * Trace sinks observe model requests/responses, tool dispatch/results, structured
 * output retries, replan requests, and review gates. Payload pointers are borrowed
 * for the duration of the callback only.
 *
 * @param agent Agent handle.
 * @param sink Trace sink copied by value into the agent.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_add_trace_sink(turbo_agent_t *agent,
                                                const turbo_agent_trace_sink_t *sink);

/**
 * @brief Register one TurboParser JSON-native trace sink on an agent.
 */
CXX_C_API int turbo_agent_add_trace_json_value_sink(
    turbo_agent_t *agent, const turbo_agent_trace_json_value_sink_t *sink);

/**
 * @brief Enable or disable automatic trace history capture into runtime state.
 *
 * When enabled, emitted trace events are appended under `state.trace_events`
 * as JSON objects so checkpointed or resumed runs can inspect prior execution
 * without requiring an external sink.
 *
 * @param agent Agent handle.
 * @param enabled Non-zero to enable, zero to disable.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_set_trace_history_enabled(turbo_agent_t *agent,
                                                           int enabled);

/**
 * @brief Attach an optional persistent store to an agent.
 *
 * The store is used by explicit load/save helpers and is intentionally separate
 * from in-memory execution state so callers can choose their own persistence backend.
 *
 * @param agent Agent handle.
 * @param store Store callbacks copied by value into the agent. Pass NULL to clear.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_set_store(turbo_agent_t *agent,
                                           const turbo_agent_store_t *store);

/**
 * @brief Create a simple heap-backed in-memory store implementation.
 * @return Store callbacks with owned user_data, or a zeroed store on failure.
 */
CXX_C_API turbo_agent_store_t turbo_agent_store_memory_create(void);

/**
 * @brief Register a guardrail hook set on an agent.
 *
 * Guardrails are read-only policy checks that may reject model requests,
 * model responses, tool dispatch, or tool results with a reason string.
 *
 * @param agent Agent handle.
 * @param guardrail Guardrail set copied by value into the agent.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_add_guardrail(turbo_agent_t *agent,
                                               const turbo_agent_guardrail_t *guardrail);

/**
 * @brief Attach a ready-made tool-dispatch guardrail backed by turbo_agent_policy.
 *
 * This adapter evaluates bridged `turbo_action_tool` definitions before tool
 * execution and rejects tool calls that policy denies or marks as requiring
 * approval.
 *
 * @param agent Agent handle.
 * @param action_tool_registry Action tool registry used to build the bridged tools.
 * @param policy Optional policy copied by value. Pass NULL for defaults.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_add_action_policy_guardrail(
    turbo_agent_t *agent, const turbo_action_tool_registry_t *action_tool_registry,
    const turbo_agent_policy_t *policy);

/**
 * @brief Return whether the active review note approves exactly one tool call.
 *
 * The workflow tool node stores approval requests as structured review notes.
 * This helper validates that the review is currently approved and that the
 * note's call id, tool name, and arguments JSON match the tool dispatch being
 * considered.
 *
 * @param state Agent state containing the current review gate.
 * @param call_id Tool call id from the pending model tool call.
 * @param tool_name Tool name from the pending model tool call.
 * @param arguments_json Canonical arguments JSON used for dispatch.
 * @return 1 when the approved review matches exactly, else 0.
 */
CXX_C_API int turbo_agent_tool_approval_review_matches(
    const json_value_t *state, const char *call_id, const char *tool_name,
    const char *arguments_json);

/**
 * @brief Create a standalone runnable from a callback.
 * @param config Runnable configuration copied by value.
 * @return Runnable handle or NULL on failure.
 */
CXX_C_API turbo_agent_runnable_t *
turbo_agent_runnable_create(const turbo_agent_runnable_config_t *config);

/**
 * @brief Destroy a runnable handle.
 * @param runnable Runnable handle, may be NULL.
 */
CXX_C_API void turbo_agent_runnable_destroy(turbo_agent_runnable_t *runnable);

/**
 * @brief Invoke a runnable against JSON input.
 * @param runnable Runnable handle.
 * @param input Borrowed input JSON.
 * @param out_output Output JSON owned by caller on success.
 * @return 0 on success, negative on error.
 */
CXX_C_API int turbo_agent_runnable_invoke(const turbo_agent_runnable_t *runnable,
                                           const json_value_t *input, json_value_t **out_output);

/**
 * @brief Compose two runnables into a simple left-to-right pipeline.
 * @param first First runnable. Must outlive the composed runnable.
 * @param second Second runnable. Must outlive the composed runnable.
 * @return Runnable handle or NULL on failure.
 */
CXX_C_API turbo_agent_runnable_t *
turbo_agent_runnable_pipe(const turbo_agent_runnable_t *first,
                           const turbo_agent_runnable_t *second);

#ifdef __cplusplus
}
#endif

#endif


