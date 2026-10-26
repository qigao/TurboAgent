# Durable cancellation fences for Jobs

Public C11 API: `turbo_agent_job_cancel.h`, implemented by Harness over the existing
borrowed Runtime Store. This is a serialized Host control plane. A request means
stop future Job progress; it does not assert callback termination, an absent
external effect, a rejected quality decision, or successful delivery.

## Ownership and publication

The Host serializes cancellation with all Job/Harness operations and external
delivery. For an active synchronous Inspect, signal its existing cancellation
source, keep every borrowed owner alive, and wait for the operation to return.
Only then call `turbo_agent_job_request_cancel` at the observed Job revision.
There is no concurrent Store writer contract, interrupt thread, background
watcher, distributed fencing token, forced callback termination or hard deadline.
A blocked noncooperative callback still blocks quiescence.

The request uses one atomic create-if-absent in `agent_job_cancellations`, keyed by
Job ID. It is independent of the bounded proposal revision slots, so cancellation
also works at revision 0 or after all 64 slots are consumed. The record has exactly
six fields, with its own schema version 1:

| Field | Contract |
| --- | --- |
| schema_version | Integer 1 |
| kind | `job_cancellation` |
| job_id | Exact existing Job ID |
| job_revision | Exact complete observed head, 0..64 |
| request_id | Host-selected 1..64 ASCII letters, digits, `_` or `-` |
| reason_ref | Host-owned nonempty reference, at most 1024 bytes; no embedded NUL |

The API validates the entire existing descriptor/history before publishing. A
stale expected head returns EBUSY. Duplicate requests return EALREADY, including
identical content. Publication errors retain the original status; there is no
automatic readback, retry, overwrite or put fallback. A failed acknowledgement
may have committed. The Host explicitly reads and compares Job/head/request/reason
identity to reconcile; EALREADY alone does not acknowledge its intended request.
An unsupported atomic backend returns ENOTSUP.

`turbo_agent_job_get_cancellation` validates the complete Job history and exact
record shape/join, returns owned JSON, and clears output on error. Absence is
ENOENT; a future positive schema is ENOTSUP; invalid metadata is EPROTO; Store
errors remain distinct. Job creation also refuses a surviving cancellation record
whose descriptor was lost, preventing accidental identity reuse.

## Progress and recovery boundaries

| Operation | With a valid cancellation request |
| --- | --- |
| Propose or bind at the current head | ECANCELED, no next revision |
| Bound Job Inspect, including committed replay | ECANCELED, no callback/Run/result publication |
| Record new acceptance or rejection | ECANCELED, no new decision |
| Prepare a delivery intent | ECANCELED, no new intent |
| Read Job snapshot or cancellation | Available as control/history observations |
| Read existing acceptance/delivery | Still validates current policy, source, canonical execution and applicable evidence |
| Record a receipt for an existing intent | Still permitted after full existing proof and identity checks, so Host can reconcile a prior send |

Progress checks distinguish explicit cancellation from missing, malformed, future
or unreadable control evidence. Only ENOENT admits progress. Execution checks
before source verification and again immediately before attempt lookup/claim and
before returning a committed result. Acceptance and intent publication check
before proof verification, before creation, and after acknowledgement. Any final
read failure withholds output while preserving the committed record; explicit
queries reconcile it. These repeated observations do not create an atomic
transaction or support racing owners.

The existing live token is checked before dispatch and again at the bound Job
result-delivery boundary. Late explicit cancellation returns ECANCELED with
TURBO_TOOL_CANCELLED; a late deadline returns ETIMEDOUT with
TURBO_TOOL_DEADLINE_EXCEEDED. Result/reference stay NULL; the certificate may
describe an actual committed execution. Canonical attempt result and Run success
remain intact. A live token does not implicitly create a durable request; until
Host publishes one after quiescence, a separately authorized call with a fresh
token can still replay the same committed attempt.

An existing delivery intent remains `outcome_unknown` after cancellation until a
matching Host-verified receipt is recorded. Cancellation cannot infer whether a
send occurred. Host queries the destination by its stable idempotency key and
validates the response before recording a receipt. A delivered view continues to
mean a Host attestation, not an independently verified network fact. Recovery
reads do not authorize another send. The Host must check cancellation and
serialize that observation through any external delivery; an earlier intent or
an ENOENT observed before cancellation is insufficient permission to send later.

## Compatibility and qualification

Job descriptor/history, attempt, Run, acceptance, intent and receipt schemas are
unchanged; snapshots keep `created`, `proposed` or `attempt_bound`. No result is
copied into cancellation metadata. There is no resume, record deletion or request
replacement API. Continuing cancelled work needs a new Job identity. Preserve
all records on rollback. Older binaries and direct compiled/Session APIs do not
enforce this additive Job fence; a Host must not route cancelled work through
them. A mixed-version writer deployment is unsupported.

Unit coverage checks all proposal phases, bounds, stale heads, unsupported claims,
pre-publication and lost-ack errors, malformed/future/misjoined records, orphaned
cancellations, fail-closed read boundaries, late token/deadline results, verified
Host callbacks, and receipt reconciliation without erasing execution facts.
Installed C11/C++17 consumers compile and exercise the new API.

Twelve SQLite crash cases cover pending, committed/accepted and remotely sent but
unreceipted Jobs at four boundaries: before creation, after creation, returned
lost acknowledgement and returned success. Five fresh application processes per
case initialize, crash with exact exit 86/87, inspect, explicitly reconcile and
replay. Independent destination receipts and send/callback counters verify that
post-send cancellation never resends and cancellation never fabricates a receipt.
These are specified process exits, not arbitrary OS kill, power-loss or
concurrent-writer qualification.

This completes the durable cancellation-request slice. Asynchronous scheduling,
terminal cancellation acknowledgement, remote worker supervision, bound-Job
amendment/replanning and production delivery connectors remain open. M1 remains
incomplete; real-model quality/cost/latency is tracked separately in #278.

[Local asynchronous supervision](job-async-supervision.md) now retains a bound
Inspect owner through actual callback exit and serial cancellation publication,
with explicit status/wait/result retirement. Durable terminal acknowledgements
and remote supervision remain separate work.
