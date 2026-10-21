#ifndef TURBO_AGENT_SESSION_H
#define TURBO_AGENT_SESSION_H

#include <stddef.h>

#include <turbo_agent_api.h>

#include "turbo_agent.h"
#include "turbo_agent_context.h"
#include "turbo_agent_inbox.h"
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
 * `turbo_agent_config_apply_env(...)`. When `runtime_store` lacks any of
 * put/get/list, a heap-backed runtime store is created; the incomplete supplied
 * store is ignored and remains caller-owned.
 *
 * A complete supplied RuntimeStore is consumed even if construction fails:
 * its user_data_free callback, if present, runs during failure cleanup or at
 * Session destruction. MemoryStore transfers only on successful construction;
 * on failure the caller still owns it. Do not reuse consumed store copies.
 * KnowledgeStore and Retriever are borrowed and must outlive their use by Session.
 * Agent configuration resources follow turbo_agent_create's ownership rules.
 *
 * Session-owned strings are copied before success. NULL/empty optional thread,
 * memory namespace, parent-link and retrieval settings become owned empty strings.
 * Copy failure destroys the partial Session and returns NULL without publishing
 * a partially configured handle. Construction and destruction require exclusive
 * ownership; callback code must remain loaded until its owned resources release.
 *
 * @param config Borrowed configuration, read during construction.
 * @return Session handle or NULL on invalid input or construction failure.
 */
CXX_C_API turbo_agent_session_t *
turbo_agent_session_create(const turbo_agent_session_config_t *config);

/**
 * Create a Session using a versioned Runtime Store descriptor without changing
 * the legacy, unversioned Session config layout. The config's legacy
 * runtime_store must be entirely zeroed; otherwise creation fails and the caller
 * retains both descriptors. A complete extended backend transfers ownership
 * once construction begins, including on later construction failure.
 */
CXX_C_API turbo_agent_session_t *
turbo_agent_session_create_with_runtime_store_ex(
    const turbo_agent_session_config_t *config,
    const turbo_agent_runtime_store_ex_t *runtime_store);

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
 * The object remains valid after Session destruction. Returns 0 on success,
 * -1 for invalid arguments or construction failure. No execution is started.
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
 * The arrays own their content independently of the Session and registry.
 */
CXX_C_API int turbo_agent_session_get_tool_schemas(
    const turbo_agent_session_t *session, json_value_t **out_tool_schemas_json);

/**
 * @brief Return one startup health snapshot for host-side harness validation.
 *
 * The returned JSON object is owned by the caller. It contains `ok`, `errors`,
 * `capabilities`, `workflow`, `structured_output`, `tool_schema_status`, and
 * `tool_schemas`. A selected provider's invalid tool schema yields `ok=false`
 * and `invalid_tool_schemas`; failure of an unselected format alone does not
 * make startup unready. The all-format `tool_schemas` field is null if any
 * format fails. Returns 0 for a completed report even when `ok` is false;
 * returns -1 for invalid arguments or failure to construct the report.
 * Reports remain valid after Session destruction. These read-only queries
 * require the Session and registry to stay alive and exclude concurrent
 * mutation/destruction for the duration of the call.
 */
CXX_C_API int turbo_agent_session_get_startup_diagnostics(
    const turbo_agent_session_t *session, json_value_t **out_diagnostics_json);

/* ── Observability sinks ───────────────────────────────────────────────────── */

/**
 * @brief Register a live trace sink on the Session's owned Agent.
 * Returns 0 after copying the sink descriptor, or -1 without consuming user_data.
 * On success, Agent calls user_data_free (if provided) once at destruction for
 * this registration. The caller must keep user_data valid until that release;
 * without a release callback, its lifetime remains the caller's responsibility.
 * Events are borrowed only during synchronous callbacks on the execution thread.
 * Registration, history configuration and destruction require quiescent execution;
 * callbacks must not register sinks or destroy the Session/Agent.
 */
CXX_C_API int turbo_agent_session_add_trace_json_value_sink(
    turbo_agent_session_t *session, const turbo_agent_trace_json_value_sink_t *sink);

