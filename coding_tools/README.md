# Workspace coding tools

`contracts/workspace.schema` owns the tool parameter shape. SaltsUtils IDL
generates the native DataBind codec, while `tool_schema.mustache` projects the
same declaration into the model-facing JSON Schema. The projection currently
supports required string fields and rejects unsupported kinds at compile time.
It is private to this tool pack, not a second general-purpose IDL compiler.

The generated codec owns decoded strings; handlers clear them before returning.
Tool registration clones the generated JSON Schema. Registry-owned bindings
outlive synchronous calls and are freed when the registry is destroyed.
Existing result JSON and read-only tool names remain unchanged.
Read and list operations also use retained Salts root capabilities rather than
check-then-open host path concatenation. Reads remain bounded if a file grows
during the call. `fs.list` accepts `.` only as the admitted root selector; other
dot segments remain invalid. Missing roots now fail during registration.
Paths are limited to 64 KiB before serialization/decoding. Parallel calls have
independent codecs and decoded requests; there is no shared mutable codec.
Unknown members, missing/wrong-type paths, and embedded NUL return a rejected
tool result. Allocation failure remains a tool execution error.

Read/list handlers use the existing v4 context-aware tool definition. The caller lends its
cancellation token and deadline for one synchronous call; each call owns its
codec, decoded request, file/directory handle, and unpublished result. Checks run
before decoding/access, between read chunks/directory entries, and before result
publication. Cancellation and deadline expiry close handles and discard partial
results, returning CANCELLED or DEADLINE_EXCEEDED with a NULL output. Blocking OS
calls themselves are not forcibly interrupted. Existing legacy callbacks remain
available and invoke the same implementation without a context. Tool names,
schemas, successful/failed JSON shapes, limits and registration rollback remain
unchanged; no persisted state or public configuration migration is required.

Code generation runs during the build using the matching installed
`salts-idlc`. Cross compilation requires `SaltsUtils_IDLC_EXECUTABLE` and
`SaltsUtils_IDLC_HOST_SALTS_ROOT` pointing to a qualified host SDK. The compiler
is not a runtime dependency. Generated files stay in the build tree.
Android presets read these paths from `TURBOAGENT_HOST_IDLC` and
`TURBOAGENT_HOST_SALTS_ROOT`. Android CI restores the Linux host compiler and
Salts runtime from the same pinned native packages as the target SDK.
The current SDK requires explicit `BINARY_CODEC` for MESSAGE and custom C
template generation. The pack consumes only the JSON native codec; it does
not add a binary tool protocol. Custom templates use the public compiler CLI
with the same host SDK selection, qualified against 4.3.0-rc.2.

IDL also provides native plugin and Wasm projections. These are distinct
execution boundaries: a generated ABI does not grant filesystem/process
permissions or replace TurboAgent's tool policy, cancellation, quotas, and
side-effect journal. A Wasm artifact additionally needs an implemented core
module. This pack currently executes native handlers; it does not publish a
plugin or claim a generated Wasm implementation.

This replaces handwritten parameter schemas with one declaration, at the cost
of a build-time host tool and a private DataBind dependency. Rollback consists
of restoring the prior internal registration schemas; no stored session or
public configuration migration is involved.

## Workspace execution contract

`fs.search` accepts `{"path":".","pattern":"literal text"}`. Both fields are
required strings generated from `WorkspaceSearch` in the existing IDL. It searches
one file or recursively searches a directory for a case-sensitive, nonempty UTF-8
literal (up to 4096 bytes, without NUL/CR/LF). Each matching line appears once as
`{path,line,text}` with a workspace-relative path and a 1-based line number; LF and
its preceding CR are excluded. File order follows filesystem enumeration; matches
within a file follow line order. Hidden files are included; no gitignore or regex
semantics are implied.

Search is parallel-safe and read-only. A registry-owned binding retains the root
and immutable copied limits. Each synchronous invocation owns its generated
codec/request, one file buffer, traversal directory handles, and result tree.
Line views borrow the file buffer only until copied into JSON. No mutable state
is shared between searches, and registry destruction requires quiescence.
All opens use Salts root capabilities. Explicit link paths are rejected; links
and special entries encountered during recursion are counted and skipped.

The per-file byte limit is `min(max_read_bytes, 16 MiB)`; traversal and match limits each use
`max_list_entries`, capped at 10,000 entries and 1,000 matches respectively.
Returned path/text bytes share `min(max_result_bytes, 128 KiB)` (JSON framing
excluded). This leaves room for JSON escaping and at most 1,000 match envelopes
within the Agent's default 1 MiB serialized output budget. A host may impose a
smaller executor limit independently.
Each call additionally reads at most 16 MiB in total and respects the SDK's
64-component path depth. Oversized files are skipped; exhausted traversal,
read, match, depth or result budgets produce `truncated=true` with the bounded
matches collected so far. Binary/non-UTF-8 files are skipped and counted.
At the exact total read budget, the invocation checks the current size through
its already-open file handle without reading another byte. A fully read file
still goes through UTF-8/binary validation and line matching; unread bytes cause
truncation without publishing that file's partial contents. A failed size query
follows the existing I/O failure path. The invocation retains sole ownership of
the buffer/handle through validation, matching and cleanup; this size check does
not provide a snapshot against concurrent external writers.
`metrics` reports entries visited, files searched, bytes read, output bytes,
and skipped binary/oversized/special files. A limited result is not evidence that
an unreported match does not exist; narrow `path` to continue searching.

