# Praktor Tool Pack Design

## Decision context

TurboAgent owns model interaction, tool policy, approvals, durable
thread/run/checkpoint state, tool journaling, and bounded tool execution.
Praktor owns deterministic YAML workflow orchestration.

The integration must not create a second agent state machine or let model input
select executable workflow files.

## Selected boundary

Each host-reviewed YAML workflow is registered as one ordinary TurboAgent tool.

With a WorkflowPlan-capable Praktor SDK, registration compiles the YAML into an
immutable/revalidatable plan and binds the plan identity to the tool:

```mermaid
flowchart LR
  H[TurboAgent policy / review] --> A[Tool admission]
  A --> P[Praktor WorkflowPlan]
  P --> D[Digest + dependency closure]
  P --> S[Generated input schema]
  P --> E[Effect manifest]
  P --> Q[Harness-safe profile]
  D --> T[Fixed tool identity]
  S --> T
  E --> T
  Q --> T
```

The workflow path is host registration state and never appears in model
arguments. PraktorTools now requires the released Praktor HostTool baseline
(ABI 2.5+); older SDKs fail fast instead of selecting a source/compatibility
fallback. Workflow-config v1 callers retain their reviewed path execution
semantics inside that modern SDK baseline.

## Alternatives

### One generic run(path, inputs) tool

Rejected. It makes filesystem path selection model-controlled and collapses
workflows with different side effects into one policy identity.

### Make Praktor an Agent runtime

Rejected. It would duplicate TurboAgent thread/turn/checkpoint and policy state.
Praktor remains an execution backend only.

### Register one tool per reviewed workflow

Selected. Tool identity, schema, execution policy, capability requirements, and
reviewed plan are fixed before inference.

## Ownership and lifecycle

| Item | Contract |
|---|---|
| Pack | owns one tool registry and the linked Praktor API view |
| Workflow binding | owns an immutable WorkflowPlan for v2/v3 configs; v1 retains the reviewed absolute-path contract |
| Tool schema/name/description | copied by the Tool Registry |
| Model arguments | borrowed for one callback and serialized as workflow inputs |
| Praktor full result | published to Tool Execution Context v2 detail sink, then released |
| Model-facing result | `agent_output` when provided by Praktor; canonical result is the legacy fallback |
| Lifecycle events | translated from Praktor events into canonical Turbo trace events |
| Destruction | releases the WorkflowPlan before destroying the binding |

## Execution control and observability

TurboAgent Tool Definition v4 is context-aware. Tool Execution Context v2
carries:

- cancel token;
- monotonic deadline;
- thread/run/turn/tool-call lineage;
- canonical event sink;
- full-detail sink.

The adapter maps cancel/deadline into Praktor execution control. When Praktor
exposes lifecycle events, it calls observed WorkflowPlan execution and forwards
lineage without injecting those values into workflow variables.

```mermaid
sequenceDiagram
  participant H as Harness
  participant A as Praktor Adapter
  participant P as Praktor Runtime
  participant J as Tool Journal
  participant M as Model

  H->>A: tool call + execution context
  A->>P: execute reviewed plan
  P-->>H: workflow/task trace events
  P-->>A: canonical result
  A-->>J: full result via detail sink
  A-->>M: agent_output projection
```

The Tool Executor serializes event-sink delivery across parallel tool workers
and persists backend detail in the durable tool journal. Committed replay
restores compact output and detail without re-running side effects.

## Policy boundary

Every workflow requires `runtime_tools`.

With WorkflowPlan metadata, Praktor effects map conservatively into TurboAgent
policy capabilities:

| Praktor effect | TurboAgent capability |
|---|---|
| `network` | `network` |
| `process`, `system_control` | `shell` |
| `filesystem_write` | `patch` |
| `outside_workspace` | `outside_workspace` |
| `plugin`, `native_extension`, `model_api` | `custom_tools` |
| `filesystem_read` | no additional capability |

Unknown effects widen to the conservative set plus `custom_tools`.

There is one intentionally narrower resolution rule for Praktor HostTool plans:
if every unknown reason is exactly a frozen HostTool identity, every identity is
present in a compiler-approved RuntimeTools projection, and the raw
`harness_safe` profile has no rejection reason other than those HostTool
unknowns, TurboAgent resolves only that `host_tool` uncertainty. The workflow
capability set then unions the real required capabilities of every projected
RuntimeTool. Any other unknown reason remains fail-closed.

Host-supplied capability requirements are unioned with discovered/resolved
effects and therefore cannot narrow the analysis.

Without WorkflowPlan support, the legacy conservative set remains
`network + shell + patch + outside_workspace`.

## Harness-safe profile and HostTool authority

Workflow config v2 introduced `require_harness_safe` and keeps it enabled by
default. Workflow config v3 adds the borrowed `approved_host_tools`
RuntimeTools projection.

For ordinary plans, registration still requires
`profiles.harness_safe.qualified=true`. For HostTool plans, raw Praktor
metadata intentionally reports `unknown_effects=true`; TurboAgent may treat
the plan as harness-safe only after the HostTool-only resolution rule above
proves that every frozen HostTool identity is inside the approved projection.

```mermaid
flowchart LR
  C[AgentCompiler admission] --> P[approved RuntimeTools projection]
  W[Reviewed WorkflowPlan] --> V[Praktor HostTool preflight]
  V --> B[TurboAgent HostTool bridge]
  P --> B
  B --> R[RuntimeTools execute-with-context]
  R --> N[Native]
  R --> X[WasmToolPack / TurboWasm]
  R --> M[MCP / remote]
```

The projection is borrowed execution authority, not a discovery registry.
Praktor receives no access to the full RuntimeTools catalog and performs no
backend-specific Native/Wasm/MCP selection.

At invocation, unresolved templates are not value-schema-validated during
preflight because placeholders may not yet have their final scalar type.
After Praktor resolves the template, TurboAgent runs the canonical RuntimeTools
schema validator immediately before dispatch.

Workflow config v1 remains accepted and retains its reviewed-path execution
semantics; v2 retains WorkflowPlan semantics unchanged.

## Result semantics

Praktor success and workflow-level failure are normal tool results.

For harness-native workflows, the canonical result is split:

- full `tasks` and diagnostic detail -> detail sink / durable journal;
- declared public `agent_output` -> model-facing tool result.

This avoids feeding raw task stdout/stderr to the model unless the workflow
explicitly exports it.

Negative request/plan/contract ABI errors are converted to structured error
objects containing `workflow_status=error`, result code, error phase, and
message. Plan mismatch is detected before workflow side effects.

## Capacity and compatibility

- `max_workflows` is a hard registration bound.
- `max_result_bytes` bounds the canonical Praktor result before projection.
- Tool Execution Context ABI v1 remains accepted; v2 adds observation sinks.
- Workflow config ABI v1 remains accepted; v2 adds harness-safe admission; v3
  adds borrowed approved HostTool authority.
- Praktor ABI 2.5+ / `PRAKTOR_CAPABILITY_HOST_TOOL` is the fail-fast baseline
  for PraktorTools.
- No older-SDK, source-checkout, SHA, or package-version fallback is used.

The module remains optional behind `ENABLE_PRAKTOR_TOOLS`.
