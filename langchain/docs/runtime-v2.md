# Runtime V2

`Runtime V2` does not replace `Runtime V1`. It adds one thin host-facing layer
above it.

`Runtime V1` solved durable execution:

- `thread`
- `run`
- `checkpoint`
- `resume`
- `fork`
- `history`

`Runtime V2` solves two remaining host problems:

- repeated runtime bootstrap glue
- ad hoc long-term memory key naming
- packaging one configured app/session as a reusable tool
- read-only thread lineage / branch inspection without re-implementing runtime queries

The current `Runtime V3` staging work builds on that base without changing the
durable runtime store contract. The first orchestration foundation is:

- one canonical `supervisor_versions` state lane for `active_agent`,
  `target_agent`, `handoff_reason`, `inbox`, and `handoff_history`
- one minimal `turbo_agent_install_supervisor_loop(...)` installer that adds
  supervisor/handoff route nodes while leaving concrete agent nodes to the host

## New public headers

Include:

```c
#include "turbo_agent_session.h"
#include "turbo_agent_memory_store.h"
#include "turbo_agent_subagent.h"
#include "turbo_agent_subgraph.h"
#include "turbo_agent_runtime_remote.h"
#include "turbo_agent_runtime_remote_client.h"
```

Or use the aggregate include:

```c
#include "turbo_langchain.h"
```

## Agent session

`turbo_agent_session_t` is a thin wrapper around:

- one `turbo_agent_runtime_t`
- one optional `turbo_agent_t`
- one optional `turbo_agent_memory_store_t`
- remembered `thread_id`
- remembered latest `run_id`
- remembered latest non-null `checkpoint_id`

It exists to remove host repetition, not to invent new runtime semantics.

The underlying runtime surface will also expose
`turbo_agent_runtime_get_thread_timeline_bind(...)` and
`turbo_agent_runtime_list_thread_lineage(...)`; the session and app wrappers
keep the same timeline shape and lineage shape, only with their own remembered
thread context.

### Session config

`turbo_agent_session_config_t` contains:

- `runtime_store`
- `memory_store`
- `agent_config`
- `thread_id`
- `env_path`
- `load_env`
- `overwrite_env`
- `workflow_kind`
- `memory_namespace`
- `parent_agent_run_id`
- `parent_tool_call_id`
- `parent_tool_name`
- `parent_graph_run_id`
- `call_frame_id`
- `knowledge_store`
- `knowledge_query`
- `knowledge_kind`
- `knowledge_uri_prefix`
- `knowledge_limit`
- `retriever`
- `retriever_query`
- `retriever_kind`
- `retriever_uri_prefix`
- `retriever_limit`
- `retriever_scope`

When `load_env` is non-zero, the session applies:

```c
turbo_agent_config_apply_env(...)
```

That path already reuses the parser layer's:

- `turbo_dotenv_load(...)`
- `turbo_dotenv_load_default(...)`

before creating the agent.

This means real work can pick up:

- `OPENAI_API_KEY`
- `OPENAI_BASE_URL`
- `OPENAI_MODEL`
- `OPENAI_PROVIDER`

from `.env` without making every host duplicate the same bootstrap code.

When `workflow_kind` is
`TURBO_AGENT_SESSION_WORKFLOW_KNOWLEDGE_ENGINEERING`, `knowledge_store` must
point at an opened `turbo_agent_knowledge_store_t`. The preset builds the
engineering loop with a leading knowledge-context node, using
`knowledge_query` when supplied or the latest user input otherwise.
`knowledge_kind`, `knowledge_uri_prefix`, and `knowledge_limit` narrow the
retrieval query before the planner runs.

When `workflow_kind` is
`TURBO_AGENT_SESSION_WORKFLOW_RETRIEVER_ENGINEERING`, `retriever` must point at
an opened `turbo_retriever_t`. The preset builds the same engineering loop with
a leading retriever-context node, using `retriever_query` when supplied or the
latest user input otherwise. `retriever_kind`, `retriever_uri_prefix`, and
`retriever_limit` narrow the retrieval query, while `retriever_scope` controls
the memory-context layer scope added to planner input.

### Session API

- `turbo_agent_session_create(...)`
- `turbo_agent_session_destroy(...)`
- `turbo_agent_session_agent(...)`
- `turbo_agent_session_runtime(...)`
- `turbo_agent_session_memory_store(...)`
- `turbo_agent_session_workflow_kind(...)`
- `turbo_agent_session_memory_namespace(...)`
- `turbo_agent_session_model(...)`
- `turbo_agent_session_base_url(...)`
- `turbo_agent_session_provider_name(...)`
- `turbo_agent_session_has_api_key(...)`
- `turbo_agent_session_thread_id(...)`
- `turbo_agent_session_last_run_id(...)`
- `turbo_agent_session_last_checkpoint_id(...)`
- `turbo_agent_session_get_thread(...)`
- `turbo_agent_session_get_run(...)`
- `turbo_agent_session_get_checkpoint(...)`
- `turbo_agent_session_get_thread_state_bind(...)`
- `turbo_agent_session_get_run_state_bind(...)`
- `turbo_agent_session_get_checkpoint_state_bind(...)`
- `turbo_agent_session_get_child_run(...)`
- `turbo_agent_session_get_child_checkpoint(...)`
- `turbo_agent_session_list_runs(...)`
- `turbo_agent_session_list_child_runs(...)`
- `turbo_agent_session_list_checkpoints(...)`
- `turbo_agent_session_list_thread_lineage(...)`
- `turbo_agent_session_get_thread_timeline_bind(...)`
- `turbo_agent_session_load_history_events_bind(...)`
- `turbo_agent_session_load_child_history_events_bind(...)`
- `turbo_agent_session_apply_command_bind(...)`
- `turbo_agent_session_resume_command_bind(...)`
- `turbo_agent_session_fork_command_bind(...)`
- `turbo_agent_session_start_bind_graph(...)`
- `turbo_agent_session_resume_bind_graph(...)`
- `turbo_agent_session_fork_bind_graph(...)`
- `turbo_agent_session_create_loop_graph(...)`
- `turbo_agent_session_create_review_graph(...)`
- `turbo_agent_session_create_engineering_graph(...)`
- `turbo_agent_session_create_preset_graph(...)`
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
- `turbo_agent_session_resume_preset_bind_graph(...)`
- `turbo_agent_session_fork_preset_bind_graph(...)`

