# Job Inspect admission intents

`turbo_agent_job_admission.h` adds explicit write-ahead intent to the bounded
asynchronous Job operation. `turbo_agent_harness_submit_admitted_job_inspect`
requires a Host-stable admission ID (1..64 ASCII letters/digits/underscore/hyphen).
It uses that ID for the existing terminal observation too, with the same copied
Job binding and supervisor UUID. Legacy and recorded submissions remain unchanged,
including their zero-Store-access guarantee on queue rejection.

## Ordering and ownership

The caller's path is:

1. Validate/copy bounded inputs, allocate the execution/token/UUID, retain owners
   and acquire the existing exclusive Harness reservation.
2. Read and validate the complete bound Job at the expected revision; reject a
   durable cancellation or unreadable/malformed cancellation record.
3. Attempt exactly one create-if-absent in `agent_job_admissions`. Only an OK
   acknowledgement permits step 4.
4. Try the existing bounded thread pool. Only its successful return hands an
   execution handle to the caller.

No execution mutex is held over Store I/O. This new call can synchronously block
in Store; owners and reservation remain retained. The Host serializes all Runtime,
Store and policy access, including this interval. Its worker continues to validate
current source/authority, cancellation, deadline and canonical execution facts.
An intent grants no authority. The record contains no compiler configuration,
capability grant or process-local monotonic deadline, and cannot reconstruct a
handle or resume a worker after process restart.

Allocation/input/reservation failures happen before any admission write. Store
errors retain their original status, clear the output and never enqueue, including
lost acknowledgements that actually committed. There is no retry, readback, put
fallback or cleanup deletion. Duplicate IDs return EALREADY without enqueueing,
even when the prior request never reached a queue. The first record/UUID remains
immutable. An explicit query is the Host's historical reconciliation operation;
it does not promote a failed acknowledgement to successful enqueue.

Queue ENOBUFS/ESHUTDOWN (and other original queue errors) can leave an acknowledged
intent. These failures return no handle and invent no terminal observation. A
different admission ID is not permission to retry a canonical STARTED/unknown
attempt. A successful queue return still uses the existing recorded worker and
result-retirement rules. Observation errors withhold result transfer and preserve
operation/cancellation facts independently.

## Immutable schema v1

Collection `agent_job_admissions`, key `job_id:admission_id`. Exactly ten fields:

| Field | Value |
| --- | --- |
| schema_version | Integer 1 |
| kind | `job_inspect_admission_intent` |
| admission_id, observation_id | Both equal the Host-selected admission ID |
| supervisor_id | Canonically formatted supervisor UUID, distinct from canonical Run/execution IDs |
| job_id, job_revision, proposal_version | Exact complete bound Job head/proposal |
| thread_id, attempt_id | Exact frozen attempt binding |

All strings have exact bounded identities; the schema cannot hold a result,
certificate, goal, prompt, raw diagnostic, quality decision, deadline or receipt.
`turbo_agent_job_admission_get` validates the complete Job history and every
field/type/identity join, returning independently owned JSON. It clears outputs
on error: ENOENT for absence, ENOTSUP for future schemas, EPROTO for corrupt
records; backend read errors propagate. Historical queries perform no writes,
current authority/source/cancellation checks, canonical result lookups or queue
operations. Use a quiescent Runtime or a fresh process. Admission and terminal
observation are queried separately; compare their supervisor UUID and all binding
identities rather than accepting an unrelated observation under a reused ID.

## Recovery limits

The optional [queue disposition receipt](job-queue-receipts.md) now observes
native rejection or selected run/cancel callbacks. The admitted API itself keeps
its original behavior; missing receipts still leave the pre-selection gap unknown.

An intent is not an atomic transaction with a native queue. The same record can
survive a crash immediately before enqueue or after successful enqueue. It does
not attest queue acceptance, callback entry, Run creation, result delivery,
finalizer completion or remote stop. Missing terminal metadata leaves those
facts unknown; recovery never fills in a missing observation or automatically
retries work. Canonical attempt/Run/effect recovery remains the existing separate
contract. Host intent and cancellation are separate immutable records, not a
multi-record transaction, lease or cross-process exclusion protocol.

Durability follows the chosen Store. Memory is volatile; unsupported atomic
backends return ENOTSUP. Preserve records on rollback. Older binaries ignore
this collection and do not acquire its opt-in admission fence. There is no
distributed scheduler, queue commit receipt, durable uncommitted cancellation
intent recovery, resumable execution handle or remote finalizer acknowledgement.
These remaining parts of #213 are still open; real-model qualification stays
in #278, and independent RPC issue #287 remains unresolved.

## Qualification

Unit cases cover copy/identity joins, one immutable write before queue access,
write errors and lost acknowledgements with zero dispatch/readback, duplicate
IDs, full/closed queues with retained intent, stale/cancelled/unreadable Jobs,
reservation and early owner release during blocked publication, all schema
fields, future versions, historical reads and current result-transfer gates.

Eight SQLite cases use six fresh application processes each: init, crash,
inspect, denied authority, duplicate reconciliation and final verification.
Cuts cover pre-create, post-create, returned lost acknowledgement, successful
enqueue before dispatch, full queue, closed pool, pool cancellation and completed
Inspect. An independent database records the actual supervisor lineage before
the admission write and maintains invocation counters. Exact exits 86/87 are
required. Fresh queries preserve absence/identity, create no Run or observation,
and do not send. Duplicate submissions in fresh processes fail before dispatch.
The completed case retains one canonical committed attempt/Run and matching
terminal observation; all other cases retain zero callback/Run activity.
