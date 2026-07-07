# Turbo LangChain

`turbo_langchain` is the reusable library layer for graph-driven LLM workflows in TurboNet.
It contains the runtime and data-model pieces that should stay portable across desktop,
server, and embedded-friendly native builds:

- graph execution and checkpoints
- prompt/message builders
- model provider selection
- tool registries and schemas
- host-agnostic tool runtimes and registry bridges
- action-tool bridges
- agent runtime

It does not include shell execution, builtin file tools, or engineering-loop application
helpers. Those live in `agent/` and are exposed through the `turbo_agent` target.

## Long-term aim

`turbo_langchain` is intended to grow into a reusable agent kernel for coding
and operator-style hosts, closer in spirit to tools such as OpenClaw or Claude
Code than to a one-off demo loop.

The target shape is:

- host-agnostic workflow runtime in `langchain/`
- stable action and tool contracts instead of CLI-specific behavior
- cross-platform tool execution through pluggable runtimes
- schema-driven runtime data bind for context, tool payloads, and checkpoints
- native and sandboxed backends, including DLL-style native tools and wasm3-based tools
- policy, trace, checkpoint, review, and replan semantics that stay stable across hosts

This module should own the reusable semantics. Concrete CLIs, shells, approval
UX, and product-specific orchestration remain outside it.

## CMake targets

- `turbo_langchain`
- `TurboNet::LangChain`

Compatibility targets kept for existing users:

- `turbo_graph`
- `TurboNet::Graph`

## Public headers

For a one-shot include, use:

```c
#include "turbo_langchain.h"
```

Or include only the narrower headers you need, such as `turbo_graph.h` or
`turbo_agent.h`.

For the local JSON-RPC bridge over the durable runtime surface, use:

```c
#include "turbo_agent_remote_app.h"
#include "turbo_agent_remote_session.h"
#include "turbo_agent_runtime_remote.h"
#include "turbo_agent_runtime_remote_client.h"
```

That layer is transport-agnostic by design. It dispatches local JSON-RPC 2.0
requests over an existing `turbo_agent_runtime_t`; it does not start an HTTP
endpoint, own a store, or define a second persisted runtime state source.

Above the typed remote client, `turbo_agent_remote_session.h` now provides one
thread-scoped facade that caches `thread_id`, `last_run_id`, and
`last_checkpoint_id` across remote starts, resumes, forks, and thread-command
helpers. `turbo_agent_remote_app.h` adds one thinner wrapper that fixes one
remote `graph_name` and forwards the same inspect/control helpers through that
session layer.

`turbo_prompt.h` now exposes parallel bind-native message and template helpers,
and `turbo_tool_registry.h` exposes bind-native tool execution. That lets hosts
keep prompt input, tool arguments, and tool results on one runtime value model
instead of bouncing through ad hoc JSON strings.
It also exposes canonical bind-native message schemas and validation helpers, so
upper layers can depend on one explicit message contract instead of implicit
`role/content` object conventions.

For host-agnostic tool execution and `tool_registry` bridging, use:

```c
#include "turbo_tool_runtime.h"
```

That layer defines a small vtable-based runtime contract so native callbacks,
shared-library loaders, and future wasm3-backed tool hosts can converge on one
execution surface.

For the optional wasm3-backed runtime backend, use:

```c
#include "turbo_tool_runtime_wasm3.h"
```

That backend loads guest modules through TurboNet's wasm3 wrapper and maps a
small exported tool ABI back into the same `turbo_tool_runtime_t` surface.
The generic `turbo_tool_runtime_default_create(...)` entry now resolves to this
wasm3 backend; hosts that want in-process callbacks must opt into the native
runtime explicitly.

For schema-driven runtime value binding, use:

```c
#include "turbo_runtime_data_bind.h"
```

That layer exposes a small host-neutral value tree plus a builder vtable meant
for future schema codecs. The default implementation is in-memory and portable;
future MIR-backed codecs should target that vtable instead of baking host
details into public APIs. `turbo_chain` and `turbo_graph` now also expose small
bind-boundary helpers so hosts can keep JSON as an implementation detail rather
than a required application data model.

For binary wire parsing primitives, use:

```c
#include "turbo_runtime_binary_reader.h"
```

That layer defines the canonical little-endian cursor and var-string rules the
future binary codec should reuse for validation, interpreter fallback, and MIR
JIT generation. The codec should compile against one wire contract, not hide
multiple ad hoc readers in different backends.

For declarative binary layouts and ABI planning, use:

```c
#include "turbo_runtime_binary_schema.h"
```

That layer describes canonical binary object fields, validates payloads against
the wire contract, and collects the exact
`turbo_runtime_data_bind_value_api_t` callbacks a future MIR parser will need.

For MIR-backed binary parser planning and JIT stubs, use:

```c
#include "turbo_runtime_binary_mir.h"
```

That layer turns binary schema requirements into a stable external-symbol plan
and can JIT a parser entry against the vendored MIR runtime. The current
implementation has three specialized fast paths plus one conservative fallback:

- all scalar fields: field-by-field MIR assembly over one shared reader
- all repeated-scalar fields: MIR skeleton plus one repeated-value bridge per field
- all string-key-map scalar fields: MIR skeleton plus one map-value bridge per field
- mixed schemas or nested objects: validated interpreter fallback through one stable
  parser entry

This keeps one real parser surface available now while MIR specialization grows
incrementally behind the same ABI contract.

The Agent surface is layered from low to high as:

- `turbo_agent.h` for core config and lifecycle
- `turbo_agent_runtime.h` for durable thread/run/checkpoint execution
- `turbo_agent_session.h` for runtime session helpers
- `turbo_agent_subagent.h` for subagent-as-tool adapters
- `turbo_agent_subgraph.h` for graph-native subgraph nodes
- `turbo_agent_memory_store.h` for namespaced long-term memory records
- `turbo_agent_extensions.h` for optional middleware, trace, store,
  guardrail, and runnable helpers
- `turbo_agent_state.h` for state inspection and mutation
- `turbo_agent_sse.h` for SSE aggregation helpers
- `turbo_agent_graph.h` for graph nodes and predicates
- `turbo_agent_workflow.h` for canned workflow installers

`Runtime V3` starts one minimal multi-agent orchestration foundation on top of
that stack:

- `turbo_agent_state.h` now exposes one canonical `supervisor_versions` lane
  for `active_agent`, staged handoff target/reason, inbox, and handoff history
- `turbo_agent_workflow.h` now exposes
  `turbo_agent_install_supervisor_loop(...)` as the minimal supervisor/handoff
  route scaffold; callers still add the concrete agent nodes themselves

Agent-state inspection and mutation helpers live in:

```c
#include "turbo_agent_state.h"
```

That keeps state-schema access separate from graph topology and workflow
presets, while `turbo_agent.h` includes it for convenience.
The state helper layer now also exposes bind-boundary creation and snapshot
helpers so hosts can inspect workflow/control state without adopting the JSON
tree as their own application model.

SSE stream aggregation helpers live in:

```c
#include "turbo_agent_sse.h"
```

That keeps wire-format reconstruction separate from the runtime agent lifecycle
surface, while `turbo_agent.h` includes it for convenience.

Optional middleware, trace, store, guardrail, and runnable helpers live in:

```c
#include "turbo_agent_extensions.h"
```