### Session behavior

The session does one useful thing on top of raw runtime calls:

- after `start`, it remembers thread/run/checkpoint ids
- after `resume`, it updates thread/run and keeps the last non-null checkpoint
- after `fork`, it updates thread/run and still keeps the last source checkpoint

That means callers may pass `NULL` for `checkpoint_id` on resume/fork and let
the session use the last remembered checkpoint.

The same wrapper rule now exists for runtime inspection:

- `get_run(NULL, ...)` resolves to the latest remembered run
- `get_checkpoint(NULL, ...)` resolves to the latest remembered checkpoint
- `get_run_state_bind(NULL, ...)` resolves to the latest remembered run
- `get_checkpoint_state_bind(NULL, ...)` resolves to the latest remembered checkpoint
- `list_checkpoints(NULL, ...)` resolves to checkpoints for the latest remembered run
- `load_history_events_bind(NULL, NULL, ...)` resolves to the latest remembered
  run/checkpoint pair
- `turbo_agent_session_get_thread_timeline_bind(...)` resolves the latest
  remembered thread and returns one timeline bundle with `thread`,
  `resolved_current_run`, `resolved_current_checkpoint_id`,
  `resolved_current_checkpoint`, `latest_run`, `pending_run`, `runs`,
  `current_run_checkpoints`, and `history_events`

`resolved_current_checkpoint` is a lightweight summary for the current thread
head. Full checkpoint records still come from `get_checkpoint(...)`.
That lightweight summary is intentionally small and stable:
`id`, `run_id`, `parent_checkpoint_id`, `status`, `created_at`, `next_node`,
`seq`, and numeric `steps`.

For an interrupted thread, hosts should treat the timeline bundle like this:

```json
{
  "thread": {"id": "thr_..."},
  "resolved_current_run": {"id": "run_interrupt"},
  "resolved_current_checkpoint_id": "ckpt_review",
  "resolved_current_checkpoint": {
    "id": "ckpt_review",
    "run_id": "run_interrupt",
    "parent_checkpoint_id": null,
    "status": "interrupted",
    "created_at": "2026-04-16T12:00:00Z",
    "next_node": "review",
    "seq": 1,
    "steps": 1
  },
  "latest_run": {"id": "run_interrupt"},
  "pending_run": {"id": "run_interrupt"},
  "runs": [{"id": "run_interrupt"}],
  "current_run_checkpoints": [{"id": "ckpt_review"}],
  "history_events": [{"type": "node_start"}]
}
```

The important part is the selection rule, not the literal ids: for interrupted
threads, `resolved_current_run` and `pending_run` point at the interrupted run,
and `resolved_current_checkpoint` is always the lightweight summary for that
head checkpoint.

`turbo_agent_session_list_thread_lineage(...)` is a separate read-only view for
browser-style inspection:

- it returns lineage for one thread without changing state
- it does not change `resume_bind_graph(...)` / `fork_bind_graph(...)`
  semantics
- it is meant for host/UI lineage browsers, not for execution control
- the payload includes:
- `thread_id`
- `latest_run_id`
- `pending_run_id`
- `root_checkpoint_id`
- `branches`
- each `branches[]` entry includes:
- `run_id`
- `checkpoint_id`
- `branch_root_checkpoint_id`
- `parent_checkpoint_id`
- `status`
- `updated_at`

The same defaulting rule also now applies to command application:

- `apply_command_bind(NULL, command, ...)` resolves to the latest remembered
  checkpoint before producing a fresh `state_override`
- `resume_command_bind(NULL, command, ...)` resolves the latest remembered
  checkpoint, applies the command, then resumes in one call
- `fork_command_bind(NULL, command, ...)` resolves the latest remembered
  checkpoint, applies the command, then forks in one call
- the runtime also exposes thread-scoped command helpers, so hosts can start
  from one `thread_id` and let the runtime resolve the pending run and latest
  checkpoint internally before command application
- `resume_checkpoint_bind_graph(...)` / `fork_checkpoint_bind_graph(...)`
  are the explicit checkpoint-scoped replay aliases; they continue or fork
  directly from one concrete historical checkpoint
- `apply_checkpoint_command_bind(...)`,
  `resume_checkpoint_command_bind(...)`, and
  `fork_checkpoint_command_bind(...)` are the matching explicit
  checkpoint-scoped command aliases; they keep command application pinned to
  one concrete historical checkpoint
- `turbo_agent_runtime_get_checkpoint_context(...)` /
  `turbo_agent_session_get_checkpoint_context(...)` /
  `turbo_agent_app_get_checkpoint_context(...)` return one historical
  checkpoint as a read-only aggregate bundle with `checkpoint_summary`,
  `state`, `run`, `thread`, and `history_events`; the result is a context
  inspect view, not a replacement for `get_checkpoint(...)`
- `resume_thread_bind_graph(...)` / `fork_thread_bind_graph(...)` do the same
  for replay: they resolve the thread's current checkpoint internally, then
  forward the caller's state override into the existing resume/fork path
- `update_checkpoint_state_bind(...)` / `update_thread_state_bind(...)`
  formalize the low-level state-edit surface: they merge one bind-native state
  patch into the resolved checkpoint state and return one full
  `state_override`, without mutating persisted checkpoint records in place
- `resume_checkpoint_state_bind_graph(...)` /
  `fork_checkpoint_state_bind_graph(...)` and their thread-scoped variants
  collapse state patch plus resume/fork into one host-facing time-travel call
- completed-only threads have no current checkpoint, so the thread-scoped
  replay helpers fail explicitly instead of inventing one
- `get_latest_run(thread_id)` resolves the newest run on one thread by
  `updated_at`
- `get_pending_run(thread_id)` resolves the newest interrupted run on one
  thread
- `get_latest_checkpoint(run_id)` resolves `run.latest_checkpoint_id` without
  forcing the host to read and unpack the run record itself
- `turbo_agent_session_get_thread_timeline_bind(thread_id)` resolves the thread
  timeline with pending run first, otherwise latest run, so hosts do not need
  to make that choice themselves