/**
 * @brief Register a live observer using the trace-to-observer projection.
 * Registration ownership and callback lifetimes match add_trace_json_value_sink.
 * Recognized trace names use the existing observer mapping; unmapped names are
 * ignored. Each delivered envelope owns a trace copy for the callback duration.
 * Copy the event to retain it. Unlike observe_history, successful registration
 * retains the sink and its release callback until Agent destruction.
 */
CXX_C_API int turbo_agent_session_add_observer_json_value_sink(
    turbo_agent_session_t *session, const turbo_agent_observer_json_value_sink_t *sink);

/**
 * @brief Toggle trace retention in execution state; live sink delivery is unchanged.
 * Nonzero enables capture, zero disables it. Existing state history is not erased.
 * Returns 0 on success, or -1 when Session has no Agent (including NULL Session).
 */
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
 * Resolves the newest run by `updated_at`. Prefer `get_thread_head_state_json_value`
 * for thread-head semantics.
 */
CXX_C_API int turbo_agent_session_get_thread_state_json_value(
    turbo_agent_session_t *session, json_value_t **out_state);

/**
 * @brief Load the session thread head state snapshot.
 *
 * The runtime resolves the newest interrupted run first, then falls back to
 * the newest run by `updated_at`.
 */
CXX_C_API int turbo_agent_session_get_thread_head_state_json_value(
    turbo_agent_session_t *session, json_value_t **out_state);

/**
 * @brief Load the session thread's latest-run trace events.
 *
 * Resolves by `updated_at`. Prefer `get_thread_head_trace_events_json_value` for
 * thread-head semantics.
 */
CXX_C_API int turbo_agent_session_get_thread_trace_events_json_value(
    turbo_agent_session_t *session, json_value_t **out_events);

/** @brief Load the session thread head trace-event snapshot. */
CXX_C_API int turbo_agent_session_get_thread_head_trace_events_json_value(
    turbo_agent_session_t *session, json_value_t **out_events);

/**
 * @brief Load one run's latest persisted state.
 *
 * When `run_id` is NULL or empty, uses the session's latest cached run id.
 */
CXX_C_API int turbo_agent_session_get_run_state_json_value(
    turbo_agent_session_t *session, const char *run_id,
    json_value_t **out_state);

CXX_C_API int turbo_agent_session_get_run_trace_events_json_value(
    turbo_agent_session_t *session, const char *run_id,
    json_value_t **out_events);

/**
 * @brief Load one checkpoint's serialized state.
 *
 * When `checkpoint_id` is NULL or empty, uses the session's latest cached
 * checkpoint id.
 */
CXX_C_API int turbo_agent_session_get_checkpoint_state_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id,
    json_value_t **out_state);

/* ── State-override preparers (read + merge; no persistence) ──────────────── */

CXX_C_API int turbo_agent_session_prepare_checkpoint_state_override_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const json_value_t *state_patch,
    json_value_t **out_state_override);

CXX_C_API int turbo_agent_session_prepare_thread_state_override_json_value(
    turbo_agent_session_t *session, const json_value_t *state_patch,
    json_value_t **out_state_override);

/**
 * @brief Prepare one checkpoint-scoped state override from a TurboParser JSON-native patch.
 *
 * Loads the checkpoint state, recursively merges object fields from
 * `state_patch`, and returns the resulting full state. Arrays, scalars and null
 * replace the target value. Does not persist the prepared override.
 */
CXX_C_API int turbo_agent_session_apply_checkpoint_state_patch_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const json_value_t *state_patch,
    json_value_t **out_state_override);

/**
 * @brief Prepare one thread-head state override from a TurboParser JSON-native patch.
 *
 * The runtime resolves the thread's newest interrupted run first, falls back
 * to the newest run by `updated_at`, then applies the patch. Does not persist.
 */
CXX_C_API int turbo_agent_session_apply_thread_state_patch_json_value(
    turbo_agent_session_t *session, const json_value_t *state_patch,
    json_value_t **out_state_override);

/* ── Command preparers / appliers (no graph execution) ────────────────────── */

/**
 * @brief Prepare one checkpoint-scoped command-derived state override.
 *
 * Interprets the command against the checkpoint's serialized state and returns
 * the resulting state override. Does not persist.
 */
CXX_C_API int turbo_agent_session_prepare_checkpoint_command_override_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const json_value_t *command,
    json_value_t **out_state_override);

