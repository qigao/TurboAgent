# `turbo_langchain` 与 LangChain / LangGraph 差距分析

本文以仓内现状为基线，对照官方 LangGraph / LangChain 文档，说明：

- 我们已经具备什么
- 还缺哪些产品级能力
- 每一项缺口最小应从哪里切入

本结论基于 2026-04-16 查阅的官方文档：

- LangGraph persistence: <https://docs.langchain.com/oss/python/langgraph/persistence>
- LangGraph memory: <https://docs.langchain.com/oss/python/langgraph/memory>
- LangGraph streaming: <https://docs.langchain.com/oss/python/langgraph/streaming>
- LangGraph interrupts / human-in-the-loop: <https://docs.langchain.com/oss/python/langgraph/interrupts>
- LangGraph subgraphs: <https://docs.langchain.com/oss/python/langgraph/use-subgraphs>
- LangChain multi-agent / subagents: <https://docs.langchain.com/oss/python/langchain/multi-agent/subagents>
- LangChain runtime / middleware / agent surface: <https://docs.langchain.com/oss/python/langchain/agents>

## 已具备的基础能力

若只看“可运行 agent graph + durable resume/fork”，本仓已不再是玩具实现。

### 1. Durable runtime 已成型

现有 `Runtime V1/V2` 已具备：

- `thread / run / checkpoint` 三类正式 record
- `start / resume / fork / history` 持久化执行语义
- stable `topology_id`，不再依赖函数地址或进程局部布局
- file / memory 两种 runtime store
- `session / app` 两层 host wrapper
- `.env` 驱动的 provider bootstrap

这部分已能对齐 LangGraph “durable execution + checkpoint persistence”的最小骨架。

### 2. State / snapshot / interrupt 语义已成线

现有实现已具备：

- `workflow_snapshot`
- `control_snapshot`
- review / replan / failure / guardrail 状态面
- 基于 `TURBO_GRAPH_EXEC_INTERRUPTED` 的中断与恢复
- `state_override` 驱动的 resume/fork

这意味着“人审中断后继续跑”已经是正式能力，而不是临时补丁。

### 3. Tool / event / trace contract 已基本收敛

现有实现已具备：

- canonical tool-result envelope
- tool failure 不再靠手拼 JSON
- canonical bind-native event contract
- SSE aggregation helpers
- child-run / parent-run lineage 贯穿 `runtime -> session -> app -> subagent -> tool_result`

这部分已经比很多只会“调一次模型”的轻量库更接近可产品化 runtime。

### 4. 预置 workflow 与 subagent bridge 已有雏形

现有实现已具备：

- loop / review / engineering 预置 graph
- subagent-as-tool adapter
- shared app / stateless subagent 两种模式
- parent/child run lineage 持久化

这已经开始触到 LangGraph / LangChain 的 multi-agent 使用面。

## 与 LangGraph 仍有的主要差距

以下缺口不是“能不能跑”，而是“离 LangGraph 级产品面还差什么”。

### 1. 缺正式的 thread state API，而不只是 checkpoint resume

LangGraph 强调的不只是 checkpoint 存档，还包括：

- 查看 thread 当前 state
- 查看历史 state
- 在 host 侧更新 state 后继续运行
- time-travel 到某个历史点继续分叉

我们现在的核心恢复载体仍是：

- 读取 checkpoint record
- 手动构造 `state_override`
- 再调 `resume_bind_graph(...)` 或 `fork_bind_graph(...)`

这在 runtime 内核层面成立，但 host ergonomics 仍偏底层。此处已经补了第一步正式 API：

- `get_thread_state_bind(...)`
- `get_run_state_bind(...)`
- `get_checkpoint_state_bind(...)`
- `apply_command_bind(...)`
- `resume_checkpoint_bind_graph(...)` / `fork_checkpoint_bind_graph(...)`
- `resume_thread_bind_graph(...)` / `fork_thread_bind_graph(...)`