- `load_thread_history_events_bind(thread_id)` prefers the newest interrupted
  run on the thread, then falls back to the newest run, and replays that run's
  durable history in one call
- `replay_history_bind(...)` / `replay_thread_history_bind(...)` do not define
  a second observer event model; they replay the same durable history objects
  through the existing `turbo_event_sink_bind_fn` callback boundary
- `turbo_event_stream_mode_accepts_bind(...)` and
  `turbo_event_stream_filter_sink_bind(...)` provide the core stream-mode
  adapter for that same event boundary. The modes are transport-agnostic:
  `all`, `messages`, `updates`, `tools`, and `debug`. HTTP/SSE surfaces should
  be thin adapters over this core stream contract, not the source of the
  runtime event model.
- `turbo_agent_session_start_text_stream(...)`,
  `turbo_agent_session_start_messages_stream(...)`,
  `turbo_agent_app_start_text_stream(...)`, and
  `turbo_agent_app_start_messages_stream(...)` are the default-workflow
  convenience wrappers over the same filter. They keep the normal session/app
  `summary` plus final state contract while narrowing live callbacks to one
  stream mode.
- `turbo_agent_session_batch_text(...)` and `turbo_agent_app_batch_text(...)`
  are the first default-workflow batch helpers. They execute user-text inputs
  sequentially through the same session/app state rules and return one JSON
  item per input.
- `turbo_runnable_batch_bind(...)`,
  `turbo_runnable_from_agent_session(...)`, and
  `turbo_runnable_from_agent_app(...)` expose the same default workflows as
  bind-native runnables. They share the existing runnable `invoke` and `stream`
  path, add sequential array batch fallback, and keep the result object shape
  as `summary`, `state`, and `output_text`.
- `turbo_runnable_wrap_bind(...)` adds the first bind-native wrapper hook
  surface. `before_invoke` may replace the input, `after_invoke` may replace
  the output, and the wrapper applies the same hooks for invoke, stream, and
  batch without taking ownership of the inner runnable.
- `observe_history_bind(...)` / `observe_thread_history_bind(...)` are the
  host-facing observer bridge for that same durable history. They reuse the
  existing history facts and narrow them into one observer family:
  `model_delta`, `tool_call_started`, `tool_result`, `state_updated`,
  `interrupted`, `completed`. Each emitted observer object carries
  `kind="observer"`, one narrowed `type`, and the original canonical event
  clone under `event`.
- `turbo_agent_session_add_trace_bind_sink(...)` /
  `turbo_agent_app_add_trace_bind_sink(...)` are the live-side bridge for the
  same canonical trace event shape, and
  `*_set_trace_history_enabled(...)` lets hosts persist those trace events into
  run state for later checkpoint or thread inspection
- `turbo_agent_session_add_observer_bind_sink(...)` /
  `turbo_agent_app_add_observer_bind_sink(...)` are the live-side observer
  counterparts. They sit above the existing trace stream and do not create a
  second persisted observer log.
- `get_*_trace_events_bind(...)` is the durable snapshot convenience layer for
  the same data. It is intentionally derived from persisted state, not a second
  observer log, and returns an empty array when no trace history exists yet.
- `get_thread_observability_index(...)` and the session/app wrappers are the
  matching thread-scoped observability bundle: they aggregate the existing
  `thread`, `latest_run`, `pending_run`, `thread_timeline`, `thread_lineage`,
  `branch_tree`, `history_events`, and `trace_events` facts, then derive
  top-level quick-read fields such as `current_status`,
  `current_interrupt_reason`, `current_pending_action`,
  `current_checkpoint_summary`, `latest_run_status`, `latest_run_updated_at`,
  `pending_run_id`, `pending_checkpoint_id`, `has_failure`,
  `has_model_error`, `has_guardrail_rejection`, `replan_requested`,
  `current_failure_reason`, `current_review_note`, `has_pending_review`,
  `has_handoff`, `active_agent`, and one `counts` object on top. They do not
  add a new persisted log or a second store.
- `list_observability_indexes(...)` is the runtime-only cross-thread
  counterpart: it returns one narrowed observability summary per persisted
  thread, sorted by thread `updated_at` descending, while session/app remain
  current-thread scoped.
- `list_observability_indexes_filtered(...)` is the matching runtime-only
  filter layer for cross-thread lists. The current filter keys are `status`,
  `has_pending_review`, `has_failure`, `has_handoff`, `active_agent`,
  `has_model_error`, `has_guardrail_rejection`, `replan_requested`,
  `current_interrupt_reason`, `latest_run_status`,
  `latest_run_updated_after`, `latest_run_updated_before`, and
  `thread_id_prefix`; it also supports `sort_by`, `sort_order`, and `limit`.
  The default ordering remains `latest_run_updated_at desc`; `sort_by=thread_id`
  defaults to `asc`.
- `turbo_agent_runtime_remote_dispatch_jsonrpc(...)` is one transport-agnostic
  local JSON-RPC 2.0 dispatcher over that same runtime surface. The current
  method set currently includes `runtime.start`, `runtime.resume`,
  `runtime.fork`, `runtime.applyCommand`,
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
  one optional `memory_store`, the same bridge also exposes
  `memory.getRecord`, `memory.putRecord`, `memory.listRecords`,
  `memory.deleteRecord`, and `memory.queryRecordsEx`. Remote memory stays
  canonical record-first on this surface and does not expose one legacy raw
  `memory.list` transport contract. It exists so a future HTTP or RPC adapter
  can reuse one stable runtime contract instead of re-encoding calls ad hoc.
- `turbo_agent_runtime_remote_dispatch_jsonrpc_text(...)` is the transport-
  neutral raw JSON text adapter over that same dispatcher.
- `turbo_agent_runtime_remote_handle_http_jsonrpc(...)` is the current HTTP-
  like adapter for `POST /v1/runtime/jsonrpc`. It still does not start a
  listener or own a server; it only maps adapter-level path/method/body checks
  to HTTP status while keeping JSON bodies aligned with the same runtime
  contract.
- `turbo_agent_runtime_remote_client_call_json(...)` is the current consumer-
  side bridge over `rpc_client`. It calls that same JSON-RPC contract, returns
  parsed `result` payloads, and normalizes error replies into one JSON object
  with `code`, `message`, `http_status`, and `transport_error`.
