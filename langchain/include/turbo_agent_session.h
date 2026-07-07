#ifndef TURBO_AGENT_SESSION_H
#define TURBO_AGENT_SESSION_H

#include <platform.h>

#include "turbo_agent.h"
#include "turbo_agent_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_session_s turbo_agent_session_t;
typedef struct turbo_agent_knowledge_store_s turbo_agent_knowledge_store_t;
typedef struct turbo_retriever_s turbo_retriever_t;

typedef enum turbo_agent_session_workflow_kind_e {
  TURBO_AGENT_SESSION_WORKFLOW_LOOP = 0,
  TURBO_AGENT_SESSION_WORKFLOW_REVIEW = 1,
  TURBO_AGENT_SESSION_WORKFLOW_ENGINEERING = 2,
  TURBO_AGENT_SESSION_WORKFLOW_KNOWLEDGE_ENGINEERING = 3,
  TURBO_AGENT_SESSION_WORKFLOW_RETRIEVER_ENGINEERING = 4
} turbo_agent_session_workflow_kind_t;

typedef struct turbo_agent_session_config_s {
  turbo_agent_runtime_store_t runtime_store;
  turbo_agent_memory_store_t memory_store;
  turbo_agent_config_t agent_config;
  const char *thread_id;
  const char *env_path;
  int load_env;
  int overwrite_env;
  turbo_agent_session_workflow_kind_t workflow_kind;
  const char *memory_namespace;
  const char *parent_agent_run_id;
  const char *parent_tool_call_id;
  const char *parent_tool_name;
  const char *parent_graph_run_id;
  const char *call_frame_id;
  turbo_agent_knowledge_store_t *knowledge_store;
  const char *knowledge_query;
  const char *knowledge_kind;
  const char *knowledge_uri_prefix;
  size_t knowledge_limit;
  turbo_retriever_t *retriever;
  const char *retriever_query;
  const char *retriever_kind;
  const char *retriever_uri_prefix;
  size_t retriever_limit;
  const char *retriever_scope;
} turbo_agent_session_config_t;

/* ── Scope and input-kind for resume / fork execution ─────────────────────── */

typedef enum turbo_agent_session_scope_e {
  /* Operate on an explicit or session-cached checkpoint. */
  TURBO_SESSION_SCOPE_CHECKPOINT = 0,
  /* Operate on the session's owned thread head. */
  TURBO_SESSION_SCOPE_THREAD = 1,
} turbo_agent_session_scope_t;

typedef enum turbo_agent_session_input_kind_e {
  /* Input is a full state override (replaces existing state). */
  TURBO_SESSION_INPUT_OVERRIDE = 0,
  /* Input is a state patch (objects merged, scalars/arrays replaced). */
  TURBO_SESSION_INPUT_PATCH = 1,
  /* Input is a runtime command object. */
  TURBO_SESSION_INPUT_COMMAND = 2,
} turbo_agent_session_input_kind_t;

typedef struct turbo_agent_session_exec_options_s {
  turbo_agent_session_scope_t scope;
  turbo_agent_session_input_kind_t input_kind;
  /* SCOPE_CHECKPOINT: explicit checkpoint id, or NULL to use session's cached
   * last_checkpoint_id.  Ignored for SCOPE_THREAD. */
  const char *checkpoint_id;
} turbo_agent_session_exec_options_t;

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

/**
 * @brief Create one high-level session wrapper over agent + runtime.
 *
 * The session owns:
 * - one durable runtime
 * - one optional agent created from `agent_config`
 *
 * When `load_env` is non-zero, `agent_config` is first passed through
 * `turbo_agent_config_apply_env(...)`. When `runtime_store` is zeroed, a
 * heap-backed runtime store is created.
 *
 * @param config Session configuration copied by value.
 * @return Session handle or NULL on failure.
 */
CXX_C_API turbo_agent_session_t *
turbo_agent_session_create(const turbo_agent_session_config_t *config);

/**
 * @brief Destroy one session wrapper.
 * @param session Session handle, may be NULL.
 */
CXX_C_API void turbo_agent_session_destroy(turbo_agent_session_t *session);

/* ── Accessors ─────────────────────────────────────────────────────────────── */

