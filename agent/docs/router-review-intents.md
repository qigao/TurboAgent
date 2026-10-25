# Review-resume intents

`turbo_agent_router_task_review_with_intent()` adds an optional write-ahead record
to one explicit approve/reject resume of a **live** Router tool-loop task. It reuses
the ordinary exact-checkpoint review command, original deadline and bounded Execution
pool. `turbo_agent_router_load_review_intent()` reads one record after restart without
constructing an Agent or starting work. Existing review APIs remain available without
automatic recording; the Host chooses which path to use.

## Identity and ownership

Before enqueue, the new path checks the source checkpoint and Run ids, thread and
Host-parent identity against bounded metadata retained from the actual terminal
Execution. It then atomically creates a record in `router_review_intents` using a
Host-known unique 1..64-byte ASCII letter/digit/underscore/hyphen `review_id`.
The Host Runtime/store must be independent of every endpoint Runtime/store. It is
borrowed only for this synchronous call; the private gate context is cleared before
enqueue and no worker retains that store.

| Record fields | Meaning |
| --- | --- |
| `task_id` | Stable first Execution UUID for the live task. |
| `execution_id`, `previous_execution_id` | Prospective new segment UUID and the actual previous terminal segment UUID. |
| `admission_id` | Initial Host admission id when present; null for a legacy initial submit. |
| `source_run_id`, `source_checkpoint_id` | Exact canonical source references, independent of a later thread head. |
| `decision`, `review_segment` | Explicit `approve`/`reject` intent and next accepted-segment ordinal. An unqueued attempt can share that ordinal with a later explicit attempt. |
| `endpoint`, `model`, `provider`, `thread_id`, `parent_run_id` | Selected route and bounded Host lineage metadata. |
| `model_turns`, `max_model_turns`, clocks and heartbeat bound | Original task limits and origin-process timing; no deadline or model-turn budget renewal. |

Version 1 also records its schema, Host review id, UTC observation, `pre_review_dispatch_intent`
semantics and `origin_process_monotonic` clock domain. It stores no tool name/arguments,
tool result, answer, policy grant or resumable handle. The exact tool/call/arguments
remain owned by the canonical checkpoint and ordinary Runtime review command. Caller
result copies and loaded metadata cannot modify that approval. A record is not another
result owner, a full payload fingerprint or cryptographic authentication of a backend.

## Error boundaries

All existing running/terminal/pending-review, current authority, cancellation/loss,
deadline and 32 accepted-resume checks still apply. Host controls remain serialized;
policy/registry/hook replacement must remain quiescent across all running/queued work
and callbacks. Source records are trusted canonical data under exclusive endpoint
ownership; the identity reads and command preparation are not a cross-record transaction.

- Atomic write error, including successful commit followed by lost acknowledgement,
  prevents enqueue. Backend status is preserved; no implicit read/put/list fallback or
  retry occurs. Explicitly load the same id to reconcile a possible surviving record.
- Existing ids return EALREADY, even if the new decision or task differs. This is a
  conflict, never acknowledgement or a grant. The prior record is not overwritten.
- Queue rejection after writing preserves the intent and consumes its id. The previous
  task result, checkpoint, review state and accepted-resume count remain available.
- A successful queue admission follows ordinary review ownership transfer: old unclaimed
  results are released, the stable task id stays fixed and the new segment has one result
  transfer. Time spent preparing/writing still consumes the original deadline.

New source-identity reads preserve backend statuses; errors from existing Runtime
command preparation retain the existing review error behavior. No failing path consumes
a prior approval/result or changes the canonical checkpoint. Rejection intent is also
written before dispatch; a rejected queue/write attempt does not make rejection sticky
on the original paused task. A Host can make a different explicit decision with a fresh
id after reconciliation, under current authority and the original deadline.

## Reconciliation

An intent records a **prospective** segment. Its UUID can belong to an Execution that
was never queued. After a crash it does not prove dispatch, callback entry, rejection,
completion or delivery. Its decision cannot revive a lost task handle or authorise a
new process to resume an old checkpoint. Loading is read-only and accepts no state
override. Existing explicit terminal observations can link an actual current segment
UUID to canonical Run/checkpoint evidence; missing observations remain unresolved.

Use the original endpoint-to-store binding and exact source references, not a newer
thread head or a replacement backend with the same endpoint label. Origin monotonic
numbers cannot renew a lease in another process. In particular, a STARTED Tool Journal
entry remains UNKNOWN even if a loaded intent says `approve`; do not retry that effect.
Any new work still requires a distinct explicit Host decision and current authority.

Records are immutable through this API, bounded to 16 KiB, model/provider to 1024 bytes,
run/checkpoint/parent ids to 128 and other ids to 64. The loader validates the complete
field set, types/NUL, identity relationships, segment/model-turn bounds and clocks, clears
output on every failure, and returns owned JSON on success. Invalid schema/identity is
EPROTO; oversized fields ENOBUFS. Backend reads/parsing may precede projection limits.
Store capacity/retention and atomic scope follow the existing backend. SQLite supplies
its committed-record contract; memory remains volatile and file creation is unsupported.

This covers explicit review admission, not automatic lifecycle-completion audit,
durable task resumption, remote takeover or real-model quality/cost acceptance. No
public struct layout, Router ABI version, initial-intent schema or Tool Journal schema
changes. #213 retains those remaining workstreams.

## Qualification

Tool-loop tests cover both decisions with/without initial intents, result preservation
on lost-ack/write/queue failure, reused ids across tasks and turns, consecutive segment
links, source Run/checkpoint mismatches and read errors, deadline during persistence,
store lifetime during a running callback, independent concurrent approvals and strict
metadata loading. The initial Router/admission suites remain regression gates.

Thirteen process cases cover both decisions before/after atomic writing, after lost-ack
return and queue rejection, before delivery and after terminal observation, plus an
approve-only cut after a separately committed tool effect but before callback return.
Each requires exact `_Exit(86)`, boundary and real pre-enqueue pool-counter evidence.
Independent processes reopen Host/worker/oracle SQLite stores, compare task/segment/
source references, canonical Journal/claim phases and model/effect/delivery counts.
The new Host denies ungranted work, rejects a reused review id, and explicitly completes
a different healthy operation once. That later Run advances the same thread head;
inspection still uses the original exact Run/checkpoint. The old STARTED effect is
left unresolved and never retried. These are controlled process cuts while OS/cache
remain alive, not physical power-loss qualification.