- `turbo_agent_runtime_remote_client_start_bind_graph(...)`,
  `resume_bind_graph(...)`, `fork_bind_graph(...)`,
  `get_thread_state_bind(...)`, `get_checkpoint_context(...)`,
  `get_run(...)`, `get_checkpoint(...)`, `list_checkpoints(...)`,
  `load_history_events_bind(...)`, `get_run_trace_events_bind(...)`,
  `get_checkpoint_trace_events_bind(...)`,
  `get_memory_record(...)`, `put_memory_record(...)`,
  `delete_memory_record(...)`, `query_memory_records_ex(...)`,
  `query_memory_records(...)`, and `list_memory_records(...)`,
  `get_thread_timeline_bind(...)`, `get_branch_tree(...)`,
  `get_thread_observability_index(...)`,
  `list_observability_indexes(...)`,
  `list_observability_indexes_filtered(...)`,
  `list_child_runs(...)`,
  `get_supervisor_inspect(...)`, `get_orchestration_inspect(...)`,
  `resume_thread_command_bind(...)`, and `fork_thread_command_bind(...)` are
  the first typed helpers above that generic call. They map back onto the
  existing runtime `summary/state/context/index` contract instead of
  introducing a second client-only result model.
- That typed remote client layer now also includes
  `get_child_inspect(...)`, `get_child_orchestration_inspect(...)`, and
  `get_child_multi_agent_inspect(...)`. Those helpers stay read-only and are
  assembled from the same remote thread-state, observability, child-run, and
  child-checkpoint/history facts rather than adding a second persistence
  surface on the client side.
- `turbo_agent_remote_session_t` now adds one thread-scoped host facade above
  the typed remote client. It caches `thread_id`, `last_run_id`, and
  `last_checkpoint_id` from successful remote summaries, then reuses those ids
  for thread/run inspect helpers, `start_text(...)` / `start_messages(...)`,
  `invoke_text(...)` / `invoke_messages_text(...)`, `invoke_json(...)` /
  `invoke_messages_json(...)`, `memory_list_records(...)`,
  `memory_get_record(...)`, `memory_put_record(...)`,
  `memory_validate_record(...)`, `memory_query_records(...)`,
  `memory_query_records_ex(...)`, thread-state inspect, checkpoint-context reads,
  observability/timeline/history/observer/trace/branch-tree/lineage accessors,
  supervisor inbox/handoff/inspect reads, child-run listing,
  child run/checkpoint/history/trace inspect,
  child inspect/orchestration/multi-agent inspect,
  orchestration inspect, and
  thread-command resume/fork helpers.
- `turbo_agent_remote_app_t` now adds one thinner app facade that owns one
  `turbo_agent_remote_session_t` plus one fixed `graph_name`. It keeps the
  remote app layer additive and avoids pretending that the remote bridge owns a
  local `turbo_graph_t *` or `turbo_agent_runtime_t`. The same app facade also
  forwards `memory_list_records(...)`, `memory_get_record(...)`,
  `memory_put_record(...)`, `memory_validate_record(...)`,
  `memory_query_records(...)`, and `memory_query_records_ex(...)`.
- `turbo_agent_runtime_remote_iris_mount(...)` is the current Iris adapter over
  that same bridge. It binds by app/path pair through Iris's RPC endpoint
  registry, so it can now coexist with native `rpc_setup_endpoint(...)` routes
  on the same `iris_app_t` as long as endpoint paths differ.

Those remote session/app facades still reuse the exact same remote JSON-RPC
contract. They only cache ids and forward to the existing typed remote client
helpers; they do not define one second remote result shape or one second
persisted runtime state source.

The dispatcher does not define a second runtime result shape. It keeps the
existing runtime payloads and wraps them in JSON-RPC 2.0 envelopes:

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

Long-term memory is explicit on the same remote transport but stays separate
from `runtime.*` inspect/control. Remote memory stays canonical record-first:
`memory.listRecords` returns canonical records, `memory.deleteRecord` returns
`{ "deleted": true }` on success, and the bridge does not expose one legacy
raw `memory.list` result shape. The current list/delete/query shapes are:

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

The current record-first memory query shape is:

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

`memory.queryRecordsEx` mirrors the local
`turbo_agent_memory_query_options_t` shape. `sort_by` currently supports `id`,
`namespace`, `kind`, and `key`; `sort_order` supports `asc` or `desc`;
`limit=0` means no truncation. `created_after` and `created_before` apply one
inclusive lower/upper bound to canonical `created_at`, and the current
comparison depends on canonical ISO-8601 timestamp strings so lexicographic
string order matches time order.

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

The current minimum JSON-RPC error matrix is:
- `-32600`: invalid request
- `-32601`: method not found
- `-32602`: invalid params
- `-32603`: internal runtime or dispatch failure

`-32600` means the JSON-RPC envelope itself is malformed. `-32602` means the
envelope is valid but the selected runtime method receives malformed or
missing params.

The stable remote wrapper contract is locked by these golden fixtures:
- `runtime_remote_thread_control.golden.json`
- `runtime_remote_graph_runs.golden.json`
- `runtime_remote_inspect.golden.json`
- `runtime_remote_errors.golden.json`
- `runtime_remote_invalid_params.golden.json`

The host-facing inspect bundle contract is locked by these cross-surface golden
fixtures:
- `child_orchestration_inspect.golden.json`
- `child_multi_agent_inspect.golden.json`

The invalid-params golden set intentionally covers both simple inspect methods
and graph-bound/thread-bound methods so the dispatcher keeps one stable
parameter-validation contract.

Where params are stable, the thread-control / graph-run / inspect golden sets
also lock canonical request shapes, not only success envelopes. That keeps
`graph_name` / `thread_id` / `checkpoint_id` / `command` / `state` /
`state_patch` payload shapes aligned with the dispatcher implementation.

Graph-bound success responses are also golden-locked so the dispatcher keeps
one stable `summary/state` envelope for resume/fork and thread-scoped replay
methods.
- `get_child_trace_events_bind(...)` is the child-lineage counterpart: it reads
  trace history from the resolved child checkpoint when present, otherwise from
  the child run snapshot, without adding a new lineage factsource.