That keeps lifecycle/configuration APIs separate from optional runtime
extension points, while `turbo_agent.h` includes them for convenience.

Workflow graph helpers for the agent live in:

```c
#include "turbo_agent_graph.h"
```

That keeps the graph-node and edge-predicate surface out of the base runtime
header.

Workflow installer presets live one step higher:

```c
#include "turbo_agent_workflow.h"
```

Use that header only when you want the canned loop topologies.

## Runtime V1

`turbo_agent_runtime.h` adds one durable runtime layer on top of the existing
graph runtime. It does not replace `turbo_graph`; it wraps one graph run in
stable records:

- `thread`
- `run`
- `checkpoint`

The runtime keeps the current graph semantics:

- `TURBO_GRAPH_EXEC_INTERRUPTED` stays the interrupt primitive
- graph checkpoints keep their current schema
- state mutation still happens through `turbo_agent_state_*`

What changes is persistence and replay:

- `start_bind_graph(...)` creates one thread/run and executes one segment
- `resume_bind_graph(...)` reloads one persisted checkpoint and continues the
  same run
- `fork_bind_graph(...)` starts a new run from an old checkpoint
- `get_*_state_bind(...)` reads persisted thread/run/checkpoint state without
  forcing the host to parse checkpoint JSON manually
- `get_latest_run(...)`, `get_pending_run(...)`, and `get_latest_checkpoint(...)`
  remove the common "list then sort/filter" host boilerplate
- `turbo_agent_runtime_get_thread_timeline_bind(...)` bundles one thread,
  the resolved current run, the latest run, the pending run, all runs, the
  current run checkpoints, and history events into one host-facing view; the
  resolved current run rule is pending run first, otherwise latest run
- `turbo_agent_session_get_thread_timeline_bind(...)` and
  `turbo_agent_app_get_thread_timeline_bind(...)` expose the same timeline
  shape at their respective wrapper layers
- `load_thread_history_events_bind(...)` removes the remaining
  "resolve pending/latest run, then replay history" boilerplate
- `apply_command_bind(...)` turns one checkpoint plus one host-facing command
  into a fresh `state_override`
- `resume_command_bind(...)` / `fork_command_bind(...)` collapse command-apply
  plus continue/fork into one host-facing call
- `apply_thread_command_bind(...)` / `resume_thread_command_bind(...)` /
  `fork_thread_command_bind(...)` do the same at thread scope, so hosts can
  stay on one thread id instead of resolving pending runs or latest
  checkpoints first
- `resume_checkpoint_bind_graph(...)` / `fork_checkpoint_bind_graph(...)` are
  the explicit checkpoint-scoped replay aliases; they take a concrete
  checkpoint id and continue or fork directly from that historical point
- `apply_checkpoint_command_bind(...)`, `resume_checkpoint_command_bind(...)`,
  and `fork_checkpoint_command_bind(...)` are the matching explicit
  checkpoint-scoped command aliases; they keep command application pinned to
  one concrete historical checkpoint instead of relying on cached checkpoint
  fallback
- `resume_thread_bind_graph(...)` / `fork_thread_bind_graph(...)` are the
  thread-scoped replay convenience path: they resolve the current checkpoint
  from the thread, then forward the caller's state override into the existing
  replay path
- `update_checkpoint_state_bind(...)` / `update_thread_state_bind(...)` are the
  low-level state-edit counterparts to command helpers: they return one merged
  full-state override without mutating persisted checkpoint records in place
- `resume_checkpoint_state_bind_graph(...)` /
  `fork_checkpoint_state_bind_graph(...)` and their thread-scoped variants
  collapse state patch plus resume/fork into one host-facing time-travel call
- completed-only threads have no current checkpoint, so the thread-scoped
  replay helpers fail explicitly instead of guessing a replay target
- interrupted summaries now carry `interrupt_reason`, `pending_node`,
  `pending_action`, legacy `available_commands`, and structured
  `available_command_descriptors`
- optional host/UI metadata such as `category`, `input_mode`,
  `requires_input`, `suggested_title`, `primary_key`, `accepted_keys`,
  `fallback_keys`, `supports_json_value`, `placeholder`, `example_payload`,
  and `success_state_hint` may also live on each descriptor; hosts use them to
  render action panels and input forms, but they are host-facing metadata only
  and do not change the legacy `available_commands` list, the command name
  lookup, or the resume/fork semantics
- `load_history_events_bind(...)` replays persisted segment events as one
  bind-native array
- `replay_history_bind(...)` / `replay_thread_history_bind(...)` feed the same
  durable history events back through the existing `turbo_event_sink_bind_fn`
  callback boundary, so hosts can observe persisted runtime history without
  inventing a second event contract
- `turbo_event_stream_mode_accepts_bind(...)` and
  `turbo_event_stream_filter_sink_bind(...)` add a transport-agnostic stream
  mode adapter above the same canonical events. Hosts can filter existing
  runtime/session/app sinks into `all`, `messages`, `updates`, `tools`, or
  `debug` style streams before exposing them through CLI callbacks, TurboHTTP
  SSE, WebSocket, or another transport.
- `turbo_agent_session_start_text_stream(...)`,
  `turbo_agent_session_start_messages_stream(...)`,
  `turbo_agent_app_start_text_stream(...)`, and
  `turbo_agent_app_start_messages_stream(...)` are the high-level default
  workflow wrappers over that same filter. They let hosts ask for one stream
  mode while still receiving the normal `summary` and final state from the
  existing session/app execution path.
- `turbo_agent_session_batch_text(...)` and `turbo_agent_app_batch_text(...)`
  add the first high-level batch entry points. They run a sequence of user-text
  inputs through the same default workflow and return one JSON result item per
  input.
- `turbo_runnable_batch_bind(...)`,
  `turbo_runnable_from_agent_session(...)`, and
  `turbo_runnable_from_agent_app(...)` put the default session/app workflow on
  the same bind-native runnable surface as chains, graphs, and state graphs.
  `turbo_runnable_wrap_bind(...)` adds the first before/after hook wrapper for
  bind-native runnable input and output shaping.
  Runnable batches execute sequentially over an input array and can still be
  composed with `turbo_runnable_pipe(...)`.
- `observe_history_bind(...)` / `observe_thread_history_bind(...)` are the
  matching host-facing observer bridge: they map the same durable history
  facts into one narrower observer event family
  (`model_delta`, `tool_call_started`, `tool_result`, `state_updated`,
  `interrupted`, `completed`) without persisting a second log; each observer
  object carries `kind="observer"`, one narrowed `type`, and the original
  canonical event clone under `event`
- `turbo_agent_session_add_trace_bind_sink(...)` /
  `turbo_agent_app_add_trace_bind_sink(...)` are the live-side convenience
  wrappers for the same canonical trace event shape, and
  `*_set_trace_history_enabled(...)` lets hosts persist those trace events into
  run state for later checkpoint or thread inspection
- `turbo_agent_session_add_observer_bind_sink(...)` /
  `turbo_agent_app_add_observer_bind_sink(...)` are the live-side observer
  bridge wrappers for that same trace stream; they reuse the existing trace
  facts and emit the same observer categories where a stable mapping exists
- `get_*_trace_events_bind(...)` is the matching durable snapshot convenience
  layer; it reads persisted state and returns `trace_events` directly, so hosts
  no longer need to unwrap `get_*_state_bind(...)` by hand
