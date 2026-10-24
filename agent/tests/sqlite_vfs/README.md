# SQLite VFS Tool Journal recovery conformance

This test-only fixture runs the real ToolExecutor over the unchanged production
SQLite Store source. A forwarding VFS wraps the native `unix`/`win32` file
implementation. The phase-aware Store adapter only arms the VFS; it always calls
the Store and returns its actual status. The existing Store-level fault tests
remain independent.

The Store source is compiled into this fixture with its two public entry names
renamed. This makes VFS registration and Store I/O use the same SQLite instance
even when the Runtime DLL and the executable each statically link SQLite on
Windows. No test hook, public ABI field or Journal schema change is shipped.

## Cases and expected disk evidence

For both STARTED and COMMITTED persistence, the suite includes a passing control
and one-shot failures of:

| Native file | Operation | SQLite error |
| --- | --- | --- |
| Rollback journal | `xWrite` | `SQLITE_FULL` |
| Rollback journal | `xWrite` | `SQLITE_IOERR_WRITE` |
| Rollback journal | `xSync` | `SQLITE_IOERR_FSYNC` |
| Main database | `xWrite` | `SQLITE_IOERR_WRITE` |
| Main database | `xSync` | `SQLITE_IOERR_FSYNC` |

These original failures are returned before the selected native operation. Each
immediately disarms itself, allowing SQLite's actual rollback I/O and later writes/syncs.
File flags and the canonical target path restrict injection to this Journal
database or its rollback journal. The external effect counter uses a separate
SQLite database opened explicitly through the native VFS.

| Failed transition | Original callback count | Actual Journal phase after rollback | Admission fence | Restart result |
| --- | --- | --- | --- | --- |
| STARTED | 0 | `planned` | `admitted` | replayed UNKNOWN; 0 callbacks |
| COMMITTED | 1 | `started` | `admitted` | replayed UNKNOWN; 0 callbacks |
| Passing control | 1 | `committed` with exact result | `admitted` | replayed result; 0 callbacks |

The admitted fence means that a failed STARTED write does **not** authorize an
automatic retry, even when this test oracle proves no callback occurred.

One private directory with spaces is created per case. Separate processes run
initialization, injection, native disk inspection, recovery, inspection, another
replay, and final inspection. Native inspection bypasses Runtime/Store readers
and the injected VFS. It checks `integrity_check`, the actual Journal and claim
identity/phase/output, the untouched sentinel and the independently committed
effect counters. Both fresh Runtime/Executor owners must return without another subject
callback or Journal rewrite. A distinct identity also commits on the original
connection after the one-shot fault, proving that I/O resumed.

## Partial writes, errors after native success and in-VFS process cuts

The partial-write/process-cut slice added 36 cases (18 per persistence phase): the original six,
six new reported-error cases and six process cuts. The new reported errors are
partial journal/database writes, full journal/database writes followed by an
error, and successful journal/database syncs followed by an error. A partial
write changes a contiguous half of the requested bytes containing at least one
byte different from the old file; identical writes do not consume the fault.
The actual native file is read back after each selected partial/full write,
before returning the error or terminating the process. Counts, offsets and
changed/read-back bytes are recorded; no payload is merely pretended written.

The six cuts terminate with exact `_Exit(86)` before/after the first selected
journal/database `xSync`, or after a partial journal/database write. The child
must emit the matching phase/case and VFS-cut markers. A returned I/O error,
unrelated failure or different exit status cannot satisfy the crash test.
These cuts skip SQLite rollback and all destructors. They occur before the
transaction commit point, including after database sync but before journal
invalidation, so the interrupted transition must roll back.

Before recovery, CMake copies and hashes the database/rollback-journal bytes
without opening SQLite. No helper repairs the files first: the production Store
is the first database opener after the cut. For database-write/sync cuts its VFS
must observe actual main-file recovery writes and syncs during Store open.
The original subject returns replayed UNKNOWN with zero new subject callbacks.
The recovery process then admits a distinct healthy identity; a second process
replays both. Independent native read-only inspection verifies old phase,
admitted fence, integrity and effect counters after each restart.

## Actual transaction commit boundary

