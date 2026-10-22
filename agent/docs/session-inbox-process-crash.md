# Session Inbox process-crash recovery

These native CTests exercise the real Session before-turn claim/message/event
path and SQLite Runtime Store. A fresh private directory with spaces is created
per case. Independent processes initialize counters, exit at a selected cut,
inspect persisted evidence, configure/recover the Inbox and reopen once more.
Only exact C11 `_Exit(86)` satisfies the crash stage; unrelated errors, missed
cuts and timeouts fail. Evidence is retained on failure. The custom CLI main
follows the existing process fixture pattern; CMake only orchestrates processes.

| Cut | Observed model entries | Durable owning-Run event | Raw transition | Expected recovered state |
| --- | ---: | ---: | --- | --- |
| after_claim | 0 | 0 | claimed | queued |
| in_model | 1 | 0 | claimed | queued |
| before_applied | 1 | 1 | claimed | applied |
| after_applied | 1 | 1 | applied | applied |
| before_delivery | 1 | 1 | applied | applied |
| after_following_run | 1 | 1 | claimed | applied |

The model fixture receives the canonical Inbox message through the actual
Session hook, records an independent committed observation and returns a fixed
offline Responses reply. It invokes no paid model or tools. The in-model cut
occurs before returning a model response. Application delivery remains zero in
every crash/recovery case; APPLIED means the input has a persisted matching event,
not that a task, model answer, tool effect or external delivery succeeded.

The following-Run case withholds applied-transition writes, then explicitly
completes a second no-op graph whose state contains no Inbox event. Both Session
finalizers encounter the injected write failure. The original Run retains the
actual input event while the thread head advances. Recovery must not forget that
event merely because it is absent from the newer head.

The native verifier checks immutable payload, latest transition sequence/status,
claiming Run identity, actual Run/head states, matching applied-event identity and
independent model/delivery counters. Recovery must not invoke a model or launch a
workflow. Queued inputs can be explicitly claimed/requeued; applied inputs cannot
be claimed again. Another process checks the recovered state remains stable.

The compiled Inspect Session-finalization unit fixture also uses a real Session
Run to create/bind its Inbox event. It does not bind a fabricated event that was
never added to Run state. Existing authority and canonical typed-result recovery
assertions are retained.

Run the normal native presets after building the full graph:

```sh
ctest --preset ci-linux-release -R 'test_session_inbox_crash_|test_turbo_agent_harness_compiled' --output-on-failure
ctest --preset ci-windows-release -R 'test_session_inbox_crash_|test_turbo_agent_harness_compiled' --output-on-failure
```

This is not automatic workflow resumption, tool-effect reconciliation, a durable
authority epoch, concurrent Inbox ownership, arbitrary OS kill, mid-transaction
interruption, power-loss qualification or a globally exactly-once delivery
protocol. Policy setters remain quiescent. Linux/Windows native process behavior
requires actual CTest evidence; SDK-only macOS/Android jobs do not qualify it.
Issues #74 and #213 remain open for the remaining recovery/admission matrix.
