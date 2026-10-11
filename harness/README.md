# Harness host boundary

Link `TurboAgent::Harness` for the embedded App/Session host adapter,
transport-neutral server, and JSON-lines transport. Public header names remain
`turbo_agent_harness.h`, `turbo_agent_harness_server.h`, and
`turbo_agent_harness_transport.h`.

The library depends on Agent's public interfaces, including
`turbo_agent_session_execution.h` for asynchronous start/resume/fork submission.
It does not include Agent-private headers or own durable state. Session completes
summary synchronization and inbox work before Execution publishes its result.
Harness retains its App and graph until the operation's terminal notification,
even when callers release their handles early.

Existing APIs, admission limits, event framing, replay, cancellation, and deadline
semantics are preserved. Rebuild hosts and deploy the complete SDK because the
embedded adapter's function exports have moved from Agent into Harness.

See the [lifecycle design](../agent/docs/harness-adapter.md),
[server protocol](../agent/docs/harness-server.md), and
[module migration](../agent/docs/module-separation.md#harness-and-session-submission-boundary).
