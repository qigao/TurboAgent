# Job proposal coordinator, schema v1

The additive [cancellation control API](job-cancellation.md) now fences future
proposal/binding and bound Job execution without changing the v1 history schema.
Cancellation is separate control evidence; snapshots retain their proposal state.

Status: implemented proposal/binding foundation, not a complete Job scheduler or
M1 workbench. Public C11 API: `turbo_agent_job.h`, owned by Harness. This API uses
a caller-supplied Runtime, never selects a backend and does not consume it.

## Records and ownership

| Collection | Key | Immutable content |
| --- | --- | --- |
| `agent_jobs` | Job ID | schema_version=1, id, goal, acceptance_criteria, max_revisions=64 |
| `agent_job_revisions` | Job ID + `_` + three-digit revision 001..064 | schema_version=1, job_id, revision, previous_revision, kind, kind-specific fields |

IDs contain only ASCII letters/digits/underscores/hyphens, with Job IDs at most
64 bytes. The fixed-width revision suffix is unambiguous. Every record is
published with RuntimeStore create-if-absent. There is no mutable head, list-order
dependency, put fallback, additional SQLite table or copied tool result.

The Host serializes operations with other Job access and record mutation; it must
quiesce all callers before destroying a borrowed Runtime. Conditional insertion
protects a particular next slot from replacement, including a competing publisher
that wins after preflight. It is not a distributed owner lease or scheduler fence.
No multi-process execution ownership is claimed. SQLite's existing exclusive
connection ownership remains the persistence boundary for the process tests.

Creation freezes one goal and one textual acceptance criterion, each 1..4096
bytes. A goal edit requires a different Job ID in v1. Before claiming a descriptor,
creation checks all 64 history slots and rejects surviving orphan history so a
lost descriptor cannot attach a replacement goal to old proposal records.
Duplicate descriptors return EALREADY, even for identical inputs.

## State transitions and exact joins

| Current state | Operation | Published record / next state |
| --- | --- | --- |
| created, revision 0 | propose(expected=0) | revision 1, proposal |
| proposed | propose(expected=head) | next revision, proposal; old versions retained |
| proposed | bind compiled Inspect(expected=head) | next revision, attempt_bound |
| attempt_bound | any new proposal/binding | EBUSY, no write |

A proposal record adds `source_json` and `source_revision`; its version is its
revision number. Source bytes are frozen exactly, at most 16 KiB, without embedded
NUL. The API does not parse/compile them or infer freshness from the Host's
1..128-byte revision label. The existing Compiler and Host remain responsible
for source preconditions, schema, effect, capability and current-grant admission.

A binding adds `proposal_version`, `thread_id` and `attempt_id`, using the existing
compiled Inspect identity limits (80/64 ASCII bytes). Its proposal_version must
join the latest prior proposal. Binding occurs before Host dispatch, but does
not create/read an attempt or certify that a callback started. A bound Job is
unresolved even if a tool later commits; no inferred acceptance/delivery occurs.
The Host can use the bound Job execution adapter described below to select the
exact stored source and scope. Direct stable-attempt API composition remains the
Host's responsibility; the binding operation itself never verifies that an
attempt exists.

Reads validate the descriptor and every one of 64 possible revision slots.
They check schema, field counts, types, bounds, Job identity, contiguous sequence,
previous revision and proposal-version joins, and prohibit records after binding.
A gap followed by a higher slot is EPROTO, not a valid partial head. Data outside
the v1 slot namespace is not part of this protocol. Queries return an independent
snapshot containing descriptor, current state, frozen proposal and binding only.
The bounded scan retains at most the descriptor, current proposal/binding and
one read record; it does not depend on backend list completeness or ordering.

## Publication failure and recovery

Each append publishes one self-contained record referencing already acknowledged
history. A descriptor with no revision is a valid created Job. There is no
multi-record atomic commit across proposal, binding, Run or compiled attempt.
The backend's documented atomicity/durability applies; memory is not durable and
a backend without atomic create returns ENOTSUP rather than get+put emulation.

Any failed publication returns its original status, with no retry or post-error
readback. A failed acknowledgement can still leave a committed descriptor,
proposal or binding. The Host explicitly reopens and inspects before deciding
its next action. Missing records remain distinct from read/parse/I/O errors.
A stale expected revision returns EBUSY; a competing conditional claim EALREADY;
64 occupied revisions ENOSPC. No record or slot is reused after exhaustion.

Recovery of an already bound Job preserves the exact proposal/attempt identity
and does not dispatch, replace a proposal, create another attempt, or claim a tool
result. Existing Harness rules still control committed-result replay and refusal
to redispatch STARTED/unknown attempts. A Job binding never grants authority or
weakens policy checks, even when a result is already committed.

