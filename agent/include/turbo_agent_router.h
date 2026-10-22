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

typedef struct turbo_agent_router_s turbo_agent_router_t;
typedef struct turbo_agent_router_task_s turbo_agent_router_task_t;

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
 * A shared authority tool lane excludes every other Router task until release,
 * including while waiting for approval. Caller input review flags are cleared.
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

#ifdef __cplusplus
}
#endif
#endif