- `get_child_checkpoint_context(...)` is the one-shot inspect counterpart when
  the parent output item already carries `child_checkpoint_id`.
- `get_child_thread_timeline_bind(...)` is the one-shot child-thread timeline
  counterpart when the parent output item already carries `child_thread_id`.
- `get_child_branch_tree(...)` is the one-shot child-thread branch-tree
  counterpart when the parent output item already carries `child_thread_id`.
- `list_child_checkpoints(...)` is the child-run list counterpart when the
  parent output item already carries `child_run_id`.
- `get_child_inspect(...)` is the host-facing child detail bundle: it reuses the
  same child lineage facts to return `run`, `checkpoints`, optional
  `latest_checkpoint`, optional `checkpoint_context`, optional
  `thread_timeline`, optional `branch_tree`, and child `history_events` /
  `trace_events` in one read-only object.
- `get_child_orchestration_inspect(...)` is the parent/child orchestration
  counterpart on session/app: it keeps `child_inspect` intact and adds
  `parent_agent_run_id`, `parent_tool_call_id`, and `parent_tool_name` as the
  parent lineage wrapper.
- `get_child_multi_agent_inspect(...)` is the higher-level multi-agent
  aggregate on session/app: it reuses the current thread's
  `supervisor_inspect` / `orchestration_inspect` and the resolved
  `child_orchestration_inspect`, without creating a new runtime persistence
  surface.
- `get_supervisor_inbox(...)` / `get_supervisor_handoff_history(...)` are the
  control-state mailbox readers on session/app. They are derived from the
  current thread state and intentionally do not create a second runtime store
  collection.
- `get_supervisor_inspect(...)` is the matching one-shot control-state inspect
  bundle on session/app. It returns `supervisor`, `inbox`,
  `handoff_history`, `control_snapshot`, and `workflow_snapshot` in one
  read-only object.
- `get_orchestration_inspect(...)` is the next session/app aggregate layer:
  it bundles `supervisor_inspect`, `thread_timeline`, `thread_lineage`,
  `branch_tree`, and `child_runs` in one read-only multi-agent inspect object
  without introducing a new runtime persistence layer.
- `append_supervisor_inbox_message_bind(...)` is the matching write-side
  mailbox helper: it returns one state override bind value that the host can
  feed into the existing replay/resume/fork surfaces.
- interrupted summaries now expose `interrupt_reason`, `pending_node`,
  `pending_action`, legacy `available_commands`, and structured
  `available_command_descriptors` so hosts do not need to infer the next UI
  action from raw state alone

This is not a new special case. It removes a host-side special case.

The descriptor metadata is host-facing only: it helps a UI choose how to
present a command, but the runtime still resolves and executes the legacy
command name exactly as before.

When `workflow_kind` and optional `memory_namespace` are set on the session
config, the default wrappers:

- `turbo_agent_session_start_text(...)`
- `turbo_agent_session_start_messages(...)`
- `turbo_agent_session_invoke_text(...)`
- `turbo_agent_session_invoke_messages_text(...)`
- `turbo_agent_session_invoke_json(...)`
- `turbo_agent_session_invoke_messages_json(...)`

let the host act much closer to a `create_agent(...).invoke(...)` surface
without restating the same preset and memory namespace on every call, whether
the host begins from one user string or one canonical message array.

### App API

`turbo_agent_app_t` is one deliberately thin wrapper above session:

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
- `turbo_agent_app_get_thread_state_bind(...)`
- `turbo_agent_app_get_run_state_bind(...)`
- `turbo_agent_app_get_checkpoint_state_bind(...)`
- `turbo_agent_app_get_child_run(...)`
- `turbo_agent_app_get_child_checkpoint(...)`
- `turbo_agent_app_list_runs(...)`
- `turbo_agent_app_list_child_runs(...)`
- `turbo_agent_app_list_checkpoints(...)`
- `turbo_agent_app_list_thread_lineage(...)`
- `turbo_agent_app_get_thread_timeline_bind(...)`
- `turbo_agent_app_load_history_events_bind(...)`
- `turbo_agent_app_replay_history_bind(...)`
- `turbo_agent_app_replay_thread_history_bind(...)`
- `turbo_agent_app_load_child_history_events_bind(...)`
- `turbo_agent_app_apply_command_bind(...)`
- `turbo_agent_app_resume_command_bind(...)`
- `turbo_agent_app_fork_command_bind(...)`
- `turbo_agent_app_resume_preset_bind_graph(...)`
- `turbo_agent_app_fork_preset_bind_graph(...)`
- `turbo_agent_app_resume_preset_command_bind(...)`
- `turbo_agent_app_fork_preset_command_bind(...)`
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

This layer does not invent a second runtime. It removes one more host-side
special case by packaging a configured session as a more direct
`create_agent(...).invoke(...)`-style surface.

The app-level `turbo_agent_app_get_thread_timeline_bind(...)` wrapper follows
the same resolved-current-run rule as the session/runtime layer: pending run
first, otherwise latest run.

`turbo_agent_app_list_thread_lineage(...)` follows the same read-only lineage
inspection contract as the runtime/session layer. The same browser-facing
inspect surface is also exposed as:

- `turbo_agent_runtime_get_branch_tree(...)`
- `turbo_agent_session_get_branch_tree(...)`
- `turbo_agent_app_get_branch_tree(...)`

It is for time-travel and lineage browsing only; it does not alter resume,
fork, or checkpoint replay behavior. The minimum payload should include:

- `thread_id`
- `current_run_id`
- `latest_run_id`
- `pending_run_id`
- `current_checkpoint_id`
- `current_checkpoint_summary`
- `current_branch`
- `branches`
- `edges`