## Versioning, rollback and validation boundary

Unknown positive schema versions return ENOTSUP; malformed v1 data returns
EPROTO. There is no implicit migration, deletion or in-place rollback. Older
binaries must reject unfamiliar versions; retain v1 readers for existing Jobs
when introducing another version. No API promises recovery from malicious store
rewrites or arbitrary record deletion; records remain in the Host trust domain.

Qualification covers real memory storage, conditional claim collision, unavailable
atomic insert, stale head, byte bounds, schema/join/gap rejection and lost
acknowledgements. Eight SQLite process cases cut before/after root/proposal/binding
publication and after returned lost-ack errors; each uses separate init, crash,
read-only inspection, explicit recovery and verification processes. These tests
exercise coordination metadata, not new tool execution crash guarantees.
A Harness integration test shows denied policy still invokes zero callbacks,
committed replay invokes the callback once overall, and Job records own no result.

The bound execution adapter now selects the exact persisted source/scope and
returns canonical attempt/Run references. Remaining M1 work includes source
freshness, external evidence verification, delivery intent and
ambiguous-delivery reconciliation. Async execution, cancellation supervision,
replanning of bound Jobs and distributed ownership need separate contracts.
Real-model quality/cost remains issue #278. This foundation does not mark M1 done.

## Bound Job execution adapter

`turbo_agent_harness_execute_job_inspect_json` reads a Job by ID from the Harness's
own Runtime, inside its submission exclusion. The expected Job head revision must
match a bound snapshot and its thread scope must match the Harness's captured
compiled-attempt scope. Source bytes and attempt ID come only from that independent
snapshot; this API accepts no caller override. Reading another Runtime's handle or
falling back to another source/attempt is not part of this adapter.

The adapter uses the existing compiled Inspect engine for every admission gate,
Host policy check, cancellation check, attempt claim, typed result validation and
Session finalization. A persisted source is compiled again; a Job binding does not
make it executable. STARTED/unknown attempts remain unresolved and fail closed;
committed attempts replay without a second callback, under current policy and the
same frozen plan/contract. The Host still owns source freshness validation and
must serialize all Job access and record mutation for the synchronous call.

Success returns independent result, certificate and an ephemeral execution reference:

| Reference field | Exact identity |
| --- | --- |
| job_id, job_revision, proposal_version | Persisted Job head and frozen proposal |
| thread_id, attempt_id, attempt_record_id | Stable compiled-attempt scope and canonical result key |
| execution_id | Compiled execution journal metadata key for this API call |
| run_id, run_thread_id | Actual Runtime Run from the completed Session summary |
| plan_hash, authority_generation | Admitted execution facts used by the existing engine |

The compiled execution metadata UUID and Runtime Run UUID are distinct; tests load
both records by their own returned keys and compare their exact references. On replay,
the attempt identity stays fixed while a new delivery Run is finalized. The reference
has schema_version=1 and kind=compiled_inspect, contains no typed result and is not
persisted. Job metadata is not updated and remains attempt_bound after success.
A returned reference is an observation of this completed operation, not proof of
Job acceptance, an external delivery receipt or a new grant.

A lookup failure preserves the store status. Wrong Job phase/revision returns EBUSY;
a thread mismatch EPROTO; no Run/attempt/tool is started by these rejections. All
JSON outputs clear on rejection, except the existing post-compilation certificate
contract on execution failure. If the tool result commits but constructing the
reference fails, ENOMEM withholds result/reference; explicit retry addresses the
same bound attempt. There is no implicit retry or alternate execution path.

The nine existing compiled Inspect process cuts also run through this adapter in
Job mode, with independent invocation/delivery counters and canonical-record checks.
They cover pre/post claim, pre/post Run-journal start, callback, pre/post result commit,
Run-journal completion and pre-delivery exit. Each restart verifies that the Job
stays bound, denied/stale authority still rejects, and committed replay does not
repeat the callback. Legacy direct-attempt crash cases remain separate; Job mode
has no source override, so its script omits the legacy caller-source-mismatch stage.


## Verified Host acceptance

[Host acceptance](job-acceptance.md) now revalidates current admission and the exact
canonical attempt, compiled journal and completed Runtime Run state snapshot before
publishing a separate immutable decision/evidence-reference record. It does not
change Job v1 proposal history, copy a typed result or imply external delivery.
Normal Run completion is joined through state_snapshot; no Checkpoint is assumed.

[Delivery intents and Host receipts](job-delivery.md) now compose accepted Jobs
with stable idempotency keys and explicit outcome reconciliation. They preserve
Job v1 history and canonical result ownership; Harness never sends automatically.
