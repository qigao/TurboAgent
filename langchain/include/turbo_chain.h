#ifndef TURBO_CHAIN_H
#define TURBO_CHAIN_H

#include <turbo_agent_api.h>
#include <json_parser.h>

#include "turbo_runtime_json.h"
#include "turbo_event.h"
#include "turbo_event_log.h"
#include "turbo_model.h"
#include "turbo_prompt.h"
#include "turbo_tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_chain_s turbo_chain_t;
typedef struct turbo_chain_exec_ctx_s turbo_chain_exec_ctx_t;

typedef enum {
  TURBO_CHAIN_OK = 0,
  TURBO_CHAIN_STOP = 1,
  TURBO_CHAIN_ERROR = -1,
  TURBO_CHAIN_INVALID_ARGUMENT = -2,
  TURBO_CHAIN_OUT_OF_MEMORY = -3,
  TURBO_CHAIN_DUPLICATE_STEP = -4
} turbo_chain_status_t;

typedef void (*turbo_chain_user_data_free_fn)(void *user_data);

typedef int (*turbo_chain_step_fn)(turbo_chain_exec_ctx_t *ctx, void *user_data);
typedef int (*turbo_chain_json_value_step_fn)(turbo_chain_exec_ctx_t *ctx, void *user_data);

struct turbo_chain_exec_ctx_s {
  turbo_chain_t *chain;
  json_value_t *state;
  json_value_t *json_value_state;
  size_t step;
  const char *step_name;
  int stop;
  turbo_event_sink_json_value_fn event_sink;
  void *event_sink_user_data;
};

/**
 * @brief Create a linear chain runtime.
 * @param name Optional chain name.
 * @return Chain handle or NULL on allocation failure.
 */
CXX_C_API turbo_chain_t *turbo_chain_create(const char *name);

/**
 * @brief Destroy a chain runtime.
 * @param chain Chain handle, may be NULL.
 */
CXX_C_API void turbo_chain_destroy(turbo_chain_t *chain);

/**
 * @brief Add a custom step callback to the chain.
 * @param chain Chain handle.
 * @param name Unique step name.
 * @param fn Step callback.
 * @param user_data Opaque step user data.
 * @param user_data_free Optional destructor for user_data.
 * @return Status code.
 */
CXX_C_API turbo_chain_status_t
turbo_chain_add_step(turbo_chain_t *chain, const char *name, turbo_chain_step_fn fn,
                     void *user_data, turbo_chain_user_data_free_fn user_data_free);

/**
 * @brief Add a TurboParser JSON-native custom step callback to the chain.
 * @param chain Chain handle.
 * @param name Unique step name.
 * @param fn JsonValue step callback.
 * @param user_data Opaque step user data.
 * @param user_data_free Optional destructor for user_data.
 * @return Status code.
 */
CXX_C_API turbo_chain_status_t
turbo_chain_add_json_value_step(turbo_chain_t *chain, const char *name, turbo_chain_json_value_step_fn fn,
                          void *user_data, turbo_chain_user_data_free_fn user_data_free);

/**
 * @brief Add a prompt step that renders a template into `state.messages`.
 * @param chain Chain handle.
 * @param name Unique step name.
 * @param role Message role such as `system`, `user`, or `assistant`.
 * @param template_text Prompt template with `{{key}}` placeholders.
 * @return Status code.
 */
CXX_C_API turbo_chain_status_t
turbo_chain_add_prompt_step(turbo_chain_t *chain, const char *name, const char *role,
                            const char *template_text);

/**
 * @brief Add a model step that reads `state.messages` and appends an assistant message.
 * @param chain Chain handle.
 * @param name Unique step name.
 * @param model Model adapter definition. The adapter is copied by value and
 *              remains caller-owned, including `user_data`.
 * @param tools Optional tool registry exposed to the model callback.
 * @return Status code.
 */
CXX_C_API turbo_chain_status_t
turbo_chain_add_model_step(turbo_chain_t *chain, const char *name,
                           const turbo_model_t *model,
                           const turbo_tool_registry_t *tools);

/**
 * @brief Add a tool execution step that consumes `state.pending_tool`.
 * @param chain Chain handle.
 * @param name Unique step name.
 * @param tools Tool registry used to execute pending tool calls.
 * @return Status code.
 */
CXX_C_API turbo_chain_status_t
turbo_chain_add_tool_step(turbo_chain_t *chain, const char *name,
                          const turbo_tool_registry_t *tools);