Eight further cases bracket journal invalidation with an I/O error or exact
process exit, before and after each STARTED/COMMITTED transaction. The current
exclusive Store retains its rollback journal and commits by zeroing its header.
The VFS distinguishes this from initial header creation by reading and requiring
the valid eight-byte journal magic before selecting the zero-header write.
After native success it reads the actual zeroed bytes back. Other commit styles
(delete or truncate) are not silently treated as covered by this selector.

| Interrupted persistence | Side of invalidation | Actual Journal phase | Subject effects | Restart result |
| --- | --- | --- | --- | --- |
| STARTED | Before | `planned` | 0 | UNKNOWN |
| STARTED | After | `started` | 0 | UNKNOWN |
| COMMITTED | Before | `started` | 1 | UNKNOWN |
| COMMITTED | After | `committed` + exact original output | 1 | Exact committed result |

Reported-error cases initially return UNKNOWN and withhold output, even after
actual commit. UNKNOWN at that caller boundary must not overwrite a canonical
committed record. Same-handle replay and two independent restarts re-read the
record; after COMMITTED invalidation they return its original success and output,
without a subject callback or Journal rewrite. The admitted fence remains intact.

Crash tests independently check the raw pre-recovery journal header: valid magic
before invalidation, eight zero bytes afterwards. The first production Store
reopen must perform recovery writes/syncs before the boundary, and none after it.
Native disk inspection then verifies phase, status, identity, output and external
effect counts. These cases distinguish a lost commit acknowledgement from an
uncommitted transaction with an unknown effect.

## Interrupting hot-journal recovery itself

Fourteen cases seed a real hot journal by exiting after the original database
sync, then interrupt production Store open twice in independent processes.
Per persistence phase, the seven profiles are recovery-write before/after,
recovery-sync before/after, partial recovery write followed by a sync cut,
and persistent write/sync errors until failed-open cleanup completes.

The VFS is armed before the production Store opens; no Runtime/Executor or
helper SQLite connection repairs the primary database first. Exact exit codes,
recovery-arm markers and native operation/mode markers are required for cuts.
A partial first attempt can restore all changed bytes, so its second cut occurs
at sync instead of assuming another changed partial write remains available.

Persistent-error cases require `SALTS_EIO`, every output Store callback/context
cleared (including preexisting sentinels), all VFS files closed and successful
VFS removal. The chosen write/sync keeps failing through connection destruction;
it does not turn into a one-shot success while SQLite cleans up.

After each failed attempt, raw journal magic and its full SHA-256 must match the
original hot journal. Raw database/journal snapshots are kept on failure. Separate
processes inspect only the external effect database, requiring unchanged subject
counts and no healthy effect yet; they cannot repair the primary database.
After both failures, a clean production Store open must finish recovery. The
original subject still replays UNKNOWN without another callback, a distinct
healthy identity commits once, and a final owner replays both. Native inspection
checks integrity, exact prior phase/admitted fence, identities and effect counts.

## Running and scope

Normal native `BUILD_TESTS=ON` builds include all 58 CTests with label
`sqlite-vfs`. `TURBOAGENT_SQLITE_VFS_CONFORMANCE=ON` builds only this fixture when
the full test suite is disabled. The macOS SDK profile uses that option and runs
`ctest --preset ci-macos-sqlite-vfs --verbose --no-tests=error`; Linux and Windows
run the cases as part of their full suite. Cross-compiled Android does not run
this host-native fixture.
The 14 recovery-restart cases also have label `sqlite-recovery-restart`.

Failures retain their database directory and report the failing stage. Logs
record the native VFS name, fault count/error/file flags, write offset/length,
post-fault writes/syncs and callback/effect counts.

This qualifies these reported synchronous I/O errors and controlled partial
writes/process exits while the OS and its caches remain running. It does not
simulate device power loss, cache loss/reordering, arbitrary torn-write patterns,
random kill positions, permanent device failure, WAL or every commit style/I/O
position. Issue #74 remains open for that wider matrix.
SQLite interface references: [VFS](https://www.sqlite.org/c3ref/vfs.html),
[file methods](https://www.sqlite.org/c3ref/io_methods.html),
[result codes](https://www.sqlite.org/rescode.html).
Commit/recovery reference: [atomic commit](https://www.sqlite.org/atomiccommit.html).
