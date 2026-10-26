# Verified Host acceptance for bound Inspect Jobs

Status: implemented acceptance slice above the existing Job/Harness execution
adapter. This is an explicit Host decision, not automatic assessment of an
acceptance criterion and not an external delivery receipt. The public C11 API is
`turbo_agent_job_acceptance.h`, owned by Harness.

## Authority and exact joins

Both record and read operations hold Harness submission exclusion and freshly
compile the persisted Job proposal under the caller's compiler restrictions
intersected with current Host grants. The compilation/result-contract gate is
shared with ordinary compiled Inspect execution. No tool/model callback, graph
execution or new Run is started by review. Host must serialize Job access, keep
policy/registry/code stable until return. The opt-in [verified constructor](job-host-verifier.md)
requires Host source/evidence callbacks and rechecks them on every operation;
existing constructors keep manual source/evidence verification preconditions.

A candidate is the execution-reference JSON returned by the bound Job adapter.
It is an observation, not an authorization token: the review API reads the actual
records and refuses missing, invalid, pending, stale or cross-wired evidence.

| Evidence | Required relationship |
| --- | --- |
| Job descriptor/history | Exact Job ID/head/proposal version, bound thread and attempt |
| Current admission | Exact source-derived plan hash, certificate, result schema, effects, current generation |
| Canonical compiled attempt, schema 2 | Derived length-prefixed key; committed, tool OK, typed result validates under the current admitted tool |
| Compiled journal, schema 1 | Exact execution key and attempt reference; committed, matching authority/certificate, no result payload |
| Actual Runtime Run | Exact ID/thread, completed Inspect graph, persisted state_snapshot contains that compiled execution/attempt identity and certificate |

A normal completed Run need not have a Checkpoint. Its persisted `state_snapshot`
already binds the compiled execution metadata to the real Run ID. Review checks
that snapshot instead of assuming every graph edge produced a durable checkpoint.
The three graph phases remain admission/start/execution, while the compiled
attempt and journal are the pre-callback durable gates. These semantics do not
add a generic per-edge checkpoint guarantee to Runtime.

The current 64-bit plan hash is an identity aid, not a security seal. The complete
certificate/schema/effect/current-grant checks remain mandatory. Records are in
the Host trust domain; this is not tamper-proof storage or a distributed fence.

## One immutable Host decision

Collection `agent_job_acceptance`, keyed by Job ID, contains one schema 1 record:

| Field | Meaning |
| --- | --- |
| schema_version, kind | 1, host_job_acceptance |
| job_id, job_revision, proposal_version | Exact immutable Job binding being reviewed |
| decision | accepted or rejected, explicitly chosen by Host |
| evidence_ref | Host evidence identifier, 1..1024 bytes; not fetched/evaluated by this API |
| execution_reference | The validated 13-field metadata reference; no typed result |

Publication is a single create-if-absent, with no put fallback, retry or readback.
All fallible record construction occurs before publication. A second decision,
even identical or contradictory, returns EALREADY; v1 offers no overwrite or
amendment. A correction/replan currently requires a new Job identity. The source
Job v1 schema/history remains unchanged and its state remains attempt_bound;
applications compose this separate verified decision into their presentation.

Only a valid completed typed Inspect result can be reviewed by this slice. Host
may reject it because quality/criteria were not met; rejection is not a fabricated
tool failure. The API never infers acceptance from callback success, updates
criteria, issues a grant, redispatches an unresolved effect or sends a delivery.

## Failure, recovery and policy

Missing/backend statuses remain distinct; schema versions not understood return
ENOTSUP, malformed or mismatched records EPROTO, and STARTED/unknown attempts
EBUSY. Revocation or same-grant generation replacement prevents current delivery
of a decision/reference, while the historical record remains unchanged.

Failed publication returns its original backend status and NULL output; the record
may nevertheless have committed. Host explicitly reads/revalidates to reconcile.
If the write never occurred, Host may explicitly submit its original decision and
evidence again after inspecting absence. No automatic decision reconstruction or
tool replay occurs. There is no transaction spanning policy, Job, tool result,
Run, review, or external systems; backend guarantees and quiescent Host ownership
still apply. Unknown versions are never migrated or rewritten in place.

Unit coverage includes typed-result corruption, every reference identity, cross-
wired completed Runs, pending attempts, canonical read errors, decision duplicates,
Host quality rejection, unsupported publication, lost acknowledgements, future
schemas and policy invalidation. Four SQLite crash cases cut before/after atomic
publication, after a returned error, and after acknowledged acceptance. Seven
fresh processes per case initialize, crash, inspect, deny, replace generation,
recover and read again. An independent Host-input/counter database stores only
reference metadata. Every recovery keeps one original tool invocation and one
Runtime Run; external delivery count stays zero.

Acceptance evidence remains Host-authored. [Delivery intent/receipt metadata](job-delivery.md)
now composes this decision with explicit Host reconciliation, without sending.
The [Host verification seam](job-host-verifier.md) now enforces installed source
and external evidence callbacks; production adapters still require qualification.
Amendment/replanning,
production delivery connectors and cancellation supervision remain future product work. Real-model quality and
cost qualification stays independently tracked in issue #278. M1 is not complete.
