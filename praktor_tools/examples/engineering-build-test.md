# Reviewed engineering build/test workflow

This Host example implements the engineering workflow slice proposed in [#225](https://github.com/qigao/TurboAgent/pull/225).
The reviewed YAML calls five fixed HostTools: request check, build, build-result check, test and test-result check.
The three pure checkers in `engineering_workflow_checks.h` are real implementations. The embedding Host supplies
`engineering.build` and `engineering.test`; the offline tests supply fixtures for these two backends only.
This example does not compile a real project by itself or add a default CLI tool or installed public API.

## Contracts

| Tool | Input | Result required by this workflow |
| --- | --- | --- |
| `engineering.build` | `preset`, `source_revision` | Integer `exit_code`, actual `source_revision`, opaque `artifact_id` |
| `engineering.test` | `preset`, `source_revision`, build's `artifact_id` | Integer `exit_code`, actual `source_revision`, actual `artifact_id`, integer `tests_total/passed/failed/skipped` |
| Three native checkers | Resolved request or tool evidence | Boolean `true` only when its check passes; failure stops dependent work |

The request admits only `debug` or `release`, with a source revision of 1–64 ASCII letters, digits, underscores or hyphens.
Artifact IDs use the same 64-byte identifier bound; they are Host-resolved handles, not model-provided paths.
Full JSON byte lengths are checked before C-string comparison; embedded NUL and control/path syntax are rejected.
Counts are bounded to 1,000,000. This example's fixed completion criterion is: positive total, all tests passed,
zero failed/skipped, exit codes zero, and matching source/artifact identities. Expected optional skips need a separately
reviewed criterion; this profile never labels skipped tests as passed.

The Host's backend must enforce the selected workspace, immutable source snapshot/preset mapping, artifact identity,
process argument/environment limits, deadlines, cancellation and output limits. A preflight revision check is not an
atomic workspace snapshot. Recheck relevant preconditions at the backend's actual commit boundary. Report real exits,
counts and identifiers; tool descriptions or a model's proposed revision do not establish them.
Use structured process arguments or reviewed commands selected by the Host. No workflow, command or plugin path comes from the model.

## Host composition

1. Explicitly register the two build/test tools, including accurate schemas, effects, capabilities and borrowed implementation lifetimes.
2. Call `engineering_register_checks()` on that quiescent registry. Its three additions roll back if registration fails.
3. Project exactly the five named tools using `turbo_tool_registry_project()`.
4. Create a Praktor tool pack with `max_workflows=1` and `max_result_bytes=65536` for this example.
5. Call `engineering_register_workflow(pack, reviewed_absolute_yaml_path, projection)`.
   It requires WorkflowPlan and HostTool support, enables harness-safe qualification and registers the stable name `engineering_build_test`.
   Unsupported SDK profiles are rejected; this helper does not select the adapter's legacy reviewed-path behavior.
6. Select the Skill against the pack registry under the Host's grant. Keep the pack, source/projection registries and
   every borrowed dependency alive and immutable until dependent executions, selections and Agents have drained and been destroyed.

The helper requires `runtime_tools`, `shell` and `patch`; additional requirements derived from the HostTools remain mandatory.
These requirements do not grant permission or provide hard process isolation. The Host supplies the real authorization
boundary and approvals required for its effects. Policy replacement remains quiescent, including callbacks and result delivery.

Skill content for a dedicated Host-configured workspace:

```markdown
---
name: build-and-test
description: Build and verify a Host-selected engineering target
tools: [engineering_build_test]
capabilities: [runtime_tools, shell, patch]
---

Use only Host-selected presets and source revisions. Call the registered macro
with structured parameters. Report build/test evidence and incomplete work.
Declare completion only after Host verification of the actual tool result.
```

The workflow owns its input/output contract; the adapter consumes its generated schema.
There is no second YAML parser or executor. Tasks have explicit dependencies and no retries or model calls.
WorkflowPlan pins the reviewed root and dependencies; changed workflow bytes fail before build/test callbacks.

## Completion and recovery

Call `engineering_workflow_verified(actual_tool_result, bound_request)` before declaring completion.
Praktor's existing adapter may return `TURBO_TOOL_OK` together with a structured workflow failure.
The helper checks workflow status, verification output, requested preset/revision, artifact identity and actual counts again;
successful transport or a model-generated summary is insufficient. Use the original frozen request and canonical result.

Compact outputs support model reasoning. Full Praktor task evidence goes through the existing detail sink/journal;
the example adds no result database. Cancellation may leave a completed build while preventing tests, and does not undo effects.
There is no persistent multi-step claim, approval wait or crash recovery protocol here. Unknown effects must be reconciled
before a new attempt; this macro is exclusive and non-idempotent, with no automatic whole-workflow replay.
This is an ordinary registered macro-tool path, not a compiled Harness Inspect or compiler-qualified write workflow.

`test_engineering_workflow.c` executes the shipped YAML with offline HostTool fixtures: successful evidence and lineage,
failed/stale/malformed build results, failed/skipped/empty/inconsistent test counts, changed revision/artifact IDs,
invalid and NUL-containing requests, pre-start/in-build cancellation, changed YAML, missing authority, Skill grants,
and delivery-summary validation. It runs through the normal PraktorTools CTest graph when `ENABLE_PRAKTOR_TOOLS=ON`.
The current CI enables Praktor runtime tests on Linux; Windows keeps Praktor disabled, and macOS/Android qualify SDK builds.
These fixtures do not qualify a real compiler, remote service, sandbox or cross-process recovery backend.