/**
 * @brief Prepare one thread-head command-derived state override.
 *
 * Resolves thread head, applies command, returns override. Does not persist.
 */
CXX_C_API int turbo_agent_session_prepare_thread_command_override_json_value(
    turbo_agent_session_t *session, const json_value_t *command,
    json_value_t **out_state_override);

/**
 * @brief Apply one host-facing runtime command to a checkpoint state.
 *
 * When `checkpoint_id` is NULL or empty, uses the session's latest cached
 * checkpoint id.
 */
CXX_C_API int turbo_agent_session_apply_command_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const json_value_t *command,
    json_value_t **out_state_override);

/**
 * @brief Apply one host-facing runtime command to an explicit checkpoint.
 *
 * Requires a non-empty `checkpoint_id`; does not fall back to the session's
 * cached checkpoint.
 */
CXX_C_API int turbo_agent_session_apply_checkpoint_command_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const json_value_t *command,
    json_value_t **out_state_override);

/** @brief Apply one host-facing runtime command to the session's owned thread. */
CXX_C_API int turbo_agent_session_apply_thread_command_json_value(
    turbo_agent_session_t *session, const json_value_t *command,
    json_value_t **out_state_override);

/* ── Supervisor / inbox / inspect ─────────────────────────────────────────── */

/* These queries return caller-owned JSON (release with json_free), independent
 * of the stored state and valid after Session destruction. Keep the Session
 * alive and serialize queries with mutations for the duration of each call.
 * They return 0 on success and -1 on query/construction failure. */

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
 * `latest_handoff_event`, `control_snapshot`, and `workflow_snapshot`.
 * Missing inbox/history are empty arrays; no handoff event is represented by null.
 */
CXX_C_API int turbo_agent_session_get_supervisor_inspect(
    turbo_agent_session_t *session, json_value_t **out_inspect_json);

/**
 * @brief Load one host-facing orchestration inspect bundle.
 *
 * Returns a JSON object with `supervisor_inspect`, `thread_timeline`,
 * `thread_lineage`, `branch_tree`, and `child_runs`.
 * An unavailable timeline is represented by null; failure to obtain the
 * supervisor, lineage, branch tree, or child-run data fails the query.
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
 * The caller-owned state can be fed into a resume/fork exec_options as an
 * OVERRIDE input. It owns its JSON and remains valid after Session destruction.
 * Does not persist state by itself. Returns -1 on query or construction failure.
 */
CXX_C_API int turbo_agent_session_append_supervisor_inbox_message_json_value(
    turbo_agent_session_t *session, const char *source_agent, const char *text,
    json_value_t **out_state_override);

CXX_C_API int turbo_agent_session_get_checkpoint_trace_events_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id,
    json_value_t **out_events);

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
CXX_C_API int turbo_agent_session_get_child_thread_timeline_json_value(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_timeline);

/**
 * @brief Load one child branch tree referenced by a parent output item.
 */
CXX_C_API int turbo_agent_session_get_child_branch_tree(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_branch_tree_json);

/**
 * @brief Load one child execution inspect bundle from a parent output item.
 *
 * Borrows session/output_item for the call. Returns 0 with an independently
 * owned report (release with json_free), or -1 on failure. Once arguments are
 * accepted, failure leaves the output NULL. Run, checkpoint list, history and
 * trace queries are required. A nonempty child_checkpoint_id also requires its
 * checkpoint and context; absent checkpoint fields are JSON null. Timeline and
 * branch-tree query failures remain optional and produce JSON null fields.
 */
CXX_C_API int turbo_agent_session_get_child_inspect(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_inspect_json);

/**
 * @brief Load one child orchestration inspect bundle from a parent output item.
 *
 * Copies parent lineage fields and owns the child report. Missing/empty lineage
 * is JSON null. Ownership and failure behavior match get_child_inspect.
 */
CXX_C_API int turbo_agent_session_get_child_orchestration_inspect(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_inspect_json);

