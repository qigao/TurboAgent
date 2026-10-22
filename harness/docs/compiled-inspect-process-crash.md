# Compiled Inspect process-crash qualification

Issue #74 includes recovery gates beyond the synchronous read-only Inspect slice.
These nine native CTests extend the [Direct Journal process tests](../../agent/docs/tool-journal-process-crash.md)
to the actual public Harness attempt API, compiler, Session Run and bounded SQLite
Runtime Store. They do not introduce a recovery service or broaden Inspect effects.

## Process and ownership

CTest runs a CMake driver which creates a fresh private directory with spaces in
its name. It launches independent native fixture processes for initialization,
crash, evidence inspection, denied authority, changed plan, stale generation,
explicit recovery and another replay. The absent-attempt case skips plan/stale
checks because an absent identity can legitimately admit a new plan or generation.
Each child has a ten-second deadline and each test a ninety-second deadline.

Only exact exit 86 from C11 `_Exit(86)` satisfies the crash stage. This skips all
Harness, Session, Runtime, thread-pool and SQLite destructors and exit handlers.
Loader errors, unrelated failures, timeouts and missed cuts fail. New processes
open the existing database only after the previous process exits. The driver
removes only its newly created private directory on complete success; failures
retain evidence. No source markers or stdout strings establish success.

The C fixture checks real returned statuses, persisted JSON and independent
counters. Its custom CLI main follows the existing Agent/coding process fixtures;
ordinary unit suites continue to use TinyTest. CMake only orchestrates processes.
The target links `TurboAgent::Harness` through public interfaces, runs under native
`BUILD_TESTS`, and is excluded from cross compilation. No workflow, dependency pin,
public API, production execution or schema changes are needed.

`compiled_inspect_attempts` is the sole canonical typed-result owner. A thin store
adapter delegates to real SQLite `create_if_absent`/`put` and exits immediately
before or after the selected successful operation. Every Run-journal record must
reference the same attempt and plan, and contain no second `result` payload.

The registered tool remains sequential READ/READ_ONLY with `runtime_tools` and a
strict result schema. A separate SQLite database counts callback observations and
validated application deliveries. The callback counter is test instrumentation,
not a business WRITE effect or permission to admit mutating Inspect tools. No model
call is needed; any attempted model transport fails the test.

## Expected crash evidence

| Cut | Canonical attempt | First Run reference | Observed callbacks | Explicit recovery |
| --- | --- | --- | ---: | --- |
| before_claim | absent | absent | 0 | One invocation under current Host grant |
| after_claim | started | absent | 0 | UNKNOWN, no invocation |
| before_run_started | started | absent | 0 | UNKNOWN, no invocation |
| after_run_started | started | started | 0 | UNKNOWN, no invocation |
| in_callback | started | started | 1 | UNKNOWN, no invocation |
| before_committed | started | started | 1 | UNKNOWN, no invocation |
| after_committed | committed | started | 1 | Replay canonical typed result |
| after_run_committed | committed | committed | 1 | Replay canonical typed result |
| before_delivery | committed | committed | 1 | Replay canonical typed result |

`in_callback` exits after the independently committed invocation observation and
before constructing its result. `before_delivery` exits after the synchronous
Harness call returns a validated result, before the application records delivery;
it is not an injected policy update or internal Harness assignment hook.

The first restart inspects this exact evidence with delivery count zero. It
checks thread/attempt/tool/plan identity, schema, READ effect, generation, status,
certificate and canonical result presence or absence. Unknown is conservative:
even the STARTED cuts with zero observed callbacks must not redispatch. Fresh
recovery Runs may add their own references; they never rewrite the canonical
STARTED attempt into a claimed success.

## Current authority and replay

Every executing process explicitly configures its own current Host grant before
calling the API. No grant or generation is restored from the database. The
fixture's identically initialized Agents happen to use the same local generation
ordinal; that equality is not a cross-process authority identity or a substitute
for fresh Host admission. This suite does not establish a durable policy epoch.

A denied current Host policy must reject before any attempt read/write, Run
reference write, callback or result delivery. For an existing attempt, valid but
different arguments change the frozen plan and must fail EPROTO with no canonical
overwrite. A separate process applies an identical allowed policy twice while
quiescent, verifies equal capability masks but different generations and a stale
snapshot, then proves the old attempt cannot run or replay. All setters finish
before execution starts; no store/tool/transport/event callback mutates policy.

An absent attempt can execute once under the current explicit grant. STARTED
returns EBUSY/UNKNOWN_SIDE_EFFECT without a result or callback; repeated recovery
stays unknown. COMMITTED returns the canonical typed result, passes the current
strict result contract and does not enter the callback. Only explicit successful
recovery records one application delivery. Another independent replay validates
the result without recording a second delivery. This is an oracle, not a globally
exactly-once delivery protocol or automatic settlement loop.

## Verification and remaining gates

Run the normal native presets from the repository root, for example:

```sh
ctest --preset ci-linux-release -R '^test_compiled_inspect_crash_' --output-on-failure
ctest --preset ci-windows-release -R '^test_compiled_inspect_crash_' --output-on-failure
```

CI must still build/test the full configured graph. Linux/Windows qualify these
process tests; macOS/Android SDK-only jobs do not qualify their runtime behavior.
This suite covers specified API/store boundaries, not arbitrary OS kill,
mid-SQLite-transaction interruption, power loss, concurrent same-identity callers,
nested/forked ownership, interrupted Inbox claim recovery, remote takeover or the
Wasm/MCP/Praktor/CMeta-CFlow matrix. The original crashed Run and application
delivery remain separate facts; replay uses a new explicit Session Run, not raw
Runtime restoration. Issues #74 and #213 remain open for their broader gates.

Rollback removes these test registrations, fixture, driver and documentation;
there is no database migration or production behavior to undo.