/** @brief Return the owned agent handle, or NULL when runtime-only. */
CXX_C_API turbo_agent_t *turbo_agent_session_agent(const turbo_agent_session_t *session);

/** @brief Return the owned durable runtime handle. */
CXX_C_API turbo_agent_runtime_t *
turbo_agent_session_runtime(const turbo_agent_session_t *session);

/** @brief Return the owned optional long-term memory store callbacks. */
CXX_C_API const turbo_agent_memory_store_t *
turbo_agent_session_memory_store(const turbo_agent_session_t *session);

/**
 * @brief Return the tool registry visible to the owned agent.
 *
 * The returned pointer is borrowed. It may be NULL when the session is
 * runtime-only or the owned agent has no tools.
 */
CXX_C_API const turbo_tool_registry_t *
turbo_agent_session_tool_registry(const turbo_agent_session_t *session);

/** @brief Return the number of tools visible to the owned agent. */
CXX_C_API size_t turbo_agent_session_tool_count(const turbo_agent_session_t *session);

/** @brief Return the effective model string known to the session. */
CXX_C_API const char *turbo_agent_session_model(const turbo_agent_session_t *session);

/** @brief Return the effective base URL string known to the session. */
CXX_C_API const char *turbo_agent_session_base_url(const turbo_agent_session_t *session);

/** @brief Return the effective provider name known to the session. */
CXX_C_API const char *turbo_agent_session_provider_name(
    const turbo_agent_session_t *session);

/** @brief Return whether the session resolved any API key. */
CXX_C_API int turbo_agent_session_has_api_key(const turbo_agent_session_t *session);

/** @brief Return the current session thread id, or NULL before first start. */
CXX_C_API const char *turbo_agent_session_thread_id(const turbo_agent_session_t *session);

/** @brief Return the last run id observed by the session. */
CXX_C_API const char *turbo_agent_session_last_run_id(
    const turbo_agent_session_t *session);

/** @brief Return the most recent non-null checkpoint id observed by the session. */
CXX_C_API const char *turbo_agent_session_last_checkpoint_id(
    const turbo_agent_session_t *session);

/** @brief Return the default canned workflow kind remembered by the session. */
CXX_C_API turbo_agent_session_workflow_kind_t
turbo_agent_session_workflow_kind(const turbo_agent_session_t *session);

/** @brief Return the default long-term memory namespace prefix. */
CXX_C_API const char *turbo_agent_session_memory_namespace(
    const turbo_agent_session_t *session);

/**
 * @brief Return a host-facing snapshot of the session's configured harness capabilities.
 *
 * The returned JSON object is owned by the caller. It reports read-only facts
 * such as workflow kind, runtime/agent presence, memory store presence, tool
 * count, model routing metadata, and optional knowledge/retriever wiring.
 */
CXX_C_API int turbo_agent_session_get_capabilities(
    const turbo_agent_session_t *session, json_value_t **out_capabilities_json);

/**
 * @brief Return model-provider tool schema views for the session's tool registry.
 *
 * The returned JSON object is owned by the caller and contains `tool_count`,
 * `has_tool_registry`, `registry`, `openai_responses`, `openai_chat`,
 * `openai_compatible_chat`, and `anthropic`. Empty or missing registries return
 * empty arrays. Invalid schemas or provider-incompatible tool names fail with
 * -1 so hosts can reject a bad harness configuration before execution.
 */
CXX_C_API int turbo_agent_session_get_tool_schemas(
    const turbo_agent_session_t *session, json_value_t **out_tool_schemas_json);

/**
 * @brief Return one startup health snapshot for host-side harness validation.
 *
 * The returned JSON object is owned by the caller. It contains `ok`, `errors`,
 * `capabilities`, and `tool_schemas`. Tool schema validation failures are
 * reported as `ok=false` with an `invalid_tool_schemas` error instead of
 * hiding the rest of the diagnostic context.
 */
CXX_C_API int turbo_agent_session_get_startup_diagnostics(
    const turbo_agent_session_t *session, json_value_t **out_diagnostics_json);

/* ── Observability sinks ───────────────────────────────────────────────────── */

CXX_C_API int turbo_agent_session_add_trace_bind_sink(
    turbo_agent_session_t *session, const turbo_agent_trace_bind_sink_t *sink);