/**
 * @brief Load one multi-agent inspect bundle for a child tool-result output item.
 *
 * Requires supervisor, orchestration and child-orchestration queries to succeed.
 * Ownership and failure behavior match get_child_inspect; no partial report is
 * published if any required query fails.
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
 * On success, out_events owns an independent JSON array; release with json_free.
 *
 * Selects the first nonempty source in this order: explicit run_id, explicit
 * checkpoint_id, cached last run id, cached last checkpoint id. Supplying both
 * explicit ids selects the run. Failure does not retry another source or change
 * cached ids. No source returns -1. Runtime query errors are propagated.
 */
CXX_C_API int turbo_agent_session_load_history_events_json_value(
    turbo_agent_session_t *session, const char *run_id, const char *checkpoint_id,
    json_value_t **out_events);

/**
 * @brief Load current thread history.
 *
 * Prefers the newest interrupted run on the thread, then falls back to the
 * newest run by `updated_at`.
 */
CXX_C_API int turbo_agent_session_load_thread_history_events_json_value(
    turbo_agent_session_t *session, json_value_t **out_events);

/**
 * @brief Synchronously replay borrowed events using load_history's source order.
 * The callback and user data are borrowed for the call; event pointers are valid
 * only during the callback. Copy any event that must outlive it. Returns 0 on
 * successful replay (including empty history), or -1 on invalid input/read failure.
 */
CXX_C_API int turbo_agent_session_replay_history_json_value(
    turbo_agent_session_t *session, const char *run_id, const char *checkpoint_id,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data);

/** @brief Synchronously replay current thread history with the same callback borrowing contract. */
CXX_C_API int turbo_agent_session_replay_thread_history_json_value(
    turbo_agent_session_t *session, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data);

/**
 * @brief Observe persisted history through the runtime-backed observer bridge.
 *
 * The sink and user data are borrowed for this synchronous call; user_data_free
 * is not invoked. Callback event pointers are valid only during the callback.
 * The Runtime may emit a terminal record after history events. A later failure
 * does not undo callbacks already delivered.
 *
 * Selects the first nonempty source in this order: explicit run_id, explicit
 * checkpoint_id, cached last run id, cached last checkpoint id. Supplying both
 * explicit ids selects the run. Failure does not retry another source or change
 * cached ids. No source returns -1. Runtime query errors are propagated.
 */
CXX_C_API int turbo_agent_session_observe_history_json_value(
    turbo_agent_session_t *session, const char *run_id, const char *checkpoint_id,
    const turbo_agent_observer_json_value_sink_t *sink);

/**
 * @brief Observe the current session thread lineage through the observer bridge.
 * The sink/event borrowing and partial-delivery contract matches observe_history.
 */
CXX_C_API int turbo_agent_session_observe_thread_history_json_value(
    turbo_agent_session_t *session, const turbo_agent_observer_json_value_sink_t *sink);

CXX_C_API int turbo_agent_session_get_thread_timeline_json_value(
    turbo_agent_session_t *session, json_value_t **out_timeline);

/* ── Child history / trace ─────────────────────────────────────────────────── */

/**
 * @brief Load one child run history referenced by a parent output item.
 *
 * Uses nonempty `child_checkpoint_id` when present, otherwise `child_run_id`.
 * A failed explicit checkpoint query is returned without retrying by run id.
 * On success, the caller owns the event array and releases it with json_free().
 */
CXX_C_API int turbo_agent_session_load_child_history_events_json_value(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_events);

/**
 * @brief Load one child run's persisted trace events from a parent output item.
 *
 * Uses nonempty `child_checkpoint_id` when present, otherwise `child_run_id`.
 * A failed explicit checkpoint query is returned without retrying by run id.
 * On success, the caller owns the event array and releases it with json_free().
 */
CXX_C_API int turbo_agent_session_get_child_trace_events_json_value(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_events);

/* ── Graph factories ───────────────────────────────────────────────────────── */

/**
 * Graph factory lifetime contract (all factories below): the caller owns a
 * successful graph and releases it with turbo_graph_destroy(). Nodes borrow the
 * Session's Agent and persistent context configuration. Keep Session and any
 * configured knowledge store/retriever alive until graph use has ended, and
 * destroy graphs before Session. Caller configuration strings are copied when
 * creating Session; external backends remain caller-owned. Factories require
 * quiescent Session access. Allocation or node/edge installation failure returns
 * NULL and releases the incomplete graph. Example: create_loop_graph(session),
 * start_graph(session, graph, ...), turbo_graph_destroy(graph), destroy(session).
 */

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
 * Returns NULL without an owned Agent, with missing required context backend,
 * or when `kind` is unknown (as well as construction failure).
 */
