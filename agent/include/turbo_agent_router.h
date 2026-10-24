#ifndef TURBO_AGENT_ROUTER_H
#define TURBO_AGENT_ROUTER_H

#include "turbo_agent.h"
#include "turbo_agent_execution.h"
#include "turbo_agent_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_ROUTER_ABI_VERSION 1u
#define TURBO_AGENT_ROUTER_MAX_ENDPOINTS 16u
#define TURBO_AGENT_ROUTER_MAX_MODEL_TURNS 16u
#define TURBO_AGENT_ROUTER_MAX_REVIEW_RESUMES 32u
#define TURBO_AGENT_ROUTER_GROUP_MAX_TEXT_BYTES 65536u
#define TURBO_AGENT_ROUTER_GROUP_MAX_INPUT_BYTES 1048576u
#define TURBO_AGENT_ROUTER_OBSERVATION_COLLECTION "router_observations"
#define TURBO_AGENT_ROUTER_OBSERVATION_MAX_BYTES 16384u

typedef struct turbo_agent_router_s turbo_agent_router_t;
typedef struct turbo_agent_router_task_s turbo_agent_router_task_t;
typedef struct turbo_agent_router_group_s turbo_agent_router_group_t;

typedef struct turbo_agent_router_group_result_s {
  json_value_t *summary;
  json_value_t *state;
} turbo_agent_router_group_result_t;

typedef enum turbo_agent_router_role_e {
  TURBO_AGENT_ROUTER_PLAN = 1,
  TURBO_AGENT_ROUTER_EXECUTE = 2
} turbo_agent_router_role_t;

/** Host-declared model features, separate from Tool Authority grants. */
typedef enum turbo_agent_router_model_capability_e {
  TURBO_AGENT_ROUTER_TEXT = 1,
  TURBO_AGENT_ROUTER_JSON = 2,
  TURBO_AGENT_ROUTER_TOOL_PROPOSALS = 4
} turbo_agent_router_model_capability_t;

typedef struct turbo_agent_router_endpoint_s {
  const char *id;
  turbo_agent_t *agent;
  turbo_agent_runtime_t *runtime;
  unsigned roles;
  unsigned capabilities;
  unsigned max_difficulty;
  unsigned max_input_tokens;
  unsigned max_output_tokens;
  /** Host price snapshot, in micro-USD per million tokens; each <= 1e9. */
  uint64_t input_price_micros;
  uint64_t output_price_micros;
} turbo_agent_router_endpoint_t;

typedef struct turbo_agent_router_config_s {
  size_t struct_size;
  unsigned abi_version;
  turbo_agent_t *authority_agent;
  const turbo_agent_router_endpoint_t *endpoints;
  size_t endpoint_count;
} turbo_agent_router_config_t;

typedef struct turbo_agent_router_request_s {
  size_t struct_size;
  unsigned abi_version;
  turbo_agent_router_role_t role;
  unsigned difficulty;
  unsigned required_capabilities;
  unsigned input_tokens;
  unsigned output_tokens;
  uint64_t budget_micros;
  unsigned timeout_ms;
  unsigned heartbeat_timeout_ms;
  const char *thread_id;
  const turbo_agent_runtime_parent_link_t *parent_link;
} turbo_agent_router_request_t;

CXX_C_API void turbo_agent_router_config_init(turbo_agent_router_config_t *config);
CXX_C_API void turbo_agent_router_request_init(turbo_agent_router_request_t *request);

/**
 * Bounded, Host-owned model routing. Endpoint ids/configuration are copied;
 * agents, runtimes and their dependencies are borrowed through task release.
 * Endpoints must have distinct agents/runtimes, neither shared with the authority
 * agent. The Host serializes Router/task APIs and all other use of these agents,
 * runtimes and stores. Policy replacement remains quiescent, including queued
 * work, callbacks and result reads; generation is not a policy lock.
 * Returns SALTS_OK/EINVAL/ENOMEM. *out_router is NULL on failure.
 */
CXX_C_API int turbo_agent_router_create(const turbo_agent_router_config_t *config,
                                        turbo_agent_router_t **out_router);
