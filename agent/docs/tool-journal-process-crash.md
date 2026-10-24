# Tool Journal process-crash regression

The existing Tool Journal tests close/reopen SQLite owners and inject storage errors.
The native `test_tool_journal_crash_*` CTests additionally terminate a real process
without cleanup at ten actual Executor/SQLite/tool/delivery boundaries. Independent
processes then reopen the database and verify stored evidence, current authorization,
identity rejection, explicit recovery and another replay.

This is a bounded sequential Direct ToolExecutor slice of
[#74](https://github.com/qigao/TurboAgent/issues/74), supporting
[#213](https://github.com/qigao/TurboAgent/issues/213)'s recovery acceptance work.
The production slice adds an atomic backend-entry fence; it does not add automatic
UNKNOWN recovery or complete either issue gate.

## Native fixture and independent evidence

CTest runs a CMake process driver which requires exit **86** from the crash worker.
An unrelated failure, loader error, timeout, missing cut or successful return fails
the case. It does not use WILL_FAIL or parse a success marker. All state/result/effect
assertions execute in the C fixture against actual Runtime Store/ToolExecutor APIs;
CMake only launches processes, checks exact exit codes and bounds execution time.
The fixture needs a custom C main instead of a generated TinyTest main so each
subprocess can select its mode and intentionally skip all cleanup. The existing
unit/integration TinyTest suites remain enabled and unchanged.

Each case creates fresh private databases in the CTest build tree, including spaces
in the path. Normal verification processes close all owners. The crash worker uses
C11 `_Exit(86)`, skipping Runtime/Executor/SQLite destructors and exit handlers; it is
not owner destruction followed by reopen inside the same process. The driver waits
for that process to exit before starting a new one. Each child has a ten-second bound;
each case has a ninety-second CTest timeout. Only its newly created case directory is
removed on success; failures retain database evidence and print its exact location.

The registered context Tool has explicit SEQUENTIAL execution, NONE idempotency,
known WRITE effect, `custom_tools` capability, strict argument/result contracts and
stable thread/run/turn/call identities. Its callback commits an increment to a separate
SQLite **effect counter**, then returns JSON and independently captured detail. A
separate delivery counter changes only after the Host explicitly receives and validates
a successful result. These two counters are test oracles, not a replacement result
store, recovery ledger or production delivery protocol. The original Tool Journal
remains the only saved tool output/detail owner.

The Journal backend is the real bounded SQLite Runtime Store. A thin test adapter
calls the real put/create-if-absent, or terminates immediately before/after it at the
chosen phase.
The effect cut terminates inside the actual callback after its independent commit.
The delivery cut terminates after Executor return and result validation, before the
Host's delivery receipt. Typed get statuses are preserved; execution checks that
neither a compatibility list nor an alternative backend was used.

## Expected cut states

| Cut | Main Journal | Admitted fence | Effect count | Explicit same-identity recovery |
| --- | --- | --- | ---: | --- |
| before planned write | absent | absent | 0 | Admit one call, commit its result. |
| after planned write | planned | absent | 0 | Invoke once, commit its result. |
| before admitted fence | planned | absent | 0 | Invoke once, commit its result. |
| after admitted fence | planned | present | 0 | UNKNOWN_SIDE_EFFECT; no callback/result/delivery. |
| before started write | planned | present | 0 | UNKNOWN_SIDE_EFFECT; no callback/result/delivery. |
| after started write | started | present | 0 | UNKNOWN_SIDE_EFFECT; no callback/result/delivery. |
| after effect commit | started | present | 1 | UNKNOWN_SIDE_EFFECT; no callback/result/delivery. |
| before committed write | started | present | 1 | UNKNOWN_SIDE_EFFECT; no callback/result/delivery. |
| after committed write | committed | present | 1 | Replay canonical output/detail; no callback. |
| before Host delivery | committed | present | 1 | Replay canonical output/detail; no callback. |

Every crash leaves delivery count zero. An admitted fence remains uncertain even at
the deliberately chosen cuts where this fixture knows the effect count is zero:
production cannot infer that fact and must not automatically redispatch. Unknown
outcomes preserve the main phase and admitted fence through repeated restart attempts,
with no saved output/detail invented. For known results, the explicit recovery Host validates the
strict result and detail before one receipt; the next verification replay intentionally
does not perform another delivery. This does not claim exactly-once external delivery.

Before any authorized recovery, a new process installs a denied Host policy and
attempts the same identity. It must receive no output/detail, enter no callback and
perform no Journal read/write/list; the durable phase/effect/delivery evidence stays
unchanged. When a record exists, another process changes valid arguments and then
the thread identity. Both attempts must return EPROTO without callback, output,
overwrite or compatibility query. This checks current admission before both new
execution and committed-result replay; persisted records cannot grant authority.

The verifier reads actual Journal phase, identity, result/status and detail, alongside
the independent effect/delivery counts. Replayed results must satisfy the current
registry's strict result contract. It never infers success from process return alone,
a model statement, an absent Session event or an observation saved by the Router.
No historical authority snapshot or generation counter is restored as a grant in
another process; the Host explicitly supplies current policy for each operation.
Policy replacement remains quiescent and no callback-time setter is introduced.

## Scope and reproduction

The process fixture is built under the existing BUILD_TESTS/native configuration and
excluded from cross compilation. Normal CI still configures/builds the full graph
and executes CTest. Workflows, presets, dependency pins, public ABI, main Journal schema,
identity hashing and product scheduling/ownership are unchanged. The auxiliary
`agent_tool_claims` collection contains only the versioned identity/strategy and
`phase=admitted`; it is not a result or authority store. Example focused runs
after the usual native configure/build are:

```sh
ctest --preset ci-linux-release -R '^test_tool_journal_crash_' --output-on-failure
ctest --preset ci-windows-release -R '^test_tool_journal_crash_' --output-on-failure
```

Qualify only the exact commits/platforms where these CTests actually ran. A macOS or
Android SDK build with tests disabled does not qualify process-crash behavior there.
These are abrupt process exits after/before completed record operations, not OS kill
at arbitrary instructions, power loss, partial-sector writes, mid-SQLite-transaction
crashes or allocator failure injection. They do not establish durable Router admission,
remote lease takeover, concurrently open SQLite owners, arbitrary nested/forked execution,
parallel tools, compiled typed-Inspect recovery, Wasm/MCP/Praktor behavior or all-platform
effect conformance. Those remain #74/#213 acceptance work. Unknown effects continue
to require explicit reconciliation; no globally exactly-once effect guarantee is made.

The adjacent `test_turbo_agent_tool_claim` also forces two independent executor/Runtime
handles over one serialized atomic test backend to read the same missing or PLANNED
identity concurrently. It requires one callback, an EPROTO loser for conflicting fresh
arguments, and an intact committed winner. Its interrupted two-call batch injects a
failed first terminal commit, then requires whole-batch recovery to retain that
identity as unknown, replay the second committed identity and invoke neither callback
again. Batch planning is not a multi-record transaction and does not infer a business
dependency between distinct identities. The same regression composes the atomic fence
with nested identity: two owners in one Run/turn/call admit one callback, while parent
and explicit-fork Run IDs keep otherwise identical nested calls independent and replay
each lineage without another callback. This is bounded by the backend primitive; the
legacy file/custom fallback does not gain a cross-owner guarantee.

The Journal suite separately exercises the real legacy file Store, whose callback table has
no `create_if_absent`. For both root and nested identities, a committed result survives a
close/reopen and replays without another callback. If the callback completed but the
COMMITTED write failed, the persisted STARTED record reopens as UNKNOWN and still does not
dispatch again. No admitted claim is created. This qualifies only single-owner lifecycle
recovery; the file backend still provides no cross-owner/process admission, atomic replace,
or crash-durability guarantee.

The adjacent Journal suite also runs the Executor over the real bounded SQLite Store.
An over-quota or database-full PLANNED record returns `SALTS_ENOSPC` before any callback and
leaves both Journal and claim collections empty. If PLANNED fits but the admitted-fence
create fills the database, the current call conservatively reports UNKNOWN with zero
callbacks and durable planned/no-claim evidence. It does not retry inside that call; after
the Host explicitly raises the database quota and resubmits the same identity, the durable
absence of a claim permits one admission. In terminal cases, PLANNED, admitted and STARTED
fit, the callback returns a result, and the COMMITTED write exceeds either
`max_record_bytes` or the main database `max_database_bytes`. The Executor reports UNKNOWN,
retains STARTED plus the admitted fence, and after a close/reopen returns the same UNKNOWN
without a second callback. It does not truncate or invent a committed result. The
database-full cases use SQLite's transactional rollback and do not qualify rollback-journal
disk exhaustion. The adjacent Executor regression qualifies cancel/deadline while one batch
is queued behind another: the queued call terminates before Journal admission and before the
owner is released. Token-free waiting, interruption of an already-entered native callback,
Journal entry eviction/retention and the remaining backend matrix stay open.

## Test-only SQLite VFS I/O failures

`agent/tests/sqlite_vfs` adds twelve independent-process CTests: STARTED and
COMMITTED persistence each run a passing control and five one-shot native-file
failures (rollback-journal write/full/sync and database write/sync). Its phase
adapter arms a forwarding VFS and always delegates to the real production Store.
The unchanged Store source is compiled into the fixture with renamed entry
points so that static SQLite registries cannot diverge across a Windows DLL and
its test executable.

For these injected errors, native read-only inspection after close requires a
failed STARTED to retain `planned` plus `admitted` and zero original callbacks;
a failed COMMITTED retains `started` plus `admitted` and one original callback.
Both replay UNKNOWN after independent restart without another callback. An
independent identity commits on the original connection after the one-shot fault.
Disk inspection also checks integrity, full persisted identity, output, an
untouched sentinel and independently committed effect counts. The controls replay
their exact committed result. See [fixture details](../tests/sqlite_vfs/README.md).

Linux and Windows include the cases in the normal suite; the macOS SDK profile
builds this fixture and runs its dedicated runtime CTest preset. Android remains
cross-build-only for this fixture. A platform is qualified only after its actual
VFS cases pass. These reported I/O errors followed by normal close/reopen do not
qualify arbitrary process kill, power loss, torn/partial writes, permanent disk
exhaustion or every I/O position. The broader #74 matrix remains open.