CXX_C_API turbo_graph_t *turbo_agent_session_create_preset_graph(
    const turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind);

/* ── Input state factories ─────────────────────────────────────────────────── */

/**
 * @brief Create one TurboParser JSON-native agent state seeded with an optional user message.
 *
 * `user_text` is borrowed for this call; NULL or empty text seeds no message.
 * The caller owns the returned state and releases it with json_free().
 * Returns NULL on allocation failure.
 */
CXX_C_API json_value_t *
turbo_agent_session_create_input_state_json_value(const char *user_text);

/**
 * @brief Create one TurboParser JSON-native agent state seeded with canonical prompt messages.
 *
 * `messages` must be a TurboParser JSON-native array of canonical prompt message objects.
 * Input remains caller-owned and is copied; subsequent input mutation or release
 * cannot affect the returned state. The caller releases that state with json_free().
 * Returns NULL on validation or allocation failure, without changing `messages`.
 */
CXX_C_API json_value_t *
turbo_agent_session_create_input_messages_state_json_value(
    const json_value_t *messages);

/**
 * Build a fresh loop state from a completed state plus one new user message.
 * Previous input, model replies, tool calls and results retain chronological
 * order. Run-control/final-answer fields are reset. The previous state and
 * user_text are borrowed for this call; the returned JSON is caller-owned.
 * Session must be quiescent. Returns NULL for invalid input or allocation
 * failure, including malformed event containers or known model-event fields;
 * history is never partially returned. This projects full history; it does not
 * mutate a context-compaction journal or resume an interrupted workflow.
 * Bound retained history at the host.
 * Example: next = create_follow_up_state_json_value(session, completed, text).
 */
CXX_C_API json_value_t *turbo_agent_session_create_follow_up_state_json_value(
    const turbo_agent_session_t *session, const json_value_t *completed_state,
    const char *user_text);

/**
 * @brief Create one TurboParser JSON-native agent state from user text and session memory context.
 */
CXX_C_API json_value_t *
turbo_agent_session_create_input_state_with_memory_json_value(
    const turbo_agent_session_t *session, const char *user_text,
    const char *namespace_prefix);

/**
 * @brief Create one TurboParser JSON-native agent state from canonical messages and session memory context.
 */
CXX_C_API json_value_t *
turbo_agent_session_create_input_messages_state_with_memory_json_value(
    const turbo_agent_session_t *session, const json_value_t *messages,
    const char *namespace_prefix);

/* ── Core graph execution ──────────────────────────────────────────────────── */

/**
 * @brief Start one new graph run and update cached session ids.
 *
 * Pass `event_sink` / `event_sink_user_data` to receive live canonical events;
 * pass NULL for both to run without streaming. `out_summary_json` may be NULL
 * when the caller only needs the result state.
 *
 * These synchronous graph operations share Session completion with submit:
 * update cached ids, commit bound inbox claims, then run bounded follow-ups.
 * Serialize operations on one Session and keep graph/callback data alive until
 * return. Any non-NULL returned summary/state is caller-owned, including when
 * completion fails after Runtime has produced outputs; release with json_free().
 * Completion errors propagate, so inspect the return code as well as the
 * Runtime status in a returned summary.
 * Summary ids are copied as one exclusive-owner update. Invalid required fields
 * or copy failure returns -1 with cached ids unchanged, before inbox commit or
 * follow-ups. Runtime may already have completed; this does not roll back its
 * persisted records or returned outputs. A missing/non-string/empty checkpoint
 * id retains the previous cached checkpoint.
 */
CXX_C_API int turbo_agent_session_start_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const json_value_t *state, const turbo_graph_run_options_t *options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state);

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
    const json_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_session_exec_options_t *exec_options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state);

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
    const json_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_session_exec_options_t *exec_options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state);

/* ── Preset execution (session creates and destroys the graph internally) ──── */

/**
 * @brief Start one canned workflow graph and destroy the temporary graph after the run.
 */
CXX_C_API int turbo_agent_session_start_preset(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const json_value_t *state, const turbo_graph_run_options_t *options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state);

