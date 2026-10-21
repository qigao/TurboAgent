# Workspace instructions, skills, and tool projection

## Decision

TurboAgent uses `turbo_tool_registry_t` as the only executable tool fact source.
`AGENTS.md` and skill Markdown files are instruction inputs: they may select
registered tools, but they never become executable callbacks themselves.

The workspace layer has four responsibilities:

1. Resolve `AGENTS.md` from the workspace root to the configured working
   directory. Parent instructions are appended first, so closer scopes appear
   later in the effective instruction text.
2. Discover bounded skill files under `skills/`. A modern skill is
   `<skill>/SKILL.md` with YAML frontmatter; direct `skills/*.md` files remain
   compatible as instruction-only skills.
3. Select skills using explicit `triggers`, `$skill-name`, or
   `#skills/<filename>` references.
4. Validate declared capabilities and tool names before building a non-owning,
   ordered registry projection for one task. Required capabilities remain on
   the projected registry and are checked again at dispatch time.

This separation prevents Markdown content from acquiring process, filesystem,
network, plugin, or Wasm authority merely by being present in a workspace.

## Skill format

TurboParser parses YAML frontmatter and produces the JSON value tree used by
the workspace layer. There is no second YAML/JSON representation.

```yaml
---
name: repository-review
description: Review repository correctness and regression risk
triggers: [review, code review, 审查]
tools: [fs.read, fs.list]
capabilities: [custom_tools]
---

Read relevant implementations, callers, and tests before reporting findings.
```

`name` and `description` are required for frontmatter skills. `triggers`,
`tools`, and `capabilities` accept either one string or an array of strings.
Supported capabilities are `custom_tools`, `runtime_tools`, `wasm`, `delegate`,
`network`, `shell`, `patch`, and `outside_workspace`. Unknown values are parse
errors, not silently ignored extensions.

## Ownership and state

- `turbo_agent_workspace_t` owns copied configuration, resolved instruction
  text, skill metadata, and skill bodies. It is single-threaded.
- `turbo_agent_workspace_refresh()` constructs a complete replacement snapshot
  and swaps it only after every file parses successfully. A failed refresh
  leaves the previous snapshot usable.
- `turbo_agent_workspace_selection_t` is immutable after preparation. It owns
  effective instructions and the projected registry.
- A projected registry copies definitions and schemas but borrows callback user
  data. The source registry, projected definitions, and callback dependencies
  must remain alive and registered until the selection or derived agent is
  destroyed.
- `turbo_agent_create_for_workspace()` transfers selection ownership to the
  created agent. Destroy the agent before destroying or mutating its source
  registry.

## Capability policy

Skill `capabilities` are checked against `turbo_agent_policy_t`. Tool-specific
classification may come from versioned registry metadata or be augmented with
`turbo_agent_workspace_tool_capability_t`. Requirements are combined with AND
semantics. Legacy tools without metadata require `custom_tools`; unknown
metadata fails closed. This avoids guessing authority from a tool name.

The workspace layer first decides whether a tool may be exposed to the model.
The general Agent request builders also project the registry through the copied
Host capability policy before serializing model-visible schemas for Responses,
Chat, OpenAI-compatible Chat, or Anthropic. Session schema diagnostics use the
same projection, so registered-but-ungranted tools are not advertised as
available. Before any callback or `started` journal record, the Agent tool
executor checks the same registry metadata against its copied policy.
Middleware, review, guardrails, cancellation, backpressure, output limits,
result trace, and journal commit remain in the normal Agent workflow. Direct
`turbo_tool_registry_execute*()` calls are intentionally low-level host APIs
and do not represent Agent execution.

## Workflow tool availability

Every model-driven role can complete a `model -> tool -> model` cycle against
its assigned registry:

| Role | Tool-call state | Availability |
| --- | --- | --- |
| Loop agent | Root `events` | Yes |
| Initial planner | Root `events`, or an explicitly seeded `planner_state_versions` entry | Yes |
| Replanner | Current `planner_state_versions` entry | Yes |
| Plan executor | Current `executor_state_versions` entry | Yes |
| Subagent | Child session state | Yes, when the child is explicitly assigned a registry |

Deterministic control nodes such as plan commit, review routing, plan advance,
and end do not invoke tools. An interrupted review or tool approval resumes the
same tool node; a completed or failed run cannot initiate another tool call
without a new start/resume operation.