- `get_thread_observability_index(...)` and the matching session/app wrappers
  are the read-only observability bundle for one thread: they reuse the
  existing `thread`, `timeline`, `lineage`, `branch_tree`, `history_events`,
  and `trace_events` facts, and add derived quick-read fields such as
  `current_status`, `current_interrupt_reason`, `current_pending_action`,
  `current_checkpoint_summary`, `latest_run_status`, `latest_run_updated_at`,
  `pending_run_id`, `pending_checkpoint_id`, `has_failure`,
  `has_model_error`, `has_guardrail_rejection`, `replan_requested`,
  `current_failure_reason`, `current_review_note`, `has_pending_review`,
  `has_handoff`, `active_agent`, and one `counts` object, without defining a
  second event log or store
- `turbo_agent_runtime_list_observability_indexes(...)` is the runtime-only
  cross-thread counterpart: it returns one lightweight observability summary
  per persisted thread, sorted by thread `updated_at` descending, while
  session/app remain current-thread scoped
- `turbo_agent_runtime_list_observability_indexes_filtered(...)` is the
  matching runtime-only filter layer for cross-thread lists. The current
  filter keys are `status`, `has_pending_review`, `has_failure`,
  `has_handoff`, `active_agent`, `has_model_error`,
  `has_guardrail_rejection`, `replan_requested`,
  `current_interrupt_reason`, `latest_run_status`,
  `latest_run_updated_after`, `latest_run_updated_before`, and
  `thread_id_prefix`; it also supports `sort_by`, `sort_order`, and `limit`.
  Default ordering stays `latest_run_updated_at desc`; `sort_by=thread_id`
  defaults to `asc`
- `turbo_agent_runtime_remote_dispatch_jsonrpc(...)` is one local JSON-RPC 2.0
  dispatcher above that same runtime surface. The current minimal method set
  currently includes:
  `runtime.start`, `runtime.resume`, `runtime.fork`, `runtime.applyCommand`,
  `runtime.resumeThreadCommandBindGraph`,
  `runtime.forkThreadCommandBindGraph`,
  `runtime.getThreadState`, `runtime.updateThreadState`,
  `runtime.applyThreadStatePatch`,
  `runtime.resumeThreadBindGraph`, `runtime.forkThreadBindGraph`,
  `runtime.getCheckpointContext`,
  `runtime.resumeThreadStatePatchBindGraph`,
  `runtime.forkThreadStatePatchBindGraph`,
  `runtime.getThreadObservabilityIndex`, and
  `runtime.listObservabilityIndexesFiltered`. When the remote config borrows
  one optional `memory_store`, the same dispatcher also exposes:
  `memory.getRecord`, `memory.putRecord`, `memory.listRecords`,
  `memory.deleteRecord`, and `memory.queryRecordsEx`. Remote memory stays
  canonical record-first on this surface; it does not expose one legacy raw
  `memory.list` transport shape.
  It is a contract bridge for future remote adapters, not an HTTP server
- `turbo_agent_runtime_remote_dispatch_jsonrpc_text(...)` is the raw JSON text
  adapter above that dispatcher
- `turbo_agent_runtime_remote_handle_http_jsonrpc(...)` is one HTTP-like
  adapter for `POST /v1/runtime/jsonrpc`; adapter-level path/method/body
  failures map to HTTP status, while the response body remains JSON
- `turbo_agent_runtime_remote_client_call_json(...)` is the matching consumer-
  side bridge over `rpc_client`. It keeps the same `result` payloads, returns
  normalized JSON-RPC error objects (`code`, `message`, `http_status`,
  `transport_error`), and does not invent a second remote contract
- `turbo_agent_runtime_remote_client_start_bind_graph(...)`,
  `resume_bind_graph(...)`, `fork_bind_graph(...)`,
  `get_thread_state_bind(...)`, `get_checkpoint_context(...)`,
  `get_run(...)`, `get_checkpoint(...)`, `list_checkpoints(...)`,
  `load_history_events_bind(...)`, `get_run_trace_events_bind(...)`,
  `get_checkpoint_trace_events_bind(...)`,
  `get_memory_record(...)`, `put_memory_record(...)`,
  `delete_memory_record(...)`, `query_memory_records_ex(...)`,
  `query_memory_records(...)`, `list_memory_records(...)`,
  `get_thread_timeline_bind(...)`, `get_branch_tree(...)`,
  `get_thread_observability_index(...)`,
  `list_observability_indexes(...)`,
  `list_observability_indexes_filtered(...)`,
  `list_child_runs(...)`,
  `get_supervisor_inspect(...)`, `get_orchestration_inspect(...)`,
  `resume_thread_command_bind(...)`, and `fork_thread_command_bind(...)` are
  the first typed convenience helpers above that generic client call. They keep
  the remote method names hidden while preserving the existing runtime
  `summary/state/context/index` split
- `turbo_agent_runtime_remote_client_get_child_inspect(...)`,
  `get_child_orchestration_inspect(...)`, and
  `get_child_multi_agent_inspect(...)` extend that same typed helper surface
  for child-lineage and multi-agent inspect. They are still derived from the
  existing remote thread-state, observability, child-run, and child-history
  facts instead of inventing a second client-only contract
- `turbo_agent_remote_session_start_bind_graph(...)`,
  `start_text(...)`, `start_messages(...)`, `resume_bind_graph(...)`,
  `fork_bind_graph(...)`, `invoke_text(...)`, `invoke_messages_text(...)`,
  `invoke_json(...)`, `invoke_messages_json(...)`,
  `memory_list_records(...)`, `memory_get_record(...)`,
  `memory_put_record(...)`, `memory_validate_record(...)`,
  `memory_query_records(...)`, `memory_query_records_ex(...)`,
  `get_thread(...)`, `get_latest_run(...)`, `get_pending_run(...)`,
  `get_thread_state_bind(...)`, `get_checkpoint_context(...)`,
  `get_observability_index(...)`, `get_thread_timeline_bind(...)`,
  `load_thread_history_events_bind(...)`, `replay_thread_history_bind(...)`,
  `observe_thread_history_bind(...)`, `get_thread_trace_events_bind(...)`,
  `get_branch_tree(...)`, `list_thread_lineage(...)`,
  `get_supervisor_inbox(...)`, `get_supervisor_handoff_history(...)`,
  `get_supervisor_inspect(...)`, `list_child_runs(...)`,
  `get_orchestration_inspect(...)`,
  `get_child_run(...)`, `get_child_checkpoint(...)`,
  `get_child_checkpoint_context(...)`, `get_child_thread_timeline_bind(...)`,
  `get_child_branch_tree(...)`, `list_child_checkpoints(...)`,
  `load_child_history_events_bind(...)`, `get_child_trace_events_bind(...)`,
  `get_child_inspect(...)`, `get_child_orchestration_inspect(...)`,
  `get_child_multi_agent_inspect(...)`,
  `resume_thread_command_bind(...)`, and `fork_thread_command_bind(...)` add
  one thread-scoped host facade above the typed remote client without
  inventing a second remote payload shape