/** Returns EBUSY while any task is retained; NULL is a successful no-op. */
CXX_C_API int turbo_agent_router_destroy(turbo_agent_router_t *router);

/**
 * Opt in to 1..16 retained tool-loop tasks (default 1). The default preserves
 * exclusive whole-task ownership. With limit > 1, distinct endpoint owners may
 * run models and wait for separate approvals concurrently; model-only tasks may
 * coexist. One shared Host ToolNode, including all hooks, Journal access and
 * callback/result processing, is synchronized per dispatch. Callbacks on this
 * authority Agent remain serial. This does not make external/shared resources
 * or other Routers/Agent APIs thread-safe; keep them independently owned.
 *
 * Waiting for the Host lane consumes the original deadline/cancellation budget;
 * a stopped waiter enters no Host hook or Journal. Each admitted task retains
 * its endpoint and counts toward the limit until task_release(), including a
 * paused/terminal task. Approvals remain bound to each canonical checkpoint.
 * No scheduler, FIFO/fairness guarantee, retry or cross-store transaction is
 * added. The Host serializes all control APIs and keeps policy/registry/hook
 * replacement quiescent across all running/queued segments and callbacks.
 * Returns EINVAL for invalid input; EBUSY while any task is retained.
 */
CXX_C_API int turbo_agent_router_set_tool_task_limit(turbo_agent_router_t *router,
                                                    unsigned limit);

/**
 * Select the lowest estimated cost among role/feature/difficulty/token-qualified idle
 * endpoints, breaking ties by configuration order. Requires the authority
 * agent's delegate grant and a child capability mask contained in that grant.
 * This mask check is not full tool admission (paths/approvals/effects).
 *
 * Submit a single model node through existing Execution/Runtime. No tool node,
 * router retry, failover, inbox follow-up or model-chosen endpoint is installed.
 * Tool proposals in the result require separate Host-authorized dispatch.
 * Input/options are copied before return. A finite timeout (1..86400000ms),
 * heartbeat interval <= timeout, and Host thread/parent-run identity are required.
 * Token counts/prices are admission estimates, not a billing or token hard cap.
 * Configure backend output bounds, cooperative cancellation and retries explicitly.
 * Returns EPERM for missing delegation, ENOENT for no qualified affordable
 * endpoint, EBUSY if qualified endpoints are occupied, or Executor admission
 * errors unchanged. *out_task is NULL on rejection; no alternative is retried.
 */
CXX_C_API int turbo_agent_router_submit(turbo_agent_router_t *router,
    cmeta_threadpool_t *executor, const turbo_agent_router_request_t *request,
    const json_value_t *input, turbo_agent_router_task_t **out_task);

/**
 * Opt-in supervised model/tool loop, with 2..16 model turns and one tool per
 * turn. Requires EXECUTE, TOOL_PROPOSALS, the exact same immutable ToolRegistry
 * on child and authority, and a Host-configured before_tool guardrail. The Host
 * is responsible for that guardrail's approval/path policy. Tools execute on
 * the authority Agent through its normal ToolNode, executor and Journal;
 * child capability checks also apply. Require known effects and strict result
 * contracts before dispatch; validate results after Host output hooks, before
 * another model call. No implicit execution/recovery retry is installed.
 *
 * Admission estimate = per-turn estimate * max_model_turns, including history
 * as estimated by the Host. This is not a hard billing cap. The final turn is
 * reserved for consuming results; a further proposal interrupts before effects.
 * By default a retained loop excludes every other Router task until release,
 * including while waiting for approval. set_tool_task_limit() can opt in to
 * concurrent task progress with synchronized Host dispatch and separate review.
 * Caller input review flags are cleared.
 * Other bounds, ownership, errors and quiescent requirements match submit().
 * Failures/limits interrupt with router_tool_loop.reason and failure state;
 * inspect these even when the Runtime operation itself succeeded.
 */
CXX_C_API int turbo_agent_router_submit_tool_loop(turbo_agent_router_t *router,
    cmeta_threadpool_t *executor, const turbo_agent_router_request_t *request,
    const json_value_t *input, unsigned max_model_turns,
    turbo_agent_router_task_t **out_task);

