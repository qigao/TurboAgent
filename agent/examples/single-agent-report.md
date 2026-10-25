# One Agent application over model workers

`single_agent_report.c` composes the existing Router APIs into one bounded Host task:
one goal and Host context, two independent model drafts, an explicit aggregation
model, one approved Host tool call, and one validated document delivery. It is a
working application example for [#213](https://github.com/qigao/TurboAgent/issues/213),
not a new installed Agent/Session API or a complete general assistant product.

`single_agent_report.h` contains the actual Host composition used by the executable
and `test_single_agent_report`. All model calls use existing Execution/Runtime owners,
one caller-owned bounded pool, and the same Host-supplied parent run link. The helper
does not create or validate the parent Runtime record: an embedding application must
supply its real lineage. Child run/checkpoint records remain in their existing stores.
By default the example uses memory stores and fixed child thread names for one
exclusive invocation. The optional Host trace below reserves a report namespace and
persists canonical worker stores and handoff metadata. It does not restore a parent
task, conversation, policy grant or live execution handle after restart.

## Execution and ownership

The Host reserves the sum of two worker request budget ceilings, one aggregation
ceiling and the two-turn tool-loop ceiling before admission. These are estimates,
not actual billing or a hard spend limit. One monotonic deadline covers all stages,
including time spent reviewing. Each admitted segment receives only the remaining
time through the Router's relative-timeout admission API; approval never resets it.
The Host also rejects a late review decision before resume admission. This is
cooperative execution control, not a precise wall-clock stop guarantee. The local
synchronous example uses that deadline as
its heartbeat/liveness bound and polls in short wait slices without inventing provider
heartbeats. Distributed workers need their own authenticated liveness integration.

Two disjoint Router endpoint pools use the same authority Agent: the analysis pool
contains two EXECUTE draft endpoints and one PLAN endpoint; the tool pool contains
one EXECUTE endpoint with TOOL_PROPOSALS. This prevents a cheaper tool model from
being chosen as a drafting worker. The Host releases all analysis handles before
entering the tool pool, so there are no overlapping callbacks or cross-Router claims
of lane exclusion. Concurrent outside use of these owners is unsupported.

Both workers receive one user-data context packet containing `host_goal`, `source_id`,
`source_revision` and `source_text`. Their different trusted Agent instructions define
drafting and critical review. The existing group checks canonical terminal results
and constructs provenance-bearing evidence for the aggregation model. After aggregation,
the Host forwards the original context with the actual aggregation run id and text as
another user-data packet. Model text never becomes system instructions, an approval,
an endpoint choice, or the source catalog. Every handoff is bounded and checked;
the original goal/source and each aggregation draft are at most 64 KiB, and serialized
packets at most 1 MiB, without truncation.

Each model Agent also installs a Host response guard over the original parsed Provider
response: reject embedded NUL in strings/keys, depth above 64, or more than 65,536 JSON
values. Provider text normalization uses C strings, so a check only on the normalized
canonical event would miss an already shortened raw value. Guard rejection makes the
execution fail before any handoff or tool effect. This is an explicit example policy,
not a global change to Provider APIs or every Agent's response semantics.

The tool model may propose exactly one `document_render` call. A Host guard permits
only that registered tool and demands an exact call/name/argument approval. The review
callback runs on the serialized Host after all current executions are terminal,
borrowing summary/state only until it returns. Approval resumes the canonical checkpoint;
rejecting or aborting performs no tool effect. Only one accepted approval is supported
by this example; another approval request or failure stops instead of replanning.
The synchronous review callback (including the executable's terminal input) can wait
past the deadline; it installs no background UI timer. A late response is rejected
before resume. A production UI should bound its own wait and support cancellation.
The original Host authority generation spans the whole task, and Router checks each
child generation. Policy replacement remains quiescent. A same-grant replacement
at the terminal review boundary invalidates further execution; no runtime callback changes policy.

The normal Host ToolNode/executor/Journal performs dispatch and validates strict results
before the final model turn. Ordinary ToolExecutor invokes the text/context handler;
the existing renderer originally registers a JSON value handler. This example explicitly
adds a bounded text/context adapter that parses arguments, calls that same renderer,
and serializes the owned result into the executor's malloc/free output contract.
It does not add an implicit conversion to generic RuntimeTools entry points. Existing
executor argument/output limits also apply, independently of the renderer's domain limits.
Delivery checks Execution status, Runtime result code,
canonical completion/lineage, failure state, pending tools and the loop reason. It then
requires the actual canonical tool-result receipt, validates the renderer's schema,
and returns its Markdown. A model saying the task is complete cannot substitute for a
receipt. `reference_identity_only` verifies the selected source identity, not factual
support, source freshness, model quality or completion of an engineering task.

On failure, every accepted handle is cancelled, waited to actual terminal and released,
including a first worker whose second submission failed. No fallback, unknown-effect
retry or detached cleanup is installed. A noncooperative transport can block draining
past the application deadline; this example cannot forcibly stop it or safely destroy
dependencies while it is running. Production isolation/reconciliation remains separate.
The returned phase/error identifies the stop boundary; unaudited integrations
must additionally preserve inspection/failure evidence, especially before a Runtime
failure has committed a run/checkpoint.

## Build and run

Use the existing SDK configure/build presets and build the configured graph normally.
`BUILD_EXAMPLES=ON` creates `single_agent_report` through the Agent example glob.
Native `BUILD_TESTS=ON` builds it even when examples are off, and the existing CTest
suite runs the offline `test_single_agent_report` integration scenarios. No live model
or paid request is used by those tests. For a configured Linux CI preset, for example:

```sh
cmake --preset ci-linux-release
cmake --build --preset ci-linux-release
ctest --preset ci-linux-release --output-on-failure -R 'test_single_agent_report|test_turbo_agent_router'
```

For a deliberate live invocation, configure the ordinary `OPENAI_*` provider settings
and four explicit model names: `REPORT_A_MODEL`, `REPORT_B_MODEL`, `REPORT_PLAN_MODEL`,
`REPORT_EXEC_MODEL`. Each also requires `REPORT_<A|B|PLAN|EXEC>_INPUT_PRICE` and
`REPORT_<A|B|PLAN|EXEC>_OUTPUT_PRICE`: decimal micro-USD per million tokens, 0..1e9,
from a Host-reviewed price snapshot. No model names or price recommendations are
baked in. The executable inherits one environment Provider configuration; an embedding
Host can configure each Agent independently, as the cross-Provider offline test does.

Run `single_agent_report <Host-parent-run-id> <goal> <brief-text>` from the configured
native runtime environment. The terminal displays the canonical review note and asks
for approval. The program writes the actual rendered Markdown to stdout after validation;
it does not save a file, send a message, modify a repository or export a document.
Its source catalog binds the provided brief to `brief @ host-v1` for this invocation;
a production Host must bind actual immutable source revisions. The executable uses
illustrative token estimates, four per-stage budget ceilings of 1,000,000 micro-USD
(including two workers), a 4,000,000 total estimate reservation, a 120-second whole-task
deadline, and one transport attempt. Review these Host settings and configure backend
output bounds before live use; token estimates do not enforce provider limits.

### Optional Session memory and knowledge

The executable also accepts four trailing arguments: `<knowledge-db> <memory-root>
<namespace> <key>`. In this mode explicitly configure `REPORT_KNOWLEDGE_QUERY`,
`REPORT_KNOWLEDGE_KIND` and `REPORT_KNOWLEDGE_URI_PREFIX`. It creates a runtime-only
context Session over the normal file-memory and SQLite constructors (which can
initialize directories/schema), selects that exact memory key and up to four scoped
knowledge matches, then prepares context before any model admission. Required missing
data, read errors, invalid data or bounds stop the application. No model writes memory
or chooses another source. The default four-argument invocation is unchanged.

The additive [task-context input API](../docs/task-context-input.md) copies one checked
user-data packet and passes it unchanged through workers, aggregation and tool input.
Combined payloads are limited to 1 MiB. The rendering catalog distinguishes the manual
`brief` packet from the `context` packet, both tagged `host-v1` for this invocation;
these aliases do not assert document hashes or factual support. Session options can
select up to eight memory keys and eight knowledge matches in embedding applications.
No generic Retriever, conversation-history selection or automatic memory write is added.

### Optional persistent Host trace

Set both REPORT_AUDIT_DIR and REPORT_AUDIT_ID to enable the audited path. The directory
must already exist; the id is a Host-chosen 1..32-byte ASCII letter/digit/underscore/
hyphen namespace. Missing pairs, empty/invalid ids and oversized directory strings
are rejected before model work. The CLI opens bounded SQLite stores without a memory
fallback: host.db for audit metadata, and draft.db, review.db, synthesis.db, delivery.db
for the corresponding canonical endpoint Runtime stores. Their native exclusive-owner
locks require one application owner at a time. Canonical worker databases contain
the actual inputs, selected context and results; Host audit rows contain metadata only.
An embedding Host may choose other backends but must preserve independent store owners
and serialize all control/store use.

The example-private config has paired audit_runtime/audit_id fields. Existing callers
that leave both null retain the original path. The store is borrowed synchronously
through drain/return, and neither workers nor the returned Markdown retain it.
Keep the config, audit id, input strings and endpoint/store bindings immutable and
alive through the call, including its Host review callback.
No installed API, public struct ABI or existing Router/Runtime/Journal schema changes.

After input/context and budget validation, the Host atomically reserves report_id
in report_dispatch_manifests before any admission. The immutable version-1 manifest
records parent identity, origin monotonic start/deadline and deterministic phase ids,
without goal, brief, answer or tool data. An existing id is EALREADY even for the same
goal. A manifest can survive with no accepted worker; it is prospective admission
evidence, not acknowledgement of task completion. Reusing it after restart does not
resume work. Choose a new id only for an explicit new operation under current authority.
Do not turn an unobserved old outcome into an automatic retry.

For a report id named demo, the successful path acknowledges these rows:

| Key | Collection / boundary |
| --- | --- |
| demo | report_dispatch_manifests; before all model admission. |
| demo-worker-a, demo-worker-b | router_admission_intents; before each worker enqueue. |
| demo-fan-in | router_group_observations; before publishing worker evidence to the aggregator. |
| demo-aggregate | router_admission_intents; before aggregator enqueue. |
| demo-aggregate-final | router_observations; before moving the aggregator result. |
| demo-execute | router_admission_intents; before initial tool-loop enqueue. |
| demo-pending | router_observations; before exposing the pending checkpoint to the Host review callback. |
| demo-review | router_review_intents; explicit approve/reject before resume enqueue. |
| demo-execute-final | router_observations; before taking final results, or after terminal rejection. |
| demo-ready | report_delivery_observations; after receipt validation, before returning Markdown. |

The group id and child thread names also use this bounded prefix, separating explicit
new reports from old heads and tool-call identities. The manifest's phase_ids maps the
known suffixes to exact keys. Reconcile these records with the original endpoint/store
binding and exact Runtime run/checkpoint anchors, not an arbitrary newer thread head.
Origin monotonic timestamps cannot renew leases in a new process.

Every persistence error, including commit with lost acknowledgement, stops forward
progress and withholds output. Earlier accepted tasks are still cancelled/drained/
released under the existing ownership contract. There is no get/list/put fallback,
overwrite or retry. Explicitly inspect a possible surviving record after error.
Synchronous persistence consumes the original Host deadline. The Host checks it
again before displaying a pending review and before/after the final ready write;
a late acknowledgement may leave a ready record while output remains withheld.
Failure can leave an incomplete trace prefix; absent terminal observations do not prove
that work never ran or an effect never occurred. Controlled operational stops are
recorded as described below; the trace is not a complete automatic lifecycle audit.

The ready row links the final task/current Execution, Runtime run and terminal
observation, with reference_identity_only verification. Its semantics are
pre_report_delivery_observation. It contains no Markdown, delivery acknowledgement,
result copy or recovery grant. The same ready record may survive before API return,
after return or after external delivery. A write error there still withholds the
owned candidate even if the record committed. No cross-system exactly-once delivery,
content-factuality or source-freshness claim is made.

Offline tests inject space failure and lost acknowledgement at all eleven writes.
Eight five-process cases cut before/after the manifest, after cohort capture, after
review-intent commit, before/after ready commit, after API return and after explicit
application delivery. They require exact exit 86 and null-output boundary evidence.
Independent new processes reopen all stores, compare original canonical run/checkpoint
references and model/review/render/handoff/delivery counters, reject the old report id,
require current delegation and complete a distinct healthy report once. The renderer
retains its READ contract: its counter is fixture observation, not write-effect recovery
qualification. These controlled cuts retain OS/cache and do not qualify power loss.

### Operational stops after drain

Once the manifest was acknowledged, an ordinary execution/domain failure, timeout,
review rejection/abort or Host-authority invalidation cancels and waits every retained
handle to actual terminal before releasing it. Group wait only establishes the terminal
barrier: a fresh group poll checks execution/domain success before the fan-in audit gate.
After drain, the audited path writes stop-worker-a/stop-worker-b/stop-aggregate/
stop-execute terminal observations for the handles whose results it still owns.
These keys also use the report prefix and the existing router_observations schema.
If a segment's result already moved, it links the acknowledged aggregate-final,
pending or execute-final observation with before_result_transfer semantics instead.
It never fabricates a new canonical result after transfer. The stop entry separately
records the current terminal status, result-taken, loss and approval-pending flags;
for example, cancellation clears the Router's approval-pending flag on an aborted
review. The original Runtime checkpoint can still require review; the stop record
neither durably rejects that checkpoint nor authorizes its resumption.

Only after those references are available does one immutable report_stop_observations
row at demo-stop capture post_drain_report_stop_observation semantics: report/parent,
the original operation phase/code, origin monotonic observation time, acknowledged
gate ids and retained task/Execution/thread identities. Previously released workers
or aggregators are represented by their acknowledged cohort/terminal gate ids.
The row is bounded to 16 KiB and contains no prompts, answers, raw states or tool data.
The manifest maps these prospective cleanup keys along with the normal phase ids.
Stopping and recording cleanup may outlast the original deadline; this never extends
admission, approves a pending operation or publishes a late delivery.

If any normal persistence gate returns an error, no stop audit is attempted. This
includes admission/review API errors whose public return cannot distinguish a
pre-write rejection from a committed write with lost acknowledgement. An unacknowledged
manifest or EALREADY namespace never permits appending a stop to an older report.
The possible prefix remains for explicit inspection; there is no read/list/put fallback.
Cleanup audit itself stops at its first error or conflicting id, without overwrite,
retry or further writes. Accepted owners are released even then. Independent terminal
observations can therefore survive without the report stop row.

Successful stop auditing preserves the original return code/phase. A cleanup audit
error instead returns that error at stop-audit and withholds output. The optional
example-private diagnostics output preserves operation_code/operation_phase alongside
stop_audit_attempted/stop_audit_code; it is reset for each invocation, with the original
stop fields populated on the cleanup path. The CLI supplies it and reports both errors.
Caller-owned config, diagnostic storage and backend bindings remain alive through drain
and return. No installed API or existing Router/Runtime/Journal schema is changed.

Six additional five-process SQLite cases fail the actual aggregation provider, cut
before/after its post-drain terminal observation, before/after the stop row, after
error return and after a committed stop write with a lost acknowledgement. New processes
check exact admission/observation identities, canonical Run anchors when present,
persisted stop phase/code and independent failed-return/model/tool/delivery counters.
The old report id cannot run again; current-policy denial and a distinct healthy report
are still required. These tests qualify the specified boundaries, not arbitrary crashes,
complete lifecycle/error coverage, external-effect certainty or delivery recovery.

## Two application routes

The general Agent route can use this composition for source-based reports and document
tasks, supplying context from existing Session knowledge/retrieval/memory facilities.
The engineering workbench route can use the same goal/context/evidence/approval pattern
for repository analysis, implementation proposals and risk review. It must replace the
renderer with reviewed engineering tools and add domain acceptance based on actual
build/test/diff evidence; this report example does not modify code or prove tests passed.
Neither route requires a second queue, result database or scheduler. Multi-turn context
selection, durable parent-task progress, UI integration, semantic verification and safe
recovery still need application-specific implementation. Rollback is simply to stop
using the example; installed APIs and existing callers are unchanged.