/**
 * @brief Execute all steps against a mutable chain state object.
 * @param chain Chain handle.
 * @param state Mutable state tree.
 * @return Status code.
 */
CXX_C_API turbo_chain_status_t turbo_chain_run(turbo_chain_t *chain, json_value_t *state);

/**
 * @brief Execute all steps against a TurboParser JSON state boundary.
 * @param chain Chain handle.
 * @param state Optional input state tree. NULL creates a standard empty state.
 * @param out_state Output runtime state tree owned by caller.
 * @return Status code.
 */
CXX_C_API turbo_chain_status_t
turbo_chain_run_json_value(turbo_chain_t *chain, const json_value_t *state,
                     json_value_t **out_state);

/**
 * @brief Execute all steps against a TurboParser JSON state boundary and emit canonical events.
 * @param chain Chain handle.
 * @param state Optional input state tree. NULL creates a standard empty state.
 * @param event_sink Event callback receiving canonical TurboParser JSON-native events.
 * @param event_sink_user_data Opaque pointer passed to event_sink.
 * @param out_state Output runtime state tree owned by caller.
 * @return Status code.
 */
CXX_C_API turbo_chain_status_t turbo_chain_run_json_value_stream(
    turbo_chain_t *chain, const json_value_t *state,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_state);

/**
 * @brief Execute all steps against a TurboParser JSON boundary and capture canonical events.
 * @param chain Chain handle.
 * @param state Optional input state tree. NULL creates a standard empty state.
 * @param log Event log reset and filled for this run.
 * @param out_state Output runtime state tree owned by caller.
 * @return Status code.
 */
CXX_C_API turbo_chain_status_t
turbo_chain_run_json_value_log(turbo_chain_t *chain, const json_value_t *state,
                         turbo_event_log_t *log, json_value_t **out_state);

/**
 * @brief Stop chain execution successfully from inside a custom step.
 * @param ctx Execution context.
 */
CXX_C_API void turbo_chain_ctx_stop(turbo_chain_exec_ctx_t *ctx);

/**
 * @brief Create an empty chain state object with standard fields.
 * @return State object or NULL on allocation failure.
 */
CXX_C_API json_value_t *turbo_chain_state_create(void);

/**
 * @brief Create an empty chain state as a TurboParser JSON value tree.
 * @return State object or NULL on allocation failure.
 */
CXX_C_API json_value_t *turbo_chain_state_create_json_value(void);

/**
 * @brief Ensure and return the `input` object in chain state.
 * @param state Chain state object.
 * @return Input object or NULL on failure.
 */
CXX_C_API json_value_t *turbo_chain_state_input(json_value_t *state);

/**
 * @brief Set a string key under `state.input`.
 * @param state Chain state object.
 * @param key Input key.
 * @param value String value.
 * @return Status code.
 */
CXX_C_API turbo_chain_status_t
turbo_chain_state_input_set_string(json_value_t *state, const char *key, const char *value);

/**
 * @brief Ensure and return the `messages` array in chain state.
 * @param state Chain state object.
 * @return Messages array or NULL on failure.
 */
CXX_C_API json_value_t *turbo_chain_state_messages(json_value_t *state);

/**
 * @brief Ensure and return the `model_outputs` array in chain state.
 * @param state Chain state object.
 * @return Model outputs array or NULL on failure.
 */
CXX_C_API json_value_t *turbo_chain_state_model_outputs(json_value_t *state);

/**
 * @brief Ensure and return the `tool_requests` array in chain state.
 * @param state Chain state object.
 * @return Tool requests array or NULL on failure.
 */
CXX_C_API json_value_t *turbo_chain_state_tool_requests(json_value_t *state);

/**
 * @brief Ensure and return the `tool_results` array in chain state.
 * @param state Chain state object.
 * @return Tool results array or NULL on failure.
 */
CXX_C_API json_value_t *turbo_chain_state_tool_results(json_value_t *state);

/**
 * @brief Get the last model output text stored in chain state.
 * @param state Chain state object.
 * @return Output text or NULL when absent.
 */
CXX_C_API const char *turbo_chain_state_last_output_text(const json_value_t *state);

/**
 * @brief Get the pending tool name stored in chain state.
 * @param state Chain state object.
 * @return Tool name or NULL when absent.
 */
CXX_C_API const char *turbo_chain_state_pending_tool_name(const json_value_t *state);

#ifdef __cplusplus
}
#endif

#endif
