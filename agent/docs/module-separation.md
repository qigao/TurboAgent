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
parent traversal. There is no new store, registry or persisted index.

Selection preserves backend encounter order for tied timestamps. Latest selects
by `updated_at`; pending restricts status to interrupted; head prefers pending
then latest. Cancelled/timed-out segments remain explicitly resumable but are not
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
quiescence before projecting the next state. Persistent resume, incremental
token rendering, and an approval UI are outside this CLI's current interface.

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