/**
 * Review the current terminal approval checkpoint using existing Runtime
 * approve_review/reject_review commands. approved is 0 or 1. The command loads
 * the canonical checkpoint; caller-owned result JSON cannot modify approval.
 * Approval is bound to the exact tool/call/arguments, then cleared on success.
 * A rejection records an interruption without dispatch or automatic replanning.
 * Only approval_pending is resumable, at most 32 accepted review segments, using
 * the original deadline. EBUSY while running; EINVAL for other interruptions;
 * EPERM for stale authority; ECANCELED after loss/cancellation/rejection;
 * ETIMEDOUT after deadline; ERANGE at the review bound. Executor rejection is
 * returned unchanged and preserves the previous segment's result/checkpoint.
 * On acceptance, the previous unclaimed result is released, and take_result()
 * becomes available once for the new segment. The first Execution UUID remains
 * the task id; inspect.execution_id identifies the current segment. Same owners
 * must remain alive; this API does not claim restart or remote reconciliation.
 */
CXX_C_API int turbo_agent_router_task_review(turbo_agent_router_task_t *task,
    cmeta_threadpool_t *executor, int approved);

/** Borrowed stable first Execution UUID; distinct from canonical Runtime run ids. */
CXX_C_API const char *turbo_agent_router_task_id(const turbo_agent_router_task_t *task);
CXX_C_API int turbo_agent_router_task_wait(turbo_agent_router_task_t *task, uint64_t timeout_ms);
CXX_C_API int turbo_agent_router_task_cancel(turbo_agent_router_task_t *task,
                                             turbo_cancel_reason_t reason);
/**
 * Host liveness observation, never a model assertion. Poll and heartbeat use a
 * caller-supplied monotonic timestamp (same clock as cmeta_monotonic_ms()).
 * Expiry is sticky, requests PARENT cancellation and rejects late results; it
 * does not mark an executing worker terminal, release its slot or prove that
 * external effects stopped. Poll at least once per heartbeat interval. No
 * background timer, remote heartbeat authentication or automatic retry exists.
 */
CXX_C_API int turbo_agent_router_task_poll(turbo_agent_router_task_t *task, uint64_t now_ms);
CXX_C_API int turbo_agent_router_task_heartbeat(turbo_agent_router_task_t *task, uint64_t now_ms);
/** Owned diagnostic view, not another durable run/result store. */
CXX_C_API int turbo_agent_router_task_inspect(const turbo_agent_router_task_t *task,
                                              json_value_t **out_view);
/**
 * Persist one immutable, versioned Host supervision observation through an
 * existing Runtime Store's atomic create-if-absent operation. id uses the
 * Router 1..64 ASCII letter/digit/_/- format; each observation needs a distinct
 * Host-selected id. The store must be independent of every Router endpoint
 * Runtime. Host serializes this operation and all other use of that store.
 *
 * Records selected route/task/segment/thread/parent identity, actual Execution
 * status/result code, terminal Runtime anchors when available, liveness and
 * captured-authority validity. No prompt, answer, tool arguments/results, raw
 * Runtime state, policy grant or resumable handle is persisted. Before terminal,
 * result code/anchors are null. Observe terminal before take_result(): EALREADY
 * after a segment's result was moved. Group-owned tasks permit this read-only
 * observation through a retained alias, only before group release.
 *
 * Returns OK, EINVAL, ENOMEM, EPROTO (invalid canonical metadata), ENOBUFS
 * (identity/model/provider bounds or >16 KiB serialized record), EALREADY for an
 * existing id, ENOTSUP without atomic create, or the backend error unchanged.
 * No overwrite, read/list fallback, retry, cancellation or delivery occurs.
 * A backend error may follow a committed write: explicitly load the same id to
 * reconcile and compare expected task/segment/lineage; an older colliding record
 * is not acknowledgement of this observation. Do not assume absence. Failure
 * leaves task/result ownership intact.
 *
 * This is an explicit observation after admission, not durable admission or a
 * complete audit. A crash before this call can leave unrecorded work. Durability,
 * capacity and synchronization follow the selected backend (SQLite supports
 * committed records; memory is volatile). No cross-record/tool transaction.
 * A stored observation is historical, never current liveness, authority,
 * completed delivery, effect certainty or permission to resume/retry. Monotonic
 * clocks belong to the origin process; timestamps do not order different records.
 */