CXX_C_API int turbo_agent_session_add_observer_bind_sink(
    turbo_agent_session_t *session, const turbo_agent_observer_bind_sink_t *sink);

CXX_C_API int turbo_agent_session_set_trace_history_enabled(
    turbo_agent_session_t *session, int enabled);

/* ── Runtime record queries ────────────────────────────────────────────────── */

/** @brief Load the current thread record through the owned runtime. */
CXX_C_API int turbo_agent_session_get_thread(turbo_agent_session_t *session,
                                             json_value_t **out_thread_json);

/**
 * @brief Load one run record through the owned runtime.
 *
 * When `run_id` is NULL or empty, the session uses its latest cached run id.
 */
CXX_C_API int turbo_agent_session_get_run(turbo_agent_session_t *session, const char *run_id,
                                          json_value_t **out_run_json);

/** @brief Load the newest run record for the session thread. */
CXX_C_API int turbo_agent_session_get_latest_run(turbo_agent_session_t *session,
                                                 json_value_t **out_run_json);

/** @brief Load the newest interrupted run record for the session thread. */
CXX_C_API int turbo_agent_session_get_pending_run(turbo_agent_session_t *session,
                                                  json_value_t **out_run_json);

/**
 * @brief Load one checkpoint record through the owned runtime.
 *
 * When `checkpoint_id` is NULL or empty, the session uses its latest cached
 * checkpoint id.
 */
CXX_C_API int turbo_agent_session_get_checkpoint(turbo_agent_session_t *session,
                                                 const char *checkpoint_id,
                                                 json_value_t **out_checkpoint_json);

/**
 * @brief Load one run's latest checkpoint record through the owned runtime.
 *
 * When `run_id` is NULL or empty, the session uses its latest cached run id.
 */
CXX_C_API int turbo_agent_session_get_latest_checkpoint(turbo_agent_session_t *session,
                                                        const char *run_id,
                                                        json_value_t **out_checkpoint_json);

/* ── State bind queries ────────────────────────────────────────────────────── */

/**
 * @brief Load the session thread's latest persisted state.
 *
 * Resolves the newest run by `updated_at`. Prefer `get_thread_head_state_bind`
 * for thread-head semantics.
 */
CXX_C_API int turbo_agent_session_get_thread_state_bind(
    turbo_agent_session_t *session, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Load the session thread head state snapshot.
 *
 * The runtime resolves the newest interrupted run first, then falls back to
 * the newest run by `updated_at`.
 */
CXX_C_API int turbo_agent_session_get_thread_head_state_bind(
    turbo_agent_session_t *session, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Load the session thread's latest-run trace events.
 *
 * Resolves by `updated_at`. Prefer `get_thread_head_trace_events_bind` for
 * thread-head semantics.
 */
CXX_C_API int turbo_agent_session_get_thread_trace_events_bind(
    turbo_agent_session_t *session, turbo_runtime_data_bind_value_t **out_events);

/** @brief Load the session thread head trace-event snapshot. */
CXX_C_API int turbo_agent_session_get_thread_head_trace_events_bind(
    turbo_agent_session_t *session, turbo_runtime_data_bind_value_t **out_events);

/**
 * @brief Load one run's latest persisted state.
 *
 * When `run_id` is NULL or empty, uses the session's latest cached run id.
 */
CXX_C_API int turbo_agent_session_get_run_state_bind(
    turbo_agent_session_t *session, const char *run_id,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_session_get_run_trace_events_bind(
    turbo_agent_session_t *session, const char *run_id,
    turbo_runtime_data_bind_value_t **out_events);

/**
 * @brief Load one checkpoint's serialized state.
 *
 * When `checkpoint_id` is NULL or empty, uses the session's latest cached
 * checkpoint id.
 */
CXX_C_API int turbo_agent_session_get_checkpoint_state_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_state);

/* ── State-override preparers (read + merge; no persistence) ──────────────── */

CXX_C_API int turbo_agent_session_prepare_checkpoint_state_override_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);

CXX_C_API int turbo_agent_session_prepare_thread_state_override_bind(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);

/**
 * @brief Prepare one checkpoint-scoped state override from a bind-native patch.
 *
 * Loads the checkpoint state, recursively merges object fields from
 * `state_patch`, and returns the resulting full state. Arrays, scalars and null
 * replace the target value. Does not persist the prepared override.
 */
CXX_C_API int turbo_agent_session_apply_checkpoint_state_patch_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);

