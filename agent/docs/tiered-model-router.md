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

Each task installs exactly one model node. There is no tool node, model-selected
endpoint/path, automatic aggregation, fallback endpoint or Router retry. Model tool
proposals are returned as proposals; even registered, authorized tools are not
executed by this graph. A Host that dispatches them must independently use the normal
Agent/Harness authorization, result and replay checks. Thus the first slice can
produce plans and execution drafts; it is not yet an automatic multi-worker coding loop.

Router APIs are serialized control-plane operations. Keep agents, runtimes, callback
code and their borrowed dependencies alive through task release. Do not share their
execution/store ownership outside this Router while a slot is occupied. The authority
Agent is a separate, quiescent control-plane owner. Policy replacement remains
quiescent, including queued work and callbacks; never replace it in a tool, store,
event or transport callback. Admission and model entry check captured authority;
delivery rejects a stale generation even when the same grants were reinstalled
after execution. Generation is an invalidation token, not synchronization.

## Supervision and recovery

Each accepted task has a stable existing Execution UUID and a canonical Runtime
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

Take summary/state once after terminal and verify the canonical summary status and
the task's domain result. A completed model node does not prove the task is done.
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
