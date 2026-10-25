# Persisted Router supervision observations

Router liveness and Execution failure facts can exist without a Runtime checkpoint.
In particular, an early Provider failure may return no summary/run/checkpoint at all.
The existing `task_inspect()` view is transient. A Host can now explicitly call
`turbo_agent_router_task_record_observation(task, host_runtime, id)` to retain one
bounded historical observation in its existing Runtime Store, then use
`turbo_agent_router_load_observation(host_runtime, id, &record)` after reopening it.

This is a persisted supervision slice of [#213](https://github.com/qigao/TurboAgent/issues/213).
It supports diagnosis and manual reconciliation; it is not an execution recovery API.
[The Router](tiered-model-router.md), Execution and canonical Runtime/Tool Journal
continue owning admission, runs, results, effects, cancellation and delivery.

## Record ownership and commit boundary

The Host chooses a distinct 1..64-byte ASCII letter/digit/underscore/hyphen id for
each observation. The API writes collection `router_observations` with the existing
atomic `create_if_absent` operation. An existing id returns EALREADY without overwrite,
even if the task has since advanced. An unsupported backend returns ENOTSUP; ordinary
put/get/list is never substituted. Saving does not retry or read after failure.

A backend can commit and then return an error. That error is propagated unchanged;
the Host must explicitly load the same id to learn whether its record exists. Absence
does not prove that work/effects never started. A found record can belong to an older
colliding task: compare task/current Execution/thread/parent identities with the
expected invocation before accepting it as the attempted observation. EALREADY is
a conflict, not an acknowledgement that the requested observation was recorded.
The API changes no task state, takes
no result, admits no work and requests no cancellation. It can therefore fail while
the original task remains owned and running; the Host must still supervise and drain it.

Use an existing Host Runtime independent of **every** endpoint Runtime. This prevents
the call from sharing an occupied worker's store owner. The Host serializes save/load,
store close and all other use of the selected Host store; backend callbacks must not
reenter Runtime/Router or mutate policy. All earlier Router quiescent restrictions
remain. No scheduler, audit callback, queue, second result database or background
writer is created. A retained alias to a grouped task permits observation only through
group release, under the same exclusive Host control contract.

SQLite's existing bounded backend provides committed records, FULL synchronization
and its documented exclusive-owner lock. Memory records remain volatile. The file
backend lacks atomic create and is rejected. Custom backend guarantees apply as
documented by that backend. There is no transaction between an observation, a Runtime
checkpoint, a Journal record, a tool effect or external response delivery.

This API runs **after** ordinary task admission. A crash before the call, during
admission or between explicit observations can leave no record. It is not a complete
audit or durable admission protocol. The Host chooses when to observe and must handle
every error. Multiple observations are independent immutable records; there is no
automatic latest-record index, cross-record sequence or retention/eviction policy.
Configure existing backend capacity and application retention explicitly.

## Observe before result ownership transfer

`turbo_agent_router_task_take_result_with_observation(task, host_runtime, id,
&summary, &state)` makes terminal observation acknowledgement a prerequisite for
one explicit result handoff. It first applies ordinary take-result gates: terminal,
untaken, individually owned, responsive and current captured authority. It then
records the current segment using the same version 1 observation schema and only
moves the canonical Execution summary/state after atomic create returns success.
The independent Host store is borrowed synchronously and is never retained by workers.
All Host control operations and store callbacks retain their existing restrictions.

Write failure, lost acknowledgement, unsupported atomic create or an existing id
returns the original error with null outputs and leaves canonical result ownership
unchanged. No read, list, put fallback, overwrite or retry occurs. An existing id is
a conflict even when it matches the expected task. The Host can explicitly load that
id and compare task, current Execution, thread, parent and canonical Runtime anchors,
then decide whether to use a fresh id or ordinary `task_take_result()`. This decision
cannot revive lost ownership, stale authority or an unresponsive child. A fresh id
after successful transfer returns EALREADY without another write. Group aliases
return EBUSY before persistence; group collection remains the group's responsibility.

The API also handles early Runtime errors with a null summary and owned diagnostic
state. OK is ownership transfer, not execution success. An approval-pending segment
can be observed and taken before an explicit canonical approve/reject command; the
next segment needs its own observation id. Rejecting approval retains the existing
`interrupted` / `review_rejected` result, distinct from cancellation.

An observation does not prove transfer or application delivery. The production
record contains neither a delivered flag nor another result payload. In particular,
the same terminal record can survive a cut before transfer, after transfer or after
delivery. Existing take/release/review APIs remain available without automatic audit;
this opt-in boundary does not install a complete lifecycle logger or make result
delivery transactional across systems. It grants no restart/resume/retry authority.

```c
json_value_t *summary = NULL, *state = NULL;
int rc = turbo_agent_router_task_take_result_with_observation(
    task, host_runtime, host_observation_id, &summary, &state);
/* On error, keep supervising/owning task and explicitly reconcile persistence.
 * On success, check execution status and operation code before using the pair. */
json_free(summary);
json_free(state);
```

Ten additional five-process tests cover approve/reject at these handoff cuts. The
transfer and delivery columns are separately committed test-oracle counters, not
claims made by the observation itself:

| Process cut | Terminal observation | Transfer count | Delivery count |
| --- | --- | --- | --- |
| Before atomic create | Absent | 0 | 0 |
| After atomic create, before transfer | Present | 0 | 0 |
| After lost-ack error returns | Present | 0 | 0 |
| After transfer, before delivery | Present | 1 | 0 |
| After explicit application delivery | Present | 1 | 1 |

Each case requires exact exit 86, a matching boundary marker and null-output evidence
at atomic create. Independent processes reopen Host/worker/oracle SQLite stores and
check current-segment references, canonical Journal/claim phases and model/effect/
transfer/delivery counts. A new Host performs different explicitly authorized work
once on the same thread while preserving the old record and never redelivering the
old result. These controlled process cuts keep OS/cache alive; they do not qualify
physical power loss or arbitrary interruption inside SQLite.

## Historical metadata, not execution authority

Version 1 is one flat object with these fields:

| Fields | Meaning |
| --- | --- |
| `schema_version`, `observation_id`, `semantics`, `clock_domain`, `recorded_at` | Version 1; exact Host id; `historical_host_observation`; `origin_process_monotonic`; origin UTC clock observation. |
| `task_id`, `execution_id`, `endpoint`, `model`, `provider`, `role`, `thread_id`, `parent_run_id` | Actual selected route and stable first Execution UUID, current segment UUID and Host lineage. |
| `execution_status`, `runtime_result_code` | Execution status at the read; Runtime operation code only after terminal. |
| `runtime_run_id`, `runtime_checkpoint_id`, `runtime_status`, `failure_kind` | Bounded terminal metadata from the canonical Execution result; null when unavailable. |
| `unresponsive`, `authority_current`, `approval_pending`, `max_model_turns`, `review_resumes` | Origin Host supervision facts and current segment's approval checkpoint observation. |
| `started_mono_ms`, `observed_mono_ms`, `heartbeat_mono_ms`, `deadline_mono_ms` | Origin process monotonic values. `observed` is the Host's last poll, not the save time. |
| `estimated_cost_micros`, `budget_micros`, `actual_cost_micros` | Original admission estimate/budget; actual charge remains null. |

Running/queued observations always have null result code and Runtime/failure anchors,
even if execution completes while the observation is being built. Terminal result
metadata is borrowed only during synchronous projection, before take_result(). Saving
after transfer returns EALREADY. No prompt, model answer, tool arguments/output,
checkpoint state, raw pointer, policy grant or recovery handle enters the record.
A completed run can have a run id and a null checkpoint id; observations preserve
that canonical absence rather than inventing a checkpoint for reconciliation.

Loss is a separate sticky observation from actual terminal status. A blocked callback
can remain running after loss/cancellation; recording it cannot free its reservation.
After it really ends, a new observation can preserve both its terminal status and
the earlier loss. Result delivery still rejects loss or stale authority. Reinstalling
the same policy at a legal quiescent boundary invalidates delivery as before; the
observation may record that invalidity without delivering model/tool content.

`authority_current=true` means only that captured generations matched **at the origin
observation**. It cannot grant authority in another process, resume an approval,
clear loss, prove effects stopped, prove delivery occurred or verify answer quality.
The first task UUID remains stable across review segments; the current Execution UUID
changes. Loading/mutating an owned observation has no effect on the canonical checkpoint
or its exact call/arguments approval. The existing review command remains necessary.

UTC clock values and monotonic numbers are not cross-record ordering or freshness
proofs. After restart, never compare these monotonic values against the new process's
clock to revive a lease. Parent identity and route are Host metadata, not authenticated
remote-heartbeat or external-effect attestations. The store is trusted Host storage;
schema checks are not cryptographic authenticity or tamper evidence.

## Bounds, errors and reconciliation

Identity fields use the existing Router id bound; parent/run/checkpoint/status/failure
metadata is at most 128 bytes, model/provider 1024 bytes, and serialized record 16 KiB.
Numeric fields use exact integers within the JSON safe range; status, review counts,
clock relationships and terminal/nonterminal metadata are checked. Unknown schema or
fields, invalid types, embedded NUL and inconsistent metadata fail closed. No truncation
or partially returned record occurs. Backend reads/parsing/allocation can precede these
projection bounds; use the existing SQLite record/database/scan limits as appropriate.

Save/load return Salts errors, preserving backend statuses. EINVAL covers invalid
arguments/ids or an endpoint store used for save; EPROTO invalid metadata/schema;
ENOBUFS projection bounds; ENOMEM allocation; EALREADY an existing id or a moved
terminal result; ENOTSUP unsupported atomic create; reads preserve ENOENT distinctly
from I/O failure. Every failed load clears output. UTC timestamp construction can
return EIO. Neither operation installs a fallback or automatic retry.

```c
/* host_runtime is an existing, independently owned Host Runtime Store. */
int rc = turbo_agent_router_task_record_observation(task, host_runtime, host_id);
/* Handle rc and keep supervising task even when persistence failed. */
/* After explicitly reopening that same Host store: */
json_value_t *observation = NULL;
rc = turbo_agent_router_load_observation(host_runtime, host_id, &observation);
/* Check rc. Inspect canonical Runtime/Journal evidence and current authority
 * separately before deciding on an explicit recovery action. */
json_free(observation);
```

For terminal records with anchors, independently reopen the actual worker's durable
Runtime Store and load that run/checkpoint by exact identity. For a failure without
anchors, retain the recorded Execution failure instead of inventing a successful run.
For nonterminal/lost observations, actual remote/effect outcome remains unresolved.
A started Journal call without a committed outcome still requires explicit manual
reconciliation; the observation never makes it safe to automatically re-execute.
Additional backend crash/power-loss coverage, remote takeover, automatic restart,
complete supervision audit and [#74](https://github.com/qigao/TurboAgent/issues/74)'s
effect-recovery matrix remain separate acceptance gates. Optional initial and review
admission records are described in [admission intents](router-admission-intents.md)
and [review intents](router-review-intents.md).

The APIs add functions and constants without changing existing struct layouts,
submission paths, storage schema version, workflows or dependencies. Rollback is to
stop calling them; existing Runtime records and APIs are unchanged. These observations
are a supplement to existing evidence, not a migration of run/result ownership.

Native Router tests use actual offline Execution/Runtime, injected atomic-write/read
failures and SQLite close/reopen. They cover running/terminal consistency, early
Provider failure, sticky loss, quiescent policy invalidation, collision protection,
lost write acknowledgement, unsupported backends, invalid ids/shared stores, bounds,
corrupt records and independent ownership. The tool-loop test covers approval and
resumed segment identities while preserving canonical review and actual tool counts.
Owner destruction/reopen is not a deterministic process-kill or power-loss test.
The guarded-handoff tests additionally cover output ownership on write errors,
lost acknowledgements, cross-task collisions, early Runtime failures, stale/lost
authority, grouped aliases, invalid/aliased outputs and approved/rejected segments.
Negative controls deliberately ignore an acknowledgement error or transfer before
persistence; the handoff process cases must reject both implementations.
