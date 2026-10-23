# Native module separation

This implements the source/target, Provider, Harness, StateGraph, record-store,
durable-segment, and checkpoint/history separation milestones of
[issue #30](https://github.com/qigao/TurboAgent/issues/30). It also adopts the
provider-neutral crypto migration from [PR #70](https://github.com/qigao/TurboAgent/pull/70)
using the installed SDK's `cmeta_crypto.h` API. It does not merge that PR's
unrelated release-selection policy changes.

## Decision and ownership

The former `langchain` shared library combined graph execution, Agent state,
provider request construction, durable storage, and host protocol handling.
Renaming its alias would leave those build dependencies intact. Splitting every
file family immediately would instead introduce cycles through Agent private
state and Session/Execution completion hooks.

We extract the boundaries already supported by public contracts. Each source is
compiled into one owning library; no object or durable state is duplicated.

| Source owner | Exported target | Responsibility and dependencies |
|---|---|---|
| `runtime_core/` | `RuntimeCore` | Cancellation/deadline control; Salts Core |
| `runtime_data/` | `RuntimeData` | JSON value ownership, checked insertion, and borrowed-value copy helpers |
| `runtime_tools/` | `RuntimeTools` | Tool contracts, registry, schemas |
| `runtime/` | `Runtime` | Graph/checkpoint execution, StateGraph stores, durable records/segments, checkpoint selection/loading and raw history; only RuntimeCore/RuntimeData below it |
| `provider/` | `Provider` | Wire request encoding, prompt messages/templates, response/SSE conversion, and embedding helpers; Runtime and Salts Mustache only |
| `agent/src/` | `Agent` | Agent policy/workflow/state, App/Session and asynchronous submission, commands and inspection projections, provider callbacks and state projection, memory/context |
| `harness/src/` | `Harness` | Embedded Harness, host server, connection journal, and JSON-lines framing; public Agent API only |
| `agent_compiler/` | `AgentCompiler` | Existing compiler qualification; no Agent or LangChain dependency |
| `llm_sandbox/` | `WasmTools` | Existing bounded Wasm tool backend |

```text
Harness -> Agent -> Runtime -> RuntimeCore / RuntimeData
             +--> Provider -> Runtime
             +--> RuntimeTools
AgentCompiler --> RuntimeTools / RuntimeData / Salts CFlow
```

`Runtime` directly uses Salts `tstr_dup` instead of an Agent-private string
wrapper with identical behavior. Harness includes no Agent-private
header. Its retained connection journal remains separate from the Runtime Store;
App/Session/store, executor borrowing, cancellation, and drain ownership are
unchanged. Model calls still use the existing execution path and are not newly
compiler-qualified by this structural change.

## Migration acceptance

The physical Runtime/Provider/Agent/Harness boundaries are implemented. Agent's
durable facade intentionally retains state patches, command interpretation,
Agent trace conversion, and lineage/timeline/observability presentation. It uses
Runtime's record selection, checkpoint loading and history traversal; those
projections do not own a second checkpoint or execution store.

The checkpoint/history migration was qualified on Windows x64 Release with the
compatibility facade disabled: the full build (including examples), 67/67 CTest
tests, and installation passed. The subsequent explicit request to remove
LangChain retires the facade and its configuration switches. The public API
test's remaining module coverage now lives in Harness and includes each module's
public headers directly. Current validation results are recorded in the
[packaging qualification](../../packaging/nuget/README.md#langchain-removal).

| Issue #30 acceptance item | Evidence |
|---|---|
| Source/target ownership map | The ownership table above and each module's `CMakeLists.txt` |
| Agent/Harness public identity | Separate public headers, shared libraries and `TurboAgent::Agent` / `TurboAgent::Harness` exports |
| Compiler phases independent of LangChain | `agent_compiler/CMakeLists.txt` links RuntimeTools, RuntimeData, Salts DataBind and CFlow; its formal tests pass with the facade disabled |
| No duplicate runtime/session/checkpoint owner | Runtime owns graph checkpoints and the record-store handle; Agent owns Session/Execution; Harness uses public Session submission |
| Installed TurboAgent targets | Installed exports include RuntimeCore, RuntimeData, RuntimeTools, Runtime, Provider, Agent, Harness, AgentCompiler and WasmTools |
| Facade retirement | Explicitly requested after direct consumers migrated; library, header, wrapper symbols and wrapper-only tests removed |
| Direct consumers independent of LangChain | Tests and examples link the owning TurboAgent targets; module API coverage is retained in `harness/tests/test_turbo_agent_public_api.c` |

This completes the local source/target migration acceptance. It does not expand
the [compiler qualification boundary](../../agent_compiler/BOUNDARY.md), establish
cross-platform qualification, or publish a release. Native consumers must rebuild
and deploy the matching complete SDK because DLL symbol ownership has changed.
Reproducible commands and final default-profile results are recorded in the
[packaging qualification](../../packaging/nuget/README.md).

## Checkpoint and history boundary

`turbo_runtime_history.h` owns generic record/state queries, latest/pending/head
selection, checkpoint loading and raw event history. Existing public names and
header inclusion remain compatible. Agent resume/fork adapt command/state input
then call this boundary; Agent inspection views reuse its checkpoint sorting and
parent traversal. Explicit head publication is described below.

Selection preserves backend encounter order for tied timestamps. Latest selects
by `updated_at`; pending restricts status to interrupted. Head reads use the
published snapshot; only older threads without a head prefer pending then latest.
Cancelled/timed-out segments remain explicitly resumable but are not
newly added to the legacy pending query. State reads prefer run `state_snapshot`
and retain the documented checkpoint fallback for older records.

Checkpoint decoding borrows record metadata, while graph checkpoint construction
clones its input and optional state override. Returned record/state/event trees
are independently owned. Callers keep the store live and coordinate conflicting
mutations across multi-record queries; this adds no read transaction. The generic
history API exposes raw persisted events, leaving Agent trace/observer policy in
Agent. Run history sorts by sequence; checkpoint history follows ancestry and
emits root-first events.

Root lookup and history share one parent walker. Temporary visited-ID storage
detects repeated IDs and fails without partial output; missing referenced parents
and non-string links also fail. Traversal uses O(depth) ID storage, and history
retains O(depth) records until output assembly. Empty/missing/null parent links
terminate at the root, consistently with the former root lookup. No arbitrary
depth cap or second durable index is introduced. Sorted views clone records and
therefore use temporary storage proportional to the input; no speedup is claimed.

Checkpoint sequence/version/step decoding validates integral values before
converting to native sizes. Negative/fractional/out-of-range values now fail,
rather than truncating or risking an invalid conversion. Graph steps retain the
legacy zero default when absent. Valid schema-v3 checkpoints and persisted
fields are unchanged. The standalone Runtime tests cover fixed persisted records,
pending/latest differences, timestamp ties, old state fallback, override ownership,
history ordering, cycles/missing parents and malformed counters. Existing Agent
golden fixtures remain integration coverage for domain projections.

Consumers relink against the complete SDK; the Agent header forwards these
declarations to Runtime. Rollback restores the prior matching SDK/binaries;
no persisted-data conversion is required.

## Durable segment boundary

`turbo_runtime_segment.h` provides a versioned synchronous request for a start
state or an already loaded checkpoint, with explicit thread/run identity,
lineage, graph options, cancellation, and borrowed event callbacks. Runtime
captures the latest checkpoint and events, writes thread/checkpoint/run records,
and returns owned state and summary trees. The record store remains the only
backend owner. There is no global policy registration or additional execution
object. IDs and timestamps move to Runtime helpers, retaining the former ID
format and one shared atomic counter across Agent and Runtime callers.

Agent adapts its existing control/workflow snapshots and full command summary
through synchronous projection callbacks. Paired enter/leave callbacks preserve
the thread-local execution context: the adapter saves the enclosing context,
installs the child identity/cancellation token, and restores the saved context
on every path after entry. The existing tool-approval reset performed by context
set remains unchanged. The scope includes graph callbacks, projections, and
record persistence; no callback context is retained after execute returns.

Moving Agent state functions into Runtime was rejected because it would pull
review/replan/handoff policy into the lower layer. Supplying caller-owned
projection and scope functions keeps those rules in Agent. Generic Runtime
callers may omit projections: state is still persisted fully, control/workflow
metadata are empty objects, and the default summary contains execution identity,
status, checkpoint ID and result. Agent always supplies its existing projections,
so its serialized records, summary shape, available commands and failure markers
remain compatible. This does not qualify model execution through AgentCompiler.

Both request inputs and graph callback payloads are borrowed synchronously.
Projection output trees transfer to Runtime even on callback failure and must
not alias borrowed state. Request/graph/store mutation and destruction require
quiescence. Callers serialize conflicting thread/run operations. The graph keeps
its existing cancellation boundaries and event/checkpoint callback order.

Thread upsert precedes scope entry. An interrupted, cancelled or timed-out
checkpoint is persisted before run finalization; summary projection follows the
run write. There is no compound transaction: later failure leaves earlier writes
intact. Graph ERROR can return owned diagnostic state with -1; other failures
return NULL state/summary. Checked record-field insertion now rejects allocation
failure and frees partial trees instead of publishing incomplete records. Failed
snapshot cloning also fails the segment instead of omitting Agent projections.
Sequence/step values outside the exact range of the legacy JSON number fields
are rejected; oversized prior timestamps fail instead of being truncated.

The standalone segment test links only Runtime and covers projected checkpoint
round trips, resume/fork identity, forwarded events/checkpoints, generic empty
projections, cancellation/deadline, diagnostic error state, ABI rejection, scope
cleanup, and projection/write failures at each commit stage. Existing Agent
golden fixtures and subagent/session/tool tests cover the adapted policy path.
Consumers rebuild and deploy the complete SDK; rollback uses matching previous
binaries and requires no record format conversion.

## Record-store boundary

`turbo_runtime_store.h` exposes the existing store callback ABI, opaque
`turbo_agent_runtime_t`, constructors/destructor, and JSON record operations from
Runtime. Historical names remain source-compatible; the Agent public runtime
header includes this contract. The handle owns only the backend and its mutex.
Agent no longer sees its layout. Context, Inbox, and ToolExecutor use the public
record operations; Agent metadata timestamps remain an Agent utility.

The selected boundary moves the existing owner rather than wrapping it with a
second store or maintaining another checkpoint index. Moving the whole runtime
was rejected at this stage because summaries, review/replan commands, and state
projections still require Agent policy. Storage copies/serializes records but
does not interpret them; Agent retains collection schemas and execution state.

Creation copies callbacks and transfers backend ownership only on success.
Returned raw callback text uses the existing malloc/free ABI, including cleanup
on callback errors. JSON outputs are independent trees. A failed public get/list
clears its output slot, including invalid-argument rejection. Put preserves the
backend status; get/list normalize failure to -1. Both builtin list paths now
check array insertion and release partial results on allocation failure instead
of potentially reporting a partial list as successful.

All callbacks on one handle are serialized by its existing mutex. There is no
new queue, cache, transaction, retained handle, or automatic drain. Callbacks
must not reenter that handle; the caller stops and joins all users before
destroy, which frees the backend once. Multiple handles/processes and compound
read-modify-write sequences require external coordination. The memory backend
retains records until destruction and has no quota/eviction policy. The file
backend retains its trusted collection/id path-component contract and legacy
directory layout; this extraction adds no atomic-replace or crash-durability
guarantee. It does not alter persisted schemas, ID generation, or record ordering.

Five old `runtime_v1_{command,file_store,memory_store,observability,util}.c`
copies were excluded by CMake and not included by the compiled implementation.
They are removed; the extraction uses the active monolithic implementation as
its source. The remaining Agent runtime has one implementation of each command
and projection. Storage definitions are compiled only into Runtime.

The standalone store test links only Runtime and covers copied callback tables,
ownership on rejected/successful creation, independently owned JSON, collection
and filter semantics, error outputs, concurrent callback serialization, file
reopening, and corrupt records. Agent runtime/inbox/context/journal/session tests
remain the integration checks for persisted workflow and error behavior.
Consumers rebuild and deploy the complete SDK; rollback restores the previous
complete SDK and matching binaries without a data migration.

## StateGraph boundary

StateGraph, its memory/file snapshot stores, public headers, examples, and tests
belong to Runtime. Its only former Agent dependencies were string duplication
and JSON member construction. String duplication now uses Salts directly;
checked string/int64/clone setters belong to RuntimeData. Agent's private helper
entry points delegate to those setters and retain their existing 0/-1 results.
No Agent state projection, review command, or provider policy moves into Runtime.

The graph still owns its threads, runs, reducers, history, and pending sends.
Snapshot schema, identifiers, serialized fields, and restore/resume behavior are
unchanged; no persisted-data migration is required. Store implementation remains
included by the StateGraph translation unit, compiled once by Runtime. The
existing formal StateGraph tests now link Runtime directly and cover updates,
interrupt/resume, snapshot round trips, historical forks, child graphs, dynamic
sends, and both snapshot backends. RuntimeData tests cover independent copies,
replacement with exact int64 values, and rejected calls retaining borrowed inputs.

## Harness and Session submission boundary

`turbo_agent_session_execution.h` provides one versioned Session submit operation
for start/resume/fork. It keeps private Runtime option translation, summary
synchronization, inbox claim commit, and follow-up execution inside Agent. The
host receives an existing retained Execution handle and an optional terminal
notification. Exposing arbitrary completion hooks was rejected because it would
let a host bypass Session's consistency work or replace its result ownership.

The internal adapter allocates one completion context per accepted operation;
rejection frees it immediately and invokes no notification. Completion errors
become FAILED/result errors before terminal status is published. The worker
invokes the terminal callback once and frees the adapter without subsequently
accessing Session or graph. The callback may run before submit returns; the
successful submit path does not access the adapter after scheduling it.

Harness owns its App and per-execution graph as before. Its worker reference
keeps both alive if the caller releases its handles during a run. The existing
active slot and terminal-before-publication flag remain protected by its mutex;
the new public boundary does not add another Session state owner or queue.

`wait()` waits for result publication, not for completion of the terminal
notification. Callback user data must remain live until the callback returns;
pool drain or explicit callback synchronization provides that guarantee. The
Session submit caller must serialize Session operations; Harness supplies that
admission policy for its own handles. No new concurrent-store guarantee is made.

Tests directly exercise copied input, synchronized Session results, a terminal
callback held open after wait succeeds, invalid/full/shutdown rejection without
notification, and inbox commit failure observed by the terminal callback.
Harness tests additionally release the caller's App/execution handles during a
blocked model call and drain the executor to cover worker-owned resource cleanup.
Existing resume/fork, cancellation, deadline, Server, and inbox tests remain.

## Provider boundary

Provider now accepts `turbo_provider_format_t` and a borrowed
`turbo_provider_request_t` instead of Agent state. Agent projects history,
compacted context, effective instructions, previous-response identity, and
approved tool schemas. Provider constructs the wire fields and converts wire
responses/SSE into canonical model events. It performs no transport I/O and
owns no credentials, policy, registry, or persistent history.

Builtin singleton identities, environment selection, HTTP header callbacks, and
custom `turbo_model_provider_t` request callbacks remain in Agent. Existing
model-provider helpers delegate to Provider using an explicit builtin-to-format
mapping; custom callbacks are not replaced or globally registered. This avoids
reversing the dependency through Agent function pointers in a lower-level DLL.

Request strings/JSON are borrowed for one synchronous call. Provider clones the
projected arrays into its temporary request and returns an independently owned
serialization; the caller can then release all projections. This adds a transient
JSON copy compared with transferring projections directly into the request, in
exchange for a uniform failure/ownership contract. No performance improvement is
claimed. Failures release partial request state and leave output NULL. Stream
events remain borrowed for the callback and must be cloned to retain them.

Chat keeps `parallel_tool_calls` and `tool_choice=auto` where previously enabled;
compatible Chat omits those fields. Responses retains `previous_response_id` and
its structured-output shape. Anthropic retains system extraction and the existing
structured-output omission. Provider's standalone tests cover these differences,
borrowed-input lifetime, invalid schema/format, and response/SSE decoding; the
existing Agent tests continue to cover callback and state projection behavior.

## Compatibility, migration, and rollback

Session input construction lives in `turbo_agent_session_input.c`, separately
from Session lifecycle, execution completion, and durable-store adapters. It
uses public Session accessors; it neither copies the private Agent object nor
owns Session resources. Borrowed input messages are cloned once into the new
state. Already-owned history messages move into that state after validation.
Memory-enriched factories mutate only their newly owned state and destroy it
on load failure, without cloning the whole state at intermediate boundaries.

Request replay distinguishes provider context projection from full history.
Provider requests retain committed summaries and their event offsets; follow-up
input projects chronological history directly from the completed state without
consulting or changing the Agent's compaction state. Both paths reuse the same
event-to-message conversion. Returned JSON is independently owned, partial
construction is discarded on failure, and callers still serialize Session use
and bound retained history. Public signatures, message shapes, and persistence
formats remain unchanged; this is an internal ownership refactor with no data
migration or measured performance claim.

History projection borrows a quiescent source state and publishes only a complete,
independently owned message array. If `events` is present it must be an array;
each replayed entry must be an object with a string `kind`. Known model events
may omit `tool_calls`, but a present value must be an array. `output_text` may be
absent/null or a string; other types fail instead of silently losing content.
Unknown string event kinds remain outside conversation projection. Validation
failure frees the unpublished projection and returns the existing NULL/error
result to follow-up and provider-request builders without changing the source
or invoking a provider. No persisted representation, public signature, valid
message ordering or context-compaction offset changes are introduced.

Synchronous Session start/resume/fork and asynchronous submission share the
same Session completion operation: capture the resulting run/checkpoint IDs,
commit a bound inbox claim, then drain follow-ups up to the configured bound.
Session remains the only owner of these mutable facts. The execution adapter
borrows Session, graph, callbacks, and synchronous input through the call;
asynchronous submission retains its existing input-copy and executor-drain
contract. Calls on a Session remain serialized by the host. Completion errors
propagate without reporting success, and runtime failure skips completion.
Caller-requested outputs retain their existing ownership even on completion
failure; an omitted summary is local and freed before returning. Cancellation,
queue capacity, follow-up limits, and persisted records remain unchanged.
Synchronous option translation preserves its existing Runtime validation and
checkpoint-default behavior; the versioned asynchronous API keeps its stricter
admission checks. This removes repeated completion logic without changing either
public contract or moving private Session state into a shared struct.

High-level local text/message starts and sequential batches now live in
`turbo_agent_session_invoke.c`, using only public Session/State APIs. Result
extraction and cleanup live in `turbo_agent_session_result.c` and are shared
with the remote invoke adapter through a private header.
Streaming and non-streaming starts share input-state cleanup and preset dispatch;
the stack event filter remains borrowed only during synchronous execution.
Final-text extraction copies the selected string, while JSON conversion parses
the existing owned result state directly. Neither needs an intermediate clone of
the entire state. Conversion helpers release the result state once and preserve
the existing rule that invoke failure discards its returned summary without
rolling back Runtime records or Session ids. Batch results check JSON insertion
and transfer summary ownership only after successful attachment. They retain
sequential execution and per-item failure continuation. No public signatures,
stored formats, callback lifetime, or result ownership contracts change.

Remote text/message starts and the four invoke wrappers now live in
`turbo_agent_remote_session_invoke.c`. They use public RemoteSession accessors
and start APIs; cached ids and client ownership stay private to RemoteSession.
Remote JSON conversion no longer clones the complete state before parsing its
final answer. Both transports consume the owned result state once through the
shared result helpers and return independent text/JSON values. Admission remains
with each adapter: remote invoke requires a summary slot, while local invoke
continues to allow an omitted summary. Conversion failure still discards the
returned summary without undoing cached ids or completed/interrupted remote runs.
An execution-strategy abstraction was unnecessary because only result ownership
and conversion are shared. Existing CHttp loopback fixtures cover independent
output lifetime, malformed final JSON, interruption without a final answer,
remote execution errors and required output slots. No transport, public API,
wire format or persistent format changes are required; no performance claim is
made without measurement.

Remote durable-history replay and observer projection now live in
`turbo_agent_remote_session_observer.c`, using public RemoteSession queries and
client admission only. The opaque Session and cached ids stay in the lifecycle
owner. The observer reads a single owned index throughout synchronous delivery;
its callback envelopes clone their selected raw event once. Removing an extra
history-array clone and the terminal-record intermediate clone does not extend
borrowed lifetimes or grant callbacks ownership of the snapshot. The sink and
its user data remain caller-owned; synchronous observation never calls the sink's
cleanup function, and already delivered callbacks cannot be rolled back.

Remote projection intentionally differs from local history observation: unknown
events are skipped, `kind=observer` preserves custom/empty string types, and
trace/model/tool fields remain nested in `event`. Raw replay delivers all events.
Both paths reuse the existing trace-name mapping, but merging the full local and
remote projectors would change public behavior. Terminal synthesis selects the
pending run before the latest run, and its existing raw-type deduplication also
recognizes events filtered out by remote projection. These policies remain in
the remote adapter; no new transport strategy or generalized observer framework
is introduced. The public header now describes these existing differences and
callback ownership instead of claiming identical local/remote shapes.

CHttp loopback tests cover remote filtering/order/envelopes, copied callback
results after owner destruction, raw replay, interrupted/completed synthesis,
raw-type deduplication and invalid admission. Allocation failure cleanup is
reviewed but not injected. No API, wire or persisted-format migration is needed;
reverting the extraction must preserve these projection and lifetime contracts.

Session memory operations now live in `turbo_agent_session_memory.c` and borrow
the Session-owned store through its public getter. Store callbacks and canonical
record conversion remain with MemoryStore; state-layer construction remains with
State. The adapter preserves the raw `scope/path/text` persistence shape and
checks payload field insertion before calling the store. Query selection/error
handling is unchanged, including no list retry when an available query callback
fails. Context loading appends copied layers in query order; a later validation
failure retains earlier appended layers. The input factory instead destroys its
new state on load failure. No new cache, owner, cross-thread lifetime, API, or
stored format is introduced; hosts continue to serialize access to mutable state.

Local supervisor inbox/history queries, orchestration inspection, and inbox
override preparation live in `turbo_agent_session_inspect.c`, using only public
Session/State queries. Supervisor report projection is shared with RemoteClient
through `turbo_agent_supervisor_inspect.c`. Runtime returns an owned state tree; the
adapter reads it directly and clones selected subtrees only where the returned
report needs independent ownership. Override preparation mutates that private
tree and transfers it to the caller without persisting it. Owned snapshots and
timeline results transfer directly into reports through checked JSON insertion.
Session and Runtime remain the only state owners; queries require serialized
access and introduce no retained views or cross-thread state. Existing null/empty
fields and the optional timeline failure behavior remain unchanged. This is an
internal extraction with no public API or stored-format migration and no measured
performance claim.

The supervisor builder borrows one immutable state for the duration of its call,
returns an independent report, and owns no Session, client, query policy or cached
ids. Local and remote adapters each perform their existing state query and release
that result after projection. Remote supervisor construction now uses the same
checked JSON transfers as local construction, removing its whole-state and
snapshot clones; required selected-subtree copies preserve independent report
fields. Query/construction failure returns -1 without publishing a partial report.
Remote inbox/history reads likewise copy only their selected arrays. Two unused
Session-local supervisor/orchestration builders were removed after verifying
that the public remote wrappers delegate to RemoteClient. Active orchestration
query sequencing and its required timeline behavior remain unchanged.

Sharing the pure projection avoids a second field/ownership implementation without
introducing a transport strategy or exposing Session internals. Existing remote
loopback tests now also compare local/remote handoff projection, verify independent
subtrees after owner destruction, and cover empty fields and missing-thread errors.
No public signatures, stored formats or wire formats change. Allocation failure
cleanup is reviewed but not fault-injected; reverting the sharing would require
retaining checked writes and both adapters' existing query/ownership contracts.

Child inspect and parent-call metadata construction now share the private
`turbo_agent_child_inspect.c` builder across Session, RemoteSession and
RemoteClient. The adapters retain query admission, ordering and errors. Required
run/checkpoint/history/trace queries fail the report; an explicit checkpoint
selects history and trace before the run and is never retried against the run on
failure. Optional timeline/branch queries keep their existing behavior: local
Session queries Runtime separately; RemoteSession issues two independent index
queries through its public accessors; RemoteClient reads one index. None of these
queries infer a missing child thread from the fetched run or change cached ids.

The builder consumes an owned set of query results and clears it on success or
failure. It transfers children through checked JSON writes, emits explicit nulls
for absent optional fields and publishes only a complete report. The parent
metadata builder borrows the tool result during the call, copies nonempty strings
and consumes the child report only on successful assembly. Remote history, trace
and timeline results no longer need an additional whole-tree clone. RemoteClient
orchestration and multi-agent envelopes also check every child insertion before
relinquishing ownership.

Sharing only construction removes three copies of report shape/cleanup without
introducing a transport strategy, new state owner or query fallback. Public APIs,
field order, wire formats and persisted records stay unchanged. Existing local
child tests and CHttp loopback tests cover independent trees, owner destruction,
optional index failure, null metadata, checkpoint selection and failure cleanup.
Allocation failures have been reviewed but not injected; no performance benefit
has been measured. Reverting this internal extraction requires retaining checked
writes and each adapter's query policy; no data migration is needed.

Agent execution-summary construction, command descriptor/schema construction,
and interrupt metadata selection now live in `turbo_agent_runtime_summary.c`.
The command catalog remains the single source of descriptor data; Runtime keeps
generic execution-result serialization and persistence, while Agent owns its
review, handoff and recovery-command projections. Leaving these builders in the
execution adapter obscured unchecked JSON writes; moving Agent-specific policy
into generic Runtime would reverse the intended dependency direction.

All JSON writes performed by the extracted builders now check success. Locally
owned children transfer only on successful insertion; scalar temporaries are
released on failure, and a partial summary/descriptor is never published. The
summary callback fails with -1 after run persistence, so stored records are not
rolled back. Public signatures, field order, explicit nulls versus empty strings,
command order, schema fields, and interrupt classification remain unchanged.
In particular, the existing executor failure projection still supplies `step failed`
when there is no executor output; this extraction does not change that policy or
its allocation/error-reporting contract. Regression tests cover all parent-link
fields, independent output ownership, null/empty compatibility, interrupt priority,
and nested descriptors against existing fixtures. Allocation failure cleanup is
reviewed but not fault-injected. Reverting the extraction must retain the checked
writes to avoid restoring incomplete summaries and leaked children on failure.

Local and remote Session summary capture now share a checked identity commit in
Agent's internal utilities. Previously each string replacement freed the old id
even if copying the replacement failed, so a later allocation failure could
leave a mixed thread/run/checkpoint cache while reporting success. The new path
copies all required replacements first, frees temporary copies on failure, and
only then replaces the owned ids. Invalid input or allocation failure returns -1
without changing cached ids. This is an exclusive-owner commit, not a concurrent
atomic primitive. Existing admission remains unchanged: thread/run must be
strings (including empty strings), and missing/null/non-string/empty checkpoint
retains the previous checkpoint. No format, public signature, or dependency
change is required. Runtime/remote execution may already have committed before
summary capture fails; low-level output ownership remains unchanged, and no
execution rollback is attempted. Local completion propagates capture failure
before inbox commit or follow-up execution. Regression tests cover malformed
summaries, independent string ownership, and checkpoint retention; allocation
failure handling is reviewed but not fault-injected. Restoring the old helper
would reintroduce the partial-cache-update bug and is not a recommended rollback.

Local Session construction now checks every Session-owned string copy before
returning its handle. Previously a failed copy could silently lose a configured
thread, parent link, or retrieval filter. Partial construction remains unpublished
and uses the existing destructor for cleanup. A complete supplied RuntimeStore
retains its existing consume-on-attempt behavior; an incomplete supplied store
remains caller-owned while the existing memory fallback is used. MemoryStore
transfers at the final successful return, so all construction failures leave it
with the caller. KnowledgeStore and Retriever remain borrowed. Remote Session
likewise checks thread-id construction and frees only clients it created; a
supplied client remains borrowed. Remote start rejects thread-id copy failure
before submitting a request rather than accidentally requesting a new thread.
These are exclusive-owner initialization paths. Existing empty-value admission,
public signatures, and stored formats remain unchanged. Lifecycle regressions
cover store cleanup on success and Agent rejection, incomplete-store fallback,
independent string copies, empty values, and remote client ownership. Allocation
failure branches are reviewed, not fault-injected; reverting these checks would
restore successful construction with silently missing configuration.

Session preset graph factories now live in `turbo_agent_session_workflow.c`.
The Session struct stays private; only two internal const accessors expose its
persistent knowledge/retriever context configurations. These are borrowed
addresses, not snapshots: the installed nodes retain them for later execution.
Passing temporary configuration copies would leave dangling node user data.
Session owns copied query/filter/scope strings, borrows the external backends,
and outlives graph execution; callers destroy graphs before Session and release
backends afterward. Context configuration is now the single stored source for
backend pointers and limits, also used by diagnostics. The redundant backend and
limit fields have been removed from the private Session struct. Factories retain
existing graph/node names, edges, prerequisite checks and failure cleanup. No
public API or persisted-format change is needed, and the split is reversible
within Agent. Executable tests run the context nodes after factory return and
caller-string release, check retrieval filters/scope/limits and default query
selection, and verify backend ownership and missing-prerequisite rejection.

Live Session trace/observer registration now lives in
`turbo_agent_session_observer.c`, borrowing the owned Agent through the public
Session accessor. Successful registration transfers the copied sink's cleanup
obligation to Agent; failed registration frees only the bridge and leaves caller
user data untouched. Delivery remains synchronous on the execution thread, with
no new queue, worker, retention policy, or backpressure mechanism. Sink mutation
and owner destruction require quiescence. Each observer envelope owns its trace
copy until callback return; callers copy events they retain. Existing mapping,
unknown-name filtering, conversion-failure behavior, and release order remain
unchanged. Historical replay keeps its separate Runtime conversion because it
also handles non-trace and terminal records, with different field rules and sink
ownership. Trace-history configuration controls state retention independently of
live delivery. Ownership, rejected registration, retention toggles and transport
failure are covered by executable tests. Preset start/resume/fork wrappers now
live beside the synchronous execution dispatcher, retaining their temporary graph
through the full call and destroying it afterward. Graph factories continue to
borrow Session-owned configuration. No public signatures or stored formats change;
these moves can be reverted within Agent.

Session record/state queries, history replay/observation, and override preparation
now live in `turbo_agent_session_query.c`. This adapter uses only public Runtime
and Session accessors, leaving cached ids and Runtime ownership in Session.
History loading, replay, and observation share one source selector: explicit run,
explicit checkpoint, cached run, then cached checkpoint. It chooses once, never
retries a failed query, and does not refresh cached ids. This preserves the case
where a later completed run has no checkpoint and Session still caches an older
checkpoint. Explicit-only APIs such as checkpoint-context queries keep rejecting
empty ids; other checkpoint queries retain their existing defaults. Overrides
remain owned results and do not persist changes. Historical sinks are borrowed
synchronously, unlike registered live observers; callback event pointers expire
when the callback returns. Runtime remains responsible for event conversion and
terminal records, including partial delivery before a later read error. Keeping
these rules in an adapter avoids exposing private Session storage or creating a
second history engine. Signatures and persisted formats are unchanged; the split
can be reverted within Agent. Tests cover source precedence, query failure,
callback copies, and independent override ownership.

Child-run queries and inspection now live in `turbo_agent_session_child.c`,
using only public Session/State/Runtime interfaces. Session still owns Runtime;
the child-query adapter borrows it for synchronous calls and owns no cached
state. Runtime returns owned history, trace, and timeline trees, which move into
the report after checked insertion instead of being cloned again. Required
query or construction failures release all unpublished data. Existing field
names, null optional fields, and explicit-checkpoint precedence remain intact:
a checkpoint failure never retries by run id, while timeline and branch-tree
failures still produce null fields. `list_child_runs` stays with Session because
it resolves configured/current parent context. Sharing private Session storage
was unnecessary; this split needs no public API or data migration and can be
reverted inside Agent. Regression tests cover independent report lifetimes,
checkpoint selection, optional fields, and failure without partial output.

Session capability, tool-schema, and startup-diagnostic construction now lives
in `turbo_agent_session_diagnostics.c`. Session remains the owner of runtime and
configuration state; it lends a private view of diagnostic facts for one
synchronous call. The view carries no execution handles and is never cached.
This keeps the Session struct private while allowing report construction to be
tested through the existing public APIs. Each returned JSON tree owns its data,
and every subsequent query reads current registry facts. Report construction
checks JSON insertion failures and releases untransferred children. The owned
registry schema is transferred directly rather than cloned again. Report keys,
error ordering, provider selection, and the distinction between a completed
report and startup readiness remain unchanged. No persistence migration or new
public interface is required; the split can be reverted within Agent alone.

The [CLI host](../../cli/README.md) composes Harness and CodingTools without a
new agent execution engine. Its default workspace registry contains generated
IDL contracts for read/list/search/write/edit/command input. File operations use Salts
root capabilities; processes use Salts process ownership and bounded capture.
Patch/shell capability admission and non-idempotent journal behavior remain
Agent/RuntimeTools responsibilities. Shell execution has host privileges.

The additive Session follow-up builder projects full canonical model/tool
history before appending the new user message, then starts a fresh loop state.
Repeating start_text would omit history; appending to the old initial-input
array would put new users ahead of old model events. Reusing the existing
canonical projection avoids a separate CLI conversation format. This host
chooses explicit history/turn bounds and fails at the limit; it does not silently
discard history or reuse a prior run's compaction offset. It waits for executor
quiescence before projecting the next state. Optional persistent conversations
are described below; incremental token rendering and an approval UI remain
outside this CLI's current interface.

CLI turns now reuse Agent Workspace discovery and selection. The alternative
of parsing `AGENTS.md`/skill metadata in the CLI would duplicate existing path,
quota, and capability rules. The host prepares a fresh project memory-context
layer for each turn and retains its normal coding registry; skill declarations
are validated against that registry and the same read-only policy. Selection
does not grant extra tools or capabilities. The catalog is owned by the input
thread and refreshed only between runs. Preparation failure prevents submission;
a failed refresh does not execute with the old snapshot. The copied state owns
its instruction text before the temporary selection is destroyed. Removing this
CLI composition would restore the former host behavior without changing the
Workspace, Session, or persisted-state APIs.

Tool parameter contracts now use the SaltsUtils IDL pipeline in CodingTools.
The [workspace contract](../../coding_tools/contracts/workspace.schema) generates
the native DataBind codec and a private Mustache projection for the model-facing
JSON Schema. RuntimeTools still owns registration and execution contracts; it
does not depend on IDL compilation or DataBind. Generated code stays inside the
tool pack. See [ownership and build boundaries](../../coding_tools/README.md).

Native Plugin and Wasm projection remain SaltsUtils responsibilities. Generator
output is an ABI/adapter, not an implementation or permission grant. Future
backend integration must connect that generated boundary to the existing tool
policy, cancellation, quotas, and journal, rather than add another loader,
reflection table, Session, or agent loop.

The LangChain facade has been removed: `turbo_langchain.h`, `turbo_langchain_*`
symbols, and `TurboAgent::LangChain` are no longer provided. Consumers include
the owning module's headers and link Agent, Runtime, Provider or Harness directly.
For example, the former Agent facade wraps `turbo_agent_app_t`, so use
`turbo_agent_app.h` and its App API; the Session facade maps to
`turbo_agent_session.h`. This is an intentional source/API removal.

Runtime, Provider, Agent and Harness are mandatory parts of the agent build.
Remove `ENABLE_AGENT`, `ENABLE_LANGCHAIN` and `ENABLE_LANGCHAIN_COMPAT` from old
configuration commands. Clear their old cache entries once when reusing a build
tree: `cmake --preset win-release-user -UENABLE_AGENT -UENABLE_LANGCHAIN*`.
Chttp and SQLite are required by the core and by its installed package. Optional
tool backends retain their own switches. No alias or automatic fallback is
retained. Existing module APIs, error results, serialized
checkpoints, journal keys, and context hashes are unchanged.

Native consumers must rebuild and deploy the complete SDK because function
exports now live in separate libraries. Remove old LangChain libraries and its
header from existing SDK installations; CMake install does not delete retired
files automatically. To roll back, restore the previous complete SDK and matching
consumer binaries; no persisted-data conversion is needed.

The crypto migration removes direct OpenSSL/BoringSSL discovery, linkage, and
vcpkg entries. Salts owns its crypto provider. Journal SHA-256 still hashes
`first + NUL + second`; context SHA-256 still hashes the exact serialized bytes.
The fixed journal regression passed with both the former and new implementations.

The unused historical `vendor/cjwt` source, examples and tests have been removed.
Neither the root build nor `vendor/CMakeLists.txt` used it; retaining its separate
OpenSSL discovery and link requirements obscured the active dependency contract.
This cleanup does not remove GmSSL from the manifest: the published Chttp Crypto
component still requires that private provider closure. SDK/provider provenance
and final installed-consumer qualification remain separate release gates (#69,
#71). Historical OpenSSL examples in repository skill documentation are not
compiled production source or package dependency declarations.

## SQLite RuntimeStore and CLI publication

Persistent conversations need committed record writes and one authoritative
session head. The memory backend loses records on exit; the existing file
backend does not provide a database transaction or exclusive process ownership.
The new `turbo_runtime_store_sqlite.h` backend reuses the existing SQLite
dependency and the RuntimeStore callback ABI. A TurboDB adapter or a new
multi-record transaction API would introduce additional contracts; neither is
necessary for this bounded, single-owner publication protocol. FTS5 belongs to
derived search indexes and is not used for authoritative runtime records.

Runtime owns schema admission and callback serialization. SQLite stores
`runtime_records(collection,id,payload)` with a composite primary key, application
ID `0x54415254`, and `user_version=1`. The JSON payload uses the existing record
format. Identifiers are bound SQL values. Each put commits atomically; there is
no transaction across multiple callbacks. A list is ordered by ID and either
returns the complete bounded result or fails. Filtering still scans the selected
collection, including rows that do not match; it is not a new search engine.
The backend rejects foreign schemas, unknown versions, corrupt payloads and
limit violations instead of recreating data. See the installed header for
defaults, errors and ownership. Legacy Runtime get/list wrappers retain their
existing generic negative error contract.

The connection uses `locking_mode=EXCLUSIVE`, DELETE rollback journaling and
`synchronous=FULL`. It obtains its lock during open and retains ownership until
close, preventing a second CLI process from racing read-modify-write journal
admission. This deliberately sacrifices concurrent readers/writers. WAL files
are rejected; there is no automatic journal-mode conversion. SQLite's
[locking and synchronization contracts](https://www.sqlite.org/pragma.html#pragma_locking_mode)
and [atomic commit assumptions](https://www.sqlite.org/atomiccommit.html) still
depend on functioning filesystem locks and synchronization. The tested process
termination path is not proof of power-loss durability on every filesystem.

Runtime publication is checkpoint first, then the run referencing
it, summary construction, and finally the thread-head snapshot. A failure can
leave an unreferenced checkpoint; it must not publish a run
that points at an unwritten checkpoint. Tool journal writes remain
`planned -> started -> committed`. On backends with atomic create-if-absent, a
separate durable `agent_tool_claims` admitted fence sits between planned and
started: external side effects occur only after that fence and after the started
record is committed.
A surviving fence yields unknown-side-effect status even when a crash left the
main Journal at planned. This provides one automatic backend admission per identity
under that backend's atomic scope; it does not provide exactly-once shell effects or
guarantee a checkpoint immediately before every side effect. Backends without the
atomic primitive retain only the older single-executor compatibility path.

The additive `store_get_json_status` API preserves callback errors; the old getter
retains its 0/-1 contract and the callback struct ABI is unchanged. Builtin get
callbacks distinguish ENOENT from I/O and malformed-data failures. The tool
journal admits a new call with a point lookup, avoiding a collection scan and
its 4096-row default ceiling. Only old custom callbacks returning generic -1
retain the filtered-list absence check. File enumeration now rejects native
errors, including a non-directory collection and an interrupted enumeration,
instead of reporting an empty or partial collection.

`thread_heads/<thread_id>` schema version 1 contains an independent full run
snapshot. An adoption marker with a null snapshot is committed before a thread's
first new segment. Final head publication follows successful run persistence and
summary construction. A failed update keeps the previous published snapshot;
first-publication failure leaves the marker and returns EBUSY on a head read.
This is a per-record atomicity guarantee for SQLite; the legacy in-place file
backend does not provide crash-atomic publication. Hosts serialize execution on
each thread. There is no multi-record transaction or concurrent-writer arbitration.

Increasing timestamp precision or sorting UUIDs would not define publication
order across restarts. A head containing only a run ID would expose a changed
run after same-run resume even if head publication failed. The full snapshot
avoids both problems, at the cost of one duplicate run snapshot per thread and
an additional bounded write. It is authoritative for current/head state and
checkpoint selection. Thread event history follows the parent chain ending at
that published checkpoint, excluding later unpublished checkpoints; run records
remain the explicit historical lookup surface.
An existing empty, corrupt or unreadable head never falls back to older runs.
Confirmed head absence retains legacy selection. On first execution of an old
thread, adoption begins before graph entry; if that execution fails, old runs
remain explicitly queryable but automatic head selection fails until a segment
is successfully published. Old timestamp-based latest/pending APIs are unchanged.
After a completed fork becomes head, thread-scoped resume no longer selects an
older interrupted branch. Resuming that branch requires its explicit checkpoint;
the local JSON-RPC regression covers the rejection and explicit recovery path.

The CLI owns the SQLite connection through Harness destruction and executor
drain; Session borrows its complete callback descriptor without a destructor.
This makes early Harness/Session construction failures safe without transferring
the same owned descriptor twice. Between turns the input thread uses Runtime's
serialized APIs. Metadata is a projection in the same database, not another
conversation transcript: `cli_sessions/current` format version 1 publishes
workspace, thread, turn count, phase and exact last-run ID. It commits `active`
before submission and `ready` only after validating the completed published head,
before printing the answer. Startup rejects `active`, including ambiguous
completion-publication failures. A crash after `ready` but before stdout may
leave a completed answer in history that was never printed.

Agent-generated IDs and segment checkpoints now use Salts UUID v4 via
`turbo_runtime_record_uuid`. The old `turbo_runtime_record_id` helper keeps its
documented legacy format. Previously persisted IDs remain lookup keys and are
not rewritten. This avoids relying on a process-local counter and monotonic
clock when the same store is reopened after process or machine restart.

Migration is opt-in through `--session-db`; default memory behavior is unchanged.
Existing stores are not converted, and a failed database open never selects a
memory fallback. Rollback means using the prior SDK with its prior storage path;
preserve new SQLite files for a version that understands them. Changing schema
versions, introducing cross-record transactions, adding memory/blob retention,
and rebuilding FTS/vector indexes require separate designs and migrations. This
slice exposes completed-conversation continuation and existing SDK checkpoint
recovery, with explicit failure for unfinished CLI work. See
[CLI usage and limits](../../cli/README.md#persistent-conversations).

## CLI vector retrieval composition

The CLI vector path uses an explicitly selected embedding endpoint and model,
separate from chat credentials and mutually exclusive with FTS retrieval. A new
dedicated SQLite file contains the SDK's vector documents and CLI identity
metadata (endpoint, model, observed dimensions). Existing unmanaged vector,
session or knowledge databases are refused; no automatic migration is performed.
Changing identity requires a new file and explicit reimport. Omitting vector
launch flags restores the ordinary coding/general path.

One input-thread owner holds the vector store, embedding callback, metadata
connection and companion advisory lock (same path spelling, no external writers).
No pointer crosses the Harness queue;
selected snippets are copied into each fresh state. The existing bounded,
no-follow CLI scanner supplies text to VectorStore's splitter and per-document
transaction. Embeddings are obtained before document replacement, preserving the
old document if any request fails. Earlier committed documents remain on a later
import failure. There is no automatic retry, deletion reconciliation or watcher.

Vector imports use the existing byte splitter with a 2048-byte target and three
bytes of overlap. The adapter trims incomplete UTF-8 boundaries before embedding
and retrieval. Without overlap, a split codepoint vanished from both snippets,
and a final chunk made solely of continuation bytes caused a valid import to
fail. Three bytes cover the maximum split prefix of a four-byte codepoint; every
complete character is retained in at least one chunk. Preflight uses the same
splitter settings and reserves duplicated overlap bytes. This keeps the SDK byte
splitter contract and persisted schema unchanged; existing chunks are read as
before, and explicit per-document reimport applies the new overlap. It can add
chunks and embedding requests near boundaries. Reverting the private overlap
setting affects future imports only. Tests cover every split position of final
two-, three- and four-byte characters and retrieval after reopening.

The HTTP adapter uses public CHttp APIs with one outstanding request, a 60-second
request deadline, 50-ms cancellation polling, 256-KiB response cap and at most
4096 finite components with nonzero vector norm. The whole import is bounded to ten
minutes. A persistent index admits at most 8192 chunks and 64 MiB of serialized
row data; preflight checks precede SDK operations because its SQLite similarity
search materializes candidates. It is an exact scan, not an ANN index. Completion
views are copied during callbacks, and HTTP borrows are drained before cleanup.
Failures stop before chat submission and do not fall back to hashing or FTS.

The private CLI cancellation flag is a caller-owned `atomic_int`, borrowed
through return and transport/worker drain. The SIGINT handler only sets this
flag; the executable checks that this actual object is lock-free before installing
the handler, which therefore cannot acquire a hidden lock. Readers use relaxed loads: the flag requests a
stop and publishes no other data. This also supports Windows signal delivery on
a separate thread. Polling, request cancellation and client destruction stay on
the single input/progress owner. No installed SDK ABI or persisted format changes.

After checking persisted row/byte bounds, startup validates stored embeddings
one row at a time on the input thread. SQLite owns the borrowed text until the
next step; the temporary parsed array is freed before advancing. Each serialized
vector is bounded by the response cap and must have the pinned dimension, finite
components and unit norm (within serialization tolerance), as emitted by this
CLI. Malformed data fails admission before any embedding/chat request and is not
repaired or rewritten. Validation runs once per open under the existing exclusive
ownership contract; later imports use the validated embedding callback. There
is no schema migration or additional persistent state.

Indexed chunk replacement matches the generated `document-id#chunk-<decimal>`
identity in both memory and SQLite stores. Matching the prefix alone could remove
chunks belonging to a different document whose ID contains `#chunk-`; restricting
the remaining suffix to a nonempty decimal ordinal preserves those documents.
SQLite measures the prefix in characters so Unicode source identities retain
the same replacement semantics as memory. Existing generated IDs need no migration.

Tests use a local HTTP fixture and real SQLite files to verify wire requests,
semantic retrieval, reopening, identity/dimension mismatch, failed replacement,
pre-request and in-flight cancellation and bounded admission. In-flight cases
hold a deferred response open, inject SIGINT, and require driver exit before the
server replies or times out. A subsequent run verifies both index reopening and
completed-session resume. This tests the driver path, not physical terminal
signal delivery. A dedicated subprocess fixture opens the same private vector
owner and uses a bounded readiness/exit handshake. It verifies contention in
both parent/child directions without provider calls, OS lock release after
orderly close, abrupt exit or forced termination, and retrieval of committed
chunks after each. Killing an importer during its second pending embedding
also preserves the previous document. This does not inject failure inside a
SQLite commit or simulate power loss. Real service quality requires separate
opt-in testing. The wire contract follows the
[OpenAI embeddings API](https://developers.openai.com/api/reference/resources/embeddings/methods/create).

## CLI general mode and knowledge retrieval

The CLI now composes the existing Session loop with either coding or general
instructions. General mode registers no coding tools and does not load project
instructions. The default coding mode, public SDK interfaces and persisted
session format remain unchanged. An explicit instructions option overrides the
mode prompt without changing permissions.

KnowledgeStore owns indexed documents, chunks, content hashes and FTS5/BM25;
RuntimeStore continues to own conversation/run/checkpoint/journal publication.
The CLI owns one optional KnowledgeStore on its input thread, imports only at
startup and queries only between drained runs. It copies selected context into
each fresh turn state before submission and closes the index after worker drain.
No knowledge pointer crosses the execution queue and no knowledge mutation tool
is added to the model's registry. Index/query failure occurs before `active` is
published for the turn; partial state is discarded without invoking the model.

The integration uses FTS5 retrieval with the ordinary Session loop rather than
switching all questions into an engineering planner. The separate CLI vector
path above requires explicit embedding configuration. FTS5 absence fails
this CLI path instead of silently changing relevance semantics to LIKE.

The existing SDK directory importer follows host paths and has different scan
semantics. CLI admission instead uses bounded Salts Core root-relative reads,
then delegates content hashing, chunking and per-document transactions to
KnowledgeStore. It does not add a filesystem or search engine. Import limits,
excluded names, no-follow behavior, per-document partial commits and unverified
source freshness are described in the [CLI contract](../../cli/README.md#general-assistant-and-retrieval).
There is no new schema or automatic removal of previously indexed source data.
Because the legacy KnowledgeStore open creates missing tables, the CLI uses the
already linked SQLite dependency to admit an empty database or the existing core
tables with an unset application ID before that call. It refuses RuntimeStore
and unrelated populated databases without changing their schema. Format ownership
and document operations remain in KnowledgeStore; this does not add migrations.
Rollback is omission of these launch flags; existing databases remain available
through their original SDK owners. Formal CLI tests cover captured model input,
real store reopen and failure before invocation. Retrieval quality and throughput
are not established by those deterministic tests.

## Diagram export: implemented read-only slice

The first visualization slice composes three existing owners without a new
execution engine or graph database:

- RuntimeData's [turbo_diagram.h](../../runtime_data/include/turbo_diagram.h)
  owns a disposable JSON view and bounded Mermaid rendering. Nodes/edges are
  copied, callers own views/text, and all calls are synchronous. Source objects
  must stay stable for the call; independent views have independent ownership.
- AgentCompiler's `turbo_agent_executable_dag_diagram` projects the frozen plan,
  including dependency edges, approval/checkpoint boundaries and retry limits.
  It does not invoke tools or export argument payloads. The plan generation and
  decimal plan-hash identity remain metadata, not authorization records.
- Runtime's [turbo_runtime_diagram.h](../../runtime/include/turbo_runtime_diagram.h)
  reads the exact published head and bounded checkpoint ancestry. It does not
  infer execution order from timestamps, list unrelated runs, or expose later
  unpublished checkpoints. Its run node references `thread_heads/<thread_id>`
  rather than the independently mutable historical run record. The caller must
  keep Runtime alive and quiescent across these multi-record reads.

CLI `--diagram mermaid|json --session-db FILE` provides a usable export path
before model/workspace initialization. It reuses SQLite's existing exclusive
owner and schema checks with creation disabled. No session-state write occurs;
an active marker is preserved and clearly labeled. See
[commands and limits](../../cli/README.md#diagram-export).

Mermaid IDs are generated from node indices, independent of source IDs. Labels
follow Mermaid's documented [quoted strings and decimal entity escapes](https://mermaid.js.org/syntax/flowchart.html#entity-codes-to-escape-characters).
No URL, click callback, style directive or HTML viewer is produced. JSON node
identity/provenance is the future navigation seam; this slice does not implement
an interactive editor, SVG/PNG renderer, tool-by-tool timeline or generic Job UI.
The renderer permits cyclic relationship views; compiler admission remains the
authority for executable DAG validity.

Neighborhood navigation belongs to RuntimeData's disposable view, not to a new
store or scheduler. `turbo_diagram_neighborhood` selects a focus node and nodes
within a requested number of hops, following incoming, outgoing or both edge
directions. It includes every original edge between selected nodes, preserves
array order and source metadata, and records shortest-hop distance on each node.
CLI exposes this through `--diagram-focus`, `--diagram-direction` and
`--diagram-depth`. Incoming edges show prerequisites in compiled plan views;
outgoing edges show possible downstream dependency impact. For execution views
these directions describe checkpoint ancestry, not causality or tool execution.

The call borrows a stable input for its synchronous duration and returns a fully
owned independent JSON view only after success; failure leaves the input intact.
Fixed traversal workspace is bounded by 128 nodes/512 edges, with depth 0..127.
Before copying metadata, a bounded walk admits at most 8192 JSON values, 32 nesting
levels (root depth zero), and 1 MiB of key/string/number-token bytes in the input.
Exceeding any budget fails with ENOSPC instead of returning a silently cut graph.
Selection metadata reports omitted nodes/edges relative to the supplied view;
it never claims repository-wide completeness or source freshness. Re-querying a
selection operates only on that selection. Unknown IDs return ENOENT; malformed
graphs, including corruption outside the selected neighborhood, fail validation.
Traversal follows breadth-first distance layers, supports cycles, and uses no
external callbacks, shared mutable state, retained source pointers or I/O.
Cloning metadata retains provenance without introducing another source schema;
the preflight budget bounds the additional owned copy. This is an additive API
and CLI filter; rollback omits the filter and needs no stored-data migration.

Bounds are 128 nodes, 512 edges, 2048 UTF-8 bytes per ID/label and 1 MiB of
rendered output. Validation/limits fail without publishing partial views/text.
Array order is preserved, source data is not modified, and output owns its
copies after source/plan destruction. Lookups use bounded linear scans; no
performance improvement is claimed. Persisted-record loading retains the
backend's allocation limits independently of the smaller diagram-view limits.

This additive API/CLI change requires no data migration. Existing normal CLI
behavior is preserved; hosts can stop requesting/exporting views to roll back.
A renderer in RuntimeData avoids making Runtime depend on AgentCompiler or
duplicating Mermaid escaping in each source adapter. Tests cover frozen plan
projection, non-invocation, actual segment publication, failed publication,
malformed/missing ancestry, cycles, limits, Unicode/escaping, CLI metadata
preservation and model-free export. Browser rendering is a separate consumer
integration, not inferred from C unit-test success.

### Indexed document and chunk views

KnowledgeStore's `turbo_agent_knowledge_store_diagram` provides a source adapter
for selected indexed documents (including code files indexed as text). It emits
`knowledge` views with document/chunk identities, `contains` edges, source record
references and the stored document content hash on both ends. It reads metadata
and chunk ordinals, never chunk text or arbitrary metadata payloads. It does not
infer code calls, Markdown references, task associations or rename identities.

The caller serializes access to one borrowed KnowledgeStore. The adapter owns a
read transaction across all selected IDs so concurrent writers using another
SQLite connection cannot mix document revisions and chunks. It rejects an
already active transaction, rolls back failed reads, and publishes owned JSON
only after successful transaction completion. No schema, persisted records or
existing retrieval behavior changes. Source bytes remain authoritative: output
is labeled `indexed_snapshot`, not proof that files still match the stored hash.
Chunk IDs may be reused after reindexing; consumers must retain document hash
with chunk identity. The hash is an existing index identity, not authentication.

Admission is bounded to 32 unique document IDs, 1024 UTF-8 bytes per selected
field, and the shared total of 128 nodes/512 edges. Unknown documents, malformed
rows or overflow fail without partial output. A document with no chunks remains
a valid single node. Input order controls document order; chunks use ascending
nonnegative ordinals. These output bounds do not impose a database size or query
execution-time limit on SQLite. Store lifetime and quiescent destruction remain
with the caller; output owns its copies after store close or source reindexing.

The new `agent.knowledge.diagram` tool is registered in both existing knowledge
tool registries and uses the same implementation. It accepts selected document
IDs, optional focus/direction/hops and JSON or Mermaid output. It is an observe
operation; the ordinary CLI session export retains its existing scope. This
adds no new database or execution engine. Disabling the new tool/API rolls back
the feature without migrating data. Formal tests cover selection, revisions,
payload exclusion, concurrent snapshot reads, rollback, limits, malformed rows,
both registries and neighborhood/renderer integration.

#### Optional literal URI relationships

`turbo_agent_knowledge_store_diagram_ex` with
`TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS` (tool `include_uri_mentions: true`) adds
observed `mentions_uri` edges between selected document nodes. It searches the
indexed text for exact, case-sensitive selected target URIs with start/end or
ASCII whitespace/quote/bracket/angle/backtick/brace/parenthesis boundaries.
This is literal evidence, including mentions in code/comments/examples, not a
Markdown parser, call graph, or claim that a hyperlink can be followed. It does
not resolve relative paths, decode escapes or percent encoding, strip fragments,
normalize paths, fetch URLs, scan unselected targets or read the filesystem.

The default API and omitted/false tool flag retain metadata-only reads. Opt-in
extraction reuses the adapter's read transaction after all selected nodes exist.
It reconstructs one document at a time from contiguous chunk ordinals starting
at zero, requires reconstructed byte length to equal the stored size, and checks
UTF-8 after concatenation so byte-split code points remain valid. Embedded NUL,
missing ordinals, inconsistent size and invalid UTF-8 fail with no partial view.
The copied text budget is 256 KiB per document and 1 MiB per selection; SQL avoids
returning a text cell larger than the per-document budget. SQLite internal I/O
and execution time retain their existing limits. One owned `tstr` holds the
current body, at most 128 fixed chunk spans borrow IDs from the stable view, and
all temporaries are freed before returning. No borrowed body pointer survives
append/reallocation, stepping a statement or ending the transaction.

Each occurrence has its own edge with source/target content hashes, source byte
offset/length and starting/ending chunk IDs and byte offsets (end exclusive).
These locators refer to indexed UTF-8 bytes, not presumed live file line numbers.
Same-revision extraction is deterministic: source document order, target
document order, then occurrence order. Self/cyclic mentions are valid. Exceeding
the shared edge bound fails rather than silently dropping evidence. The root
marks scope as selected URIs, so zero edges never means no external references.
All graph filtering happens after extraction and validation; it cannot hide a
broken source or bypass budgets. No schema/data migration or extra dependency
is needed; omitting the flag rolls back this additive behavior. A later semantic
extractor must declare its own evidence, resolution and completeness contracts.

### Selected tool-journal evidence

The Agent-owned `turbo_agent_thread_tool_diagram` composes Runtime's published
thread diagram with 1..32 explicitly selected journal record IDs. It performs
point reads from `agent_tool_journal`, preserving input order and rejecting
duplicate, missing, malformed or cross-thread/run records. The shared total
remains 128 nodes/512 edges; selected fields are bounded to 1024 UTF-8 bytes.
The journal collection/version and admitted status values are shared privately
with the executor. CMeta Schema/Replay describes the copied string projection;
it grants no invocation or state-mutation authority.

The caller keeps Runtime and its store alive and quiescent throughout all reads,
including other connections/writers. This is not a cross-record read transaction.
Only one journal record is loaded at a time under the backend's existing payload
limits; the adapter copies selected metadata into an owned view, frees the record,
and publishes only on complete success. Errors discard the view and propagate;
there are no tool calls, writes, journal scans, background workers or retained
borrows. Published-head lookup retains Runtime's existing behavior, including
legacy callback absence-disambiguation where required.

Edges mean `same_run`: stored thread/run identities match the published run.
Journal records are current observations, not checkpoint-frozen evidence. A
same-run resume may have changed a journal after head publication. Root/node
metadata and Mermaid labels explicitly retain this distinction. `started` means
side effects are unknown; `committed` retains the actual status, including failure,
and does not imply tool success. The hash is stored provenance, not a recomputed
proof of arguments or authentication. Output/detail and arguments are not copied;
backend get still loads the full record before projection.

This SDK adapter is read-only and additive. CLI `--diagram-journals ID1,ID2`
opts in using 1..32 unique comma-separated lowercase SHA-256 keys. CmdParser
supplies the string; the CLI validates and copies a bounded selection before
opening SQLite. Its exclusive connection supplies the quiescent read boundary.
Default CLI lineage exports remain unchanged. All selected records are validated
before neighborhood filtering; focus cannot hide a mismatching journal. JSON
and Mermaid preserve the current-observation distinction and session phase.

CLI discovery also accepts `--diagram-journals all`, scoped to the actual
published head's run ID (not the CLI's possibly stale last-completed-run marker).
The CLI owns one exclusive SQLite connection throughout head lookup, the existing
filtered list operation, and point-read validation. This single synchronous reader
introduces no worker, queue, writer or retained runtime borrow. List payloads are
bounded by SQLite's existing 16 MiB record / 64 MiB list / 4096 scanned-row limits;
these are serialized-byte limits, not a total heap bound. At most 32 keys borrow
the returned owned array until diagram construction returns; the array is then
freed. Keys use the CLI's existing lowercase SHA-256 syntax and are sorted
lexically for stable output, not execution order. The existing Agent adapter
validates every point-read record and the combined graph before focus. Too many
matches, malformed IDs, mismatched identities and storage failures produce no
output or automatic retry. Zero matches produce the published lineage with
`journal_scope=published_run_records` and `journal_count=0`. No claim is made about
other runs, unjournaled calls or plan-step identity. Explicit key selection remains
available without discovery scans; removing the `all` option requires no data
migration or change to the SDK adapter.

It supports the existing neighborhood and Mermaid APIs. It does not associate
calls with plan steps, tool results with file versions, or claim repository-wide
completeness. Those links require producer-owned explicit evidence. Omitting
this adapter rolls back the feature without a schema migration.

## Nested tool-journal identity correction

The `planner step tool journal identity` regression in
`tests/test_turbo_agent_planner_tools.c` exposes a correctness defect: two executor
substates in one run can reuse event offset zero and the same model `call_id`.
Previously, the journal key hashed only run ID, local event offset and call ID.
Identical arguments replayed the first step's committed output in the second
step without executing its tool. Different arguments instead fail journal
admission. Planner and executor substates can collide for the same reason.
The regression originally observed two requested tool invocations but only one
callback; it now requires two. Fixing this producer identity precedes any plan-step
graph links.

The correction uses the existing owning substate array and its stable
version index to construct opaque nested turn keys:
`planner:<version>:<event>` and `executor:<version>:<event>`. Initial planning
uses the root event stream before a planner substate exists, so it uses
`planner:root:<event>`. Ordinary top-level tool-loop decimal turn keys remain
unchanged. Version identity comes from the parent's persisted
substate array, not a tool name, step text, timestamp, pointer value or guessed
compiled DAG identity. Replanning appends versions rather than reusing an index.
The same checkpoint and pending call therefore regenerate the same identity.

Ownership stays local: the workflow builds a bounded scope string using Salts
`tstr`/`fmt`, borrows it for synchronous tool dispatch, then frees it after the
existing executor has drained. The tool node owns the full turn key through
middleware, journal planning, dispatch and result collection. No descriptor,
new global registry, extra worker or public context-layout change is needed.
Malformed/missing substate ownership and allocation failures stop before dispatch.

When no scoped journal exists, a legacy-key lookup distinguishes genuine absence
from read failure. Any old record at
that key is ambiguous: do not copy it to a new scope, treat it as a successful
new step, or execute the tool again. The executor rejects automatic continuation
with `SALTS_EBUSY` for an admitted ambiguous old record and leaves
the old record untouched, including its `planned`/`started`/`committed` phase.
An existing valid scoped journal keeps the executor's current replay and unknown
side-effect rules. The JSON journal layout and schema version can remain stable,
but the opaque nested turn-key semantics and old checkpoint admission change.
The user explicitly approved that compatibility behavior. Existing malformed
records and backend read failures propagate their errors; they never become
absence. No new public struct layout or journal schema version is introduced.

Formal tests cover two executor versions, planner/executor separation,
replanning, same-scope replay even with an ambiguous legacy record present,
SQLite reopen with committed/unknown effects, legacy records in all three phases,
failed legacy reads and zero duplicate side effects. Replaying the same saved
workflow checkpoint regenerates both scoped keys without another tool invocation.
Rolling an upgraded in-progress
nested workflow back to an older binary is unsafe because it cannot recognize
scoped journal keys; retain the upgraded binary for those workflows. No automatic
data migration or deletion is proposed.

### Published request origins

The optional `TURBO_AGENT_DIAGRAM_CALL_ORIGINS` projection resolves scoped journal
turn keys against the published run's `state_snapshot`. It uses direct substate
version/event offsets, then checks the model event for one matching call ID and
tool name. The persisted substate remains authoritative; no plan step is inferred
from its array index. Ordinary/legacy unscoped keys cannot distinguish root and
older nested calls, so they retain `origin_status=unresolved_identity` without an
edge. Missing snapshot versions/events/calls retain `not_in_snapshot`; malformed
recognized scopes or contradictory/ambiguous call metadata fail the whole query.

The Agent owns one additional published-head JSON tree for this synchronous
projection, under the store's existing record limit; it frees that tree before
return. All store writers remain quiescent. It borrows event/call fields only
during validation and copies identity/locator metadata into the output. No prompt,
arguments or result payload is exported; arguments are not hashed or compared
because middleware may have transformed them before journaling. A matching
request is not proof of successful execution or checkpoint inclusion of the
current journal. No event histories or other journal collections are scanned.

Each selected event admits at most 64 call entries. The combined view retains
128 nodes/512 edges and the existing field/output bounds. Shared event nodes are
deduplicated by scoped turn key within the one published run. A run-to-event
`snapshot_contains` edge identifies a frozen request; event-to-journal
`same_call_identity` records the call's exact JSON Pointer in `thread_heads`.
All validation precedes optional neighborhood filtering and output publication.
Existing SDK calls and CLI exports omit this projection unless requested. The
extension requires no stored-record migration, new backend, execution authority
or dependency; removing the option rolls back the view alone.

### Prepared workflow step provenance

The engineering workflow and the compiled DAG backend have distinct owners.
The workflow's `plan_versions` contains normalized natural-language steps;
`executor_state_versions` contains model/tool subruns. Neither array index is
an AgentCompiler `step_id`. To record actual preparation instead of guessing
from aligned arrays or matching text, step preparation adds an optional
`plan_step_origin` object to each new executor substate before execution:
`{schema_version:1, plan_version:<index>, step_index:<index>}`. The two offsets
refer to the root's persisted `plan_versions[v].steps[s]`. Replanning appends a
new plan version; replay from an existing checkpoint retains its binding.

The workflow owns this metadata and initializes it from validated current plan
and progress arrays. Allocation and malformed-state failures stop preparation
before model/tool dispatch. Version append helpers use checked JSON insertion
and transfer the child only on success. Preparation failure is terminal for that
graph attempt; creating a version array or history entry is not a cross-field
transaction and must not be treated as permission to continue the executor.
The existing state owner serializes the completed binding with the
checkpoint; there is no new journal field, secondary index or inferred backfill.
Old executor substates without this field remain executable under their existing
contracts and export `step_origin_status=unrecorded`. Host-supplied state edits
remain trusted inputs; this locator is not a cryptographic attestation or an
approval record. Existing plans and their ordering must not be edited in place
when interpreting retained bindings; replanning appends a version.

The opt-in `TURBO_AGENT_DIAGRAM_WORKFLOW_STEPS` projection requires call origins.
For a matched executor request it validates the recorded binding and referenced
step in the same published snapshot, then adds `workflow-step:<version>:<index>`
and a `prepared_for_step` edge to the model event. Event nodes report matched,
unrecorded or not_applicable (planner) step origins. A present but invalid binding
or missing referenced step fails with EPROTO, without partial output. Step text,
plan payload and tool arguments are excluded; only identifiers and exact source
and binding pointers are copied. Shared step/event nodes are deduplicated under
the existing graph bounds. Queries use the already-owned published-head tree;
no extra history scan, storage mutation, concurrency or lifetime is introduced.

CLI `--diagram-plan-steps` requires journal selection and enables both origin
projections. Omitting it preserves existing graph output. This is an additive
snapshot field and opt-in view; old records are neither migrated nor rewritten.
Rolling back this projection leaves stored bindings inert. Compiled DAG
hash/generation/step binding still requires instrumentation in its execution
backend; this workflow relation must not be presented as that guarantee.

### Paths within a supplied diagram

`turbo_diagram_shortest_path` selects one minimum-hop connection between exact
node IDs in an existing view. It shares neighborhood validation, opaque-metadata
budgets and distance-layer traversal; it does not reload sources or build an
index. Incoming/outgoing/both traversal preserves the stored edge directions
and relation evidence. Ties choose the first eligible source-array edge in each
distance layer. Only predecessor edges are exported, not the induced subgraph;
source array order is retained and node `selection_distance` gives path position.
An identical start/end selects one node and no edges. Unknown endpoints or no
path within the requested hop limit return ENOENT with no partial output.

The synchronous caller owns and stabilizes the input JSON for the entire call.
The query uses fixed workspace bounded by 128 nodes/512 edges, borrows fields
only during traversal and clones a complete independently owned result before
publication. Allocation or budget failures discard the result; there are no
queues, background workers, storage writes or cancellation/drain obligations.
Validate the entire input (including omitted metadata) before traversing.
`selection.mode=shortest_path`, target, hop count and omitted counts describe
the result only within `scope=input_view`. Traversing mixed relation types does
not prove causality, execution order, permission or universal reachability.

CLI `--diagram-to ID` combines with `--diagram-focus ID` to select this query
instead of a neighborhood, after journal/origin validation. Direction and depth
retain their existing defaults (both, one hop); larger paths require explicit
depth. Existing exports and stored records do not change. This additive query
uses the current graph owner rather than introducing a second graph engine;
omitting the option restores the existing neighborhood output.

## Proposed artifact relationships and reviewable jobs

The [engineering task workbench plan](../../docs/design/engineering-task-workbench.md)
turns this proposal into product scenarios, an M0 read-only slice, ownership rules
and staged acceptance gates. The implementation status and boundaries below remain
applicable; the plan does not introduce another runtime or result owner.

The [dual-track product roadmap](../../docs/design/agent-product-roadmap.md) also
retains a separately deliverable general Agent product. Both routes compose the
same kernel and canonical execution evidence; engineering-specific relationships
and validators stay in the workbench layer.

Status: broader design proposal. The read-only diagram slice above is implemented;
the artifact index, durable Job lifecycle, scheduler and interactive UI are not.
The working assumption is that core discovery/review works without Praktor,
while reviewed Praktor workflows remain an optional execution backend. This
proposal adopts source analysis, relationships and incremental synchronization;
it does not assume a particular CodeSync API. The local CodeGraph index remains
a disposable development tool, not the Agent's authoritative runtime database.

### Existing foundations and missing connections

- [KnowledgeStore](../include/turbo_agent_knowledge_store.h) already owns document
  identity, URI, content hash, chunks and filtered retrieval. Its current
  [SQLite implementation](../src/turbo_agent_knowledge_store.c) attempts FTS5
  creation and uses BM25 when available. Its `tool_graph` describes recommended
  tool ordering; it is not a persisted code/document relationship index.
- [RuntimeStore](../../runtime/include/turbo_runtime_store.h), published heads,
  checkpoints and the tool journal own execution evidence. Their callback API
  does not expose cross-record transactions, compare-and-swap, or a durable
  globally ordered change feed.
- [Workflow](../include/turbo_agent_workflow.h) has review/replan loops, including
  per-step review. This does not by itself bind a human decision to an immutable
  plan version and current workspace contents.
- [AgentCompiler](../../agent_compiler/BOUNDARY.md) already freezes admitted tool
  identities, arguments and capabilities. Its
  [DAG contract](../../agent_compiler/include/turbo_agent_dag_compiler.h) carries
  dependencies, finite retries, approval/checkpoint flags and plan generation.
  [Change/Repair templates](../../agent_compiler/include/turbo_agent_templates.h)
  and [Praktor lowering](../../agent_compiler/include/turbo_agent_praktor_lowering.h)
  should remain the compilation boundary rather than be duplicated.
- [PraktorTools](../../praktor_tools/README.md) provides reviewed WorkflowPlan
  execution and lineage/result adaptation. Its inline-plan tests exercise
  Change and Repair; they do not establish a general durable Job service.

The missing layer is cross-domain identity, evidence-backed relationships and a
durable user-facing Job lifecycle linking those existing capabilities.

### Identity, relationships and ownership

#### Reuse boundaries for Salts and SaltsUtils

The graph/Job work composes the installed SDK's existing contracts. It must not
introduce a parallel reflection system, subprocess owner, schema compiler or
workflow executor. Current integrations and the next reuse points are:

| Concern | Existing owner and integration | Agent-specific responsibility |
| --- | --- | --- |
| Native type/function semantics | `Salts::CMeta`; RuntimeTools already admits Function/ABI/callable projections using semantic type equality | Tool capabilities, user approval and evidence; reflected visibility alone does not authorize execution |
| Stable internal tool declarations | CMeta `Schema`/`Replay`; KnowledgeStore now projects one catalog into ordinary and Action registry definitions | Preserve registry-specific handlers, strict input policy, Action kind/idempotence and borrowed store lifetime |
| Generated requests and artifacts | SaltsUtils `Salts::IDL`, `Salts::DataBind`, `salts-idlc` and `salts_idl_target`; CodingTools already generates native request bindings and model-facing tool schemas from `contracts/workspace.schema` | Define domain contracts once; reuse admitted Native/Plugin/WASM projections where their actual installed contracts fit |
| Shell/process execution | Salts Core `cmeta_process_*` already owns CodingTools child processes, capture, timeout and termination | Shell command policy, platform quoting, workspace, output presentation and side-effect journal |
| Asynchronous process streams | Installed `Salts::Process` / `salts/process.h` provides the bounded CFlow/NativeIO adapter over that same Core process owner | Integrate only when streaming/interactive process I/O is required, with one driver, completion-bounded buffers and explicit close/drain; current shell capture remains unchanged |
| File and CLI utilities | Salts `cmeta_fs_root_*`, UUID, strings and clock; Utils `Salts::CmdParser` already used | Workspace authority, command choices and user-visible error behavior |
| Execution and recovery | Existing CFlow execution, Runtime checkpoints/journal and AgentCompiler lowering, including the optional Praktor boundary | Job goals, proposal revisions, review records, source preconditions and acceptance criteria |

CMeta tuple schemas describe a finite native concept; they are not an alternative
to IDL wire contracts. KnowledgeStore's catalog refactor shares existing JSON
parameter schemas and emits two immutable definition arrays and the string ABI
adapters with CMeta. Each string adapter delegates to the same row's JSON value
handler; parsing, output ownership and errors remain in the existing shared
adapter. Direct-callback tests cover all eleven mappings, malformed JSON,
Unicode, mutation results and failure output, because registry execution may
prefer the JSON callback and otherwise leave the string path untested. It does
not claim those legacy JSON schemas have been migrated to IDL. Registration
still copies each definition, attaches the caller's borrowed store, and stops at
the first registration failure, retaining the same successfully registered prefix.
No public ABI, persisted data or tool authorization changes.

Migrating the remaining knowledge inputs to generated IDL decoding requires
first qualifying each entry point's admission behavior. The ordinary registry
enforces its JSON parameter schema, while legacy Action handlers can accept
additional fields and apply different defaults/conversions. A shared decoder
must retain those explicit adapters or introduce an intentionally versioned
contract; native metadata generation alone does not authorize tightening them.

For future durable Job/relationship records, first define the authoritative owner
and versioned data contract, then use the SDK's supported IDL/DataBind generation
and CMeta native descriptors. Storage commits, freshness and approval checks stay
with the domain owner. Generated metadata must not manufacture ownership, module
leases, permission or recovery guarantees. Plugin/WASM artifact generation does
not replace their existing loader, ABI admission or sandbox runtime. SDK source
checkout features must be checked against installed headers/targets before use.

#### Artifact identity

Expose one navigable view with distinct relationship meanings:

| Entity | Meaning and authority |
| --- | --- |
| Artifact / revision | File, symbol, document section or generated output; source bytes and revision are authoritative |
| Task | Desired outcome and acceptance criteria; owned by the planning domain |
| Plan / step | Versioned proposal and admitted action definitions; compiler output binds execution semantics |
| Job / attempt | One requested execution and its individual attempts; lifecycle owner references Runtime evidence |
| Approval / result | User decision on an exact proposal, or observed outcome with evidence; retained records, not inferred graph labels |

Relations include `contains`, `calls`, `documents`, `depends_on`, `reads`,
`proposes_change_to`, `produced`, `verified_by`, `supersedes` and `executed_as`.
Planned changes and observed changes must be different edges. Knowledge links
may contain cycles; scheduling dependencies must satisfy the compiler's DAG
contract. Linking an entity never grants authority to execute it.

Use stable entity IDs plus separate revision identities. Each source-derived
edge records extractor/version, source revision, exact locator, and whether it
is directly observed or inferred. A document URI or line number alone is not a
stable symbol identity. Ambiguous renames remain unresolved until supported by
evidence; a guessed match must not silently merge histories. User-authored
tasks, links and corrections are durable planning records, with the graph
holding projections of them so rebuilding an index cannot discard user work.

### Storage and synchronization

Start with a rebuildable SQLite relation-index sidecar holding entities,
revisions, typed edges, evidence references and published index generations.
Reuse existing KnowledgeStore retrieval for text candidates; FTS5 answers text
matching, while ordinary indexed adjacency queries answer relationships. This
does not require a graph-database dependency or a duplicate embedding system.
The Runtime SQLite store currently admits one specific database schema and
holds an exclusive connection: do not add sidecar tables to that file through
an unrelated connection or silently change its schema admission.

Index source revisions in bounded batches. Publish an entity revision and its
replacement outgoing edges in one index transaction, retaining the previously
published generation on failure. Mark deleted-source relationships stale or
tombstoned without deleting execution evidence. Every query reports freshness,
source revisions and truncation; return citations and paths explaining matches.
Bound candidate count, traversal depth, visited nodes, bytes and elapsed time.
An incomplete import must not claim that an unobserved dependency does not exist.

The source and index are not one transaction. A later change feed would need an
explicit durable producer contract and replay cursor. Initially import selected,
quiescent source snapshots with revision checks and publish only complete
batches; do not invent a reliable feed from best-effort callbacks or silently
scan past RuntimeStore's list limits. Reconciliation failure leaves the index
stale and visible as such. Source owners retain write authority; graph edits are
commands to those owners, followed by projection refresh.

### Review, execution and adjustment

A proposed Job owns a goal, acceptance criteria, plan version, selected source
revisions, step dependencies and referenced outputs. A one-action request is a
one-step Job. Longer work groups steps into stages or child Jobs while keeping
each expanded execution bounded. Deferred planning creates a new version when
the next stage becomes concrete; it does not hide arbitrary future execution
inside an already reviewed action.

The intended user path is: find relevant material, inspect evidence and proposed
changes, edit the proposal if needed, execute within the user's existing grant,
then inspect results and verification. The interface shows goal, current stage,
changed artifacts and exceptions first, with graph and attempt detail on demand.
Users can inspect one step or a whole stage, pause at a defined boundary, cancel,
or create a revised continuation. Review cadence follows explicit user policy;
ordinary authorized steps do not require repetitive confirmation.

Bind any approval record to the frozen proposal, tool/provider identity, inputs,
resource limits, effect scope and source preconditions. The existing 64-bit
compiler hash is an identity aid, not by itself a cryptographic authorization
token. A durable review record must retain the canonical certificate and exact
binding data. Recheck preconditions at execution: changed source, arguments or
effects require a revised proposal and re-evaluation under the existing grant.
Model-suggested or inferred graph edges cannot bypass compiler admission.

Each attempt has a new ID and references its Job, plan version, run and tool
calls. Preserve prior attempts and actual results when replanning. Do not edit a
running plan in place or label a tool return as completion without checking the
task's acceptance criteria. Recovery inspects durable journal/checkpoint state;
an unknown side effect remains unknown, rather than being automatically retried.
Rollback is an explicitly supported compensating action, not a universal promise.
Durable timers, external waits and arbitrary within-tool resumption are future
capabilities, not implied by existing graph checkpoints or Praktor integration.

The Job coordinator should serialize lifecycle transitions and persist revision
checks through its owning store. A graph projection is never the scheduler's
mutable state. Runtime remains independent of KnowledgeStore and the compiler;
an Agent/Harness-level coordinator composes public interfaces. Praktor executes
qualified deterministic workflows; the core can execute its supported admitted
actions through existing RuntimeTools. Unsupported backend semantics fail
explicitly rather than silently changing the execution engine.

### Delivery, migration and verification

1. Deliver a read-only vertical slice: connect selected documents, code symbols,
   existing plan certificates and run/tool records; support find, neighbors,
   impact and explain with citations and freshness. A code change should expose
   related documentation, affected tests and the actions that changed it.
2. Add durable Job/proposal/attempt records and a review surface around the
   existing compiler. Qualify one-step execution and Change/Repair flows before
   adding new templates. Index their authoritative records after publication.
3. Add stage composition and optional Praktor execution, then qualify pause,
   restart, replan and explicit retry across process boundaries. Durable waits
   or additional backends each need separate capability and recovery contracts.

The first slice is additive and uses a separate versioned index. Existing
Runtime, KnowledgeStore and compiler APIs/data remain readable. Rollback disables
the projection without deleting source or execution records. Later authoritative
Job storage requires an explicit schema/migration design; it must not be presented
as disposable index data. A graph database, merging all stores, or a replacement
workflow engine would add migration and ownership costs without being required
for this first slice.

Qualification must cover changed/deleted/renamed sources, ambiguous references,
partial imports, failed index publication, bounded/cyclic traversal, stale
evidence, plan mutation after review, changed provider/source preconditions,
single-writer conflict detection, process restart, duplicate delivery and unknown
side effects. Retrieval precision and indexing cost need representative measured
queries; graph structure alone is not evidence of a precision or speed gain.

## Responses conversation ownership

Live verification against a local OpenAI client exposed two wire failures:
dotted workspace tool names were rejected, and submitting only a tool result
with `previous_response_id` failed because the upstream could not find its call.
A returned response ID alone does not establish that a proxy retains or forwards
server-side conversation state.

Agent now projects the locally retained canonical history for each Responses
turn, including paired calls and results. Existing context compaction still
selects retained history. Provider owns the Responses item encoding; RuntimeTools
exports the same unambiguous name aliases already used by compatible Chat.
Canonical registry names, execution permissions and local strict argument
validation remain unchanged. Invalid history fails request construction.

Keeping the implicit remote-ID optimization, adding a proxy-specific provider,
and retrying a rejected request with another history strategy were considered.
Full local replay provides one explicit path that works after process restart
and avoids inferring remote storage or automatically repeating model requests.
It can send more history bytes; no latency or cost improvement is claimed.
The standalone Provider request API still accepts an explicit previous response
ID for hosts that manage that contract themselves.

No public C layout, configuration key or persisted schema changes. Existing
local conversation records remain the source of truth; no data migration is
needed. A rollback can restore Agent's previous request projection independently
of stored data, but restores its dependency on remote response storage. Coverage
includes alias collision rejection, parallel call/result encoding, malformed
history, context projection, and opt-in live general/coding CLI tasks on each
interface. Provider-specific opaque reasoning-item replay is outside the
canonical text/function-call contract and is not established by these cases.

Protocol reference: [OpenAI function calling](https://developers.openai.com/api/docs/guides/function-calling).

## Validation

Use an x64 VS developer environment and the SDK roots in the user preset:

```powershell
cmake --preset win-release-user
cmake --build --preset win-release-user --parallel 8
ctest --preset win-release-user --output-on-failure --timeout 120
cmake --build --preset install-win-release-user
```

Runtime graph and StateGraph tests link
only Runtime; prompt/SSE/embedding and standalone Provider tests link only Provider;
StateGraph examples link Runtime; Agent tests/examples link Agent; embedded Harness and Server tests link Harness;
the migrated public API test links Harness. This exercises actual link and behavior
boundaries instead of testing source text. Platform results are recorded in the
[SDK migration notes](../../packaging/nuget/README.md).