/**
 * @brief Resume one canned workflow graph from a checkpoint.
 *
 * `exec_options` controls scope, input semantics and checkpoint id (same rules
 * as `turbo_agent_session_resume_graph`).
 */
CXX_C_API int turbo_agent_session_resume_preset(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const json_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_session_exec_options_t *exec_options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state);

/**
 * @brief Fork one canned workflow graph from a checkpoint.
 *
 * `exec_options` controls scope, input semantics and checkpoint id (same rules
 * as `turbo_agent_session_fork_graph`).
 */
CXX_C_API int turbo_agent_session_fork_preset(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const json_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_session_exec_options_t *exec_options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state);

/* ── High-level session-default workflow entry points ──────────────────────── */

/**
 * @brief Start the session's default canned workflow from canonical prompt messages.
 *
 * When the session remembers a default memory namespace, the workflow starts
 * with long-term memory context loaded from that namespace.
 */
CXX_C_API int turbo_agent_session_start_messages(
    turbo_agent_session_t *session, const json_value_t *messages,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state);

/**
 * @brief Start the session's default workflow from canonical messages and emit one stream mode.
 */
CXX_C_API int turbo_agent_session_start_messages_stream(
    turbo_agent_session_t *session, const json_value_t *messages,
    const turbo_graph_run_options_t *options, turbo_event_stream_mode_t stream_mode,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state);

/**
 * @brief Start the session's default canned workflow from one user message.
 *
 * When the session remembers a default memory namespace, the workflow starts
 * with long-term memory context loaded from that namespace.
 */
CXX_C_API int turbo_agent_session_start_text(
    turbo_agent_session_t *session, const char *user_text,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state);

/**
 * @brief Start the session's default workflow from one user message and emit one stream mode.
 */
CXX_C_API int turbo_agent_session_start_text_stream(
    turbo_agent_session_t *session, const char *user_text,
    const turbo_graph_run_options_t *options, turbo_event_stream_mode_t stream_mode,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state);

/**
 * @brief Extract one user-facing final answer text from a TurboParser JSON-native result state.
 *
 * The state is borrowed for this call. Returned text is an independent copy
 * owned by the caller (release with free). Returns NULL if no nonempty final
 * answer is available or allocation fails; does not modify state.
 */
CXX_C_API char *turbo_agent_session_result_text(
    const json_value_t *state);

/**
 * @brief Invoke the session's default canned workflow from one user message.
 *
 * Returned text is allocated and owned by caller. `out_summary_json` may be
 * NULL when the caller does not need the runtime summary.
 * Invoke variants discard returned state and summary on execution or result
 * conversion failure. They do not roll back persisted runs or cached Session
 * ids. Returned text uses free(); returned JSON/summary uses json_free().
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
    turbo_agent_session_t *session, const json_value_t *messages,
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
    turbo_agent_session_t *session, const json_value_t *messages,
    const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json);

/**
 * @brief Invoke the session's default workflow for a batch of user messages.
 *
 * Returns a caller-owned JSON array (release with json_free). Each item contains
 * `index`, `ok`, `summary`, `output_text`, and `error`; unused fields are null.
 * Items execute sequentially through the same Session and continue after an
 * item fails. Returns 0 for a completed batch even if individual items failed;
 * invalid batch arguments or failure to construct the result array return -1.
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
 * scope/text must be nonempty; a NULL path is stored as an empty string.
 * Arguments are borrowed for the synchronous call. Returns -1 for invalid
 * input or payload construction failure; otherwise propagates the store result.
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
 * Queries canonical `context` records using namespace_prefix, then copies text
 * and metadata.scope/path into state in query order. Existing layers are kept;
 * a null path becomes an empty string. The caller owns state and must exclude
 * concurrent mutation. Loaded layers do not borrow the store or Session.
 * Returns 0 on success, -1 on query, validation, or append failure. Appends are
 * incremental: layers added before a later failure remain in state. Input
 * factories with memory discard their newly created state on any load failure.
 */
CXX_C_API int turbo_agent_session_load_memory_context(
    const turbo_agent_session_t *session, json_value_t *state, const char *namespace_prefix);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_AGENT_SESSION_H */