checkpoint-scoped replay alias 是显式 time-travel 主面；thread-scoped replay
convenience path 只是把“thread state edit -> replay”收成一条更好用的
host-facing 调用链，主事实源仍然是 thread state 与 checkpoint record 本身。

接下来最小且最直接的 host-facing 抬升，是把 thread timeline 变成一等视图：

- `turbo_agent_runtime_get_thread_timeline_bind(...)`
- `turbo_agent_session_get_thread_timeline_bind(...)`
- `turbo_agent_app_get_thread_timeline_bind(...)`

timeline 至少包含：

- `thread`
- `resolved_current_run`
- `latest_run`
- `pending_run`
- `runs`
- `current_run_checkpoints`
- `history_events`

其中 `resolved_current_run` 的解析规则固定为：pending run 优先，否则 latest run。

所以当前差距已从“完全没有 host-facing surface”收窄为“state update / time-travel / command 种类仍不完整”。

最小切口：

- 新增 `turbo_agent_runtime_get_state_bind(...)`
- 新增 `turbo_agent_runtime_update_state_bind(...)`
- 新增 `turbo_agent_runtime_replay_checkpoint_bind(...)`
- 让 `session/app` 暴露同构 wrapper，而不是迫使 host 手拼 state mutation 流程

### 2. 缺真正的 time-travel / branch inspection 产品面

LangGraph 的持久化不只为了 resume，还强调：

- 历史检查
- 分支回溯
- 从旧点再开新分支
- 以 thread 为中心浏览 lineage

我们已经有：

- `fork_bind_graph(...)`
- `list_runs(...)`
- `list_checkpoints(...)`
- `load_history_events_bind(...)`
- `turbo_agent_runtime_list_thread_lineage(...)` / `turbo_agent_session_list_thread_lineage(...)` /
  `turbo_agent_app_list_thread_lineage(...)`

但仍缺：

- “某 thread 下有哪些 branch” 的一等查询
- checkpoint 之间的 branch tree 视图
- “当前 run 相对于父 run / 父 checkpoint 的 lineage explain”
- history diff，而不只是 event 拼接

最小切口：

- 新增 `turbo_agent_runtime_get_branch_tree(...)` /
  `turbo_agent_session_get_branch_tree(...)` /
  `turbo_agent_app_get_branch_tree(...)`
- 让返回值固定包含 `thread_id`、`current_run_id`、`latest_run_id`、
  `pending_run_id`、`current_checkpoint_id`、`current_branch`、`branches`、`edges`
- 其中每个 `branches[]` 项至少包含 `run_id`、`checkpoint_id`、
  `parent_checkpoint_id`、`status`、`updated_at`、`checkpoint_summary`、
  `source_checkpoint_summary`
- `edges[]` 至少描述 checkpoint/run 之间的 lineage 连接，并携带
  `source_checkpoint_summary`；fork edge 的方向应当是从 source checkpoint
  指向 child run
- 这个 surface 只是只读 lineage 浏览面，不改变
  `resume_bind_graph(...)` / `fork_bind_graph(...)` 的语义
- `branch_root_checkpoint_id` 已可作为 branch node 的稳定入口 checkpoint
  锚点；后续仍可再补更多 checkpoint summary，进一步减少 host 侧二次推导
- 这些 summary 字段是轻量 inspect 视图，不替代 `get_checkpoint(...)`
- 新增 one-shot lineage summary JSON，给 host/UI 直接消费

补充：对于单个历史 checkpoint 的详情面，我们也正在补
`turbo_agent_runtime_get_checkpoint_context(...)` /
`turbo_agent_session_get_checkpoint_context(...)` /
`turbo_agent_app_get_checkpoint_context(...)`。它返回一个只读聚合 bundle，
把 `checkpoint_summary`、`state`、`run`、`thread`、`history_events` 放在
同一个 JSON 对象里，适合作为 host/UI 的 checkpoint detail panel，但不替代
`get_checkpoint(...)` 或 branch tree。