CXX_C_API int turbo_agent_router_task_record_observation(
    const turbo_agent_router_task_t *task, turbo_agent_runtime_t *host_runtime,
    const char *id);
/**
 * Load exactly one observation by id from a trusted Host Runtime Store, validate
 * schema/identity/size and return an owned JSON tree. No Router/Agent is needed
 * after restart. This starts no work and changes no execution/delivery state.
 * out_record is cleared on entry; release success with json_free(). Returns
 * OK, EINVAL, EPROTO, ENOBUFS, ENOMEM or backend status unchanged (including
 * ENOENT). A missing observation does not prove that work/effects never started.
 * Host must reconcile with canonical Runtime/Journal evidence and current
 * authority before any explicit recovery; #74's unknown-effect gate still applies.
 * Example: record_observation(task, host_runtime, host_id); check rc; after
 * reopening that store, load_observation(host_runtime, host_id, &record).
 */
CXX_C_API int turbo_agent_router_load_observation(turbo_agent_runtime_t *host_runtime,
    const char *id, json_value_t **out_record);
/**
 * Move Runtime summary/state once per segment after terminal. API success means ownership
 * transfer, not execution success. Inspect Execution status/runtime_result_code
 * as well: a Runtime failure can have a NULL summary and an owned failure state.
 * Successful execution still does not validate a model's answer.
 * EBUSY before terminal; EALREADY after take; EPERM for stale authority;
 * ECANCELED for an unresponsive child, with NULL outputs on every rejection.
 * Inspect committed Runtime evidence by thread/run identity; an early Runtime
 * failure can precede any run/checkpoint commit. Policy checks require quiescence. No rejected output is
 * delivered and no new attempt starts. Release successful JSON with json_free.
 */
CXX_C_API int turbo_agent_router_task_take_result(turbo_agent_router_task_t *task,
    json_value_t **out_summary, json_value_t **out_state);
/** Returns EBUSY until terminal; never detaches live work. NULL is a no-op. */
CXX_C_API int turbo_agent_router_task_release(turbo_agent_router_task_t *task);

/**
 * Adopt 2..16 already submitted model-only tasks into one bounded Host group.
 * This is ownership transfer, not batch admission: the Host submits tasks with
 * existing submit(), per-request budget/deadline and bounded Executor admission,
 * and is responsible for every accepted handle if a later submit is rejected.
 * No worker, queue, retry, endpoint reassignment or Runtime store is created.
 *
 * Tasks must be distinct, untaken, ungrouped, on the same Router, with distinct
 * thread ids and the same parent run id (1..128 bytes). Tool loops are excluded:
 * the Host reviews their checkpoints individually. id uses the same 1..64 ASCII
 * letter/digit/_/- format as Router endpoint ids. All validation/allocation
 * precedes transfer. On success tasks[0..count) become NULL; group owns their
 * graphs/Execution handles through release. On failure every slot is unchanged.
 * Former aliases return EBUSY for mutation/take/release while adopted; no alias
 * may be used after group release. Read-only task inspection/wait remains valid.
 * Returns EINVAL/ENOMEM/EBUSY/EALREADY, with NULL out_group on rejection.
 * All group APIs and other Host control operations remain serialized.
 */
CXX_C_API int turbo_agent_router_group_adopt(const char *id,
    turbo_agent_router_task_t **tasks, size_t count, turbo_agent_router_group_t **out_group);
/** Borrow the member's stable task UUID through group release; NULL for invalid index. */
CXX_C_API const char *turbo_agent_router_group_task_id(
    const turbo_agent_router_group_t *group, size_t index);
