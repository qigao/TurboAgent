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

Each failure is returned before the selected native operation. It immediately
disarms itself, allowing SQLite's actual rollback I/O and later writes/syncs.
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
effect counters. Both fresh Runtime/Executor owners must return without another
callback or Journal rewrite. A distinct identity also commits on the original
connection after the one-shot fault, proving that I/O resumed.

## Running and scope

Normal native `BUILD_TESTS=ON` builds include all 12 CTests with label
`sqlite-vfs`. `TURBOAGENT_SQLITE_VFS_CONFORMANCE=ON` builds only this fixture when
the full test suite is disabled. The macOS SDK profile uses that option and runs
`ctest --preset ci-macos-sqlite-vfs --verbose --no-tests=error`; Linux and Windows
run the cases as part of their full suite. Cross-compiled Android does not run
this host-native fixture.

Failures retain their database directory and report the failing stage. Logs
record the native VFS name, fault count/error/file flags, write offset/length,
post-fault writes/syncs and callback/effect counts.

This qualifies reported synchronous I/O errors followed by normal close and
independent reopen. It does not simulate torn writes, partial writes, arbitrary
process kill, power loss, permanent device failure, WAL, after-success errors,
or all possible I/O call positions. Issue #74 remains open for that wider matrix.
SQLite interface references: [VFS](https://www.sqlite.org/c3ref/vfs.html),
[file methods](https://www.sqlite.org/c3ref/io_methods.html),
[result codes](https://www.sqlite.org/rescode.html).