- `turbo_agent_remote_app_start_bind_graph(...)`,
  `start_text(...)`, `start_messages(...)`, `resume_bind_graph(...)`,
  `fork_bind_graph(...)`, `invoke_text(...)`, `invoke_messages_text(...)`,
  `invoke_json(...)`, `invoke_messages_json(...)`,
  `memory_list_records(...)`, `memory_get_record(...)`,
  `memory_put_record(...)`, `memory_validate_record(...)`,
  `memory_query_records(...)`, `memory_query_records_ex(...)`,
  `get_thread(...)`, `get_latest_run(...)`, `get_pending_run(...)`,
  `get_thread_state_bind(...)`, `get_checkpoint_context(...)`,
  `get_observability_index(...)`, `get_thread_timeline_bind(...)`,
  `load_thread_history_events_bind(...)`, `replay_thread_history_bind(...)`,
  `observe_thread_history_bind(...)`, `get_thread_trace_events_bind(...)`,
  `get_branch_tree(...)`, `list_thread_lineage(...)`,
  `get_supervisor_inbox(...)`, `get_supervisor_handoff_history(...)`,
  `get_supervisor_inspect(...)`, `list_child_runs(...)`,
  `get_orchestration_inspect(...)`,
  `get_child_run(...)`, `get_child_checkpoint(...)`,
  `get_child_checkpoint_context(...)`, `get_child_thread_timeline_bind(...)`,
  `get_child_branch_tree(...)`, `list_child_checkpoints(...)`,
  `load_child_history_events_bind(...)`, `get_child_trace_events_bind(...)`,
  `get_child_inspect(...)`, `get_child_orchestration_inspect(...)`,
  `get_child_multi_agent_inspect(...)`,
  `resume_thread_command_bind(...)`, and `fork_thread_command_bind(...)` add
  one graph-name-configured app facade on top of that remote session layer
- `turbo_agent_runtime_remote_iris_mount(...)` mounts that same adapter on an
  `iris_app_t` through Iris's app-local RPC endpoint registry, so it may now
  coexist with Iris's own `rpc_setup_endpoint(...)` on the same app as long as
  the paths differ

The remote contract keeps the existing runtime JSON results and only wraps them
in JSON-RPC 2.0 envelopes. Two stable happy-path examples are:

```json
{
  "jsonrpc": "2.0",
  "id": "req-start",
  "method": "runtime.start",
  "params": {
    "graph_name": "remote-skeleton",
    "thread_id": "remote-thread-1",
    "state": {},
    "options": { "interrupt_before_nodes": ["end"] }
  }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": "req-start",
  "result": {
    "summary": {
      "thread_id": "remote-thread-1",
      "status": "interrupted",
      "run_id": "run_...",
      "checkpoint_id": "ckpt_..."
    },
    "state": { "visited_start": true, "visited_end": false }
  },
  "error": null
}
```

```json
{
  "jsonrpc": "2.0",
  "id": "req-context",
  "method": "runtime.getCheckpointContext",
  "params": { "checkpoint_id": "ckpt_..." }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": "req-context",
  "result": {
    "context": {
      "checkpoint_summary": { "id": "ckpt_..." },
      "run": { "id": "run_..." },
      "thread": { "id": "remote-thread-1" },
      "state": {},
      "history_events": []
    }
  },
  "error": null
}
```

Long-term memory is explicit on the same remote transport but stays separate
from `runtime.*` inspect/control. Remote memory stays canonical record-first:
`memory.listRecords` returns canonical records, `memory.deleteRecord` returns
`{ "deleted": true }` on success, and the bridge does not expose one legacy
raw `memory.list` result shape. The current query shape is:

```json
{
  "jsonrpc": "2.0",
  "id": "req-memory-list",
  "method": "memory.listRecords",
  "params": { "namespace_prefix": "project/" }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": "req-memory-list",
  "result": {
    "records": [
      {
        "id": "project/demo::context",
        "namespace": "project/demo",
        "kind": "context",
        "key": "context",
        "text": "remember this note"
      }
    ]
  },
  "error": null
}
```

```json
{
  "jsonrpc": "2.0",
  "id": "req-memory-delete",
  "method": "memory.deleteRecord",
  "params": {
    "namespace": "project/demo",
    "key": "context"
  }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": "req-memory-delete",
  "result": { "deleted": true },
  "error": null
}
```

The current canonical memory query shape is:

```json
{
  "jsonrpc": "2.0",
  "id": "req-memory-query",
  "method": "memory.queryRecordsEx",
  "params": {
    "namespace_prefix": "project/",
    "kind": "context",
    "key_prefix": "con",
    "text_substring": "remember",
    "created_after": "2026-02-01T00:00:00Z",
    "created_before": "2026-02-28T23:59:59Z",
    "sort_by": "key",
    "sort_order": "desc",
    "limit": 1
  }
}
```

`sort_by` currently supports `id`, `namespace`, `kind`, and `key`;
`sort_order` supports `asc` or `desc`; `limit=0` means no truncation.
`created_after` and `created_before` apply one inclusive lower/upper bound to
canonical `created_at`. The current comparison depends on canonical
`created_at` staying in one stable ISO-8601 string shape so lexicographic
string order matches time order.

The current stable error matrix is:
- `-32600`: invalid request
- `-32601`: method not found
- `-32602`: invalid params
- `-32603`: internal runtime or dispatch failure

`-32600` is used when the JSON-RPC envelope itself is malformed. `-32602` is
used when the JSON-RPC envelope is valid but the selected runtime method
receives malformed or missing params.

The remote JSON-RPC contract is also locked by these golden fixtures under
`langchain/tests/fixtures/runtime_v2/`:
- `runtime_remote_thread_control.golden.json`
- `runtime_remote_graph_runs.golden.json`
- `runtime_remote_inspect.golden.json`
- `runtime_remote_errors.golden.json`
- `runtime_remote_invalid_params.golden.json`
- `child_orchestration_inspect.golden.json`
- `child_multi_agent_inspect.golden.json`

The invalid-params golden set intentionally covers both simple inspect calls
and graph-bound/thread-bound calls, so method-level parameter contracts do not
drift separately from the dispatcher implementation.

Where params are stable, the thread-control / graph-run / inspect golden sets
also lock canonical request shapes, not only success envelopes. That keeps
`graph_name` / `thread_id` / `checkpoint_id` / `command` / `state` /
`state_patch` payload shapes aligned with the dispatcher implementation.

Graph-bound success responses are also golden-locked so the remote bridge keeps
one stable `summary/state` envelope for resume/fork and thread-scoped replay
paths.
- `get_child_trace_events_bind(...)` extends that same convenience to
  subagent/tool-result lineage by resolving `child_checkpoint_id` first and
  then falling back to `child_run_id`
- `turbo_agent_session_get_child_checkpoint_context(...)` /
  `turbo_agent_app_get_child_checkpoint_context(...)` are the matching
  one-shot inspect helpers when a parent tool-result output already carries
  `child_checkpoint_id`
- `turbo_agent_session_get_child_thread_timeline_bind(...)` /
  `turbo_agent_app_get_child_thread_timeline_bind(...)` are the matching
  child-thread timeline helpers when a parent tool-result output already
  carries `child_thread_id`
- `turbo_agent_session_get_child_branch_tree(...)` /
  `turbo_agent_app_get_child_branch_tree(...)` are the matching child-thread
  branch-tree helpers when a parent tool-result output already carries
  `child_thread_id`