同一条线下，也适合把 `apply_checkpoint_command_bind(...)`、
`resume_checkpoint_command_bind(...)`、`fork_checkpoint_command_bind(...)`
作为显式 checkpoint-scoped command 主面保留下来；thread-scoped command
入口继续只是便利层。

同时，`available_command_descriptors` 还应继续强化 host contract，而不是
新增执行入口。像 `placeholder`、`example_payload`、`success_state_hint`
这类字段，能直接降低 UI/CLI 侧对 review note、feedback、final output
表单的二次猜测成本。

README 和 tests 现在已经把 checkpoint context、thread timeline、branch tree
以及 command descriptors 收成了最小 golden 形状；后续再补能力时，优先
围绕这些稳定字段扩展，而不是重新定义一套并行契约。

### 3. 缺更正式的 subgraph / nested graph durable contract

LangGraph 把 subgraph 当一等构件，而不是仅把另一个 agent 包成 tool。

我们现在对 nested execution 的主路径是：

- subagent tool adapter
- parent/child run linkage
- nested call-frame metadata: `parent_graph_run_id` / `call_frame_id`

这足以表达“agent 调另一个 agent”，也给 graph-native subgraph 留下了可持久化
的父图运行与调用帧锚点。当前最小 subgraph contract 已能把子图运行、子图
checkpoint、父图调用帧和 `subgraph_result` envelope 串到同一 runtime 事实源，
但还不足以表达：

- graph 内直接嵌 graph
- 父图与子图共享或映射 state channel
- 子图 checkpoint 与父图 checkpoint 的正式一致性关系
- subgraph interrupt 冒泡到父图时的标准语义

最小切口：

- 已先把 `parent_graph_run_id` / `call_frame_id` 贯穿 runtime parent link、
  run/checkpoint record、summary、subagent result 与 tool-result output accessor
- 已补 `turbo_agent_subgraph_node(...)` /
  `turbo_agent_install_subgraph_node(...)` 作为 graph-native 最小合同
- 子图运行可以不经 tool envelope，而走 graph-native call frame
- `subgraph_result` 已包含 child status booleans 与 interrupted child 的
  `pending_checkpoint_id` / `pending_node`，host 可通过现有 checkpoint API
  resume/fork 子图
- checkpoint record 继续以 `parent_checkpoint_id` + `parent_graph_run_id` /
  `call_frame_id` 表达嵌套关系，不新建第二套事实源

### 4. Memory 已进入 record-first / store-query 第一阶段，但离 LangGraph 的 memory/store 产品面仍有距离

LangGraph 官方 memory 面更强调：

- 跨线程长期记忆
- profile / semantic memory / store 检索
- memory 与 thread state 的分层
- 在 graph 内按 namespace / query 读写

我们当前已有：

- `turbo_agent_store_t`
- `turbo_agent_memory_store_t`
- namespaced context 读写
- `memory_list_records(...)` / `memory_query_records(...)`
- `turbo_agent_memory_get_record(...)`
- `turbo_agent_memory_put_record(...)`
- `turbo_agent_memory_validate_record(...)`
- `turbo_agent_memory_query_records_ex(...)`
- `session/app` 同构 memory wrapper
- canonical memory record：`id / namespace / kind / key / text / metadata / created_at`

这说明 memory 已不再只是随手拼出来的 KV/context 注入层；本地 host-facing
surface 已经进入 record-first 第一阶段。

但距离 LangGraph 的 memory/store 产品面，仍主要差在：

- 仍缺语义检索
- 仍缺 memory index / ranking / recall contract
- 仍缺 profile / semantic memory 这类更高层 recall contract
- remote 侧虽已补显式 `memory.*` record-first contract，并且现在已有
  `memory.getRecord` / `memory.putRecord` / `memory.listRecords` /
  `memory.deleteRecord` / `memory.queryRecordsEx` 这一组最小 transport
  contract；其中 `listRecords` 返回 canonical records，`deleteRecord`
  成功返回 `{ "deleted": true }`，且 remote 不暴露 legacy raw
  `memory.list`。但它还没有更强的 remote query/index/retrieval 产品面；
  目前 remote `memory.queryRecordsEx` 也只对齐到了本地 filter +
  metadata/id filter + `created_at` time-window + `sort_by / sort_order /
  limit` 这一层 ergonomics
