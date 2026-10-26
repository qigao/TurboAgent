# Mandatory Host verification for bound Jobs

Status: implemented opt-in explicit-store constructor in
`turbo_agent_job_verifier.h`. A verified Harness requires both source and external
acceptance-evidence callbacks, copies their configuration at construction, and
offers no setter to disable them. Existing constructors retain their documented
manual Host verification preconditions. The Harness configuration ABI and all
persisted schemas remain unchanged.

## Host trust and callback lifecycle

`turbo_agent_harness_create_verified_with_store(config, verifier, out)` rejects a
NULL/incomplete verifier, wrong ABI/size, incomplete Store or invalid nested
configuration before consuming any stores. RuntimeStore/MemoryStore failure
transfer otherwise matches `create_with_store`. No external verification runs
during construction. The configuration value is copied before Session creation;
callback code and user_data remain borrowed, not reference-counted or consumed.
They must outlive all Harness references and executions. Changing the caller's
original configuration cannot disable the installed checks.

Callbacks run synchronously under Harness submission exclusion with no Harness
mutex held. Inputs are borrowed immutable JSON/strings only until return; do not
mutate/free/retain them. Reentrant Harness submissions fail EBUSY. Host must keep
Session/Job ownership, policy, registry and code quiescent, and pin/serialize the
external source/evidence view through the synchronous operation. Verification is
bounded, read-only work; Host implements any remote credentials, I/O limits,+cancellation and deadline handling. These callbacks must not send messages or
produce effects. No dynamic DSO lease or hard timeout is added.

This is a Host integration seam, not an evidence provider, network client or
automatic assessment system. Library code cannot establish an independent truth
about an arbitrary evidence identifier or prevent a trusted Host from supplying
an incorrect verifier. [The rooted local file adapter](job-file-verifier.md) now
checks pinned manifests and actual source/evidence bytes; additional adapters
need their own qualification. Direct compiled-attempt and ordinary Session APIs retain their
existing contracts; verification here applies only to bound Job APIs, and is not
a process sandbox or universal admission hook.

## Verification points

| Point | Callback and actual input |
| --- | --- |
| Before bound Job execution | source: validated persisted Job snapshot, frozen proposal bytes and source_revision |
| Before returning a committed Job result/reference | source again: refuse changed/unavailable inputs even after tool/Run commit |
| Before acceptance publication/read and all delivery operations | source: freshly loaded snapshot before current compiler/Host admission |
| After exact canonical attempt/result, journal and Run snapshot joins | evidence: snapshot, exact execution reference, actual validated canonical typed result, explicit proposed/stored decision and evidence_ref |
| After successful evidence verification | source again before returning verification to the acceptance/publication layer |

Source revision is still a label: the Host callback must resolve it and compare
the frozen proposal's input snapshot with its authoritative current source view.
Repeated checks are observations, not a transaction or distributed fence. The
Host's pinning/quiescence contract remains essential across the intervals.

Evidence verification must join the external evidence to the exact Job/proposal,
criteria and execution/result and support the supplied accepted/rejected decision.
Only already validated typed successful Inspect results reach that callback.
Host still explicitly chooses a quality decision; a verifier does not choose one,
promote rejection to acceptance, change criteria or grant tool/delivery authority.
The callback sees the canonical result borrowed from its existing record; no
second persistent result or verification cache is introduced.

## Errors, committed outcomes and restart

Return SALTS_OK only for verified input. All nonzero callback statuses are
preserved, including missing/unavailable/stale/unknown evidence and deadlines.
There is no default-allow result, automatic callback retry, cached success,
backend repair or alternate path in a verified Harness. Initial source failure
precedes Run/tool creation. A final source failure may follow successful canonical
commit; result/reference are withheld while the certificate and actual tool status
can still describe that committed attempt. Explicit retry rechecks source and
addresses the same attempt, preserving committed-result replay without invoking
the tool again. There is no rollback of a real completed Inspect.

Evidence failure precedes acceptance/delivery publication and withholds read
output; historical records remain unchanged. Failed Store acknowledgements still
return their original status without readback/retry. No verifier callback is
added after a failed publication to infer success. Reconciliation is explicit.

No persisted record is marked permanently externally verified. Every verified
Harness acceptance/delivery operation consults its current Host verifier again,
including for records created by an older/manual constructor. Restart requires
Host to reinstall complete live callbacks; old stored success cannot substitute.
Rollback retains all existing Job/attempt/review/delivery evidence. Using a legacy
constructor restores its manual-verification responsibility, not a verified claim.
External receipt/destination/payload verification still belongs to the Host
delivery adapter; this callback verifies acceptance evidence, not network sends.

## Qualification and remaining scope

Unit tests cover incomplete constructor ownership, copied callbacks, exact
canonical result/reference inputs, source/evidence error propagation, final-source
refusal after commit, explicit same-attempt replay, quality rejection, canonical
validation ordering, callback reentrancy and source recheck after evidence.

The nine delivery SQLite crash cuts also run in verified mode. Ten fresh processes
per case include inspection, denied/stale policy, changed source, a missing external
evidence target, cross-wired external Run evidence, recovery and replay. Source,
evidence catalog and evidence metadata live in the independent Host SQLite DB;
the Runtime APIs never write/repair them. Test Host explicitly restores its
external view after each refusal. One original Inspect invocation and one Run
survive; a post-send crash is reconciled against the test destination without a
second send request. This qualifies the callback seam and local recovery ordering,
not a production external provider's quality or remote consistency guarantees.

Additional production adapters, cancellation supervision, amendment/replanning and broader
Job scheduling remain open. M1 is not complete. Real-model quality/cost/latency
stays in issue #278 and no paid model or real external messaging is used here.