- `turbo_agent_session_list_child_checkpoints(...)` /
  `turbo_agent_app_list_child_checkpoints(...)` are the matching child-run list
  helpers when a parent tool-result output already carries `child_run_id`
- `turbo_agent_session_get_child_inspect(...)` /
  `turbo_agent_app_get_child_inspect(...)` bundle `run`, `checkpoints`,
  optional `latest_checkpoint`, optional `checkpoint_context`, optional
  `thread_timeline`, optional `branch_tree`, and child `history_events` /
  `trace_events` into one host-facing inspect object
- `turbo_agent_session_get_child_orchestration_inspect(...)` /
  `turbo_agent_app_get_child_orchestration_inspect(...)` add parent lineage
  fields `parent_agent_run_id` / `parent_tool_call_id` / `parent_tool_name`
  around nested `child_inspect`, without redefining the existing child detail
  bundle
- `turbo_agent_session_get_child_multi_agent_inspect(...)` /
  `turbo_agent_app_get_child_multi_agent_inspect(...)` are the next host-facing
  aggregate: they bundle current-thread `supervisor_inspect` /
  `orchestration_inspect` together with resolved `child_orchestration_inspect`
- `turbo_agent_session_get_supervisor_inbox(...)` /
  `turbo_agent_app_get_supervisor_inbox(...)` and
  `turbo_agent_session_get_supervisor_handoff_history(...)` /
  `turbo_agent_app_get_supervisor_handoff_history(...)` expose the current
  mailbox/control arrays directly from thread state; they return arrays and do
  not define a new runtime store collection
- `turbo_agent_session_get_supervisor_inspect(...)` /
  `turbo_agent_app_get_supervisor_inspect(...)` are the matching one-shot
  control-state inspect bundle; they return `supervisor`, `inbox`,
  `handoff_history`, `control_snapshot`, and `workflow_snapshot` in one
  read-only object
- `turbo_agent_session_get_orchestration_inspect(...)` /
  `turbo_agent_app_get_orchestration_inspect(...)` are the next aggregate
  layer above that: they bundle `supervisor_inspect`, `thread_timeline`,
  `thread_lineage`, `branch_tree`, and `child_runs` in one host-facing
  multi-agent inspect object without adding a new runtime collection
- `turbo_agent_session_append_supervisor_inbox_message_bind(...)` /
  `turbo_agent_app_append_supervisor_inbox_message_bind(...)` are the matching
  write-side mailbox helpers; they return a full state override bind value for
  the existing replay/resume/fork surfaces instead of mutating persisted state
  in place
- `turbo_agent_runtime_get_checkpoint_context(...)` /
  `turbo_agent_session_get_checkpoint_context(...)` /
  `turbo_agent_app_get_checkpoint_context(...)` expose one historical checkpoint
  as a read-only aggregate bundle with `checkpoint_summary`, `state`, `run`,
  `thread`, and `history_events`; this is a context inspect view, not a
  replacement for `get_checkpoint(...)`
- `turbo_agent_runtime_get_branch_tree(...)` / `turbo_agent_session_get_branch_tree(...)`
  / `turbo_agent_app_get_branch_tree(...)` expose a read-only time-travel
  inspect surface for one thread or lineage root; the returned tree is for
  browsing only and does not change `resume_bind_graph(...)` /
  `fork_bind_graph(...)` semantics

The branch-tree inspect payload should include, at minimum, the resolved
thread and run identity plus the branch topology:

- `thread_id`
- `current_run_id`
- `latest_run_id`
- `pending_run_id`
- `current_checkpoint_id`
- `current_checkpoint_summary`
- `current_branch`
- `branches`
- `edges`

`pending_run_id` is narrowed to the current branch head here: it is only
present when the latest/current run is itself interrupted. `current_branch`
duplicates the branch node for `current_run_id`, and `current_checkpoint_id`
exposes that branch head's checkpoint id when available.
`current_checkpoint_summary` is the lightweight summary for that checkpoint.
Branch nodes may also carry `branch_root_checkpoint_id` as the stable entry
checkpoint anchor for that branch.

`branches` can carry the active and historical branch nodes, while `edges`
describes the lineage links between checkpoints and runs. For fork edges, the
direction is always from the source checkpoint to the child run created from
that checkpoint.

Reference backends included in `Runtime V1`:

- memory store: heap-backed, good for tests and embedding
- file store: `threads/`, `runs/`, `checkpoints/` JSON files under one root

The runtime store is intentionally separate from `turbo_agent_set_store(...)`:

- `turbo_agent_store_t` still owns agent memory slots
- `turbo_agent_runtime_store_t` owns thread/run/checkpoint lineage

### Minimal flow

1. Build a graph that may stop at a review gate or other interrupt point.
2. Create a runtime with either the memory or file store.
3. Start one run with bind-native state.
4. If the run returns `interrupted`, inspect `control_snapshot` and
   `workflow_snapshot` through the persisted checkpoint record.
5. Mutate a fresh state copy with `turbo_agent_state_*`.
6. Resume the checkpoint with `state_override`, whether built directly with
   `turbo_agent_state_*` or via `apply_command_bind(...)`.

The smallest end-to-end example lives in
`langchain/examples/runtime_review_resume.c`.

If you build examples through CMake, the target name is:

- `langchain_runtime_review_resume_example`

For the query side of the runtime surface, use
`langchain/examples/runtime_history_inspect.c`.
That example shows:

- `get_thread(...)`
- `get_run(...)`
- `get_checkpoint(...)`
- `list_runs(...)`
- `list_checkpoints(...)`
- `load_history_events_bind(...)`
- stable nullable `summary.parent_*` fields

Its CMake target name is:

- `langchain_runtime_history_inspect_example`

## Runtime V2

`Runtime V2` adds two thin host-facing layers above `Runtime V1`:

- `turbo_agent_session.h`
- `turbo_agent_memory_store.h`
- `turbo_agent_subagent.h`
- `turbo_agent_subgraph.h`

`turbo_agent_session_t` wraps one runtime plus one optional agent and can load
provider settings from `.env` through `turbo_agent_config_apply_env(...)`,
which reuses the parser layer's `turbo_dotenv_load(...)`. That means real
hosts can bootstrap:

- `OPENAI_API_KEY`
- `OPENAI_BASE_URL`
- `OPENAI_MODEL`
- `OPENAI_PROVIDER`

without duplicating the same environment glue everywhere.

The same session object may now also own one optional
`turbo_agent_memory_store_t`, so hosts can keep durable runtime lineage and
long-term memory behind one wrapper instead of carrying two separate handles.
It may also remember one default `workflow_kind` and one default
`memory_namespace`, and optional parent-run lineage metadata, so the
highest-level text and canonical-message calls stop repeating the same preset,
memory scope, and parent context every time.

For local-document workflows, `turbo_agent_session_config_t` can also borrow a
`turbo_agent_knowledge_store_t` plus optional query/kind/URI-prefix/limit
filters. With
`TURBO_AGENT_SESSION_WORKFLOW_KNOWLEDGE_ENGINEERING`, the session preset adds a
knowledge-context node before the engineering planner, so retrieved SQLite/FTS5
context is part of the same graph run and checkpoint history.

