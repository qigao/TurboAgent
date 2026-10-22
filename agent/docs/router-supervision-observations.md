# Persisted Router supervision observations

Router liveness and Execution failure facts can exist without a Runtime checkpoint.
In particular, an early Provider failure may return no summary/run/checkpoint at all.
The existing `task_inspect()` view is transient. A Host can now explicitly call
`turbo_agent_router_task_record_observation(task, host_runtime, id)` to retain one
bounded historical observation in its existing Runtime Store, then use
`turbo_agent_router_load_observation(host_runtime, id, &record)` after reopening it.

This is the first persisted supervision slice of [#213](https://github.com/qigao/TurboAgent/issues/213).
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
does not prove that work/effects never started. The API changes no task state, takes
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
Process crash cuts, durable admission, remote takeover, automatic restart, complete
supervision audit and [#74](https://github.com/qigao/TurboAgent/issues/74)'s backend
effect-recovery matrix remain separate acceptance gates.

The change adds two functions and constants without changing existing struct layouts,
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
