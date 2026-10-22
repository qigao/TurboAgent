# Tiered model routing

The first implementation slice of [#213](https://github.com/qigao/TurboAgent/issues/213)
is a Host-controlled Router over existing Agent, Execution and Runtime APIs.
It supports independently configured planning/execution models on the same or different
Providers. It does not introduce another scheduler, durable store or provider protocol.

## Selection and budgets

Configure up to 16 endpoints with distinct Agent and Runtime owners. Each endpoint has
an id, role mask, Host-declared model features, difficulty threshold, token bounds and
an explicit price snapshot in micro-USD per million input/output tokens. Models and
Provider credentials are configured on each ordinary Agent, outside model output.

For each request, filter by role, required model features, difficulty and token bounds,
then require an estimated cost within the request budget. Among idle, authorized
endpoints choose the lowest estimate; equal estimates use configuration order.
Each token direction is rounded up separately to a micro-unit. Difficulty, token
estimates and model features are trusted Host metadata, not a learned quality claim.
The estimate is admission policy, not an enforced provider billing/token limit.
Configure real backend output limits and retry policy separately.

The request's `model_route` state field is replaced with the actual decision before
Runtime admission. It records endpoint/model/provider, selection reason, request
traits, prices, estimate and budget. Runtime checkpoints this state normally.
Existing `usage_records` preserve provider token evidence. Actual cost remains unknown;
the Router neither invents a charge nor promises lower cost, latency or better quality.

## Authority and execution

The authority Agent must explicitly grant `delegate`. The chosen child's capability
mask must be a subset of the authority Agent's current mask. This is an admission
filter, not full tool authorization: workspace paths, approvals, tool identities,
effects and result schemas still belong to the existing execution layer.

The original `submit()` path installs exactly one model node. There is no tool node, model-selected
endpoint/path, automatic aggregation, fallback endpoint or Router retry. Model tool
proposals are returned as proposals; even registered, authorized tools are not
executed by this graph. A Host that dispatches them must independently use the normal
Agent/Harness authorization, result and replay checks. Thus the first slice can
produce plans and execution drafts; it is not yet an automatic multi-worker coding loop.

Router APIs are serialized control-plane operations. Keep agents, runtimes, callback
code and their borrowed dependencies alive through task release. Do not share their
execution/store ownership outside this Router while a slot is occupied. The authority
Agent is separately owned; the opt-in tool loop below uses it for Host tool dispatch. Policy replacement remains
quiescent, including queued work and callbacks; never replace it in a tool, store,
event or transport callback. Admission and model entry check captured authority;
delivery rejects a stale generation even when the same grants were reinstalled
after execution. Generation is an invalidation token, not synchronization.

## Supervision and recovery

Each accepted task has a stable first Execution UUID and a canonical Runtime
thread/run/checkpoint identity. The request must bind a Host thread id and parent run
link. Execution's bounded pool admission, deadline, cancellation and one-time result
transfer remain authoritative. No Session inbox/follow-up or secondary run is created.

The Host supplies monotonic heartbeat observations and polls each active handle.
A missed interval sets a sticky `unresponsive` observation, requests parent cancellation
and blocks late result delivery. It does not manufacture a terminal state or free an
executing worker's slot. Heartbeats received after expiry cannot revive it. A transport
that ignores cooperative cancellation can remain executing: hard isolation, remote
heartbeat authentication and process termination are not supplied here. The finite
Execution deadline uses the real monotonic clock; heartbeat timestamps must use the
same clock in production. Tests can advance heartbeat observations without sleeping.

Use `task_inspect()` for the selected route, stable execution/thread ids, actual
Execution status/result code, observed elapsed time and liveness facts. Poll timestamps
determine observed elapsed time, not a separately measured provider latency. These
observations are not durable; a Host needing a durable supervision audit must record
them through its existing audit integration. Runtime remains the source of run evidence.

Take summary/state once per Execution segment after terminal and verify Execution status, its underlying
`runtime_result_code`, the summary when present, and the task's domain result.
Result-transfer success only transfers ownership. A Provider/graph failure can
return a null summary and an owned state carrying `model_error`; it is still an
Execution failure. Runtime can fail before committing a run/checkpoint, so neither
this API nor the route state proves that every failed attempt has durable evidence.
A Host needing that audit must capture the failure state and inspection facts through
its existing audit integration. A completed model node does not prove the task is done.
An unresponsive or stale-authority result is withheld; inspect the existing Runtime
using the bound thread/run identity for reconciliation. Release returns busy until
terminal, and Router destruction returns busy until all task handles are released.
No automatic retry, unknown-effect replay, detached worker reclamation, persistent
lease takeover or crash recovery guarantee is added. #74's recovery gate remains open.

## Host sketch

```c
turbo_agent_router_config_t config;
turbo_agent_router_request_t request;
turbo_agent_router_t *router = NULL;
turbo_agent_router_task_t *task = NULL;
turbo_agent_runtime_parent_link_t parent = {.parent_agent_run_id = host_run_id};
turbo_agent_router_config_init(&config);
config.authority_agent = authority; /* quiescent, delegate explicitly granted */
config.endpoints = reviewed_endpoints; /* each has its own Agent and Runtime */
config.endpoint_count = endpoint_count;
turbo_agent_router_request_init(&request);
request.role = TURBO_AGENT_ROUTER_EXECUTE;
request.difficulty = 30;
request.input_tokens = 1000;
request.output_tokens = 500;
request.budget_micros = host_budget;
request.timeout_ms = 30000;
request.heartbeat_timeout_ms = 5000;
request.thread_id = host_child_thread_id;
request.parent_link = &parent;
/* Check every return; keep dependencies live. Poll/heartbeat from the Host loop,
 * wait/cancel as needed, take and verify the canonical result once, then release
 * the terminal task and destroy the Router. */
```

`test_turbo_agent_router` uses offline Responses/compatible-Chat fixtures through actual
model/Runtime execution. It covers selection, model features, budgets, same/different
Providers, authority denial, liveness, cooperative cancellation/deadline, one-time
delivery, quiescent generation invalidation, tool proposals without effects,
duplicate state owners, bounded queue rejection and cost rounding/tie order.
Real model quality, hard spend/output limits, durable supervision and remote worker
reconciliation remain separate acceptance work; #213 stays open for those slices.


## Opt-in supervised tool loop

`turbo_agent_router_submit_tool_loop(..., max_model_turns, ...)` adds a bounded
single-child model/tool loop for execution requests. The ordinary `submit()` path
continues to return proposals without dispatch. Choose 2..16 model turns and require
`TOOL_PROPOSALS`; the total admission estimate reserves the per-turn token estimate
multiplied by that bound. Include growing history in the Host estimate, and configure
provider output/retry limits separately. The estimate is still not a hard spend cap.
The final turn consumes the preceding result; another proposal interrupts before
another effect. This supports supervised general Agent actions and engineering
Host tools with the same execution boundary; it is not multi-worker aggregation.

The selected child and authority Agent must share the exact same immutable canonical
ToolRegistry. The authority Agent must have a Host-configured `before_tool` guardrail.
The Host remains responsible for its approval, path and domain policy; the existence
of a hook alone does not prove those policies. The child proposes tools, but its
capability grants are checked before the authority Agent's ordinary ToolNode runs.
Host middleware, policy, guardrails, tool executor and Journal remain authoritative.
Wire aliases resolve to canonical names. Each turn permits one call, avoiding
ambiguous batch approval and partial dispatch. Registry and hook mutation while
executing is unsupported, as is policy replacement outside quiescence.

The loop requires declared known effects and a strict canonical result schema before
dispatch. Effect facts do not grant authority. The private checked ToolNode explicitly
validates a successful output after Host output middleware/guardrails, including a
Journal replay, before a next model call. A malformed or schema-invalid result stops
the loop with `invalid_tool_result`. A callback/guardrail/unknown-effect failure stops
with `tool_failed`. Raw Journal transport facts are preserved: a callback that returned
success can have a committed raw result even when this opt-in domain validation fails.
The loop does not rewrite that record into a different callback outcome. Generic
ToolNode and direct RuntimeTools invocation retain their existing opt-in validation
semantics. Provider or graph errors can still precede a canonical checkpoint.

When a guardrail returns `approval_required`, Runtime interrupts with the existing
exact tool/call/argument review note and a normal checkpoint. `task_review(task, pool,
1)` prepares the existing `approve_review` command against that fixed canonical
checkpoint and resumes the same run through Execution. Caller-owned result JSON is
never used as a state override. Middleware runs again: changed arguments require
fresh approval. After successful dispatch the review is cleared, so reused call ids
in later model turns need fresh approval. `task_review(..., 0)` records rejection and
stops without effects or automatic replanning. Initial input review flags are cleared.
Only `approval_pending` can be resumed; failures and turn limits are not retry points.

Review is explicit Host input, bounded to 32 accepted segments, under the original
absolute deadline. Missing heartbeat while an Execution segment is running remains
sticky and blocks review/delivery. A terminal approval wait has no running worker;
it is bounded by the original deadline, not a manufactured worker heartbeat. Explicit
cancellation of a paused approval blocks later review. Queue rejection preserves the
previous result/checkpoint and does not start another endpoint. On accepted review,
unclaimed previous JSON is released and the new segment has one result transfer.
The task id remains the first Execution UUID; `inspect.execution_id` identifies the
current segment. Retaining the first Execution keeps that id alive without creating
another result owner. Derived approval checkpoint identity is copied before terminal
publication; Runtime owns the actual state and records throughout.

One authority tool lane is held through task release. A retained loop excludes all
other Router tasks, and a loop cannot start while any model task is retained. This
bounded serial choice preserves Host hooks, executor and single-writer store ownership;
it does not add concurrent policy reads/writes, parallel Host dispatch or a second
scheduler. To enable those later requires explicit ownership of shared hook state,
policy snapshots/locks and their lifetimes, store concurrency, approval identity and
callback cancellation. Generation alone provides none of that synchronization.

Rollback is to keep using `submit()`; existing callers, struct layouts and registrations
are unchanged. `test_turbo_agent_router_tool_loop` exercises real Runtime/Execution,
Host ToolNode/Journal and offline Responses/compatible-Chat providers: successful
loops, approval/resume/rejection, argument and call-id binding, quiescent generation
invalidation, child/Host permission, replay result validation, unknown effects,
result middleware, turn bounds, bounded queue rejection and liveness/deadline gates.
No paid-model quality, durable supervision, remote loss recovery or multi-worker
acceptance claim is made; #213 remains open for those remaining gates.
