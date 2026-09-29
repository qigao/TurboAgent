# Runtime V1

`Runtime V1` lifts one graph execution into durable runtime records without
changing the existing graph or agent semantics.

It adds three first-class records:

- `thread`
- `run`
- `checkpoint`

It keeps the old rules intact:

- `TURBO_GRAPH_EXEC_INTERRUPTED` is still the interrupt primitive
- graph checkpoints keep their current schema
- state changes still happen through `turbo_agent_state_*`

What changes is persistence:

- one run can be inspected after it stops
- one checkpoint can be resumed later
- one old checkpoint can be forked into a new run
- one run's segment events can be replayed as durable history

## Public surface

Include:

```c
#include "turbo_agent_runtime.h"
```

Core types:

- `turbo_agent_runtime_t`
- `turbo_agent_runtime_store_t`

Core entry points:

- `turbo_agent_runtime_create(...)`
- `turbo_agent_runtime_destroy(...)`
- `turbo_agent_runtime_store_memory_create(...)`
- `turbo_agent_runtime_store_file_create(...)`
- `turbo_agent_runtime_start_json_value_graph(...)`
- `turbo_agent_runtime_resume_json_value_graph(...)`
- `turbo_agent_runtime_fork_json_value_graph(...)`
- `turbo_agent_runtime_get_thread(...)`
- `turbo_agent_runtime_get_run(...)`
- `turbo_agent_runtime_get_checkpoint(...)`
- `turbo_agent_runtime_get_thread_state_json_value(...)`
- `turbo_agent_runtime_get_run_state_json_value(...)`
- `turbo_agent_runtime_get_checkpoint_state_json_value(...)`
- `turbo_agent_runtime_list_runs(...)`
- `turbo_agent_runtime_list_checkpoints(...)`
- `turbo_agent_runtime_load_history_events_json_value(...)`
- `turbo_agent_runtime_apply_command_json_value(...)`

## Data model

`thread` record fields:

- `id`
- `created_at`
- `updated_at`

`run` record fields:

- `id`
- `thread_id`
- `parent_run_id`
- `forked_from_checkpoint_id`
- `graph_name`
- `topology_id`
- `status`
- `created_at`
- `updated_at`
- `latest_checkpoint_id`
- `state_snapshot`
- `result`

`checkpoint` record fields:

- `id`
- `thread_id`
- `run_id`
- `parent_checkpoint_id`
- `seq`
- `status`
- `created_at`
- `graph_name`
- `topology_id`
- `next_node`
- `steps`
- `checkpoint_json`
- `control_snapshot`
- `workflow_snapshot`
- `events`

The runtime does not invent a second checkpoint schema. It stores the existing
checkpoint JSON inside `checkpoint_json`.

## Stores

`Runtime V1` ships with two reference stores.

Memory store:

- heap-backed
- good for tests and embedding
- lost when the process exits

File store:

- writes JSON files under one root
- layout:
  - `threads/<id>.json`
  - `runs/<id>.json`
  - `checkpoints/<id>.json`
- survives process restart

This store is deliberately separate from `turbo_agent_set_store(...)`.

- `turbo_agent_store_t` owns agent memory slots
- `turbo_agent_runtime_store_t` owns runtime lineage

Do not mix them. They solve different problems.

## Start, resume, fork

### Start

`start_json_value_graph(...)`:

- creates or reuses one `thread`
- creates one new `run`
- executes one graph segment
- returns:
  - summary JSON
  - resulting TurboParser JSON-native state

If the graph completes, the run is persisted as `completed`.

If the graph interrupts, the run is persisted as `interrupted` and one
checkpoint record is written.

### Resume

`resume_json_value_graph(...)`:

- loads one persisted checkpoint
- optionally replaces checkpoint state with `state_override`
- continues the same `run_id`

This is the general interrupt recovery path. No special command DSL is needed.

### Fork

`fork_json_value_graph(...)`:

- loads one old checkpoint
- starts one new `run_id`
- keeps:
  - `parent_run_id`
  - `forked_from_checkpoint_id`

Use this when you want to branch history instead of continuing the same run.

## History

`load_history_events_json_value(...)` walks the persisted checkpoint chain and
rebuilds segment events in chronological order.

That means:

- history is derived from checkpoint lineage
- no duplicate full-history blob is stored
- resume and fork stay consistent with persisted segments

You may load history by:

- `run_id`
- `checkpoint_id`

## State access

`Runtime V1` now also exposes host-facing state access without forcing the host
to parse checkpoint JSON manually.

- `get_checkpoint_state_json_value(...)` loads the serialized checkpoint state
- `get_run_state_json_value(...)` loads the latest persisted run state
- `get_thread_state_json_value(...)` resolves the latest run on the thread and loads
  its state

These entry points stay on the TurboParser JSON-native value boundary, so hosts do not
need to adopt the JSON DOM as their application state model.

## Command apply

`Runtime V1` still uses `state_override` as the resume/fork primitive, but the
runtime now also ships one minimal command helper:

- `apply_command_json_value(...)`

That helper reads one persisted checkpoint state, applies one canonical
host-facing command, and returns a fresh TurboParser JSON-native `state_override`.

The first supported commands are:

- `approve_review`
- `reject_review`
- `request_replan`
- `append_feedback`
- `append_user_message`
- `override_final_output`

## Minimal flow

1. Build a graph that can interrupt.
2. Create a runtime with a memory or file store.
3. Start one run with TurboParser JSON-native state.
4. If status is `interrupted`, load the checkpoint record.
5. Inspect `control_snapshot` and `workflow_snapshot`.
6. Build a fresh state override with `turbo_agent_state_*`, or ask the runtime
   to build one through `apply_command_json_value(...)`.
7. Resume the checkpoint, or fork it into a new run.

## Example targets

Two examples are wired into CMake.

`langchain_runtime_review_resume_example` shows:

- start
- interrupt before review
- inspect checkpoint
- mutate state
- resume
- fork

`langchain_runtime_history_inspect_example` shows:

- `get_run(...)`
- `get_checkpoint(...)`
- `list_runs(...)`
- `list_checkpoints(...)`
- `load_history_events_json_value(...)`

The examples are also registered as smoke tests, so their printed output is now
part of the runtime contract instead of an untested demo.

## What Runtime V1 does not do

It does not yet add:

- high-level `create_agent(...)`
- long-term memory search/index APIs
- subgraph persistence modes
- server, HTTP, MCP, or A2A surfaces

Those belong to the next layer. `Runtime V1` only establishes the durable
execution core.