/**
 * @brief Prepare one thread-head state override from a bind-native patch.
 *
 * The runtime resolves the thread's newest interrupted run first, falls back
 * to the newest run by `updated_at`, then applies the patch. Does not persist.
 */
CXX_C_API int turbo_agent_session_apply_thread_state_patch_bind(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);

/* ── Command preparers / appliers (no graph execution) ────────────────────── */

/**
 * @brief Prepare one checkpoint-scoped command-derived state override.
 *
 * Interprets the command against the checkpoint's serialized state and returns
 * the resulting state override. Does not persist.
 */
CXX_C_API int turbo_agent_session_prepare_checkpoint_command_override_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override);

/**
 * @brief Prepare one thread-head command-derived state override.
 *
 * Resolves thread head, applies command, returns override. Does not persist.
 */
CXX_C_API int turbo_agent_session_prepare_thread_command_override_bind(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override);

/**
 * @brief Apply one host-facing runtime command to a checkpoint state.
 *
 * When `checkpoint_id` is NULL or empty, uses the session's latest cached
 * checkpoint id.
 */
CXX_C_API int turbo_agent_session_apply_command_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override);

/**
 * @brief Apply one host-facing runtime command to an explicit checkpoint.
 *
 * Requires a non-empty `checkpoint_id`; does not fall back to the session's
 * cached checkpoint.
 */
CXX_C_API int turbo_agent_session_apply_checkpoint_command_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override);

/** @brief Apply one host-facing runtime command to the session's owned thread. */
CXX_C_API int turbo_agent_session_apply_thread_command_bind(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override);

/* ── Supervisor / inbox / inspect ─────────────────────────────────────────── */

/**
 * @brief Load the current thread supervisor inbox as a JSON array.
 */
CXX_C_API int turbo_agent_session_get_supervisor_inbox(
    turbo_agent_session_t *session, json_value_t **out_inbox_json);

/**
 * @brief Load the current thread supervisor handoff history as a JSON array.
 */
CXX_C_API int turbo_agent_session_get_supervisor_handoff_history(
    turbo_agent_session_t *session, json_value_t **out_history_json);

/**
 * @brief Load one host-facing supervisor/mailbox inspect bundle.
 *
 * Returns a JSON object with `supervisor`, `inbox`, `handoff_history`,
 * `control_snapshot`, and `workflow_snapshot`.
 */
CXX_C_API int turbo_agent_session_get_supervisor_inspect(
    turbo_agent_session_t *session, json_value_t **out_inspect_json);

/**
 * @brief Load one host-facing orchestration inspect bundle.
 *
 * Returns a JSON object with `supervisor_inspect`, `thread_timeline`,
 * `thread_lineage`, `branch_tree`, and `child_runs`.
 */
CXX_C_API int turbo_agent_session_get_orchestration_inspect(
    turbo_agent_session_t *session, json_value_t **out_inspect_json);

/**
 * @brief Load one thread-scoped observability index.
 */
CXX_C_API int turbo_agent_session_get_observability_index(
    turbo_agent_session_t *session, json_value_t **out_index_json);

/**
 * @brief Append one supervisor inbox message and return a state override.
 *
 * The returned bind value can be fed into a resume/fork exec_options as an
 * OVERRIDE input. Does not persist state by itself.
 */
CXX_C_API int turbo_agent_session_append_supervisor_inbox_message_bind(
    turbo_agent_session_t *session, const char *source_agent, const char *text,
    turbo_runtime_data_bind_value_t **out_state_override);

CXX_C_API int turbo_agent_session_get_checkpoint_trace_events_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_events);

/* ── Child run / checkpoint inspection ────────────────────────────────────── */

/**
 * @brief Load one child run record referenced by a parent tool-result output item.
 *
 * The `output_item` must carry `child_run_id`.
 */
CXX_C_API int turbo_agent_session_get_child_run(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_run_json);

