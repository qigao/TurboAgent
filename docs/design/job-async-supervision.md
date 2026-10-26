# Local asynchronous supervision for bound Jobs

`turbo_agent_job_execution.h` adds a narrow Harness operation owner over the
existing bounded Salts executor. Its worker calls the same synchronous bound Job
Inspect implementation and uses the same Session, Runtime, canonical attempt and
Run records. No second execution backend, scheduler or persistent result store is
introduced. This is local single-owner supervision, not a remote worker protocol.

## Ownership and admission

`turbo_agent_harness_submit_job_inspect` copies Job identity, compiler scalar
limits and up to 32 capability names of 1..64 bytes before returning. Deadline is
an absolute monotonic timestamp; queue delay counts. Source and authority are
admitted afresh when the worker runs. A successful enqueue does not acknowledge
Job existence, authority, callback entry or execution success.

Harness/App/Session are retained. The bounded pool, registry/callback code, Store
callbacks and verifier data remain borrowed. Host keeps them alive through pool
drain and must not mutate policy, Job or Store while the worker can access them.
After terminal, Host may make serialized quiescent control changes; result take
revalidates them. Direct Session/Agent/Store APIs remain under their existing Host
ownership contracts, and must not bypass the reservation concurrently.

One Harness submission reservation spans queued, running and terminal-but-not-
retired phases. Other Harness operations return EBUSY through that interval.
Taking once retires it even when result admission fails; final handle release
also retires it. A worker-owned reference keeps the reservation and Harness alive
if the client releases early. Release is neither cancellation nor detachment.
Drain the pool before freeing borrowed callback/registry/verifier owners.
Operation terminal notification can precede the pool finalizer's last reference
release. If retiring by release, pool drain establishes completed reference
cleanup before asserting fresh admission. Taking a terminal result retires the
reservation directly before it returns.

Submission uses `cmeta_threadpool_try_submit_task` with run, cancel and finalize
callbacks. Queue saturation and shutdown preserve ENOBUFS/ESHUTDOWN, publish no
handle, and perform no Store access or fallback execution. An accepted task has
exactly one run-or-cancel callback and one finalize, including CANCEL_PENDING;
finalize releases the worker reference. A shutdown policy cannot silently leak a
queued handle/reservation by dropping its run callback.

## Cancellation and terminal observation

| Observation | Meaning |
| --- | --- |
| cancel returns OK | First explicit request identity/reason copied in memory and cooperative token signalled |
| wait returns ETIMEDOUT | Caller wait expired; operation may still own callback/Store access |
| RUNNING with cancellation_requested=1 | Cancellation is requested; callback exit and persistence are not acknowledged |
| terminal state | Inspect/pool-cancel callback has settled and any accepted cancellation publication attempt returned; no further callback/Store work remains |
| cancellation_status=OK | Durable cancellation request publication acknowledged |
| cancellation_status is another error | Original publication/open error; possible lost acknowledgement requires explicit reconciliation |
| cancellation_requested=0, cancellation_status=ENOENT | No explicit persistent request was accepted |

The first explicit request wins under the handle mutex; later callers get
EALREADY. The worker closes cancellation admission after synchronous Inspect
returns. A caller racing that closure either has its exact request accepted or
gets EALREADY; that code never means persistence success. No handle mutex is held
across Store or Host verifier calls. Status/wait are safe alongside cancellation;
Host exclusively serializes take/release with control changes.

An accepted explicit request is published once, by the worker on the same Runtime
after Inspect becomes quiescent and before terminal publication. The existing
six-field `agent_job_cancellations` schema and create-if-absent contract are reused.
There is no retry or error readback. Inspect status and cancellation publication
status remain separate: canonical execution may have committed successfully while
late cancellation withholds delivery, and a cancellation write can fail after
the callback has exited. Host reads and compares the stored request explicitly
after terminal to reconcile an uncertain acknowledgement.

Pool CANCEL_PENDING uses the same settling path without dispatching Inspect or
creating a Run. It persists a cancellation only if an explicit request was
accepted. Pool shutdown or deadline expiry alone cannot invent a Host request ID.
Pending cancellation callbacks can wait behind other running pool work; closing
the pool does not promise instantaneous terminal notification.

A noncooperative callback retains ownership until it returns. Neither a request,
deadline nor wait expiry forcibly stops it. Terminal is a local observation held
by this handle, not a new persistent terminal-acknowledgement record or proof that
remote effects stopped. A process crash loses the handle and any uncommitted
in-memory request. Recovery uses the existing durable Job/cancellation/attempt
records; canonical STARTED/unknown records are never automatically retried or
rewritten as cancelled. A supervisor state describes only its own local call.

## Result transfer

The worker discards its transient typed result/certificate and retains only the
execution reference. `take_result` does no dispatch, Run creation or replay. It
reuses the acceptance layer's canonical attempt/journal/Run identity checks to
read the sole typed result, without inventing a quality decision or invoking an
evidence callback for an acceptance that has not happened.

At actual transfer, it recompiles current proof, revalidates source, authority,
canonical result schema/certificate/identities and cancellation, then checks
source/control/authority/token again before exposing independently owned JSON.
It never returns an earlier buffered result when current records are unreadable.
The same serialization-order-sensitive certificate equality as acceptance remains
in force; no new canonicalization promise is made.

Before terminal, take returns EBUSY without consuming. A terminal take attempt is
consumed and retires the reservation even on error; outputs remain NULL. Later
takes return EALREADY. Terminal status describes worker completion at its observed
time; a later source change, generation replacement, cancellation or deadline can
still reject transfer. Host can explicitly reopen/reconcile after retirement;
there is no implicit retry or second tool invocation.

## Qualification and remaining work

Native unit cases exercise blocked callbacks and Store publication, queue
admission/rejection, copied inputs, explicit and pool-only queued cancellation,
pool CANCEL_PENDING finalization, early releases, concurrent status/cancellation
callers, first-request arbitration, publication failures/lost acknowledgements,
fresh result admission and one-shot retirement. Installed C11/C++17 consumers
exercise the exported ABI and asynchronous ownership path.

Twelve additional SQLite process cases drive queued async cancellation through
the three existing pending/committed/previously-sent states and four publication
boundaries. Each uses five fresh processes and exact exit 86/87, then checks the
same independent destination receipt and invocation/send counters. A prior send
is reconciled without sending again; the original synchronous cancellation cases
remain separate.

Persistent admission before queueing, durable terminal acknowledgements, remote
worker heartbeats/leases and recovery of in-memory cancellation intent remain
open. This slice does not complete M1 or #213. Real-model quality/cost/latency
remains #278; all fixtures use offline tools and a simulated destination.
