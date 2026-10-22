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
The example uses memory stores and fixed child thread names for one exclusive invocation;
it does not persist a parent task, conversation, supervision audit or recovery record.

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
before the final model turn. Delivery checks Execution status, Runtime result code,
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
The returned phase/error identifies the stop boundary; production audit integrations
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