For hosts that already own a retrieval layer, the same preset surface can borrow
one generic `turbo_retriever_t` via `retriever`, `retriever_query`,
`retriever_kind`, `retriever_uri_prefix`, `retriever_limit`, and
`retriever_scope`. With
`TURBO_AGENT_SESSION_WORKFLOW_RETRIEVER_ENGINEERING`, the session/app default
workflow injects retriever context before planning without coupling the host to
the SQLite knowledge-store schema.

The lineage-browser surface above is read-only: it exposes thread/run/checkpoint
ancestry for inspection, but it does not alter `resume_bind_graph(...)` /
`fork_bind_graph(...)` semantics.

If even that is too low-level for the host, `turbo_agent_app_t` now provides
one thinner surface above session:

- `turbo_agent_app_create(...)`
- `turbo_agent_app_destroy(...)`
- `turbo_agent_app_session(...)`
- `turbo_agent_app_thread_id(...)`
- `turbo_agent_app_last_run_id(...)`
- `turbo_agent_app_last_checkpoint_id(...)`
- `turbo_agent_app_workflow_kind(...)`
- `turbo_agent_app_memory_namespace(...)`
- `turbo_agent_app_memory_store(...)`
- `turbo_agent_app_get_thread(...)`
- `turbo_agent_app_get_run(...)`
- `turbo_agent_app_get_checkpoint(...)`
- `turbo_agent_app_get_child_run(...)`
- `turbo_agent_app_get_child_checkpoint(...)`
- `turbo_agent_app_list_child_runs(...)`
- `turbo_agent_app_load_child_history_events_bind(...)`
- `turbo_agent_app_list_runs(...)`
- `turbo_agent_app_list_checkpoints(...)`
- `turbo_agent_app_get_branch_tree(...)`
- `turbo_agent_app_list_thread_lineage(...)`
- `turbo_agent_app_load_history_events_bind(...)`
- `turbo_agent_app_replay_history_bind(...)`
- `turbo_agent_app_replay_thread_history_bind(...)`
- `turbo_agent_app_start_text(...)`
- `turbo_agent_app_start_text_stream(...)`
- `turbo_agent_app_start_messages(...)`
- `turbo_agent_app_start_messages_stream(...)`
- `turbo_agent_app_invoke_text(...)`
- `turbo_agent_app_invoke_messages_text(...)`
- `turbo_agent_app_invoke_json(...)`
- `turbo_agent_app_invoke_messages_json(...)`
- `turbo_agent_app_batch_text(...)`
- `turbo_agent_app_memory_get(...)`
- `turbo_agent_app_memory_put(...)`
- `turbo_agent_app_memory_put_context(...)`
- `turbo_agent_app_memory_delete(...)`
- `turbo_agent_app_memory_list(...)`

It does not invent a second runtime. It only packages one configured session as
a more obvious `create_agent(...).invoke(...)`-style entrypoint.

For composition inside larger graphs and tool runtimes, `Runtime V2` now also
adds:

- `turbo_agent_subagent_add_tool_runtime(...)`
- `turbo_agent_subagent_add_tool_registry(...)`

Subagent tools preserve the borrowed session preset configuration, including
knowledge-store and generic retriever workflow settings, so a delegated agent
can use the same local-context engineering loop as a top-level session/app.

Those adapters let one configured session/app behave as a tool without adding a
second execution model. Shared mode reuses one owned app and therefore keeps
thread/run lineage across tool calls; stateless mode creates one fresh
ephemeral app per invocation.

Subagent result envelopes also expose `child_thread_id`, `child_run_id`,
`child_checkpoint_id`, and `child_status`, and parent `tool_results.outputs[]`
can surface the same child lineage through `turbo_agent_state_*` helpers. The
same output item can then be resolved through
`turbo_agent_session_get_child_run(...)`,
`turbo_agent_session_get_child_checkpoint(...)`,
`turbo_agent_session_load_child_history_events_bind(...)`,
`turbo_agent_app_get_child_run(...)`, and
`turbo_agent_app_get_child_checkpoint(...)`,
`turbo_agent_app_load_child_history_events_bind(...)`.
When no parent lineage is known, top-level `parent_agent_run_id`,
`parent_tool_call_id`, `parent_tool_name`, `parent_graph_run_id`, and
`call_frame_id` remain present as `null` so hosts can treat the result envelope
as one stable shape.

For hosts that already know parent context before starting a child run, runtime
records may now also persist `parent_agent_run_id`, `parent_tool_call_id`,
`parent_tool_name`, `parent_graph_run_id`, and `call_frame_id`, and child runs
can be indexed later through
`turbo_agent_runtime_list_child_runs(...)`,
`turbo_agent_session_list_child_runs(...)`, and
`turbo_agent_app_list_child_runs(...)`. When that lineage is known up front, the
same fields are also echoed on the returned runtime `summary`. The graph/run
pair gives hosts a stable nested call-frame marker without introducing a second
event log or store.

When one subagent starts during an active parent tool call and the child session
did not set explicit parent metadata, those same fields are now inherited
automatically from the current parent run + tool context. The same active-tool
fallback also applies to `list_child_runs(NULL, ...)` on the session/app
wrappers, so hosts can query child runs without restating the parent run id
inside that tool execution window.

For graph-native nesting, `turbo_agent_subgraph.h` adds
`turbo_agent_install_subgraph_node(...)`. The installed bind-native node starts
the child graph through the same durable runtime and records the child run with
the current parent run id plus `parent_graph_run_id` / `call_frame_id`. The
parent state receives one `subgraph_result` envelope by default, including
child status booleans and pending child checkpoint hints when the child graph
interrupts. Hosts resume or fork that child through the same runtime checkpoint
APIs; the child run/checkpoint records remain the durable source of truth.

`turbo_agent_memory_store_t` is a separate long-term memory surface with
explicit `namespace + key + value_json` records. Stores may now also expose one
native `query(namespace_prefix, kind, key_prefix, text_substring)` callback;
when absent, the host-facing `*_memory_query_records(...)` helpers still fall
back to `list + canonicalize + filter`. That native `query` callback returns
the same canonical record shape used by `*_memory_list_records(...)` /
`*_memory_query_records(...)`, not the raw `namespace/key/value_json` entries
used by `list(...)`. It does not replace:

`Runtime V3` also raises the canonical memory-record contract into one explicit
host-facing helper group:

- `turbo_agent_memory_get_record(...)`
- `turbo_agent_memory_put_record(...)`
- `turbo_agent_memory_validate_record(...)`
- `turbo_agent_memory_query_records_ex(...)`
- session/app wrapper twins for the same helpers

That keeps the old raw KV surface intact while giving hosts one stable
record-first path for `context` records and one struct-based query entrypoint.

`turbo_agent_memory_query_options_t` carries `namespace_prefix`, `kind`,
`key_prefix`, `text_substring`, `id_prefix`, `metadata_scope`,
`metadata_path_prefix`, `created_after`, `created_before`, `sort_by`,
`sort_order`, and `limit`. `sort_by` supports `id`, `namespace`, `kind`, and
`key`; `sort_order` supports `asc` or `desc`; `limit=0` means no truncation.
`created_after` and `created_before` apply one inclusive lower/upper bound to
canonical `created_at`, and the current implementation relies on stable
ISO-8601 timestamp strings so the window can be checked lexicographically. The native backend
callback remains the stable four-filter
`query(namespace_prefix, kind, key_prefix, text_substring)` contract;
`id_prefix`/`metadata_scope`/`metadata_path_prefix`/
`created_after`/`created_before`/`sort_by`/`sort_order`/`limit` stay in the
host-facing layer as canonical record post-processing.