- `context` record 已 canonical，但更丰富的 typed record family 仍未展开

因此下一步最小切口不再是“先把 record schema 立起来”，而是：

- 在现有 canonical memory record 之上继续扩 `kind` 与 `metadata` 约束
- 把 memory store callback 继续稳定在 `put/get/list/query`
- `query(...)` 当前已做到 filter + substring / prefix + metadata/id filter +
  `created_at` time-window + host-facing sort/limit，其中时间窗仍依赖
  canonical `created_at` 的 ISO-8601 字符串按字典序比较；embedding 检索
  留到下一轮
- 让 host/UI 能区分“runtime durable state inspect”和“long-term memory query”

这里还要刻意保留一条边界：当前 remote runtime bridge 没有把
`memory_store` 折叠进 `runtime.*` JSON-RPC 方法集，而是单独补了
`memory.getRecord` / `memory.putRecord` / `memory.listRecords` /
`memory.deleteRecord` / `memory.queryRecordsEx` 这一组 memory-facing
contract。也就是说，remote memory 已经有了最小 record-first 主面，但它
仍不是“runtime state 附属字段”那种混合设计。

### 5. 流式运行面还偏协议级，未到 runtime-host 级

LangGraph 的 streaming 面不只是 SSE parser，还包括：

- stream mode 区分 state / updates / messages / debug
- 运行中断点对 host 的正式反馈
- host 可边收边决策

我们已有：

- `turbo_agent_sse.h`
- provider SSE 聚合
- trace 事件

但这些更偏“把模型协议收回来”，而不是“把 runtime 执行过程以 host 友好方式推出去”。

最小切口：

- 为 runtime/session 新增统一 stream observer 接口
- 事件种类至少分为：
  - model delta
  - tool call started
  - tool result
  - state updated
  - interrupted
  - completed
- 让 host 无需自己拼 protocol event -> runtime event 的映射

### 6. 缺更完整的 command surface，human-in-the-loop 仍未完全产品化

LangGraph 的 interrupt / human-in-the-loop 文档强调：

- 审批
- 修改
- 拒绝
- 继续

这些动作最好是 host-facing command，而不是“自己改 state 再 resume”。

我们当前已补了一层最小 command surface：

- `approve_review`
- `reject_review`
- `request_replan`
- `append_feedback`
- `append_user_message`
- `override_final_output`

但离真正产品面仍有距离。当前做法的优点是简洁统一，但缺点也明显：

- host 需要知道该改哪些 state key
- 缺少受约束的 command schema
- review / replan / fixup 等人工动作未收敛成产品接口

最小切口：

- 新增 canonical runtime command schema，例如：
  - `approve_review`
  - `request_replan`
  - `append_feedback`
  - `override_final_output`
- 内部仍翻译成 `state_override`
- 对 host 暴露稳定 command contract，而不暴露内部 state 细节

### 7. Multi-agent orchestration 仍是“工具桥接”，还不是正式框架层

LangChain / LangGraph 在 multi-agent 侧已把这些模式讲清楚：

- supervisor
- handoff
- subagent
- tool-style delegation

我们现在仅正式做了：

- subagent as tool
- parent/child lineage 持久化
- `supervisor_versions` 主状态 lane
- supervisor loop installer
- session/app mailbox surface：
  - `get_supervisor_inbox(...)`
  - `get_supervisor_handoff_history(...)`
  - `get_supervisor_inspect(...)`
  - `append_supervisor_inbox_message_bind(...)`

还没正式定型：