`branches` carries the branch nodes for the current thread or lineage root.
`edges` describes lineage links between checkpoints and runs. For fork edges,
the direction is always from the source checkpoint to the child run created
from that checkpoint. `pending_run_id` only reflects the current branch head:
it is present when the latest/current run is interrupted, and null when only
an older side branch remains pending. `current_branch` is the branch node for
`current_run_id`, and `current_checkpoint_id` is that branch head's checkpoint
id when one exists. `current_checkpoint_summary` is the lightweight summary for
that checkpoint. Branch nodes may also carry `branch_root_checkpoint_id` as the
stable entry checkpoint anchor for that branch. Branch nodes also carry
`checkpoint_summary` and `source_checkpoint_summary`, and lineage edges carry
`source_checkpoint_summary`; these are lightweight inspect views, not full
records, and they do not replace `get_checkpoint(...)`.
Those checkpoint summaries use the same stable field set:
`id`, `run_id`, `parent_checkpoint_id`, `status`, `created_at`, `next_node`,
`seq`, and numeric `steps`.

For a forked thread, the minimum branch-tree shape should look like this:

```json
{
  "thread_id": "thr_...",
  "current_run_id": "run_fork",
  "latest_run_id": "run_fork",
  "pending_run_id": null,
  "current_checkpoint_id": null,
  "current_checkpoint_summary": null,
  "current_branch": {
    "run_id": "run_fork",
    "parent_run_id": "run_base",
    "source_checkpoint_id": "ckpt_review",
    "branch_root_checkpoint_id": "ckpt_review",
    "checkpoint_id": null,
    "checkpoint_summary": null,
    "source_checkpoint_summary": {
      "id": "ckpt_review",
      "run_id": "run_base",
      "parent_checkpoint_id": null,
      "status": "interrupted",
      "created_at": "2026-04-16T12:00:00Z",
      "next_node": "review",
      "seq": 1,
      "steps": 1
    }
  },
  "branches": [
    {
      "run_id": "run_base",
      "parent_run_id": null,
      "source_checkpoint_id": null,
      "checkpoint_id": "ckpt_review",
      "checkpoint_summary": {
        "id": "ckpt_review",
        "run_id": "run_base",
        "parent_checkpoint_id": null,
        "status": "interrupted",
        "created_at": "2026-04-16T12:00:00Z",
        "next_node": "review",
        "seq": 1,
        "steps": 1
      },
      "source_checkpoint_summary": null,
      "branch_root_checkpoint_id": "ckpt_review"
    },
    {
      "run_id": "run_fork",
      "parent_run_id": "run_base",
      "source_checkpoint_id": "ckpt_review",
      "checkpoint_id": null,
      "checkpoint_summary": null,
      "source_checkpoint_summary": {
        "id": "ckpt_review",
        "run_id": "run_base",
        "parent_checkpoint_id": null,
        "status": "interrupted",
        "created_at": "2026-04-16T12:00:00Z",
        "next_node": "review",
        "seq": 1,
        "steps": 1
      },
      "branch_root_checkpoint_id": "ckpt_review"
    }
  ],
  "edges": [
    {
      "kind": "fork",
      "source_checkpoint_id": "ckpt_review",
      "target_run_id": "run_fork",
      "source_checkpoint_summary": {
        "id": "ckpt_review",
        "run_id": "run_base",
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
```

Hosts should read `edges[]` as the durable fork relation and treat
`branch_root_checkpoint_id` as the stable anchor for a branch, instead of
reconstructing those relations from raw checkpoint chains themselves.

`turbo_agent_*_get_checkpoint_context(...)` is the matching one-shot inspect
surface for a single historical checkpoint. It bundles the lightweight
`checkpoint_summary`, serialized `state`, its run, its thread, and
`history_events` so hosts can render a checkpoint detail panel without
reassembling those records themselves.

The README now includes compact golden examples for checkpoint context, thread
timeline, branch tree, command descriptors, and the newer multi-agent
session/app inspect bundles. Those examples mirror the stable field sets this
runtime section describes.

For review-style interrupts, the host now has two valid paths:

- keep `apply_command_bind(...)` when it wants to inspect or persist the
  produced override state itself
- use `resume_command_bind(...)` / `fork_command_bind(...)` when it only wants
  a one-shot "command then continue" path
- use `resume_preset_command_bind(...)` / `fork_preset_command_bind(...)` when
  it wants that same one-shot path on the configured preset workflow surface

`available_command_descriptors` is now the preferred host-facing contract. Each
descriptor carries:

- `name`
- `label`
- `description`
- `resume_mode`
- `args_schema`
- optional host/UI metadata like `category`, `input_mode`,
  `requires_input`, `suggested_title`, `primary_key`, `accepted_keys`,
  `fallback_keys`, `supports_json_value`, `placeholder`,
  `example_payload`, and `success_state_hint`

`available_commands` remains as a legacy string array for compatibility.

The descriptor shape is intentionally small and host-oriented. Hosts should use
these fields to render action panels and input forms; they do not alter the
legacy `available_commands` list. A review gate now looks like this on the
no-input side:

```json
{
  "name": "approve_review",
  "label": "Approve Review",
  "suggested_title": "Approve Review",
  "description": "Approve pending review and continue the interrupted run.",
  "category": "review",
  "input_mode": "none",
  "placeholder": null,
  "success_state_hint": "Marks review as approved so execution can continue.",
  "example_payload": {
    "kind": "approve_review",
    "approved": true
  },
  "resume_mode": "resume_or_fork",
  "primary_key": "approved",
  "accepted_keys": ["approved"],
  "fallback_keys": [],
  "requires_input": false,
  "supports_json_value": false,
  "args_schema": {
    "type": "object",
    "properties": {
      "approved": {
        "type": "boolean",
        "description": "Optional explicit approval flag. Defaults to true."
      }
    },
    "required": []
  }
}
```

And a text-input command looks like:

```json
{
  "name": "append_feedback",
  "label": "Append Feedback",
  "description": "Append one host feedback message to the user-visible input.",
  "category": "input",
  "input_mode": "text",
  "placeholder": "Add one short feedback message for the next run.",
  "success_state_hint": "Appends one user-visible feedback message to the canonical input.",
  "example_payload": {
    "kind": "append_feedback",
    "text": "please add tests"
  },
  "resume_mode": "resume_or_fork",
  "primary_key": "text",
  "accepted_keys": ["text", "message"],
  "fallback_keys": ["message"],
  "requires_input": true,
  "supports_json_value": false
}
```