Child sessions do not implicitly inherit the parent's registry or capability
policy. Hosts must project and assign the child registry explicitly so that
delegation does not broaden authority.

## CLI skills and workflow composition

Both CLI modes prepare the workspace catalog on the input thread for each new
user turn. General mode uses an empty registry; it can select instruction-only
skills but cannot satisfy a skill's coding-tool requirements. Coding mode
selects its tool registry **only from host CLI arguments**: the default
`--trust-profile inspect` registers `fs.read/list/search` and denies host
shell/patch, while explicit `--trust-profile trusted-local` registers workspace
writers and the host shell. `--read-only` is the inspect alias; interactive
approval remains unavailable without a real Host callback. Catalog selection,
AGENTS.md and Skills never grant additional authority. General-mode workspace
instructions are an intentional behavior extension; callers wanting unrelated
instructions should select a separate workspace with `-C`.

The CLI's `--workflow loop|review|engineering` chooses an existing Session
preset, with `loop` remaining the default. Rather than applying a planner JSON
instruction to every model call, the CLI seeds the existing JSON-native
`planner_state_versions` array with a separate initial input. Executor input
and subsequent conversation replay do not inherit that planning instruction.
This uses the current state schema and adds no public C ABI or database format.

Instruction context has one source of truth on the current turn's root state.
New executor and replan substates clone its `memory_context` before publication.
The CLI also copies context into
its initial planner substate. These are owned snapshots, not borrowed references
to a temporary workspace selection. Allocation failure prevents publication and
propagates to the run. Existing persisted substates remain their recorded snapshots.
Substate copies increase retained context bytes; the CLI's context bounds, graph
step limit, three-replan limit, and per-turn deadline bound this work. No memory
or performance improvement is claimed.

FTS/vector retrieval continues before submission on the input thread. Selecting
SDK knowledge/retriever graph presets instead would move backend access into the
worker and duplicate CLI retrieval; those presets remain SDK-only. The worker
owns execution state until completion, and the next catalog refresh occurs only
after it drains. Completed-turn resume projects conversation history into a
fresh state and seeds a new plan, not an interrupted checkpoint continuation.
Review gates remain enforced; the CLI has no approval UI or implicit approval.

No stored-data migration is required. Returning to `--workflow loop` restores
the original execution topology for subsequent completed-conversation turns.
The workspace tests cover both modes, FTS/context propagation, replanning and
persisted-history continuation; live tests cover skill adherence under all three
choices and coding repair through loop and engineering execution.

## WasmToolPack composition

`turbo_wasm_tool_pack_t` is an optional host-owned source registry for resident
Wasm tools. The host explicitly adds each module with a bounded pack
configuration, a per-module execution policy, and a TurboWasm policy. The pack
never scans a directory and never loads a module named only by Markdown.

The two policy layers have different jobs:

- `turbo_agent_policy_t` controls whether a selected skill may expose and
  execute a Wasm tool (`allow_runtime_tools`). `WasmToolPack` registrations
  carry the `runtime_tools` requirement automatically.
- `turbo_wasm_policy_t` is cloned into the VM and enforces the module root,
  imported capabilities, linear-memory limit, call budget, and App I/O limits.

Use `turbo_wasm_tool_pack_registry()` as the workspace source registry. Add
workspace mappings only for extra host requirements not already declared by
the pack. Destroy
all workspace selections and agents before destroying the pack. Module
registration is atomic; duplicate names, exhausted pack capacity, empty tool
catalogs, or invalid execution policy leave existing tools unchanged. Current
TurboWasm VMs are not reentrant, so the pack rejects `PARALLEL_SAFE`.

Minimal host composition (the module path is relative to the policy root):