/**
 * @brief Load one child checkpoint record referenced by a parent tool-result output item.
 *
 * The `output_item` must carry `child_checkpoint_id`.
 */
CXX_C_API int turbo_agent_session_get_child_checkpoint(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_checkpoint_json);

/**
 * @brief Load one child checkpoint inspect context referenced by a parent output item.
 */
CXX_C_API int turbo_agent_session_get_child_checkpoint_context(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_context_json);

/**
 * @brief Load one child thread timeline referenced by a parent output item.
 *
 * The `output_item` must carry `child_thread_id`.
 */
CXX_C_API int turbo_agent_session_get_child_thread_timeline_bind(
    turbo_agent_session_t *session, const json_value_t *output_item,
    turbo_runtime_data_bind_value_t **out_timeline);

/**
 * @brief Load one child branch tree referenced by a parent output item.
 */
CXX_C_API int turbo_agent_session_get_child_branch_tree(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_branch_tree_json);

/**
 * @brief Load one child execution inspect bundle from a parent output item.
 */
CXX_C_API int turbo_agent_session_get_child_inspect(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_inspect_json);

/**
 * @brief Load one child orchestration inspect bundle from a parent output item.
 */
CXX_C_API int turbo_agent_session_get_child_orchestration_inspect(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_inspect_json);

/**
 * @brief Load one multi-agent inspect bundle for a child tool-result output item.
 */
CXX_C_API int turbo_agent_session_get_child_multi_agent_inspect(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_inspect_json);

/* ── List / lineage / timeline queries ────────────────────────────────────── */

/** @brief List all runs for the current session thread. */
CXX_C_API int turbo_agent_session_list_runs(turbo_agent_session_t *session,
                                            json_value_t **out_runs_json);

/**
 * @brief List child runs by `parent_agent_run_id`.
 *
 * When `parent_agent_run_id` is NULL or empty, the session uses its configured
 * default parent agent run id, then falls back to the current execution context.
 */
CXX_C_API int turbo_agent_session_list_child_runs(
    turbo_agent_session_t *session, const char *parent_agent_run_id,
    json_value_t **out_runs_json);

/**
 * @brief List checkpoints for one child run from a parent output item.
 */
CXX_C_API int turbo_agent_session_list_child_checkpoints(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_checkpoints_json);

/**
 * @brief List checkpoints for one run.
 *
 * When `run_id` is NULL or empty, uses the session's latest cached run id.
 */
CXX_C_API int turbo_agent_session_list_checkpoints(turbo_agent_session_t *session,
                                                   const char *run_id,
                                                   json_value_t **out_checkpoints_json);

/** @brief List one thread-scoped lineage summary. */
CXX_C_API int turbo_agent_session_list_thread_lineage(turbo_agent_session_t *session,
                                                      json_value_t **out_lineage_json);

CXX_C_API int turbo_agent_session_get_branch_tree(turbo_agent_session_t *session,
                                                  json_value_t **out_branch_tree_json);

/** @brief Load one explicit checkpoint inspect context. */
CXX_C_API int turbo_agent_session_get_checkpoint_context(
    turbo_agent_session_t *session, const char *checkpoint_id,
    json_value_t **out_context_json);

/* ── History / replay / observe ───────────────────────────────────────────── */

/**
 * @brief Load persisted history events.
 *
 * When `run_id` is NULL or empty, uses the session's latest cached run id.
 * When `checkpoint_id` is NULL or empty, uses the session's latest cached
 * checkpoint id.
 */
CXX_C_API int turbo_agent_session_load_history_events_bind(
    turbo_agent_session_t *session, const char *run_id, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_events);

/**
 * @brief Load current thread history.
 *
 * Prefers the newest interrupted run on the thread, then falls back to the
 * newest run by `updated_at`.
 */
CXX_C_API int turbo_agent_session_load_thread_history_events_bind(
    turbo_agent_session_t *session, turbo_runtime_data_bind_value_t **out_events);

CXX_C_API int turbo_agent_session_replay_history_bind(
    turbo_agent_session_t *session, const char *run_id, const char *checkpoint_id,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data);

CXX_C_API int turbo_agent_session_replay_thread_history_bind(
    turbo_agent_session_t *session, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data);

