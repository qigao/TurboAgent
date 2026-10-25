# Router admission intents

`turbo_agent_router_submit_with_intent()` adds a synchronous write-ahead gate to
initial Router submission. It selects the ordinary endpoint and builds the existing
Execution first, then atomically creates an immutable intent **before** bounded pool
admission. `max_model_turns` is zero for a model-only task or 2..16 for a tool loop.
Existing submission APIs, request/config layouts and Journal schema are unchanged.

The Host supplies an independent Runtime Store and a unique 1..64-byte ASCII
letter/digit/underscore/hyphen admission id known before calling submit. The record
is stored in `router_admission_intents` using existing `create_if_absent`; ordinary
put/get/list is never substituted and no error causes an implicit retry. Endpoint
Runtime/stores must be independent of that Host store. Serialize Host store/control
operations; callbacks must not reenter the Router or mutate its configuration.

## The two boundaries

The record's semantics are `pre_dispatch_intent`. It contains the exact first
Execution UUID, selected endpoint/model/provider, role, thread and complete supplied
parent link, Host-declared request bounds, price snapshot, estimate/budget and original
monotonic deadline. The UUID is assigned before persistence and remains the ordinary
stable Router task id after successful admission. `task_inspect()` and canonical
`model_route` state additionally expose the Host `admission_id` for these tasks.

This records metadata, not another prompt/result store. It contains no input body,
answer, tool arguments/results, authority grant, resumable handle or actual charge.
Runtime/Execution/Journal retain their existing canonical ownership. The intent is
not a complete input fingerprint: the Host id must identify exactly one intended
invocation; metadata similarity cannot authenticate a colliding request.

| Outcome | Task handle | Intent and work |
| --- | --- | --- |
| Routing/authority/input/setup rejection | null | No intent write or pool admission. |
| Atomic write failure, including committed write with lost acknowledgement | null | No enqueue from this invocation; a record may already exist. Explicitly load its known id. |
| Existing id (`EALREADY`) | null | No enqueue or overwrite, even for identical metadata. This is a conflict, not success. |
| Pool rejects after successful write | null | Preserve the intent; return the original pool error. The id remains consumed. |
| Submit succeeds | owned live task | Intent exists; supervise, cancel, drain and release normally. The task can fail before a Runtime checkpoint. |
| Process dies during/after these steps | no recoverable handle | An intent alone does not distinguish before-enqueue, queued, running or completed work. |

The intent write and the pool/Runtime/Journal are not a cross-system transaction.
There is no post-write fallback, automatic queue admission from a loaded record,
reassignment or retry. A live caller receiving a definitive pool rejection knows
that invocation was not queued. After a crash only the surviving record remains;
that process-local knowledge must not be invented from an intent.

Persistence/queue time consumes the original deadline. If writing the record takes
the task past its deadline, controlled Runtime execution produces its normal timed-out
checkpoint before model entry. A record is not a promise that work ran or will run.
The synchronous gate's borrowed context is cleared before enqueue; the Host store
only needs to outlive the submit call, not the worker, approval pause or task handle.

## Reconciliation and limits

`turbo_agent_router_load_intent(host_runtime, id, &record)` reads exactly one id and
validates version, complete field set, types, embedded NUL, identity, request bounds,
clock relationships and estimated-price arithmetic. It clears output on every error
and returns an owned tree on success. The record remains usable after all original
Router/Agent/store owners are destroyed. Invalid schema/identity returns EPROTO,
oversized fields ENOBUFS, unsupported atomic creation ENOTSUP; backend status codes,
including ENOENT and I/O failure, are preserved. Reads never fall back to a scan.

Records are limited to 16 KiB, model/provider/parent tool name to 1024 bytes, other
parent ids to 128, and ordinary Router ids to 64. Projection limits do not cap earlier
input copying or backend read/parse allocations. Store quotas/retention are existing
backend/application responsibilities. Do not delete/recycle an id to make an
unresolved external effect retryable. SQLite supplies its existing committed-record
contract; memory is volatile and file storage lacks the required atomic primitive.
This adds no physical power-loss or arbitrary-corruption guarantee.

After a successful submit, an explicit existing
`turbo_agent_router_task_record_observation()` can link the intent's task UUID to
terminal canonical Runtime run/checkpoint ids. Read that exact run for reconciliation,
not a later unrelated thread head. Retain the original endpoint-to-store binding:
an endpoint label does not locate or authenticate a replacement backend. A missing
terminal observation is unresolved;
neither an intent nor an old observation establishes current liveness, authority,
delivery, answer quality or effect certainty. Origin monotonic clocks cannot renew
leases in another process. Apply current Host authority and #74's UNKNOWN-side-effect
gate to every explicit recovery decision.

This API records initial dispatch intent. Approval resumes continue through their
ordinary exact-checkpoint API and are not automatically recorded here. Hosts can
explicitly use [review intents](router-review-intents.md) for those segments. Complete
supervision, durable work resumption, remote-loss reconciliation and actual-model
quality/cost evaluation remain separate #213 workstreams.

## Qualification

Router tests exercise actual offline Provider/Execution/Runtime flows, atomic-store
failures, lost acknowledgement, id conflicts with changed metadata, bounded queue
rejection, unsupported backends, isolated store lifetime, persistence-time deadline,
strict metadata validation and SQLite reopen with canonical observation/run links.
Tool-loop tests cover initial intent through a separate approval resume and reject
unsupported atomic storage before model/hook/effect entry.

Seven process tests require exact `_Exit(86)` plus a matching boundary marker:
before/after atomic intent creation, after lost-ack return, after queue rejection,
inside model entry after an independently committed counter, after terminal execution
before delivery, and after an explicit terminal observation before delivery. Independent
restart processes reopen Host/worker/oracle SQLite stores, inspect records and effect/
delivery counters, reject current-denied or duplicate submissions without fallback,
then explicitly admit a different healthy identity once. Failure evidence is retained.
These are controlled process cuts with OS/cache alive, not physical power loss.