- 多 agent 调度策略和冲突边界
- 更完整的 handoff / mailbox inspect bundle
- agent-to-agent shared state 的更高层 orchestration surface

最小切口：

- 新增 `turbo_agent_workflow_install_supervisor_loop(...)`
- 新增 `handoff` canonical event / output contract
- 定义多 agent shared state 最小 schema：
  - active_agent
  - inbox
  - handoff_reason
  - handoff_history

### 8. Observability 仍偏调试，不够产品化

LangGraph 生态和 LangSmith 思路的强点之一，是 run inspection / traces / state inspect / event debugging 一体化。

我们已有：

- trace
- checkpoint events
- history replay
- snapshots
- thread-scoped observability index bundle
- quick-read observability fields (`current_status`, `current_checkpoint_summary`,
  `has_pending_review`, `has_handoff`, `active_agent`)
- index-friendly interrupt/run summary fields (`current_interrupt_reason`,
  `current_pending_action`, `latest_run_status`, `latest_run_updated_at`,
  `pending_run_id`, `pending_checkpoint_id`)
- control-state summary fields (`has_failure`, `has_model_error`,
  `has_guardrail_rejection`, `replan_requested`, `current_failure_reason`,
  `current_review_note`)
- runtime-only cross-thread observability summary list
- runtime-only cross-thread observability summary filters

但仍缺：

- 结构化 run summary 查询
- 按时间、状态、provider、tool、error 类型过滤
- 更强的跨 thread / run inspection query/filter
- 给 UI 或服务层直接消费的标准 trace record

最小切口：

- 在 runtime store 之上加一个可选 trace index collection
- 固定一份 trace record schema
- 先不引 LangSmith 类服务端，只把 record/query contract 立住

### 9. 缺 server / remote runtime / MCP / A2A 面

LangGraph 产品面并不止本地库，还覆盖：

- server-hosted runs
- remote graph execution
- thread/run HTTP API
- 与外部 agent 协议的结合

本仓当前仍是本地嵌入式库优先，尚未进入：

- HTTP runtime service
- MCP-facing run host
- A2A session bridge
- remote inspector / dashboard

本轮已补的最小切口只有一层本地 JSON-RPC 2.0 dispatcher skeleton：