- `turbo_agent_store_t` for state memory slots
- `turbo_agent_runtime_store_t` for thread/run/checkpoint lineage

`Runtime V2` also adds preset graph builders around one owned agent:

- `turbo_agent_session_create_loop_graph(...)`
- `turbo_agent_session_create_review_graph(...)`
- `turbo_agent_session_create_engineering_graph(...)`
- `turbo_agent_session_create_preset_graph(...)`

and direct preset-run wrappers:

- `turbo_agent_session_create_input_state_bind(...)`
- `turbo_agent_session_create_input_messages_state_bind(...)`
- `turbo_agent_session_start_preset_bind_graph(...)`
- `turbo_agent_session_start_preset_text(...)`
- `turbo_agent_session_start_text(...)`
- `turbo_agent_session_start_text_stream(...)`
- `turbo_agent_session_start_preset_messages(...)`
- `turbo_agent_session_start_messages(...)`
- `turbo_agent_session_start_messages_stream(...)`
- `turbo_agent_session_result_text(...)`
- `turbo_agent_session_invoke_preset_text(...)`
- `turbo_agent_session_invoke_text(...)`
- `turbo_agent_session_invoke_preset_messages_text(...)`
- `turbo_agent_session_invoke_messages_text(...)`
- `turbo_agent_session_invoke_preset_json(...)`
- `turbo_agent_session_invoke_json(...)`
- `turbo_agent_session_invoke_preset_messages_json(...)`
- `turbo_agent_session_invoke_messages_json(...)`
- `turbo_agent_session_batch_text(...)`
- `turbo_agent_session_memory_get(...)`
- `turbo_agent_session_memory_put(...)`
- `turbo_agent_session_memory_put_context(...)`
- `turbo_agent_session_memory_delete(...)`
- `turbo_agent_session_memory_list(...)`
- `turbo_agent_session_memory_list_records(...)`
- `turbo_agent_session_memory_get_record(...)`
- `turbo_agent_session_memory_put_record(...)`
- `turbo_agent_session_memory_validate_record(...)`
- `turbo_agent_session_memory_query_records(...)`
- `turbo_agent_session_memory_query_records_ex(...)`
- `turbo_agent_session_load_memory_context(...)`
- `turbo_agent_session_create_input_state_with_memory_bind(...)`
- `turbo_agent_session_create_input_messages_state_with_memory_bind(...)`
- `turbo_agent_session_start_preset_text_with_memory(...)`
- `turbo_agent_session_invoke_preset_text_with_memory(...)`
- `turbo_agent_session_invoke_preset_json_with_memory(...)`
- `turbo_agent_session_resume_preset_bind_graph(...)`
- `turbo_agent_session_fork_preset_bind_graph(...)`

This is now a minimal runnable composition surface for bind-native
`invoke` / `stream` / `batch`: chains, graphs, state graphs, and session/app
default workflows can be wrapped and piped. The wrapper surface is still a
small before/after hook layer, not a full LangChain middleware or
remote-runnable framework.

It now also keeps one canonical message-array path, so hosts that already have
full prompt messages do not need to flatten them into one fake user string
before invoking a preset workflow.

For the full `Runtime V2` design and API notes, see:

- `langchain/docs/runtime-v2.md`
- `langchain/docs/langgraph-gap-analysis.md`

Use the two examples for different questions:

- `runtime_review_resume.c`: how to interrupt, override state, resume, and fork
- `runtime_memory_record_helpers.c`: how to use record-first memory helpers on
  the store/session/app layers
- `runtime_history_inspect.c`: how to inspect persisted runs, checkpoints, and history

For the remote host-facing inspect layers, use
`langchain/examples/runtime_remote_inspect_helpers.c`.
That example mounts one in-process Iris JSON-RPC bridge, starts one interrupted
thread, and then walks the three remote host layers with one representative
inspect helper per layer so the smoke test stays small and stable:

- typed remote client:
  prints `get_supervisor_inspect(...)`
- remote session wrapper:
  prints `get_orchestration_inspect(...)`
- remote app wrapper:
  prints `get_child_multi_agent_inspect(...)`

The source comments point at the matching sibling helpers on each layer, so the
example still serves as the entrypoint for the full supervisor/orchestration/
child-multi-agent inspect trio without turning one example into a huge remote
round-trip stress test.

Its CMake target name is:

- `langchain_runtime_remote_inspect_helpers_example`

For a fuller walkthrough, see:

- `langchain/docs/runtime-v1.md`
- `langchain/docs/langgraph-gap-analysis.md`

### Example

```c
#include "turbo_agent_runtime.h"
#include "turbo_agent_graph.h"
#include "turbo_agent_state.h"

turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
turbo_graph_t *graph = build_review_graph();
turbo_runtime_data_bind_value_t *state = create_initial_review_state();
json_value_t *summary = NULL;
turbo_runtime_data_bind_value_t *result_state = NULL;

turbo_agent_runtime_start_bind_graph(runtime, graph, state, &options, NULL, &summary,
                                     &result_state);

if (strcmp(turbo_json_get_string(summary, "status"), "interrupted") == 0) {
  turbo_runtime_data_bind_value_t *override = approve_review(result_state);
  json_value_t *resume_summary = NULL;
  turbo_runtime_data_bind_value_t *final_state = NULL;

  turbo_agent_runtime_resume_bind_graph(
      runtime, graph, turbo_json_get_string(summary, "checkpoint_id"), override, NULL,
      &resume_summary, &final_state);
}
```

The example source shows the missing pieces:

- graph construction
- review-state mutation
- record lookup
- fork from an old checkpoint

### Querying durable records

Once a run has been persisted, the runtime exposes all lineage as JSON records:

```c
json_value_t *run = NULL;
json_value_t *checkpoint = NULL;
json_value_t *thread = NULL;
json_value_t *runs = NULL;
json_value_t *checkpoints = NULL;
json_value_t *lineage = NULL;
json_value_t *timeline = NULL;
turbo_runtime_data_bind_value_t *history = NULL;

turbo_agent_runtime_get_thread(runtime, thread_id, &thread);
turbo_agent_runtime_get_run(runtime, run_id, &run);
turbo_agent_runtime_get_checkpoint(runtime, checkpoint_id, &checkpoint);
turbo_agent_runtime_list_runs(runtime, thread_id, &runs);
turbo_agent_runtime_list_checkpoints(runtime, run_id, &checkpoints);
turbo_agent_runtime_list_thread_lineage(runtime, thread_id, &lineage);
turbo_agent_runtime_get_thread_timeline_bind(runtime, thread_id, &timeline);
turbo_agent_runtime_load_history_events_bind(runtime, run_id, NULL, &history);
```

This keeps the runtime model narrow:

- records stay JSON objects and arrays
- lineage inspection stays read-only and returns `thread_id`,
  `latest_run_id`, `pending_run_id`, `root_checkpoint_id`, and `branches`
- each `branches[]` entry carries `run_id`, `checkpoint_id`,
  `branch_root_checkpoint_id`, `parent_checkpoint_id`, `status`, `updated_at`,
  `checkpoint_summary`, and `source_checkpoint_summary`