/**
 * @brief Observe persisted history through the runtime-backed observer bridge.
 *
 * When `run_id` is NULL or empty, uses the session's latest cached run id.
 * When `checkpoint_id` is NULL or empty, uses the session's latest cached
 * checkpoint id.
 */
CXX_C_API int turbo_agent_session_observe_history_bind(
    turbo_agent_session_t *session, const char *run_id, const char *checkpoint_id,
    const turbo_agent_observer_bind_sink_t *sink);

/** @brief Observe the current session thread lineage through the observer bridge. */
CXX_C_API int turbo_agent_session_observe_thread_history_bind(
    turbo_agent_session_t *session, const turbo_agent_observer_bind_sink_t *sink);

CXX_C_API int turbo_agent_session_get_thread_timeline_bind(
    turbo_agent_session_t *session, turbo_runtime_data_bind_value_t **out_timeline);

/* ── Child history / trace ─────────────────────────────────────────────────── */

/**
 * @brief Load one child run history referenced by a parent output item.
 *
 * Prefers `child_checkpoint_id` when present, else falls back to `child_run_id`.
 */
CXX_C_API int turbo_agent_session_load_child_history_events_bind(
    turbo_agent_session_t *session, const json_value_t *output_item,
    turbo_runtime_data_bind_value_t **out_events);

/**
 * @brief Load one child run's persisted trace events from a parent output item.
 *
 * Prefers `child_checkpoint_id` when present, else falls back to `child_run_id`.
 */
CXX_C_API int turbo_agent_session_get_child_trace_events_bind(
    turbo_agent_session_t *session, const json_value_t *output_item,
    turbo_runtime_data_bind_value_t **out_events);

/* ── Graph factories ───────────────────────────────────────────────────────── */

/**
 * @brief Build one minimal `model -> tools -> model -> end` graph.
 *
 * Returns NULL when the session has no owned agent.
 */
CXX_C_API turbo_graph_t *
turbo_agent_session_create_loop_graph(const turbo_agent_session_t *session);

/**
 * @brief Build one planner + review + executor graph.
 *
 * Returns NULL when the session has no owned agent.
 */
CXX_C_API turbo_graph_t *
turbo_agent_session_create_review_graph(const turbo_agent_session_t *session);

/**
 * @brief Build the default engineering loop graph.
 *
 * Returns NULL when the session has no owned agent.
 */
CXX_C_API turbo_graph_t *
turbo_agent_session_create_engineering_graph(const turbo_agent_session_t *session);

/**
 * @brief Build the engineering loop with a knowledge-context node before planning.
 *
 * Returns NULL when the session has no owned agent or no configured knowledge store.
 */
CXX_C_API turbo_graph_t *
turbo_agent_session_create_knowledge_engineering_graph(
    const turbo_agent_session_t *session);

/**
 * @brief Build the engineering loop with a generic retriever-context node.
 *
 * Returns NULL when the session has no owned agent or no configured retriever.
 */
CXX_C_API turbo_graph_t *
turbo_agent_session_create_retriever_engineering_graph(
    const turbo_agent_session_t *session);

/**
 * @brief Build one canned workflow graph around the owned agent.
 *
 * Returns NULL when the session has no owned agent or `kind` is unknown.
 */
CXX_C_API turbo_graph_t *turbo_agent_session_create_preset_graph(
    const turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind);

/* ── Input state factories ─────────────────────────────────────────────────── */

/**
 * @brief Create one bind-native agent state seeded with an optional user message.
 *
 * Returns NULL on allocation or conversion failure.
 */
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_agent_session_create_input_state_bind(const char *user_text);

/**
 * @brief Create one bind-native agent state seeded with canonical prompt messages.
 *
 * `messages` must be a bind-native array of canonical prompt message objects.
 * Returns NULL on validation, allocation, or conversion failure.
 */
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_agent_session_create_input_messages_state_bind(
    const turbo_runtime_data_bind_value_t *messages);

/**
 * @brief Create one bind-native agent state from user text and session memory context.
 */
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_agent_session_create_input_state_with_memory_bind(
    const turbo_agent_session_t *session, const char *user_text,
    const char *namespace_prefix);

/**
 * @brief Create one bind-native agent state from canonical messages and session memory context.
 */
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_agent_session_create_input_messages_state_with_memory_bind(
    const turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *messages,
    const char *namespace_prefix);