- `turbo_agent_runtime_remote_dispatch_jsonrpc(...)`
- `turbo_agent_runtime_remote_dispatch_jsonrpc_text(...)`
- `turbo_agent_runtime_remote_handle_http_jsonrpc(...)`
- `turbo_agent_runtime_remote_client_call_json(...)`
- `turbo_agent_runtime_remote_client_start_bind_graph(...)`
- `turbo_agent_runtime_remote_client_resume_bind_graph(...)`
- `turbo_agent_runtime_remote_client_fork_bind_graph(...)`
- `turbo_agent_runtime_remote_client_get_thread_state_bind(...)`
- `turbo_agent_runtime_remote_client_get_checkpoint_context(...)`
- `turbo_agent_runtime_remote_client_get_run(...)`
- `turbo_agent_runtime_remote_client_get_checkpoint(...)`
- `turbo_agent_runtime_remote_client_list_checkpoints(...)`
- `turbo_agent_runtime_remote_client_load_history_events_bind(...)`
- `turbo_agent_runtime_remote_client_get_run_trace_events_bind(...)`
- `turbo_agent_runtime_remote_client_get_checkpoint_trace_events_bind(...)`
- `turbo_agent_runtime_remote_client_get_thread_timeline_bind(...)`
- `turbo_agent_runtime_remote_client_get_branch_tree(...)`
- `turbo_agent_runtime_remote_client_get_thread_observability_index(...)`
- `turbo_agent_runtime_remote_client_list_observability_indexes(...)`
- `turbo_agent_runtime_remote_client_list_observability_indexes_filtered(...)`
- `turbo_agent_runtime_remote_client_list_child_runs(...)`
- `turbo_agent_runtime_remote_client_get_supervisor_inspect(...)`
- `turbo_agent_runtime_remote_client_get_orchestration_inspect(...)`
- `turbo_agent_runtime_remote_client_get_child_inspect(...)`
- `turbo_agent_runtime_remote_client_get_child_orchestration_inspect(...)`
- `turbo_agent_runtime_remote_client_get_child_multi_agent_inspect(...)`
- `turbo_agent_runtime_remote_client_get_memory_record(...)`
- `turbo_agent_runtime_remote_client_put_memory_record(...)`
- `turbo_agent_runtime_remote_client_query_memory_records_ex(...)`
- `turbo_agent_runtime_remote_client_query_memory_records(...)`
- `turbo_agent_runtime_remote_client_list_memory_records(...)`
- `turbo_agent_runtime_remote_client_resume_thread_command_bind(...)`
- `turbo_agent_runtime_remote_client_fork_thread_command_bind(...)`
- `turbo_agent_remote_session_*`
  - 当前已覆盖 `start/resume/fork`
  - 也已补 `get_thread(...)` / `get_latest_run(...)` / `get_pending_run(...)`
  - 并补 `start_text(...)` / `start_messages(...)` / `invoke_text(...)` /
    `invoke_messages_text(...)` / `invoke_json(...)` /
    `invoke_messages_json(...)`
  - 也已补 `load_thread_history_events_bind(...)` /
    `replay_thread_history_bind(...)` /
    `observe_thread_history_bind(...)` /
    `get_thread_trace_events_bind(...)`
  - 也已补 `list_thread_lineage(...)` /
    `get_supervisor_inbox(...)` /
    `get_supervisor_handoff_history(...)` /
    `get_supervisor_inspect(...)`
  - 也已补 `list_child_runs(...)` /
    `get_orchestration_inspect(...)`
  - 也已补 `memory_list_records(...)` / `memory_get_record(...)` /
    `memory_put_record(...)` / `memory_validate_record(...)` /
    `memory_query_records(...)` / `memory_query_records_ex(...)`
  - 也已补 `get_child_run(...)` / `get_child_checkpoint(...)` /
    `get_child_checkpoint_context(...)` /
    `get_child_thread_timeline_bind(...)` / `get_child_branch_tree(...)` /
    `list_child_checkpoints(...)` / `load_child_history_events_bind(...)` /
    `get_child_trace_events_bind(...)` / `get_child_inspect(...)` /
    `get_child_orchestration_inspect(...)` /
    `get_child_multi_agent_inspect(...)`
  - 继续复用同一份 remote observability bundle，而不是新增 RPC method
- `turbo_agent_remote_app_*`
  - 当前已覆盖 `start/resume/fork`
  - 也已补 `get_thread(...)` / `get_latest_run(...)` / `get_pending_run(...)`
  - 并补 `start_text(...)` / `start_messages(...)` / `invoke_text(...)` /
    `invoke_messages_text(...)` / `invoke_json(...)` /
    `invoke_messages_json(...)`
  - 也已补 `load_thread_history_events_bind(...)` /
    `replay_thread_history_bind(...)` /
    `observe_thread_history_bind(...)` /
    `get_thread_trace_events_bind(...)`
  - 也已补 `list_thread_lineage(...)` /
    `get_supervisor_inbox(...)` /
    `get_supervisor_handoff_history(...)` /
    `get_supervisor_inspect(...)`
  - 也已补 `list_child_runs(...)` /
    `get_orchestration_inspect(...)`
  - 也已补 `memory_list_records(...)` / `memory_get_record(...)` /
    `memory_put_record(...)` / `memory_validate_record(...)` /
    `memory_query_records(...)` / `memory_query_records_ex(...)`
  - 也已补 `get_child_run(...)` / `get_child_checkpoint(...)` /
    `get_child_checkpoint_context(...)` /
    `get_child_thread_timeline_bind(...)` / `get_child_branch_tree(...)` /
    `list_child_checkpoints(...)` / `load_child_history_events_bind(...)` /
    `get_child_trace_events_bind(...)` / `get_child_inspect(...)` /
    `get_child_orchestration_inspect(...)` /
    `get_child_multi_agent_inspect(...)`
  - 仅作为 `remote_session` 之上的 graph-name facade
