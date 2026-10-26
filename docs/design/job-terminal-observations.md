# Persistent local Job operation observations

`turbo_agent_job_observation.h` adds an opt-in recorded submission over the
existing asynchronous Job operation. It saves an immutable observation after
Inspect or pool cancellation and any explicit cancellation publication settle.
It does not persist queue admission or reconstruct an execution handle.

## Publication and ownership

`turbo_agent_harness_submit_recorded_job_inspect` copies a Host-stable observation
ID (1..64 ASCII letters/digits/underscore/hyphen) alongside the existing bounded
inputs. The old submission API and status-v1 layout are unchanged. Queue rejection
still returns the original ENOBUFS/ESHUTDOWN and performs no Store access.

The worker retains the Harness reservation and Runtime owners through one atomic
create-if-absent in `agent_job_observations`, then publishes live terminal status.
No handle mutex is held across Store/Host callbacks. Releasing client owners
early cannot destroy Runtime before that attempt. Pool finalize/reference cleanup
can follow terminal notification; pool drain remains required before destroying
borrowed callback/registry/verifier owners.

`turbo_agent_job_execution_observation_status` reports EBUSY before live terminal.
After terminal its outer result is OK and its output is the original observation
publication status. Legacy handles report ENOENT in that output. Inspect status,
cancellation publication status and observation publication status remain three
separate facts. A backend EBUSY can be a terminal publication error; inspect the
outer getter result and live state rather than interpreting that integer alone.

Publication requires the complete current Job to be bound at the expected head.
It validates the exact record shape/joins before creating. Duplicate keys return
EALREADY, including identical submissions. There is no overwrite, put fallback,
retry or error readback. A lost acknowledgement may leave a committed record.
Host explicitly queries and compares observation ID, supervisor UUID and exact
Job/proposal/thread/attempt identities; an older record with the same ID is not
acknowledgement of the current operation. An Inspect failure does not suppress
an independently possible observation, and an observation error does not rewrite
Inspect/cancellation outcomes.

Durability follows the selected Runtime Store. Memory observations are volatile;
backends without atomic creation return ENOTSUP. No native persistence layer or
cross-record transaction is added.

For recorded submissions, result take withholds result/certificate/reference when
observation publication failed. If Inspect succeeded, it returns that original
observation error; otherwise it preserves the Inspect error. Taking still retires
the reservation once. A subsequent query does not silently promote an uncertain
acknowledgement or authorize another operation. Legacy result behavior is unchanged.

## Schema v1

Keys are `job_id:observation_id`, unambiguous because neither identifier permits
colon. A record has exactly seventeen fields and is at most 16 KiB serialized:

| Field | Contract |
| --- | --- |
| schema_version, kind | Integer 1, `job_execution_observation` |
| observation_id | Exact Host-selected ID |
| supervisor_id | Canonically formatted supervisor UUID, independent of Run/compiled execution IDs |
| job_id, job_revision, proposal_version | Exact complete bound Job head/proposal |
| thread_id, attempt_id | Exact frozen binding |
| dispatch | `inspect` or `pool_cancelled` |
| state | Local observed `completed`, `cancelled`, `timed_out` or `failed` |
| operation_status, tool_status | Original local operation codes; pool non-dispatch reports ECANCELED/CANCELLED |
| cancellation_requested | Boolean: an explicit Host request was accepted |
| cancellation_status | Original preceding cancellation publication result, or ENOENT when none was accepted |
| cancellation_request | Exact `{request_id, reason_ref}` metadata, or explicit null |
| execution_reference | The actual thirteen-field reference returned by successful Inspect, or explicit null when unavailable |

`dispatch=inspect` identifies entry into the bound Inspect API, not proof of tool
callback entry or Run creation. A queued cancellation can select that API path
and stop before either of those events.

The state is frozen after Inspect and cancellation publication settle, before
observation I/O. A deadline can expire during or after that I/O; actual result
take still checks its token and all current proof gates. A successful Inspect
reference can coexist with later local cancellation/deadline withholding. If
Inspect itself withheld its reference, the observation cannot invent one, even
when the canonical attempt actually committed. Absence of a reference never proves
absence of a callback, Run or external effect.

Record validation checks exact fields/types, bounded text, status coherence,
canonical UUID formatting and reference shape/binding joins. Completed metadata
requires a successful Inspect code, strict successful tool code and reference.
Pool cancellation admits no successful Inspect reference or invented Host request.
The record stores no typed result, certificate, goal/prompt, raw diagnostic text,
quality decision or delivery receipt. It cannot contain its own publication
acknowledgement status; that status is a separate observation in the live handle.

## Reopen and evidence limits

`turbo_agent_job_execution_observation_get` returns independently owned historical
metadata after validating the complete Job descriptor/history and exact record
joins. It performs no writes, list scan, model/tool execution, source/authority
check or canonical result lookup. Missing data returns ENOENT, future schemas
ENOTSUP, corrupt records/joins EPROTO, and backend read errors remain distinct.
Host serializes reads after quiescence or in a fresh process.

A historical completed observation remains readable after policy replacement or
source changes. That does not re-authorize its reference. Result delivery still
requires current Harness proof/cancellation/token validation. The metadata is a
trusted local Store observation, not a cryptographic seal, remote receipt or
cross-record transaction. Current cancellation and canonical execution facts are
queried separately where required; a recorded cancellation write error is never
rewritten merely because Host later finds the request committed.

An observation may commit before live terminal notification, pool finalization
or process exit. It confirms that the local operation had settled at its observed
point, not that every owner was destroyed or a remote worker stopped. A crash
before creation can leave no record even after a callback/Run committed; absence
cannot prove admission rejection or safe retry. Recovery never fabricates a
missing observation, resumes an old handle or retries STARTED/unknown attempts.
Records are additive and immutable; preserve them on rollback. Older binaries
ignore this collection and cannot expose the recorded view.

## Qualification and remaining work

Unit cases cover publication barriers, independent status/error reporting,
lost acknowledgements without readback, duplicate IDs across different supervisor
UUIDs, exact shapes/joins/future schemas, read errors, withheld unacknowledged
results, failed/expired/pool-cancelled operations, early owner release, legacy ABI
behavior, queue rejection without Store access and historical/current authority
separation. Installed C11/C++17 consumers exercise the additive API.

Twelve SQLite process cases cover completed Inspect, explicit queued cancellation
and pool-only cancellation at pre-create, post-create, returned lost-ack and
returned-success boundaries. Five fresh application processes per case require
exact exit 86/87, then inspect under both fresh and denied authority and verify
again. Independent invocation counters and canonical attempt/Run checks ensure
queries do not rerun a callback or fabricate an observation/receipt.

Durable pre-queue admission, crash recovery of uncommitted cancellation intent,
distributed ownership, remote worker supervision and stronger terminal/finalizer
acknowledgements remain open. M1 and #213 remain incomplete. #278 independently
tracks real-model quality/cost/latency; fixtures use offline tools only. The
unresolved typed RPC entry-observation issue #287 remains separate.
