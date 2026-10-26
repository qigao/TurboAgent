# Host delivery intent and receipt reconciliation

Status: implemented metadata slice above [verified Host acceptance](job-acceptance.md).
The C11 API is `turbo_agent_job_delivery.h`, owned by Harness. It persists one
immutable intent and one optional Host-attested successful receipt per bound Job.
It does not send, query a remote service, execute a tool/model, or create a Run.

## Authority and ownership

Every operation holds Harness submission exclusion, prepares a fresh proof from
the frozen proposal/current Host admission, and revalidates the existing Host
acceptance. That shared verification checks the committed typed canonical result,
compiled journal, actual completed Run and its persisted execution snapshot.
Only an explicit accepted decision permits delivery metadata; rejected returns
EPERM. An acceptance record is not a cached authorization token. Current policy
generation must match, including after replacement with identical grants.

Host serializes Job/delivery operations and keeps policy, registry and code
quiescent through return. This is not distributed ownership or an atomic fence
spanning policy, storage and a destination. External outbound authorization,
source freshness, destination/payload validity and receipt verification remain
Host responsibilities. The API does not evaluate identifiers or dereference URLs.
The [verified constructor](job-host-verifier.md) installs mandatory Host source
and acceptance-evidence callbacks, revalidated on every delivery operation.
Destination/payload and remote receipt verification remain delivery Host duties.

## Persistent format

All records use schema 1. Both collections are keyed by Job ID and atomically
created once; there is no mutable delivery phase or copied typed result.

| Collection | Fields |
| --- | --- |
| `agent_job_delivery_intents` | schema_version, kind=`job_delivery_intent`, job_id, job_revision, proposal_version, delivery_id, idempotency_key, destination_ref, payload_ref |
| `agent_job_delivery_receipts` | schema_version, kind=`host_job_delivery_receipt`, job_id, job_revision, delivery_id, idempotency_key, receipt_ref |

`delivery_id` has 1..64 ASCII letters/digits/underscore/hyphen. Host destination,
payload and receipt references have 1..1024 bytes with no embedded NUL. The key is
deterministically `<job_id>:<delivery_id>`; Host must namespace it across separate
stores/tenants and use that same key at its separately authorized destination.
The Job binding and acceptance are immutable and identify the canonical result;
the intent records which Host-prepared payload and destination were chosen.
Receipt identity must exactly join the intent. Arbitrary backend tampering is
outside the trust model; these records are not cryptographic seals.

`prepare_job_delivery_json` returns the independent intent after acknowledgement.
`record_job_delivery_receipt_json` returns the independent receipt after
acknowledgement. `get_job_delivery_json` returns a schema-1 `job_delivery_state`
object with `intent` and `status=outcome_unknown`, or `status=delivered` with both
`intent` and `receipt`. The receipt field is absent for unknown outcome.
Delivered means Host recorded a verified remote acknowledgement; it is an
attestation supplied by Host, not an independently observed network fact.

## Publication and explicit Host recovery

| Durable observation | Required Host behavior |
| --- | --- |
| No intent and no receipt | Decide whether to prepare the original intent before any external send |
| Intent, no receipt | Outcome unknown: may be unsent, in flight, failed, or remotely committed; query the destination by the stable key or require explicit Host review |
| Intent and matching receipt | Expose the existing delivery record under current admission; do not resend |
| Receipt without intent | EPROTO; preserve evidence and do not fabricate/repair an intent |

An intent is neither proof of sending nor an external execution grant. Persist or
explicitly reconcile it before a Host initiates delivery. After a remote success,
Host verifies the acknowledgement matches that intent/key/payload and records its
receipt reference. If the destination cannot reconcile an unknown outcome, this
API cannot safely choose a resend; it makes no exactly-once network guarantee.
There is no callback or automatic delivery/retry path in Runtime or Harness.

Both publications use one create-if-absent. All record allocation occurs before
publication. Duplicate records, including identical submissions, return EALREADY;
there is no get/put fallback, overwrite, automatic retry or error readback. A
failed acknowledgement returns the original backend status and NULL output even
if the record committed. Host explicitly calls get() to reconcile before choosing
another operation. No intent/receipt amendments or multi-destination fan-out are
offered in v1; a changed delivery requires a new Job identity.

Reads verify exact record shapes and joins. Unknown schema returns ENOTSUP,
malformed/mismatched records EPROTO, pending canonical attempts EBUSY, missing
intent/evidence ENOENT, and backend read errors remain distinct. Revocation or
stale generation withholds current output without rewriting historical records.
No transaction spans acceptance, intent, destination and receipt. A crash in any
gap therefore requires the explicit Host reconciliation above.

Older binaries ignore these additive collections and cannot expose their delivery
view; rollback must retain the records. Unknown versions are never migrated,
deleted or rewritten in place. Job v1 history and attempt_bound state are unchanged.

## Qualification and remaining scope

Unit coverage includes explicit quality rejection, missing acceptance/intent,
canonical evidence revalidation, each intent/receipt identity, orphan receipts,
future schema, policy replacement, independent read failures, publication errors,
atomic claim competition, lost acknowledgements, exclusion and immutable records.

Nine real SQLite process cases cut before/after intent publication, returned
intent errors/acknowledgements, after simulated remote send, and before/after
receipt publication plus returned receipt errors/acknowledgements. Seven fresh
processes per case initialize, crash, inspect, deny, replace generation, recover
and replay. A test-only destination in a separate SQLite database holds the
idempotency-key/payload/destination acknowledgement and independent send/invocation
counters. Recovery queries that destination before Host chooses whether to send;
post-send crashes preserve one send request, one delivery, one original Inspect
callback and one Runtime Run. No real external message is sent by qualification.
This tests Host composition and local durable gaps, not a production connector's
remote idempotency or transactional guarantees.

The source/evidence [Host verification seam](job-host-verifier.md) is available;
production adapters and their external consistency qualification, review amendment/replanning,
cancellation supervision, production delivery connectors and broader Job scheduling
remain open. M1 is not complete. Real-model quality/cost/latency remains #278.