Cancellation/deadline is checked before access, between directory entries/read
chunks/lines, and before publication. It returns the existing non-OK tool status
with no partial normal result. I/O failures return an explicit failed result
without partial matches; allocation failures propagate OUT_OF_MEMORY. Every exit
closes handles and releases temporary trees/strings. Synchronous OS calls are not
forcibly interrupted. Concurrent external edits do not provide snapshot semantics.

This fills the read-only repository-search gap without a shell command, subprocess,
regex dependency or second schema. It adds `fs.search` to both existing packs and
the CLI, preserves existing tool argument/result shapes and configuration ABI,
and extends atomic registration rollback to include search. No persisted-data
migration is required; rollback removes its registration, handler and IDL message.
`test_turbo_coding_search` covers traversal, literal/line semantics, invalid
requests, binary skips, resource bounds and pre-cancelled/deadline calls. The
workspace suite covers link rejection and registration rollback; the CLI suite
invokes search through the read-only Agent loop and checks the returned content.

The additive workspace pack registers `fs.write`, `fs.edit`, and `shell.exec`
alongside the existing read-only tools. IDL owns their string parameters. Each
invocation owns its codec, decoded strings, staging file or process, and result.
Registry-owned bindings retain immutable limits and admitted filesystem roots.
The Agent runs mutations exclusively and marks them non-idempotent; callers of
the trusted registry API must serialize mutations themselves. Destroy requires
all invocations to have returned.

File contents are UTF-8 without NUL, bounded by `max_write_bytes` (default 1 MiB).
Edit reads at most that bound, requires exactly one nonempty match, checks the
replacement size, and publishes a same-directory staging file with root-relative
rename. Failed preparation leaves the target unchanged; staging cleanup is
attempted on every failure. A successful rename is the commit point. This is
atomic visibility, not fsync durability or compare-and-swap against an external
editor. One workspace writer must own the operation. New files use 0600; existing
permission bits are copied where the platform supports them, but replacement
does not preserve inode identity, extended attributes, ownership, or hard links.
Symlinks/reparse paths are rejected. Parent directories must already exist.

Edit borrows the execution context through its read, match, and publish phases.
The invocation owns the original/replacement text and closes the read handle
before handing that text to the matching phase. Cancellation/deadline is checked
between read chunks, after matching, between staging writes, and before rename.
An interrupted read discards its partial buffer and propagates CANCELLED or
DEADLINE_EXCEEDED with NULL output, without converting it into a file failure or
match conflict. Before rename, the target remains unchanged and any staging
file is removed; cleanup failures retain their existing explicit result. After
rename commits, a late cancellation cannot turn the committed write into an
uncommitted result. These checks are cooperative and do not interrupt a blocking
OS call. Tool schemas, limits, ordinary failure results and commit semantics are
unchanged.

`shell.exec` runs `cmd.exe /d /s /c` on Windows and `/bin/sh -c` on POSIX, with
inherited environment and workspace cwd. It has host privileges, including
filesystem and network access. Agent `shell` policy controls admission; cwd is
not an isolation boundary. Defaults: 64 KiB command, 128 KiB combined captured
output, 120 seconds execution. Stdin is closed. Cancellation is checked while
waiting and terminates the entire owned process tree before callback return.
Capture overflow and timeout are reported explicitly; output must be UTF-8
without NUL to be returned as text. A terminated process may already have made
external changes and must not be replayed automatically.

After an in-flight cancel/deadline, the invocation owner explicitly terminates
the process tree and waits up to 5 seconds for its terminal result before
copying the bounded capture into the existing result shape. Final destruction
still joins the process monitor; this is not a hard real-time shutdown bound.
The result has `ok=false`, status `cancelled` or `deadline_exceeded`, the actual
`process_state`/`exit_code`, and partial UTF-8 stdout/stderr and byte/time metrics.
Binary capture remains suppressed. Termination, wait, capture or allocation
failure propagates an error instead of publishing incomplete detail.

The existing `detail_sink` borrows this result for one synchronous callback,
after process destruction and batch-file cleanup. The Agent executor copies it
and commits it in `agent_tool_journal.detail` alongside the cancellation status;
it is host diagnostic data, not automatically projected into model history or
printed by the CLI. The normal return value remains NULL on non-OK status, and
pre-spawn cancellation emits no process detail. No extra queue or output limit
is introduced: capture uses `max_process_output_bytes`, and copying is bounded
by that capture plus fixed JSON metadata/escaping. External callers that retain
detail must copy it before the sink returns. Committed cancellation is replayed
as a result, never as another process launch; cancellation cannot undo effects.
This reuses the existing context ABI and journal field, requires no data
migration, and can be reverted by removing detail publication without changing
status handling or cancellation teardown.

Windows commands run from a uniquely created workspace batch file, removed
after process-tree teardown. This preserves embedded quotes across the CRT/cmd
argument boundary. The batch selects UTF-8 code page 65001 before execution.

The CLI owns the bounded executor, Harness, and registry in that order of
lifetime: stop/wait, release Harness, drain the executor, then free tools. It
uses the existing Agent/Session workflow and CHttp provider transport. Follow-up
input must preserve chronological model/tool history; it cannot simply append
users to the initial input array ahead of old events. No second conversation
store or agent loop is introduced. Reverting these additive entry points and
the CLI requires no stored-data migration.