Taken together, these two examples define the two stable host-facing extremes:
commands that require no extra input, and commands that want one text payload.
For text-heavy commands such as `append_feedback` or `override_final_output`,
hosts should prefer `placeholder` for one-line input hints,
`example_payload` for prefilled JSON/CLI examples, and `success_state_hint`
for optimistic UI copy after command application succeeds.

### Subagent tool adapters

`Runtime V2` also adds one narrow bridge from `session/app` into the existing
tool runtime and registry surfaces:

- `turbo_agent_subagent_add_tool_runtime(...)`
- `turbo_agent_subagent_add_tool_registry(...)`

This keeps composition additive:

- shared mode reuses one owned app and keeps thread/run lineage across tool calls
- stateless mode creates one fresh ephemeral app per tool call
- tool arguments may arrive as canonical `messages`, one `input` string, or any
  JSON value serialized into text
- borrowed session preset settings are preserved, including knowledge-store and
  generic retriever engineering workflows

The returned tool payload is one JSON object carrying `ok`, durable ids, the
runtime `summary`, and either `output_text` or `output_json`.

For child-run-aware hosts, subagent tool payloads also expose:

- `child_thread_id`
- `child_run_id`
- `child_checkpoint_id`
- `child_status`
- nullable `parent_agent_run_id`
- nullable `parent_tool_call_id`
- nullable `parent_tool_name`
- nullable `parent_graph_run_id`
- nullable `call_frame_id`

When the parent records these payloads under `tool_results.outputs[]`, the
state helpers can read child lineage and nested call-frame metadata directly
from the recorded output item.

When a host already knows the parent run context ahead of time, `Runtime V2`
may also persist `parent_agent_run_id`, `parent_tool_call_id`, and
`parent_tool_name` onto child run/checkpoint records. `Runtime V3` extends that
same parent link with `parent_graph_run_id` and `call_frame_id` so a nested
graph/subagent call can carry a stable call-frame marker without adding a
second persistence source.

Those same fields are also echoed on the returned runtime `summary` when
the run starts from `turbo_agent_runtime_start_bind_graph_linked(...)` or from
one session/app configured with parent lineage defaults.

When one subagent run starts inside an active parent tool call, and the child
session did not set explicit parent metadata, the session now also inherits the
current runtime tool context automatically:

- parent `run_id` -> `parent_agent_run_id`
- current tool `call_id` -> `parent_tool_call_id`
- current tool name -> `parent_tool_name`
- parent `run_id` -> `parent_graph_run_id`
- current tool `call_id` -> `call_frame_id`

That same fallback now also applies to:

- `turbo_agent_session_list_child_runs(session, NULL, ...)`
- `turbo_agent_app_list_child_runs(app, NULL, ...)`

when those calls happen inside the active parent tool execution window.

For graph-native nesting, `turbo_agent_subgraph.h` adds a minimal subgraph node
surface:

- `turbo_agent_subgraph_node(...)`
- `turbo_agent_install_subgraph_node(...)`

The installed node runs a child graph through the configured durable runtime,
uses the current parent runtime execution context as lineage, and writes one
`subgraph_result` envelope into the parent bind state. That envelope mirrors the
child durable ids, status booleans (`completed`, `interrupted`, `failed`), and
pending child checkpoint hints (`pending_checkpoint_id`, `pending_node`) when
the child graph interrupts. It also embeds the child `summary` / final `state`.
The envelope is a view of persisted run/checkpoint records, not a second log or
runtime store; hosts resume or fork an interrupted child through the same runtime
checkpoint APIs.

### Preset graph builders

`Runtime V2` now also adds three high-level graph builders around the owned
agent:

- `turbo_agent_session_create_loop_graph(...)`
- `turbo_agent_session_create_review_graph(...)`
- `turbo_agent_session_create_engineering_graph(...)`

These return one ready-to-run graph with stable builtin node names.

They do not replace raw workflow installers. They remove the most repetitive
host glue for the common “one session, one agent, one canned workflow” path.

For hosts that do not want to hold a temporary graph object at all, the preset
run wrappers build the graph, run `start/resume/fork`, then destroy the graph
before returning.

For the most common one-shot path, `turbo_agent_session_start_preset_text(...)`
also creates bind-native agent state from one user message before starting.

For hosts that already keep canonical prompt messages, the parallel helpers:

- `turbo_agent_session_create_input_messages_state_bind(...)`
- `turbo_agent_session_start_preset_messages(...)`
- `turbo_agent_session_invoke_preset_messages_text(...)`
- `turbo_agent_session_invoke_preset_messages_json(...)`

remove the last host-side step of collapsing a real message array into one
fake user string.

For the next step up, `turbo_agent_session_invoke_preset_text(...)` also
extracts the final user-facing answer text so hosts no longer need to convert
bind state back into JSON just to read the result.

For structured-output flows, `turbo_agent_session_invoke_preset_json(...)`
runs the same path and then parses the final answer into one JSON tree.

When the session owns one optional long-term memory store, hosts may also use:

- `turbo_agent_session_memory_store(...)`
- `turbo_agent_session_memory_get(...)`
- `turbo_agent_session_memory_put(...)`
- `turbo_agent_session_memory_put_context(...)`
- `turbo_agent_session_memory_delete(...)`
- `turbo_agent_session_memory_list(...)`
- `turbo_agent_session_load_memory_context(...)`
- `turbo_agent_session_create_input_state_with_memory_bind(...)`
- `turbo_agent_session_create_input_messages_state_with_memory_bind(...)`
- `turbo_agent_session_invoke_preset_text_with_memory(...)`
- `turbo_agent_session_invoke_preset_json_with_memory(...)`

That keeps one session, one runtime, and one optional memory store on one host
object instead of threading extra handles through application glue.

`memory_put_context(...)` stores one formal record with:

- `scope`
- `path`
- `text`

`load_memory_context(...)` then reads matching records back into
`state.memory_context.layers` and fails loudly on malformed entries.

For the common host path, the text-first wrappers:

- `turbo_agent_session_start_preset_text_with_memory(...)`
- `turbo_agent_session_invoke_preset_text_with_memory(...)`
- `turbo_agent_session_invoke_preset_json_with_memory(...)`

let one session combine user text plus long-term memory context without making
the host manually build or reserialize state objects.

## Long-term memory store

`turbo_agent_memory_store_t` is a separate surface from:

