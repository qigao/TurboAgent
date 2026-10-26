# Native Job queue disposition receipts

`turbo_agent_job_queue_receipt.h` adds an explicit receipted submission, stacked
on the existing [admission intent](job-admission-intents.md). It leaves legacy,
recorded and admitted APIs unchanged. A receipt observes an actual native queue
disposition; it does not turn the native queue into a persistent scheduler.

## Serialized publication

Preflight retains/copies inputs and owners, reserves the Harness and acknowledges
the immutable intent before trying the bounded queue. Then exactly one of these
paths owns Runtime/Store access:

| Native outcome | Receipt publisher | Selection | queue_status |
| --- | --- | --- | --- |
| A run callback is selected | That callback, before Inspect | `run` | OK |
| A pending task is selected for cancellation | That cancellation callback, before settlement | `pool_cancelled` | OK |
| The submission is rejected | Submitting thread; no callback can run | `rejected` | Original nonzero queue status |

On successful enqueue the submitting thread performs no more Store operations.
The selected callback publishes first; no execution mutex is held over Store or
Host callbacks. There is no second worker, waiting gate, concurrent Runtime writer
or replacement queue. The worker reference retains owners through publication,
operation settlement and pool finalization. Store callbacks cannot synchronously
wait on/destroy this pool or its retained owners. Original pool-drain and result
retirement obligations still apply.

Only acknowledged acceptance publication permits Inspect. A publication error,
including ENOTSUP, EBUSY, EALREADY or a lost acknowledgement, prevents Inspect and
settles locally as FAILED, operation EPROTO and tool ERROR. No terminal observation
is invented: its `dispatch=inspect` would be inaccurate when Inspect never entered.
An explicit Host cancellation accepted while publication was active is still
published once before terminal, retaining its independent status.

`turbo_agent_job_execution_queue_receipt_status` returns EBUSY before terminal,
then OK with the original publication status. Unreceipted terminal handles return
ENOENT in that output. Raw backend EBUSY at terminal is distinct from an unfinished
operation by the outer getter status. Receipt errors take precedence at actual
result take, which returns the original error, clears outputs and retires once.
Otherwise all existing canonical proof, token, cancellation, source, authority and
terminal-observation checks remain required. Receipt acceptance is no authority.
Queue delay and receipt I/O still count against the absolute execution deadline.

On rejection the API returns the original queue error and no handle; the required
`out_rejection_receipt_status` separately reports the original publication result.
It is ENOENT when that write was never attempted, including successful enqueue and
preflight/intent failures. All writes use one create-if-absent, without overwrite,
retry, error readback or put fallback. Duplicate admission IDs fail before queue
access. A failed receipt acknowledgement may have committed; a later query never
promotes that acknowledgement or authorizes retry.

## Schema and historical queries

Collection `agent_job_queue_receipts`, key `job_id:admission_id`. Schema v1 has
exactly twelve fields: the intent's ten fields with kind replaced by
`job_inspect_queue_receipt`, plus integer `queue_status` (original signed 32-bit
code) and string `selection`. An OK code requires `run` or `pool_cancelled`; a
nonzero code requires `rejected`. All Job/proposal/thread/attempt/admission/
observation identities and supervisor UUID match the immutable intent exactly.
No result, certificate, config, deadline, grant, diagnostic or receipt-of-send
payload is stored.

`turbo_agent_job_queue_receipt_get` validates complete bound Job history, the
original intent and exact receipt schema/status/identity joins, returning owned
JSON. Outputs clear on failure: ENOENT for missing intent/receipt, ENOTSUP for
future schemas, EPROTO for corruption, and original backend read errors. It is
read-only and performs no current authority/source/cancellation check, canonical
result lookup, queue operation or recovery dispatch. Host uses a quiescent Runtime
or a fresh process. Intent, receipt and terminal observation remain separate
immutable records, not an atomic multi-record transaction or cryptographic seal.

## Crash boundary

Acceptance receipts are deliberately deferred until callback selection. A crash
after successful enqueue but before selection can leave only an intent. Such
absence remains unknown; queries do not fill a missing receipt or observation.
A present acceptance receipt proves historical native admission and callback
selection, not Inspect entry, Run/effect/result success, a currently queued task,
finalizer completion or remote stop. A lost acknowledgement can leave an accepted
receipt even though this worker withheld Inspect. A rejected receipt attests only
this submission's native rejection, not absence of other attempts/effects.

Memory Store remains volatile; other durability follows the selected backend.
Preserve all records on rollback. There is no restartable queue, lease/distributed
exclusion, cancellation-intent crash recovery or remote/finalizer acknowledgement.
The remaining #213 work, native dependency instrumentation #123 and independent
RPC issue #287 stay open. #278 tracks real-model quality/cost/latency separately.

Unit tests exercise publication barriers and reentrant status queries, original
errors/lost acknowledgements, separate rejection/publication outcomes, cancellation
and shutdown overlap, early owner releases, historical/current authority separation,
all schema/identity/status joins and legacy behavior. Thirteen SQLite cases use six
fresh processes each, with exact crash exits 86/87, an independent lineage/callback
oracle, fresh/denied queries, duplicate reconciliation and final reread. They cover
run, pool-cancel and rejection at pre-create/post-create/lost-ack/returned boundaries,
plus accepted native work that crashes before callback selection. Recovery never
executes a callback, creates a Run, fabricates missing metadata or sends anything.