- `turbo_agent_runtime_remote_iris_mount(...)`
- 当前最小方法集是：
  - `runtime.start`
  - `runtime.resume`
  - `runtime.fork`
  - `runtime.applyCommand`
  - `runtime.resumeThreadCommandBindGraph`
  - `runtime.forkThreadCommandBindGraph`
  - `runtime.getThreadState`
  - `runtime.updateThreadState`
  - `runtime.applyThreadStatePatch`
  - `runtime.resumeThreadBindGraph`
  - `runtime.forkThreadBindGraph`
  - `runtime.getCheckpointContext`
  - `runtime.resumeThreadStatePatchBindGraph`
  - `runtime.forkThreadStatePatchBindGraph`
  - `runtime.getThreadObservabilityIndex`
  - `runtime.listObservabilityIndexesFiltered`
  - `memory.getRecord`
  - `memory.putRecord`
  - `memory.listRecords`
  - `memory.deleteRecord`
  - `memory.queryRecordsEx`

它只是 future transport adapter 的 contract bridge，不是 HTTP server，也
不是第二套持久化 runtime store。现在 server 侧与 consumer 侧都继续复用
同一套 thread/run/checkpoint/state JSON，而不是各自长出第二套结果模型。

这也意味着 remote runtime 目前仍然没有把 `memory_store` 折进
`runtime.*` RPC 方法集。memory 继续保持为独立的 long-term store surface，
而 remote bridge 通过显式 `memory.*` contract 暴露 record-first helper；
后续若继续扩 remote memory，也应沿这条 memory-facing 路径前进，而不是把它
伪装成 runtime state 的附属字段。

`remote_session` / `remote_app` 也只是 typed remote client 之上的
host-facing convenience facade，负责缓存 `thread_id / run_id / checkpoint_id`
与固定 `graph_name`，并不伪装成本地 `turbo_agent_session_t` /
`turbo_agent_app_t` 的完整替身。

同时，Iris bridge 现在不再依赖单个全局 `rpc_context` 或 app-level 单槽。
同一 `iris_app_t` 上的 endpoint 绑定已按 path 隔离，所以 native
`rpc_setup_endpoint(...)` 与 runtime remote bridge 可以在同一个 app 上共存，
只要路径不冲突。

这不算内核缺陷，但若目标是“产品 ready lib + host surface”，这块迟早要补。

最小切口：

- 先定义 runtime JSON RPC / HTTP 草案
- 只暴露：
  - create/start
  - resume/fork
  - get thread/run/checkpoint
  - list runs/checkpoints
  - apply command
- 先让本地 runtime 通过一层 transport adapter 暴露出来

### 10. 与 LangChain 的 runnable / middleware 组合面仍有距离

LangChain 除 agent / graph 外，还有较强的：

- runnable 组合
- middleware
- 模型选择与动态包裹
- 统一 invoke / stream / batch 风格

我们虽然已有：

- provider-neutral agent core
- extensions / middleware hooks
- prompt / tool / workflow 分层

当前已经补出一层最小“可组合 runnable surface”，把：

- sync invoke
- stream
- batch

归一到 bind-native runnable vtable 上。现在也已有最小 before/after
hook wrapper，但仍未完整覆盖的是更丰富的 middleware 链、远程 runnable、
并行 batch 调度、configurable runtime 选项等更高层组合能力。

当前已补到的最小桥接面：

- session/app 默认 workflow 已有 `start_text_stream(...)` /
  `start_messages_stream(...)`
- session/app 默认 workflow 已有串行 `batch_text(...)`
- `turbo_runnable_batch_bind(...)` 已提供通用数组批处理 fallback
- `turbo_runnable_from_agent_session(...)` /
  `turbo_runnable_from_agent_app(...)` 可把默认 workflow 包成 runnable
