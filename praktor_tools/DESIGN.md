# Praktor Tool Pack Design

## Decision context

TurboAgent already owns model interaction, tool policy, approvals, durable
thread/run/checkpoint state, MCP/Wasm/native tool composition, and bounded tool
execution. Praktor owns a separate concern: deterministic YAML workflow
orchestration.

The integration must not create a second agent state machine or let model input
select arbitrary executable workflow files.

## Selected boundary

Each host-reviewed YAML workflow is registered as one ordinary TurboAgent tool.

```text
TurboAgent policy / review
          |
     tool admission
          |
  fixed tool definition
          |
  fixed absolute YAML path
          |
      Praktor ABI
```

The tool arguments are serialized directly as Praktor workflow inputs. The
workflow path is binding state owned by the registry callback and never appears
in the tool schema.

## Alternatives

### One generic run(path, inputs) tool

Rejected. It makes filesystem path selection model-controlled, weakens tool
admission, and collapses workflows with different side effects into one policy
identity.

### Make Praktor an Agent runtime

Rejected. It would duplicate TurboAgent thread/turn/checkpoint and policy state.
Praktor remains an execution backend only.

### Register one tool per reviewed workflow

Selected. Tool schema, identity, execution policy, and capability requirements
are fixed before inference, while the YAML retains full Praktor orchestration.

## Ownership and lifecycle

| Item | Contract |
|---|---|
| Pack | owns one tool registry and the linked Praktor API view |
| Workflow binding | registry-owned; copies the absolute YAML path |
| Tool schema/name/description | copied by the Tool Registry |
| Model arguments | borrowed for one callback, serialized to canonical JSON input |
| Praktor result | Praktor-owned until `release_json`; parsed before release |
| Returned JSON | TurboParser-owned by the caller after successful tool execution |
| Destruction | dependent agents/projections must be destroyed before the pack |

Praktor's ABI major and `PRAKTOR_CAPABILITY_JSON_WORKFLOW` are checked during
pack creation.

## Concurrency and side effects

The default execution policy is `EXCLUSIVE + NONE`. Hosts may explicitly
select another existing TurboAgent execution policy for a reviewed workflow.

The pack has no queue or worker pool of its own. Bounded scheduling,
cancellation boundaries, and tool journaling remain owned by TurboAgent's
existing tool executor.

## Capacity and result bounds

- `max_workflows` is a hard registration bound.
- `max_result_bytes` is checked before parsing a Praktor-owned result.
- Capacity failure is atomic and does not mutate the prior registry.

Praktor itself owns execution-time task concurrency and any workflow-internal
resource limits.

## Policy boundary

Every registered workflow always requires `runtime_tools`.

The default additional requirements are deliberately conservative:
`network`, `shell`, `patch`, and `outside_workspace`. This prevents a
newly registered Praktor workflow from silently bypassing TurboAgent policy
simply because the host forgot to classify its YAML effects.

A host may replace the additional list only after reviewing the workflow and
its transitive reusable workflows.

## Failure semantics

Praktor success and `PRAKTOR_RESULT_EXECUTION_FAILED` both carry canonical
JSON and are returned as normal tool output. This preserves task-level failure
evidence for the model and host.

Negative Praktor ABI errors are converted to a structured object containing
`workflow_status=error`, `result_code`, `error_phase`, and `error`.
Malformed or oversized adapter results fail the tool boundary.

## Build and compatibility

The module is guarded by `ENABLE_PRAKTOR_TOOLS`, default OFF. Existing
TurboAgent builds therefore remain unchanged. Enabling it requires an installed
Praktor CMake package and links the adapter privately to `Praktor::Praktor`.

No existing TurboAgent public ABI, runtime record, protocol, or tool changes.
Rollback is disabling the option and removing Praktor tool-pack registration.