- each `edges[]` entry carries `source_checkpoint_id`, `target_run_id`, and
  `source_checkpoint_summary`
- timeline records stay a single host-facing bundle with `thread`,
  `resolved_current_run`, `resolved_current_checkpoint_id`,
  `resolved_current_checkpoint`, `latest_run`, `pending_run`, `runs`,
  `current_run_checkpoints`, and `history_events`
- `resolved_current_checkpoint` is a lightweight summary; full persisted
  checkpoint records still come from `get_checkpoint(...)`
- the per-branch and per-edge summary fields are lightweight inspect views,
  not full checkpoint records
- the current run in that bundle always resolves to pending run first, otherwise
  latest run
- event history stays bind-native
- returned summaries keep `parent_agent_run_id`, `parent_tool_call_id`, and
  `parent_tool_name` as stable nullable fields
- hosts can inspect or persist lineage without adopting private structs

### Contract examples

These compact examples match the stable shapes the tests now treat as golden
contracts:

For every inspect surface here, the lightweight `checkpoint_summary` shape is
the same small whitelist: `id`, `run_id`, `parent_checkpoint_id`, `status`,
`created_at`, `next_node`, `seq`, and numeric `steps`.
The matching golden fixture files live under
`langchain/tests/fixtures/runtime_v2/`.
That directory now covers both runtime inspect shapes and the newer
multi-agent session/app inspect bundles such as
`child_orchestration_inspect.golden.json` and
`child_multi_agent_inspect.golden.json`. It now also covers the Stage 4 memory
contract with:
- `memory_context_record.golden.json`
- `memory_json_record.golden.json`
- `memory_query_results.golden.json`
And the runtime-only cross-thread observability query contract is pinned by
`observability_queries.golden.json`.

```jsonc
// checkpoint context
{
  "checkpoint_summary": {
    "id": "ckpt_123",
    "run_id": "run_123",
    "parent_checkpoint_id": null,
    "seq": 1,
    "status": "interrupted",
    "created_at": "2026-04-16T12:00:00Z",
    "next_node": "review",
    "steps": 1
  },
  "state": {...},
  "run": {...},
  "thread": {...},
  "history_events": [...]
}

// interrupted thread timeline
{
  "thread": {"id": "thr_123"},
  "resolved_current_run": {"id": "run_123"},
  "resolved_current_checkpoint_id": "ckpt_123",
  "resolved_current_checkpoint": {
    "id": "ckpt_123",
    "run_id": "run_123",
    "parent_checkpoint_id": null,
    "seq": 1,
    "status": "interrupted",
    "created_at": "2026-04-16T12:00:00Z",
    "next_node": "review",
    "steps": 1
  },
  "latest_run": {"id": "run_123"},
  "pending_run": {"id": "run_123"},
  "runs": [{"id": "run_123"}],
  "current_run_checkpoints": [{"id": "ckpt_123"}],
  "history_events": [...]
}

// forked branch tree
{
  "thread_id": "thr_123",
  "current_run_id": "run_456",
  "latest_run_id": "run_456",
  "pending_run_id": null,
  "current_checkpoint_id": null,
  "current_checkpoint_summary": null,
  "current_branch": {
    "run_id": "run_456",
    "parent_run_id": "run_123",
    "source_checkpoint_id": "ckpt_123",
    "branch_root_checkpoint_id": "ckpt_123",
    "checkpoint_id": null,
    "checkpoint_summary": null,
    "source_checkpoint_summary": {
      "id": "ckpt_123",
      "run_id": "run_123",
      "parent_checkpoint_id": null,
      "seq": 1,
      "status": "interrupted",
      "created_at": "2026-04-16T12:00:00Z",
      "next_node": "review",
      "steps": 1
    }
  },
  "branches": [
    {
      "run_id": "run_123",
      "parent_run_id": null,
      "source_checkpoint_id": null,
      "checkpoint_id": "ckpt_123",
      "checkpoint_summary": {
        "id": "ckpt_123",
        "run_id": "run_123",
        "parent_checkpoint_id": null,
        "status": "interrupted",
        "created_at": "2026-04-16T12:00:00Z",
        "next_node": "review",
        "seq": 1,
        "steps": 1
      },
      "source_checkpoint_summary": null,
      "branch_root_checkpoint_id": "ckpt_123"
    },
    {
      "run_id": "run_456",
      "parent_run_id": "run_123",
      "source_checkpoint_id": "ckpt_123",
      "checkpoint_id": null,
      "checkpoint_summary": null,
      "source_checkpoint_summary": {
        "id": "ckpt_123",
        "run_id": "run_123",
        "parent_checkpoint_id": null,
        "status": "interrupted",
        "created_at": "2026-04-16T12:00:00Z",
        "next_node": "review",
        "seq": 1,
        "steps": 1
      },
      "branch_root_checkpoint_id": "ckpt_123"
    }
  ],
  "edges": [
    {
      "kind": "fork",
      "source_checkpoint_id": "ckpt_123",
      "target_run_id": "run_456",
      "source_checkpoint_summary": {
        "id": "ckpt_123",
        "run_id": "run_123",
        "parent_checkpoint_id": null,
        "status": "interrupted",
        "created_at": "2026-04-16T12:00:00Z",
        "next_node": "review",
        "seq": 1,
        "steps": 1
      }
    }
  ]
}

// command descriptor (no-input flavor)
{
  "name": "approve_review",
  "label": "Approve Review",
  "category": "review",
  "input_mode": "none",
  "requires_input": false,
  "placeholder": null,
  "example_payload": {
    "kind": "approve_review",
    "approved": true
  },
  "success_state_hint": "Marks review as approved so execution can continue."
}

// command descriptor (text-input flavor)
{
  "name": "append_feedback",
  "label": "Append Feedback",
  "category": "input",
  "input_mode": "text",
  "requires_input": true,
  "placeholder": "Add one short feedback message for the next run.",
  "example_payload": {
    "kind": "append_feedback",
    "text": "please add tests"
  },
  "success_state_hint": "Appends one user-visible feedback message to the canonical input."
}
```

These two examples cover the two host-facing extremes this runtime treats as
stable contract:

- no-input actions such as `approve_review`
- text-input actions such as `append_feedback`

Other descriptors may still add command-routing fields such as `primary_key`,
`accepted_keys`, `fallback_keys`, and `supports_json_value`.

## Link model

```cmake
target_link_libraries(my_app PRIVATE TurboNet::LangChain)
```

If you need builtin shell/file tools or engineering-agent helpers, link
`TurboNet::Agent` instead.

## Cross-platform intent

The library layer keeps platform-sensitive behavior narrow:

- path policy is case-sensitive on non-Windows hosts
- Windows path separators are normalized only on Windows
- tool execution should converge on host-neutral contracts with backend vtables
- runtime data binding should converge on host-neutral value builders and schemas
- binary codec work should share one validated wire reader before MIR specialization
- binary schema work should collect one explicit ABI contract before MIR emission
- MIR codegen should target one stable extern plan instead of ad hoc callback wiring
- backend implementations may use native loaders or wasm3-based sandboxes
- cross-platform utilities such as filesystem and platform helpers should be reused
  rather than reimplemented inside agent hosts
- application CLI integration tests stay outside this module

That keeps `langchain/` focused on library semantics rather than host tooling.