/* ── Core graph execution ──────────────────────────────────────────────────── */

/**
 * @brief Start one new graph run and update cached session ids.
 *
 * Pass `event_sink` / `event_sink_user_data` to receive live canonical events;
 * pass NULL for both to run without streaming. `out_summary_json` may be NULL
 * when the caller only needs the result state.
 */
CXX_C_API int turbo_agent_session_start_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state, const turbo_graph_run_options_t *options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Resume one checkpointed graph run and update cached session ids.
 *
 * `exec_options` controls the scope (checkpoint / thread), the input semantics
 * (override / patch / command), and the optional explicit checkpoint id.
 * When `input_kind` is `TURBO_SESSION_INPUT_OVERRIDE`, `input` may be NULL to
 * resume from the resolved checkpoint/thread head without replacing state.
 * `out_summary_json` may be NULL when the caller only needs the result state.
 */
CXX_C_API int turbo_agent_session_resume_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_session_exec_options_t *exec_options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Fork one new run from a checkpoint and update cached session ids.
 *
 * `exec_options` controls the scope (checkpoint / thread), the input semantics
 * (override / patch / command), and the optional explicit checkpoint id.
 * When `input_kind` is `TURBO_SESSION_INPUT_OVERRIDE`, `input` may be NULL to
 * fork directly from the resolved checkpoint/thread head without replacing
 * state. `out_summary_json` may be NULL when the caller only needs the result
 * state.
 */
CXX_C_API int turbo_agent_session_fork_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_session_exec_options_t *exec_options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/* ── Preset execution (session creates and destroys the graph internally) ──── */

/**
 * @brief Start one canned workflow graph and destroy the temporary graph after the run.
 */
