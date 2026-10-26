# Job proposal coordinator, schema v1

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
The Host must call the existing Harness stable-attempt API under the exact frozen
source and scope. This first API does not enforce that later Host call or verify
that a referenced attempt exists; those are execution integration responsibilities.

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

Remaining M1 work includes source freshness and exact dispatch binding, validated
Run/attempt result-reference observation, acceptance evidence, delivery intent and
ambiguous-delivery reconciliation. Async execution, cancellation supervision,
replanning of bound Jobs and distributed ownership need separate contracts.
Real-model quality/cost remains issue #278. This foundation does not mark M1 done.