- `turbo_agent_store_t`
- `turbo_agent_runtime_store_t`

They solve different problems:

- `turbo_agent_store_t`: explicit load/save for `state.memory`
- `turbo_agent_runtime_store_t`: durable execution lineage
- `turbo_agent_memory_store_t`: namespaced long-term memory records

When attached through `turbo_agent_session_config_t.memory_store`, the session
becomes the owner of that long-term memory store as well. The new
`memory_list_records/query_records` helpers are canonical host-facing views
layered on top of the existing raw store. They normalize each record into:

- `id`
- `namespace`
- `kind`
- `key`
- `text`
- `metadata`
- `created_at`

Legacy records currently surface `created_at = null`; formal context records
derive `kind/text/metadata` from the existing `scope/path/text` payload shape.
`Runtime V3` now also exposes a record-first helper group on top of that same
canonical shape:

- `turbo_agent_memory_get_record(...)`
- `turbo_agent_memory_put_record(...)`
- `turbo_agent_memory_validate_record(...)`
- `turbo_agent_memory_query_records_ex(...)`

The `*_query_records_ex(...)` entrypoint takes one
`turbo_agent_memory_query_options_t` with:

- `namespace_prefix`
- `kind`
- `key_prefix`
- `text_substring`
- `id_prefix`
- `metadata_scope`
- `metadata_path_prefix`
- `created_after`
- `created_before`
- `sort_by`
- `sort_order`
- `limit`

`sort_by` currently supports `id`, `namespace`, `kind`, and `key`;
`sort_order` supports `asc` or `desc`; `limit=0` means no truncation.
`created_after` applies one inclusive lower bound to canonical `created_at`;
`created_before` applies one inclusive upper bound. The current implementation
expects canonical `created_at` to remain in one stable ISO-8601 string shape
so time windows can be evaluated with lexicographic string comparison.

### Memory record shape

The stable record shape is:

- `namespace`
- `key`
- `value_json`

### Memory store API

- `turbo_agent_memory_store_memory_create(...)`
- `turbo_agent_memory_store_file_create(...)`
- `turbo_agent_memory_store_destroy(...)`
- `turbo_agent_memory_get(...)`
- `turbo_agent_memory_put(...)`
- `turbo_agent_memory_delete(...)`
- `turbo_agent_memory_list(...)`
- `turbo_agent_memory_get_record(...)`
- `turbo_agent_memory_put_record(...)`
- `turbo_agent_memory_validate_record(...)`
- `turbo_agent_memory_list_records(...)`
- `turbo_agent_memory_query_records(...)`
- `turbo_agent_memory_query_records_ex(...)`
- optional native `query(namespace_prefix, kind, key_prefix, text_substring)`
  callback on `turbo_agent_memory_store_t`

When the native `query` callback is absent, `turbo_agent_memory_list_records(...)`
and `turbo_agent_memory_query_records(...)` still fall back to the existing raw
`list(...)` callback plus canonicalization/filtering. The native `query`
callback itself returns the canonical record array shape, not the raw
`namespace/key/value_json` entries used by `list(...)`. The backend callback
contract stays on those four filter parameters; `sort_by`, `sort_order`, and
`limit` are applied in the host-facing layer as canonical record
post-processing after the canonical array is assembled.

### Reference backends

Memory backend:

- heap-backed
- good for tests and embedding

File backend:

- writes one record per file
- layout:
  - `root_dir/records/<hex(namespace)>__<hex(key)>.json`

The file backend stores raw JSON payload text and validates it again on read.
Corrupted files fail loudly.

## Minimal usage

Session bootstrap:

```c
turbo_agent_session_config_t config = {0};
turbo_agent_session_t *session;

config.runtime_store = turbo_agent_runtime_store_file_create("./runtime-store");
config.env_path = ".env";
config.load_env = 1;

session = turbo_agent_session_create(&config);
```

Long-term memory:

```c
turbo_agent_memory_store_t memory = turbo_agent_memory_store_file_create("./memory-store");

turbo_agent_memory_put(&memory, "user/alice", "profile", "{\"name\":\"Alice\"}");
```

Canonical memory query:

```c
json_value_t *records = NULL;
turbo_agent_memory_query_options_t options = {0};

turbo_agent_memory_put(
    &memory, "project/demo", "context",
    "{\"scope\":\"project\",\"path\":\"/tmp/notes.md\",\"text\":\"remember this\"}");
options.namespace_prefix = "project";
options.kind = "context";
options.key_prefix = "con";
options.text_substring = "remember";
options.created_after = "2026-02-01T00:00:00Z";
options.created_before = "2026-02-28T23:59:59Z";
options.sort_by = "key";
options.sort_order = "desc";
options.limit = 1;
turbo_agent_memory_query_records_ex(&memory, &options, &records);
```

The host-facing query options now include `namespace_prefix`, `kind`,
`key_prefix`, `text_substring`, `id_prefix`, `metadata_scope`,
`metadata_path_prefix`, `created_after`, `created_before`, `sort_by`,
`sort_order`, and `limit`. Native backend callbacks remain on the stable
four-filter query contract; `id_prefix`/`metadata_scope`/
`metadata_path_prefix`/`created_after`/`created_before`/`sort_by`/
`sort_order`/`limit` are applied as canonical record post-processing in the
host-facing layer. The current time-window filters rely on canonical
`created_at` staying in one stable ISO-8601 string shape so lexicographic
comparison remains valid.

The stable canonical record shape is also locked by the Stage 4 golden fixtures:

- `memory_context_record.golden.json`
- `memory_json_record.golden.json`
- `memory_query_results.golden.json`

One minimal host example now lives in:

- `examples/runtime_memory_record_helpers.c`

The runtime-only cross-thread observability query contract is also locked by
`observability_queries.golden.json`.

## What Runtime V2 still does not do

It still does not add:

- high-level `invoke(...)` / `stream(...)` presets
- long-term memory search and indexing
- full subgraph interrupt bubbling and shared-state mapping modes
- server, HTTP, MCP, or A2A surfaces

The new local JSON-RPC dispatcher does not change that boundary. It is only a
transport-neutral bridge over existing runtime APIs, not a hosted service.

Those remain the next layer.