- `turbo_runnable_wrap_bind(...)` 可用 before/after hook 包裹 bind-native
  runnable，并覆盖 invoke / stream / batch 三条入口
- `turbo_langchain_runnable_batch(...)` /
  `turbo_langchain_runnable_wrap(...)` /
  `turbo_langchain_runnable_from_session(...)` /
  `turbo_langchain_runnable_from_agent(...)` 已同步暴露在 facade

这能覆盖 host 最常用的 invoke / stream / batch 入口，并能与
`turbo_runnable_pipe(...)` 组合。剩余缺口主要在 middleware 链式组合与
config passthrough、remote runnable transport，以及更丰富的 batch/async 调度策略。

最小切口：

- 为 runnable 增加 config passthrough 和更完整的 middleware 链式组合
- 按需增加 batch 并发/限流策略，而不是把并发语义硬编码进默认 fallback
- 让 prompt/model/tool/workflow 都能继续挂到同一组合面，而不把高层 host 绑死在某一类 graph preset

## 缺口优先级

若只按“最快补成像 LangGraph 的产品面”排序，建议优先做这四件：

### P0: Thread state / command / time-travel host API

优先理由：

- 现内核已有 checkpoint 与 `state_override`
- 只差 host-facing contract
- 一旦补齐，就能把 review / approve / replan / manual edit 变成正式产品面

### P1: Streaming runtime observer

优先理由：

- 现有 SSE 聚合已够用
- 缺的是 runtime 事件分发层
- 补上后才能真正形成 `invoke / stream` 成对的 host surface

当前已补到的最小桥接面：

- `observe_history_bind(...)` / `observe_thread_history_bind(...)`
- `add_observer_bind_sink(...)`
- `turbo_event_stream_mode_accepts_bind(...)` /
  `turbo_event_stream_filter_sink_bind(...)` 这一层 core stream-mode filter

但这仍只是对现有 durable history 与 live trace 的 host-facing bridge，
不是第二套持久化 observer log，也还不是完整的 runtime stream narrative。
后续 TurboHTTP SSE / WebSocket / JSON-RPC notification 应该作为这层 core
stream contract 的 transport adapter，而不是把 runtime 直接绑到 HTTP。

### P2: Multi-agent orchestration contract

优先理由：

- 已有 subagent tool bridge 与 parent/child lineage
- 最适合顺着现有实现继续抬升
- 能直接缩小与 LangGraph multi-agent 文档的差距

### P3: Memory query/store 升格

优先理由：

- canonical record 与 query helper 已落地
- 但距离“能检索、能召回、能解释”还远
- 这是后续长期记忆、profile、跨线程回忆的底座

## 建议的 Runtime V3 范围

若下一轮取 `Runtime V3`，建议只收以下内容，不再横向扩散：

1. `thread state` 正式 API
2. `runtime command` 正式 API
3. `runtime stream observer` 正式 API
4. `lineage / branch inspect` 正式 API

此轮先不做：

- 继续扩向量检索产品面；基础 SQLite/FTS5 knowledge store、vector store、
  generic retriever adapter、session/app retriever preset 已经落地
- SQLite backend
- server / MCP / A2A
- 完整 graph-native subgraph resume/fork/interrupt 冒泡语义
- 完整 runnable framework

原因很简单：上述四项几乎都可复用现有 runtime/checkpoint/snapshot/trace 基础，不需要推倒重来。

## 一句话结论

`turbo_langchain` 现在已经补齐了“能 durable 地跑 agent graph”这一层，离 LangGraph 真正还差的，不再是 checkpoint 本身，而是：

- thread-state 与 command 产品面
- runtime-host streaming
- 正式 multi-agent orchestration contract
- 更强的 memory / inspect / remote surface
- 以及更清晰的 runtime remote 与 memory remote 分层

下一轮不应再大拆文件，而应围绕这些产品边界做小步抬升。
