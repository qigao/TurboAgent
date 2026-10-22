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
inspect views are not durable. The Host can explicitly
[record immutable supervision observations](router-supervision-observations.md)
through its existing Runtime Store, or use its own audit integration. Runtime
remains the source of run evidence; an observation does not authorize recovery.

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

## Bounded worker groups and explicit aggregation

The next slice adds a Host-owned group for 2..16 **model-only** Router tasks. Submit
reviewed subtasks through the existing `submit()` API, using distinct thread ids and
one common parent run id, then pass the accepted handle array to `group_adopt()`.
Successful adoption clears those array slots and transfers every task to the group;
rejection leaves every slot unchanged. An adopted task cannot be taken, cancelled,
polled or released through an old alias. Group release waits for all actual terminal
states and releases all endpoint reservations together. Read-only task inspection
and waiting do not transfer ownership.

This is a deliberate alternative to adding a batch scheduler or another supervisor
Runtime. The current supervisor/handoff scaffold routes nodes inside one graph, while
a Router group supervises independent existing Executions and Runtime stores. The
Host's existing bounded pool supplies the parallelism. No worker, queue, admission
retry, lease takeover, copied Runtime state or durable group database is added.
The group keeps bounded handles and supervision facts; Execution remains the sole
result owner. A private terminal-result borrow supports synchronous inspection only
under exclusive Host ownership, ending at take/release, without a concurrent lease.
Existing callers and config layouts remain compatible; rollback is to keep using
individual Router tasks.

Adoption is **not atomic batch admission**. If the second submit is rejected after the
first was accepted, the first still belongs to the Host. The Host must cancel/wait/
release it or explicitly retain it; a failed adoption never detaches it. Reserve all
worker estimates and any aggregation estimate in the Host's task budget before
submitting. The group reports the sum of admitted estimates but makes no retroactive
budget, actual billing or rollback claim. Endpoint reservations persist even after a
worker finishes, so another submitted task cannot silently reuse its Agent/store.

`group_poll()` observes every member at one Host monotonic timestamp. An observed
loss, authority invalidation, Provider failure, cancellation/deadline or unresolved
tool proposal records a sticky failure and requests parent cancellation of siblings.
Polling is explicit: there is no background supervision thread. A member heartbeat
first polls the entire group, so repeatedly renewing one member cannot hide another's
expiry. Late heartbeats cannot revive a failed group. `group_wait()` applies one total
wait budget; success means all terminal, including failures. It does not renew
heartbeats or prove external effects stopped. The Host must keep polling/renewing
live members according to each admitted interval, including when work is queued.

`group_build_input(group, host_task, &input)` is a fail-closed fan-in projection, not
an automatic aggregation model call. All members must be terminal, responsive under
the Host's observations, authorized under their captured parent/child generations,
and successful with nonempty model text, no pending tools and no failure state. The
projection includes group/task/endpoint/thread/Runtime run/checkpoint/parent identity
and actual model text in adoption order, regardless of completion order. Model claims
that another worker finished are never used to establish this barrier.

The returned Session-compatible input carries a JSON evidence bundle in a **user
message**. Worker text is untrusted data, not system instructions or tool authority.
The Host chooses and configures the aggregation model and its trusted instructions
separately, then explicitly calls ordinary Router `submit()` with a PLAN request.
The executable integration test exercises two real offline model workers concurrently
and sends their canonical evidence to a third independently routed aggregation model.
This supports parallel analysis, alternative solution drafts and engineering task
reviews. It does not establish answer quality, a correct code change, majority
consensus, or completed external work. The aggregation result must still be checked
and any tool proposal needs the normal Host-authorized execution path.

Both `host_task` and each worker text are limited to 64 KiB. Serialized evidence is
limited to 1 MiB, including JSON escaping and identity fields. Oversize input fails
without truncation or consuming canonical results. Building succeeds at most once;
results are still owned by Execution. `group_take_results()` transfers each original
summary/state pair once into a caller-provided array after a full terminal/authority/
loss preflight. Pairs retain adoption order, and the returned count always reports
actual transfers. Transfer success alone does not mean execution success: diagnostic
collection after failed/cancelled runs can include a NULL summary and failure state.
Loss or stale authority withholds the whole collection before any transfer.

The aggregate input is an immediate owned projection, not a second retained result
store. A policy change after it was delivered cannot recall that already owned data,
but blocks a later raw-result collection. Policy replacement remains quiescent,
including queued work and callbacks; group observation only compares generation
tokens and never makes ordinary policy fields safe for concurrent mutation.

Tool-loop tasks cannot be adopted. Parallel model workers therefore do not introduce
concurrent Host tool callbacks, approval mutation or workspace writes. Existing tool
loops keep their exclusive authority lane through task release. Parallel tool
execution would require explicit shared hook-state ownership, immutable policy leases
or locks and reclamation, store concurrency, independent approval identities and
cancellation/drain guarantees. Those mechanisms and remote/durable recovery remain
separate acceptance work, along with real-model quality/cost evaluation for #213.

`test_turbo_agent_router_group` covers real concurrent Responses/compatible-Chat
workers, explicit aggregation, adoption validation/ownership, shared parent identity,
partial queue rejection, Provider failure with sibling cancellation, unresolved tool
proposals, one-time collection, quiescent generation invalidation, heartbeat/deadline
gates, retained live ownership, oversized text and the escaped serialized limit across
sixteen individually valid worker outputs. Existing Router and tool-loop regressions
remain enabled through the unchanged native CMake/CTest presets.

## One application over these primitives

The [single Agent report example](../examples/single-agent-report.md) connects the
three slices into one Host goal: context handoff, two independent model workers,
explicit aggregation, checkpoint approval and tool-receipt-based delivery. It uses
existing public APIs and a bounded synchronous Host composition, without installing
another task scheduler or storing a second Runtime result. Its offline integration
test checks actual Provider messages and canonical run identities, rejection/abort,
quiescent authority invalidation, partial admission cleanup, Provider failure and
refusal to accept model completion claims or invalid tool results as delivery.
It is a shared application pattern for general Agent and engineering-workbench
routes, not a claim that the report renderer implements engineering task acceptance.