CXX_C_API int turbo_agent_session_start_preset(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *state, const turbo_graph_run_options_t *options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Resume one canned workflow graph from a checkpoint.
 *
 * `exec_options` controls scope, input semantics and checkpoint id (same rules
 * as `turbo_agent_session_resume_graph`).
 */
CXX_C_API int turbo_agent_session_resume_preset(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_session_exec_options_t *exec_options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Fork one canned workflow graph from a checkpoint.
 *
 * `exec_options` controls scope, input semantics and checkpoint id (same rules
 * as `turbo_agent_session_fork_graph`).
 */
CXX_C_API int turbo_agent_session_fork_preset(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_session_exec_options_t *exec_options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/* ── High-level session-default workflow entry points ──────────────────────── */

/**
 * @brief Start the session's default canned workflow from canonical prompt messages.
 *
 * When the session remembers a default memory namespace, the workflow starts
 * with long-term memory context loaded from that namespace.
 */
CXX_C_API int turbo_agent_session_start_messages(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *messages,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Start the session's default workflow from canonical messages and emit one stream mode.
 */
CXX_C_API int turbo_agent_session_start_messages_stream(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *messages,
    const turbo_graph_run_options_t *options, turbo_event_stream_mode_t stream_mode,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Start the session's default canned workflow from one user message.
 *
 * When the session remembers a default memory namespace, the workflow starts
 * with long-term memory context loaded from that namespace.
 */
CXX_C_API int turbo_agent_session_start_text(
    turbo_agent_session_t *session, const char *user_text,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Start the session's default workflow from one user message and emit one stream mode.
 */
CXX_C_API int turbo_agent_session_start_text_stream(
    turbo_agent_session_t *session, const char *user_text,
    const turbo_graph_run_options_t *options, turbo_event_stream_mode_t stream_mode,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Extract one user-facing final answer text from a bind-native result state.
 *
 * Returned text is allocated and owned by caller.
 */
CXX_C_API char *turbo_agent_session_result_text(
    const turbo_runtime_data_bind_value_t *state);

/**
 * @brief Invoke the session's default canned workflow from one user message.
 *
 * Returned text is allocated and owned by caller. `out_summary_json` may be
 * NULL when the caller does not need the runtime summary.
 */
CXX_C_API int turbo_agent_session_invoke_text(
    turbo_agent_session_t *session, const char *user_text,
    const turbo_graph_run_options_t *options, char **out_text,
    json_value_t **out_summary_json);

/**
 * @brief Invoke the session's default canned workflow from canonical prompt messages.
 *
 * Returned text is allocated and owned by caller. `out_summary_json` may be
 * NULL when the caller does not need the runtime summary.
 */
CXX_C_API int turbo_agent_session_invoke_messages_text(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *messages,
    const turbo_graph_run_options_t *options, char **out_text,
    json_value_t **out_summary_json);

/**
 * @brief Invoke the session's default canned workflow from one user message and
 * parse the final answer as JSON.
 *
 * Returned JSON is owned by caller. `out_summary_json` may be NULL when the
 * caller does not need the runtime summary.
 */
CXX_C_API int turbo_agent_session_invoke_json(
    turbo_agent_session_t *session, const char *user_text,
    const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json);

/**
 * @brief Invoke the session's default canned workflow from canonical prompt messages
 * and parse the final answer as JSON.
 *
 * Returned JSON is owned by caller. `out_summary_json` may be NULL when the
 * caller does not need the runtime summary.
 */
CXX_C_API int turbo_agent_session_invoke_messages_json(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *messages,
    const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json);

/**
 * @brief Invoke the session's default workflow for a batch of user messages.
 *
 * Returns a JSON array. Each item contains `index`, `ok`, `summary`, and either
 * `output_text` or `error`. Items execute sequentially through the same session.
 */
CXX_C_API int turbo_agent_session_batch_text(
    turbo_agent_session_t *session, const char *const *user_texts, size_t count,
    const turbo_graph_run_options_t *options, json_value_t **out_results_json);

/* ── Long-term memory store ────────────────────────────────────────────────── */

/** @brief Load one long-term memory record. */
CXX_C_API int turbo_agent_session_memory_get(const turbo_agent_session_t *session,
                                             const char *memory_namespace, const char *key,
                                             char **out_value_json);

/** @brief Persist one long-term memory record. */
CXX_C_API int turbo_agent_session_memory_put(const turbo_agent_session_t *session,
                                             const char *memory_namespace, const char *key,
                                             const char *value_json);

/**
 * @brief Persist one formal memory-context record.
 *
 * The stored JSON payload uses the stable shape: `scope`, `path`, `text`.
 */
CXX_C_API int turbo_agent_session_memory_put_context(
    const turbo_agent_session_t *session, const char *memory_namespace, const char *key,
    const char *scope, const char *path, const char *text);

/** @brief Delete one long-term memory record. */
CXX_C_API int turbo_agent_session_memory_delete(const turbo_agent_session_t *session,
                                                const char *memory_namespace, const char *key);

/** @brief List long-term memory records by namespace prefix. */
CXX_C_API int turbo_agent_session_memory_list(const turbo_agent_session_t *session,
                                              const char *namespace_prefix,
                                              json_value_t **out_records_json);

CXX_C_API int turbo_agent_session_memory_list_records(
    const turbo_agent_session_t *session, const char *namespace_prefix,
    json_value_t **out_records_json);

CXX_C_API int turbo_agent_session_memory_get_record(const turbo_agent_session_t *session,
                                                    const char *memory_namespace, const char *key,
                                                    json_value_t **out_record_json);

CXX_C_API int turbo_agent_session_memory_put_record(const turbo_agent_session_t *session,
                                                    const json_value_t *record_json);

CXX_C_API int turbo_agent_session_memory_validate_record(const json_value_t *record_json);

CXX_C_API int turbo_agent_session_memory_query_records(
    const turbo_agent_session_t *session, const char *namespace_prefix, const char *kind,
    const char *key_prefix, const char *text_substring,
    json_value_t **out_records_json);

CXX_C_API int turbo_agent_session_memory_query_records_ex(
    const turbo_agent_session_t *session, const turbo_agent_memory_query_options_t *options,
    json_value_t **out_records_json);

/**
 * @brief Load formal memory-context records from the session-owned store into state.
 *
 * Every selected record must serialize a JSON object with `scope` and `text`
 * strings plus an optional `path` string.
 */
CXX_C_API int turbo_agent_session_load_memory_context(
    const turbo_agent_session_t *session, json_value_t *state, const char *namespace_prefix);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_AGENT_SESSION_H */
