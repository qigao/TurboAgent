# Agent execution ownership and refactoring boundary

The core execution remains C11. RuntimeData owns the transport-independent event
schema and conversion functions (`turbo_event.h`); Provider links RuntimeData,
not the durable graph/store Runtime. Runtime publicly links RuntimeData so source
consumers retain the event API. Rebuild native consumers with this SDK: event
exports now belong to RuntimeData rather than Runtime. No record schema changes.

## Construction

Production hosts can use `turbo_agent_session_create_with_store` or
`turbo_agent_app_create_with_store`, or
`turbo_agent_harness_create_with_store` to reject incomplete backend configuration.
These APIs validate all three required callbacks before consuming resources,
clear their output on rejection, and never select a substitute backend.
An explicitly supplied memory backend is valid; callback completeness is not a
crash-durability claim. Select and qualify the backend separately.

The existing pointer-returning constructors preserve their documented memory
fallback for source compatibility. Once a complete backend is selected, Session
consumes RuntimeStore even if subsequent construction fails. MemoryStore only
transfers on success. App allocation failure before Session construction leaves
both stores with the caller. Harness allocates its stable thread scope before
App construction, so no fallible allocation remains after Session success.
Agent construction rejection maps to SALTS_EIO;
the underlying pointer-returning Agent constructor cannot distinguish its causes.

## Mutable state

`turbo_agent_call_state_t` owns provider request metadata and captured SSE bytes.
`turbo_agent_projection_state_t` owns the Context-published summary projection,
which survives individual provider calls. They are separate from configuration
fields in the opaque Agent implementation. Lifecycle cleanup releases both.
This is an internal ownership split, not a concurrent Agent API. Agent configuration,
HTTP transport, hooks and projections still require one exclusive execution lane.
Router's rejection of shared Agent/Runtime endpoints remains in force. A Session
and its Context use the same lane. Shared Host tool execution retains its existing
serialization and approval ownership; ToolExecutor batch parallelism is separate.

For tool execution guarantees see [the admission matrix](../../agent_compiler/BOUNDARY.md#host-selection-of-execution-guarantees).

## Job coordination boundary

The report example remains application composition. Its report-specific gate and
manifest schema must not become the generic Job schema. The proposal/binding
foundation in [Job schema v1](job-proposal-coordinator.md) belongs above
Router/Session and stores only stable compiled-attempt scope. The bound Job
execution adapter selects those persisted inputs and returns canonical attempt,
compiled execution journal and Runtime Run references, without another result
payload authority.

A minimal durable Job implementation needs a frozen versioned envelope, immutable
proposal versions, attempt references, acceptance evidence, and delivery outcome.
Its state machine must distinguish unresolved dispatch/delivery from confirmed
failure. One coordinator owns transitions; atomic create-if-absent can claim a
single transition but is not a multi-record transaction. Recovery validates exact
references and never retries an unknown external effect automatically.
The initial installed Job API covers only immutable goals, proposals and a stable
compiled-attempt binding, with its own publication/recovery tests. Acceptance,
delivery and execution supervision remain future state-machine work.
Real-model quality/cost qualification remains
tracked independently in issue #278.