/** Wait for all members under one total wait budget; no heartbeat/poll thread is installed.
 * OK means terminal, including failures. timeout_ms follows task_wait(). */
CXX_C_API int turbo_agent_router_group_wait(turbo_agent_router_group_t *group, uint64_t timeout_ms);
/**
 * Observe every member at one monotonic timestamp. The first observed loss,
 * stale authority, failed/cancelled/timed-out run or unresolved tool proposal
 * halts aggregation and requests PARENT cancellation of siblings. No result is
 * fabricated and ownership lasts until actual terminal. Observe at least once
 * per member's heartbeat interval. Failure is sticky; return its code (EPERM,
 * ECANCELED, ETIMEDOUT, EIO or EPROTO). Invalid/backward timestamps return EINVAL
 * without changing members; EALREADY after collection. No automatic fallback or
 * retry is performed.
 */
CXX_C_API int turbo_agent_router_group_poll(turbo_agent_router_group_t *group, uint64_t now_ms);
/** Poll the entire group first; renew one still-running member only if healthy.
 * EALREADY for a terminal member; a late heartbeat cannot revive a halted group. */
CXX_C_API int turbo_agent_router_group_heartbeat(turbo_agent_router_group_t *group,
    size_t index, uint64_t now_ms);
/** Request cancellation of every member; aggregation remains halted even after terminal.
 * OK on first request, EALREADY if already halted/collected; EINVAL for bad reason. */
CXX_C_API int turbo_agent_router_group_cancel(turbo_agent_router_group_t *group,
    turbo_cancel_reason_t reason);
/** Owned diagnostic projection in member order; Runtime remains the run/result owner. */
CXX_C_API int turbo_agent_router_group_inspect(const turbo_agent_router_group_t *group,
    json_value_t **out_view);
/**
 * Build one owned Session-compatible user input for an explicitly Host-selected
 * aggregation model. Requires all terminal, successful, current-authority,
 * responsive members with nonempty model text, no pending tools or failure state.
 * Metadata and text are read from canonical Execution results, never supplied by
 * a caller/model as completed-worker assertions. Includes task/endpoint/thread/
 * Runtime run/checkpoint/parent identities in stable member order.
 *
 * host_task and each worker text are at most 64 KiB; serialized evidence is at
 * most 1 MiB. ENOBUFS rejects excess without truncation. EBUSY before barrier;
 * EALREADY after a successful build or result collection; other supervision
 * errors propagate, with NULL output. Failure consumes no results. The derived
 * input is an immediate data projection, not a stored run/result or an execution
 * grant. Host instructions/policy and the aggregation model are configured
 * separately; worker text is untrusted data. No model is called by this API.
 * Explicitly submit that input through Router and verify the aggregation result.
 * This validates provenance/completion, not answer quality or task correctness.
 */
CXX_C_API int turbo_agent_router_group_build_input(turbo_agent_router_group_t *group,
    const char *host_task, json_value_t **out_input);
/**
 * Move canonical summary/state pairs once, in adoption order, after all terminal
 * and all authority/loss gates pass. Transfer success is not execution success;
 * failed runs can have NULL summary and owned failure state. Explicit cancellation
 * and failed siblings may be collected for Host diagnostics; loss/stale authority
 * withholds the entire collection. No JSON result copy is retained by the group.
 *
 * Caller supplies an empty array with capacity >= member count and frees each
 * moved JSON with json_free(). out_count is always initialized to zero and then
 * counts actual transfers, even on error. Normal preflight rejection moves none.
 * EBUSY before barrier, EALREADY after collection; individual delivery errors
 * propagate. A previously built aggregation input does not consume these pairs.
 */
CXX_C_API int turbo_agent_router_group_take_results(turbo_agent_router_group_t *group,
    turbo_agent_router_group_result_t *results, size_t capacity, size_t *out_count);
/** EBUSY until every member is terminal; never partially releases live work.
 * Releases all adopted tasks and their endpoint reservations. NULL is a no-op. */
CXX_C_API int turbo_agent_router_group_release(turbo_agent_router_group_t *group);

#ifdef __cplusplus
}
#endif
#endif