```c
#include <turbo_llm_sandbox.h>

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  turbo_wasm_policy_t *policy = NULL;
  turbo_wasm_tool_pack_t *pack = NULL;
  turbo_wasm_tool_pack_config_t pack_config;
  turbo_wasm_tool_pack_module_config_t module_config;
  char *output = NULL;
  int result = EXIT_FAILURE;

  if (argc != 3) {
    fprintf(stderr, "usage: wasm_pack <absolute-module-root> <relative-module.wasm>\n");
    return EXIT_FAILURE;
  }
  policy = turbo_wasm_policy_create();
  if (!policy ||
      turbo_wasm_policy_set_module_root(policy, argv[1]) != TURBO_WASM_OK ||
      turbo_wasm_policy_set_capabilities(
          policy, TURBO_WASM_CAP_CORE | TURBO_WASM_CAP_APP) != TURBO_WASM_OK)
    goto cleanup;

  turbo_wasm_tool_pack_config_init(&pack_config);
  pack = turbo_wasm_tool_pack_create(&pack_config);
  turbo_wasm_tool_pack_module_config_init(&module_config);
  module_config.runtime.module_path = argv[2];
  module_config.runtime.policy = policy;
  if (!pack || turbo_wasm_tool_pack_add_module(pack, &module_config) != TURBO_TOOL_OK)
    goto cleanup;

  if (turbo_tool_registry_execute(turbo_wasm_tool_pack_registry(pack), "echo_json",
                                  "{\"ok\":true}", &output) != TURBO_TOOL_OK)
    goto cleanup;
  puts(output);
  result = EXIT_SUCCESS;

cleanup:
  free(output);
  turbo_wasm_tool_pack_destroy(pack);
  turbo_wasm_policy_destroy(policy);
  return result;
}
```

## Failure semantics

Preparation is all-or-nothing. The following conditions fail before an agent
or projected registry is returned:

- malformed or unterminated YAML frontmatter;
- duplicate skill names;
- unknown or denied capabilities;
- a declared tool missing from the source registry;
- path traversal, symlink traversal, or a non-directory workspace component;
- configured byte, skill-count, selection-count, scan-depth, or tool-count
  limits being exceeded.

`turbo_agent_workspace_last_error()` supplies an operation and input summary.
It does not contain file contents, tool arguments, credentials, or model data.

## Compatibility and migration

Agents that assign `turbo_agent_config_t.tool_registry` directly now
**start with zero Tool Authority**. Registering a tool advertises its schema,
but does not grant execution. `turbo_agent_policy_default()` denies
`custom_tools`, `runtime_tools`, `delegate`, `network`, `shell`, and `patch`
until a Host explicitly grants the required capabilities using
`turbo_agent_set_tool_policy()`. Passing `NULL` resets to this fail-closed
policy; it does not restore historical implicit Host privileges.

Legacy tools added through `turbo_tool_registry_add()` retain their sequential
execution policy and count as `custom_tools` (an intentionally broad Host
grant). Prefer `turbo_tool_registry_add_v3()` with explicit, narrow capability
metadata for new tools. Hosts must set the **admitted policy** on the Agent
before invoking a model-driven tool; Agent instructions, Skills, model text and
registry registration never create a Host grant.
Applications adopt the workspace layer by:

1. Creating and populating their source tool registry as before.
2. Initializing `turbo_agent_workspace_config_t` and setting an absolute
   workspace root.
3. Optionally mapping sensitive tool names to policy capabilities.
4. Calling `turbo_agent_create_for_workspace()` for each task-specific agent,
   or calling `turbo_agent_workspace_prepare()` when the host manages the
   selection lifecycle itself.

Direct `turbo_agent_create()` with an original registry is allowed but
cannot execute its tools until the Host sets an explicit policy. A Host may
revoke privileges on a quiescent Agent with
`turbo_agent_set_tool_policy(agent, NULL)`. No persistent state or data format
is migrated. Low-level direct `turbo_tool_registry_execute*()` functions are
separate Host APIs and are **not** implicitly protected by Agent admission.

## Verification scope

TinyTest coverage includes hierarchical instruction ordering, TurboParser YAML
frontmatter, explicit trigger selection, ordered tool projection, agent-owned
selection lifetime, missing-tool failure, and policy denial. Registry tests
also cover successful projection and missing-name rejection.

## MCP and multiple resident packs

`McpToolPack` keeps MCP HTTP, authentication and protocol state in the host.
After refreshing it, combine its registry with resident Wasm or native packs
using `turbo_tool_registry_compose()`. The composite borrows callback state;
destroy the Agent and composite before refreshing or destroying any source.

One tool may require more than one Agent capability. Registry metadata and
repeated names in `tool_capabilities` are merged, and all requirements must be
allowed. `McpToolPack` registers both `runtime_tools` and `network`;
`WasmToolPack` registers `runtime_tools`. Add `network` to a Wasm tool at the
workspace layer when its TurboWasm policy grants HTTP.
Skills remain Markdown `SKILL.md` files and declare the matching capabilities
in YAML frontmatter, for example `capabilities: [runtime_tools, network]`.
