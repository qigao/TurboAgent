# Runtime V4 Agent Core 设计

## 0. 文档状态

- 状态：增量实施中。能力 1–4 的基础垂直切片已实现；能力 5 已交付只读
  `fs.read/fs.list`，能力 6 已交付 transport v2 transient retry、attempt 与 exact usage
  记录。coding 写入/进程工具、价格目录和完整 provider 分类仍未完成。
- 范围：解决以下六项核心运行能力，并保持 Runtime V1/V2/V3 现有同步入口可用。
  1. 统一执行句柄、取消、deadline、等待与状态查询。
  2. session steering/follow-up inbox。
  3. token/上下文预算、自动 compaction 与 overflow 恢复。
  4. 独立、有界、可取消的工具执行器。
  5. 默认 coding tool pack。
  6. provider retry、timeout、usage 与 cost 统计。
- 公开 API：`runtime_core` cancel token、graph/runtime controlled 入口、异步 execution
  handle、session durable inbox、context policy/compaction、tool definition v2/executor、
  CodingTools read-only pack，以及 retry policy/transport v2 已以版本化 ABI 落地。
- 数据格式：本文只新增版本化记录，不修改既有 thread/run/checkpoint 记录的既有字段语义。

## 1. 目标与边界

Runtime V4 的目标不是重写现有 graph/runtime，而是在其上增加一个完整、可组合的执行控制面：宿主可以启动一个长时间运行的 agent execution，观察它、注入消息、取消它，并获得可解释的资源、重试和费用统计。

以下内容不是 V4 的目标：

- 不把整个 agent runtime 放进 Wasm。
- 不让 Wasm guest 持有模型密钥、provider transport、session store 或策略事实源。
- 不默认并行执行所有工具。
- 不用摘要替换 durable history，也不静默丢弃历史。
- 不承诺强制中止任意 in-process C 回调；该能力需要 cooperative cancellation、Wasm trap 或独立进程隔离。
- 不在首期实现插件热重载。ABI 和生命周期为以后保留边界，但先完成确定性 unload/shutdown。

## 2. 当前仓库证据与问题定义

|级别|证据类型|当前事实|影响|
|---|---|---|---|
|HIGH|事实|`turbo_graph_run_internal(...)` 是同步 node loop，只检查 `max_steps` 和 `interrupt_before_nodes`；见 `langchain/src/turbo_graph.c:566`。|执行中没有统一 cancel/deadline 检查点，宿主不能可靠停止 model/tool/backoff 等等待。|
|HIGH|事实|模型 HTTP timeout 由 `TURBO_AGENT_MODEL_HTTP_TIMEOUT_MS` 固定为 60000 ms；见 `langchain/src/turbo_agent_transport.c:15`、`:233`。|不同 provider、交互请求和长任务不能分别配置 timeout；取消也无法贯穿 transport。|
|HIGH|事实|`turbo_agent_tool_node(...)` 按 tool call 数组逐项调用 `turbo_tool_registry_execute(...)`；见 `langchain/src/turbo_agent_workflow_tool_node.c:66`、`:170`。|发送给模型的 `parallel_tool_calls` 只是请求提示，不代表宿主真正并行。|
|HIGH|事实|`turbo_tool_runtime_vtable_t` 只提供同步 `invoke`/`invoke_json_value`，没有 execution context、progress、deadline 或 cancellation；见 `runtime_tools/include/turbo_tool_runtime.h:23`。|native/Wasm/tool bridge 无法采用同一取消和观测协议。|
|MED|事实|现有内部 `turbo_agent_execution_context_t` 只有 thread/run/tool lineage 字段；见 `langchain/src/turbo_agent_runtime_internal.h:11`。|lineage 已有基础，但不是执行生命周期或取消令牌。|
|MED|事实|`turbo_agent_runtime_store_t` 只有 `put/get/list`；见 `langchain/include/turbo_agent_runtime.h:26`。|持久 inbox、compaction 和工具结果日志不能假设数据库事务或 CAS。|
|MED|事实|`turbo_tool_runtime_wasm_config_t` 已把 TurboWasm policy 设为 capability 与 VM quota 的唯一事实源，并增加 tools/metadata/input/output 的宿主硬上限；见 `llm_sandbox/include/turbo_tool_runtime_wasm.h`。|工具沙箱已有细粒度能力与资源边界；每次 invoke 的共享 cancel/deadline 仍需在 runtime v2 调用上下文中贯通。|
|MED|事实|当前 agent 配置包含 structured-output retry 与 `parallel_tool_calls`，但没有通用 transport retry、context budget 或 usage/cost policy；见 `langchain/include/turbo_agent.h:34`。|长会话会在 provider overflow 时失败，provider 抖动和成本也不可统一观测。|

推论：上述缺口跨越 `langchain`、`runtime_tools`、`llm_sandbox` 和新的 coding tools，若把取消类型放在任一高层模块中都会造成反向依赖或复制协议，因此需要一个最小公共控制层。

## 3. 架构决策

### 3.1 候选方案

|方案|优点|问题|结论|
|---|---|---|---|
|A. 全部放进 `langchain`|改动入口集中。|`runtime_tools` 和 `llm_sandbox` 若要使用取消令牌就必须反向依赖 `langchain`，形成错误依赖方向。|不选。|
|B. 全部放进 `runtime_tools`|工具接入直接。|graph、model transport、session inbox 和 compaction 都不是工具职责；模块会成为上帝对象。|不选。|
|C. 新增最小 `runtime_core`|控制协议可被 graph、transport、tool、Wasm 共同依赖；状态归属清楚。|增加一个构建 target 和公开头文件组。|选择。|

### 3.2 目标依赖图

下图箭头表示“底层能力被上层依赖”：

```text
TurboUtils::Core                 TurboParser::Parser
       |                                |
       v                                v
TurboAgent::RuntimeCore          TurboAgent::RuntimeData
       |                                |
       +---------------+----------------+
                       v
              TurboAgent::RuntimeTools
                 /        |         \
                v         v          v
          LangChain   CodingTools   LLMSandbox
```

`LangChain` 还直接依赖 `RuntimeCore` 与 `RuntimeData`；图中省略重复边以保持可读性。

依赖约束：

- `runtime_core` 不依赖 JSON DOM、provider、graph 或 tools。
- `runtime_data` 仍以 TurboParser 为 JSON/XML/YAML 边界；V4 不恢复 DataBind/runtime_codec。
- `runtime_tools` 依赖 `runtime_core` 与 `runtime_data`，不依赖 `langchain`。
- `langchain` 组合执行对象、durable runtime、model、tool executor 与 context manager。
- `llm_sandbox` 实现 tool runtime v2，不承载 session 或 provider。
- `coding_tools` 是可选独立 target；默认工具集不污染 agent 核心。

### 3.3 唯一事实源

|状态|主事实源|派生状态|
|---|---|---|
|对话与运行历史|durable thread/run/checkpoint/event 记录|模型 context projection、UI timeline、summary。|
|execution 当前控制状态|进程内 `turbo_agent_execution_t`|observer event、wait/query 结果。终态同步写入 run 记录。|
|inbox 消息|durable inbox record|内存 ready queue。|
|compaction|durable committed compaction record|下一次模型请求的 context head。|
|工具副作用结果|tool result journal + 外部系统实际状态|聚合 tool output。|
|usage|provider response 中的原始 usage record|run/turn/model/cost 聚合。|

不变量：缓存和摘要只能从 durable history 重建；内存 inbox 不能独立推进业务状态；usage 缺失时必须是 unknown，不能按零处理。

## 4. 公共控制协议：`runtime_core`

已落地：

```text
runtime_core/
  include/turbo_runtime_control.h
  src/turbo_runtime_control.c
  tests/test_turbo_runtime_control.c
```

当前错误语义复用 TurboUtils 的 `TURBO_ECANCELED`、`TURBO_ETIMEDOUT`、
`TURBO_EBUSY` 等错误码；是否需要独立 runtime error domain 留到工具执行器和
provider retry 接入时统一决定，避免提前维护重复错误事实源。

候选核心类型：

```c
typedef struct turbo_cancel_source_s turbo_cancel_source_t;
typedef struct turbo_cancel_token_s turbo_cancel_token_t;

typedef enum turbo_cancel_reason_e {
  TURBO_CANCEL_NONE = 0,
  TURBO_CANCEL_USER = 1,
  TURBO_CANCEL_DEADLINE = 2,
  TURBO_CANCEL_SHUTDOWN = 3,
  TURBO_CANCEL_PARENT = 4
} turbo_cancel_reason_t;

typedef struct turbo_execution_control_s {
  uint32_t struct_size;
  uint32_t abi_version;
  turbo_cancel_token_t *cancel_token; /* retained by callee for async use */
  uint64_t deadline_mono_ms;          /* 0 means no deadline */
  const char *execution_id;           /* borrowed for the call */
  const char *thread_id;
  const char *run_id;
  const char *tool_call_id;
} turbo_execution_control_t;
```

协议：

- deadline 使用单调时钟的绝对时间，避免系统时间回拨。
- cancel source 只负责发出一次终止原因；token 只读，可 retain/release。
- 第一个成功写入的 cancel reason 胜出，后续请求不覆盖根因。
- `turbo_cancel_token_check(...)` 区分 cancelled 与 deadline exceeded。
- 所有 timed wait 必须能被 cancel 唤醒，不能靠轮询永久自旋。
- token 不携带 JSON、日志或策略，保证底层模块可复用。

错误域至少包含：

```text
TURBO_RUNTIME_ECANCELLED
TURBO_RUNTIME_EDEADLINE
TURBO_RUNTIME_EBACKPRESSURE
TURBO_RUNTIME_ECLOSED
TURBO_RUNTIME_ETIMEOUT
TURBO_RUNTIME_EOUTPUT_LIMIT
TURBO_RUNTIME_ECONTEXT_OVERFLOW
TURBO_RUNTIME_EUNKNOWN_SIDE_EFFECT
```

边界层把它们转换为既有 graph/tool/agent 状态；中间层不得记录日志后返回成功。

## 5. 能力 1：统一 execution、取消、deadline、wait 与状态查询

### 5.1 对象模型

新增 opaque `turbo_agent_execution_t`。它拥有：

- 一次 start/resume/fork 的 immutable 输入快照。
- cancel source 与 deadline。
- 当前状态、终态错误和最终 summary/state。
- worker task/thread 的生命周期。
- observer sink 和 usage accumulator。

它不拥有 durable history 的独立副本；thread/run/checkpoint 仍由 `turbo_agent_runtime_t` 管理。

状态机：

```text
QUEUED -> RUNNING ---------------------------------> COMPLETED
                    |   |   |                       FAILED
                    |   |   +-- interrupt_before -> INTERRUPTED
                    |   +------ deadline ---------> TIMED_OUT
                    +---------- cancel -----------> CANCELLED

QUEUED -- cancel/deadline --> CANCELLED/TIMED_OUT
```

`CANCEL_REQUESTED` 是可观测过渡标志，不是 durable 终态。终态只允许写入一次。

### 5.2 候选 API

为避免修改已公开结构体布局，能力 1 已新增以下 ABI v1 配置与入口：

```c
typedef struct turbo_agent_execution_options_s {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t deadline_mono_ms;
} turbo_agent_execution_options_t;

CXX_C_API int turbo_agent_execution_start(
    turbo_threadpool_t *executor,
    turbo_agent_runtime_t *runtime,
    turbo_graph_t *graph,
    const json_value_t *state,
    const turbo_graph_run_options_t *graph_options,
    const turbo_agent_runtime_exec_options_t *runtime_options,
    const turbo_agent_execution_options_t *execution_options,
    turbo_agent_execution_t **out_execution);

CXX_C_API int turbo_agent_execution_resume(/* same ownership pattern */);
CXX_C_API int turbo_agent_execution_fork(/* same ownership pattern */);
CXX_C_API turbo_agent_execution_t *turbo_agent_execution_retain(
    turbo_agent_execution_t *execution);
CXX_C_API void turbo_agent_execution_release(
    turbo_agent_execution_t *execution);
CXX_C_API int turbo_agent_execution_cancel(
    turbo_agent_execution_t *execution, turbo_cancel_reason_t reason);
CXX_C_API int turbo_agent_execution_wait(
    turbo_agent_execution_t *execution, uint64_t timeout_ms);
CXX_C_API int turbo_agent_execution_get_status(
    const turbo_agent_execution_t *execution,
    turbo_agent_execution_status_t *out_status);
CXX_C_API int turbo_agent_execution_take_result(
    turbo_agent_execution_t *execution,
    json_value_t **out_summary, json_value_t **out_state);
```

`executor`、`runtime`、`graph`、store 和回调 user data 为 borrowed，必须存活至
execution 终态；输入 DOM 与 option 中的字符串在提交返回前复制。调用者拥有线程池
容量与关闭策略，队列满时提交 fail fast 返回 `TURBO_EBUSY`，不发布半初始化句柄。
`take_result` 只允许成功一次，并把 summary/state 所有权移动给调用者。

当前实现还提供稳定 UUIDv7 execution id 与底层同步 runtime 返回码查询。事件 sink
沿用 `turbo_agent_runtime_exec_options_t`；`shutdown_grace_ms` 尚未进入 ABI，等待统一
session shutdown owner 落地后再加入。

### 5.3 取消检查点

必须在以下边界检查同一个 token：

1. execution 排队前与 worker 开始时。
2. graph 每个 node 之前和之后。
3. model 请求构造前、网络等待中、流式 chunk 之间和解析前。
4. retry backoff 等待中。
5. tool batch 校验前、排队等待中、每个工具调用前后。
6. native tool v2 回调内部由工具主动检查。
7. Wasm invoke 的预算/epoch 检查边界。
8. compaction 请求及提交前。
9. inbox condition wait 与 shutdown drain。

当前落地范围：graph 在每个 node 前和 route/checkpoint 后检查 token；durable runtime
把 cancelled/timed_out 作为可恢复终态写入 run summary/checkpoint；异步 execution
负责排队、等待、取消、deadline 和结果移交。model transport、retry backoff、tool、
Wasm、compaction 与 inbox 的 token 贯穿仍属于后续能力，不能把当前切片解释为强制
中止任意旧回调。

兼容限制：旧 `turbo_agent_transport_fn` 和旧 `turbo_tool_handler_fn` 不接受 token，只能在调用前后检查。因此状态必须标记 `cancel_mode=cooperative_limited`，不能宣称已强制停止正在运行的旧回调。

### 5.4 shutdown

session/runtime destroy 顺序固定为：

1. 停止接收新的 execution 和 inbox item。
2. 对活跃 execution 发出 `TURBO_CANCEL_SHUTDOWN`。
3. 唤醒 queue/backoff/waiter。
4. 在配置的 grace period 内等待 drain。
5. 对可隔离 backend 执行 hard stop；对不可中止 native callback 返回 shutdown timeout，并延迟销毁其依赖。
6. worker 与借用全部退出后销毁 store、agent 和 tool runtime。

不得在 callback 仍可能访问 session 时释放 session 内存。

## 6. 能力 2：session steering/follow-up inbox

### 6.1 语义

- `STEER`：在当前 tool batch 完成后、下一次 model call 之前注入，用来改变正在进行的任务方向。
- `FOLLOW_UP`：当前 turn 自然停止后注入，并开启下一 turn。
- `CANCEL`：不通过 inbox；直接走 execution cancel，避免排队导致停止不及时。
- steering 不回滚已经完成的外部副作用。

### 6.2 拓扑、容量与所有权

- 线程拓扑：MPSC，多个宿主线程生产，一个 session execution loop 消费。
- 实现：`turbo_deque_t` + `turbo_mutex_t` + `turbo_cond_t`。消息频率低，不采用无锁结构。
- 配置必须给出 `max_items`、`max_total_bytes`、`max_item_bytes` 和 `max_follow_ups_per_execution`；enqueue timeout 由每次调用给出。
- 满额行为：返回 `TURBO_EBUSY`；不覆盖、不静默丢弃、不隐式扩容。已 claim 的消息仍计入容量，直到 APPLIED。
- API 接收 `const json_value_t *`，入口立即序列化为 immutable UTF-8 buffer；消费侧再通过 TurboParser 解析。这样生产者后续修改 DOM 不会形成数据竞争。
- enqueue 成功后，payload 所有权归 session；enqueue 失败时调用者仍拥有输入。

### 6.3 durable inbox 协议

内存 queue 只是加速索引，durable inbox record 是事实源：

```jsonc
// immutable payload record
{
  "schema_version": 1,
  "inbox_id": "...",
  "thread_id": "...",
  "execution_id": null,
  "kind": "steer",
  "payload": {"role":"user","content":"..."},
  "payload_bytes": 123,
  "sequence": 1,
  "created_at": "..."
}

// append-only transition record
{
  "schema_version": 1,
  "transition_id": "...",
  "inbox_id": "...",
  "thread_id": "...",
  "status": "claimed",
  "action": "claimed",
  "transition_seq": 2,
  "claimed_by_run_id": null,
  "applied_event_id": null
}
```

状态迁移：

```text
QUEUED -> CLAIMED -> APPLIED
             |
             +---- execution crashed before matching event commit ----> QUEUED
```

因现有 store 只有 `put/get/list`，V4 还需新增可选、版本化的
`turbo_agent_runtime_store_v2_t`，至少提供 create-if-absent 与带 version 的
compare-exchange。V1 store 继续可用，但只能运行明确的 single-writer 模式；若宿主请求
多进程/多 runtime 共同消费同一 thread inbox，而 store 没有 V2 原子能力，必须 fail
fast。

single-writer 模式和 V2 store 都使用 append-only 两阶段记录：

1. 先以 `inbox_id` 写 immutable payload record，再以独立 transition ID 写 `QUEUED`。
2. 再发布到内存 ready queue；若发布失败，重启扫描仍可恢复。
3. 消费时追加 `CLAIMED(run_id, turn_seq)` transition；V2 store 同时用 CAS 获取
   thread consumer lease。
4. 注入的 history event 携带 `inbox_id`。
5. history/checkpoint 成功持久化后追加 `APPLIED(event_id)` transition。
6. 恢复时，从 durable thread-head state 检查 `events`：若最后一个 CLAIMED transition
   不存在匹配 event，则追加 REQUEUED；若 event 已存在则补写 APPLIED。run/state store
   无法可靠读取时 fail fast，不猜测。

该协议使重复恢复可检测；同一个 `inbox_id` 不得产生两个逻辑注入事件。

当前 V1 实现限定同一 thread inbox 只有一个 runtime/single-writer consumer。runtime 内部将
store callback 串行化，使同一进程的 MPSC enqueue 不会并发进入非线程安全 store；它不
提供多进程 lease/CAS。多 runtime 共同消费仍需未来的 `turbo_agent_runtime_store_v2_t`，
在此之前必须由宿主禁止。

### 6.4 已实现的安全点

- session 显式配置 inbox 后，在每次 model request 构建之前最多 claim 一条 STEER，写入
  canonical user message，并追加带 `event_id`/`inbox_id` 的 `inbox_message` event。
- runtime 成功持久化 history/checkpoint 后才追加 APPLIED；运行失败时保持 CLAIMED，供
  恢复协议判定。
- turn 状态为 `completed` 时，session 自动 claim FOLLOW_UP，以前一 turn 的完整 state
  开启新 run；单次公开调用最多链式执行 `max_follow_ups_per_execution` 条，避免生产者
  持续写入导致调用永不返回。
- CANCEL 始终走 execution cancel token，不进入 inbox。

### 6.5 已实现 API

```c
typedef enum turbo_agent_inbox_kind_e {
  TURBO_AGENT_INBOX_STEER = 1,
  TURBO_AGENT_INBOX_FOLLOW_UP = 2
} turbo_agent_inbox_kind_t;

typedef struct turbo_agent_inbox_config_s {
  uint32_t struct_size;
  uint32_t abi_version;
  size_t max_items;
  size_t max_total_bytes;
  size_t max_item_bytes;
  size_t max_follow_ups_per_execution;
} turbo_agent_inbox_config_t;

CXX_C_API int turbo_agent_session_inbox_configure(
    turbo_agent_session_t *session,
    const turbo_agent_inbox_config_t *config);

CXX_C_API int turbo_agent_session_enqueue(
    turbo_agent_session_t *session,
    turbo_agent_inbox_kind_t kind,
    const json_value_t *message,
    uint64_t timeout_ms,
    char **out_inbox_id);

CXX_C_API int turbo_agent_session_inbox_status(
    turbo_agent_session_t *session,
    const char *inbox_id,
    json_value_t **out_status);

CXX_C_API int turbo_agent_session_inbox_claim(...);
CXX_C_API int turbo_agent_session_inbox_mark_applied(...);
CXX_C_API int turbo_agent_session_inbox_requeue(...);
CXX_C_API int turbo_agent_session_inbox_close(...);
```

`claim/mark_applied/requeue` 是给自定义 graph/宿主安全点使用的显式协议；内置 session
loop 已自动处理 STEER 与 FOLLOW_UP。消息格式当前限定为
`{"role":"user","content":"非空字符串"}`，非法格式在持久化前返回 `TURBO_EINVAL`。

## 7. 能力 3：token budget、context projection 与 compaction

### 7.1 数据模型

durable history 永远保留完整事件；发送给模型的 context 是派生 projection：

```text
system/developer instructions
          +
latest committed compaction summary
          +
retained recent atomic segments
          +
current pending user/tool segment
```

atomic segment 至少包含：

- 一条 user message 到对应 assistant completion 的完整 turn。
- assistant tool call 与所有匹配 tool result。
- review request 与其 resume/approval event。
- subagent handoff 与返回摘要。

不得将 assistant tool call 与 tool result 拆到 compaction 边界两侧。

### 7.2 配置

```c
typedef struct turbo_agent_context_policy_s {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t context_window_tokens;
  uint64_t reserve_output_tokens;
  uint64_t compact_trigger_tokens;
  uint64_t retain_recent_tokens;
  uint64_t max_summary_input_tokens;
  uint64_t max_summary_tokens;
  uint32_t max_compactions_per_turn;
  turbo_agent_token_estimator_fn estimate_tokens;
  turbo_agent_context_summarizer_fn summarize;
  turbo_agent_context_overflow_detector_fn is_context_overflow;
  void *user_data;
} turbo_agent_context_policy_t;
```

约束：

- provider metadata 能明确给出 context window 时可作为默认；否则必须由配置提供，不能猜测。
- `reserve_output_tokens < context_window_tokens`。
- trigger 必须给 system、tools schema、当前输入和输出 reserve 留出空间。
- 预估 token 只用于 preflight；provider 返回的实际 usage 才是计费与统计事实。
- token estimator 是 provider strategy，不在核心中硬编码某家 tokenizer。

当前已实现入口：

```c
int turbo_agent_session_context_configure(...);
int turbo_agent_session_context_compact(...);
int turbo_agent_session_context_status(...);
```

session 在 model request 构造前用 estimator 对完整 provider request 做 preflight；只有达到
trigger 才压缩。summarizer 输入是 TurboParser JSON 对象
`{"previous_summary": ..., "events": [...]}`，输出必须是
`schema_version=1` 的对象且不能超过 `max_summary_tokens`。估算器、summarizer 与 overflow
识别均由宿主/provider strategy 注入；核心不猜 tokenizer、模型窗口或厂商错误文本。

### 7.3 compaction 事务

由于 V1 store 没有事务，single-writer 模式采用 PREPARED/COMMITTED 记录；多 writer
模式必须通过 store v2 CAS 更新每个 thread 的 context-head version：

1. 从 durable history 选取完整 segment 范围。
2. 保存有界 source snapshot，并计算规范序列化内容的 SHA-256 hash。
3. 以独立 child operation 调用 summarizer。
4. 校验 summary schema 和最大尺寸。
5. 写 `agent_compactions/<id>`，状态 PREPARED。
6. 写 `agent_context_heads/<thread_id>` commit marker，引用 compaction ID 与 source hash。
7. 读路径只采用有有效 commit marker 的 summary。
8. PREPARED 未提交记录可在恢复时清理或重试，不影响旧 projection。

恢复时会重新计算 PREPARED source hash，并核对 thread、range、head 与 summary schema；任何
不一致都 fail fast。V1 store 仍限定同一 thread 单 writer；多进程 writer 必须等 store v2
CAS/lease，不能把进程内 mutex 当成跨进程事务。

summary 建议使用结构化 JSON，而不是一段无约束文本：

```json
{
  "schema_version": 1,
  "goal": "...",
  "decisions": [],
  "constraints": [],
  "completed_work": [],
  "open_work": [],
  "artifacts": [],
  "tool_side_effects": [],
  "source_first_event_id": "...",
  "source_last_event_id": "...",
  "source_hash": "..."
}
```

### 7.4 overflow 恢复

provider 明确返回 context overflow 时：

1. 若本 turn 尚未 compaction，立即执行一次 compaction。
2. 只有 compaction 成功提交后，才重建请求并重试一次。
3. 若仍 overflow，返回 `TURBO_RUNTIME_ECONTEXT_OVERFLOW`，包含 estimated/actual limit、保留 segment 和工具 schema 占用摘要。
4. 不静默删除 system message、tool result 或当前输入。
5. overflow request 尚未产生 tool side effect，因此模型请求可重发；request/attempt ID 必须记录，防止 provider 异常重放被误认为新 turn。

当前垂直切片已经实现显式 overflow 检测后的“成功提交再重建请求”，且单次 model node
最多重试一次。尚未落地独立 context-overflow 错误域、request/attempt durable journal 与
provider 实际 limit 诊断，这些将与能力 6 的统一 provider retry/usage 一起完成。

当前投影只压缩 durable `events`；`state.input` 全量保留，因此不会误删当前输入或拆分
多消息输入，但超长 user input 历史仍可能无法通过压缩恢复。要压缩 input，必须先给输入
消息增加稳定 turn/event lineage，不能按数组位置猜测归属。

## 8. 能力 4：独立 tool executor

### 8.1 职责边界

workflow tool node 负责：

- 从 model event 提取 tool calls。
- middleware、guardrail、review 和 policy 决策。
- 将已批准的调用交给 executor。
- 把按原始 call 顺序排列的结果写回 state。

tool executor 负责：

- schema/name/arguments 完整性预校验。
- 并行/串行 barrier。
- 有界排队、timeout、cancel 和 progress。
- idempotency journal。
- 结果大小限制与顺序归并。

策略必须在 executor 之前完成；直接调用 `turbo_tool_runtime_invoke*` 仍明确是绕过 agent policy 的低层 API。

### 8.2 工具声明

不扩展现有公开 vtable 布局；新增 V2 descriptor/vtable：

```c
typedef enum turbo_tool_execution_mode_e {
  TURBO_TOOL_EXEC_SEQUENTIAL = 0,
  TURBO_TOOL_EXEC_PARALLEL_SAFE = 1,
  TURBO_TOOL_EXEC_EXCLUSIVE = 2
} turbo_tool_execution_mode_t;

typedef enum turbo_tool_idempotency_e {
  TURBO_TOOL_IDEMPOTENCY_NONE = 0,
  TURBO_TOOL_IDEMPOTENCY_KEYED = 1,
  TURBO_TOOL_IDEMPOTENCY_READ_ONLY = 2
} turbo_tool_idempotency_t;

typedef struct turbo_tool_runtime_tool_v2_s {
  uint32_t struct_size;
  uint32_t abi_version;
  turbo_tool_runtime_tool_t base;
  turbo_tool_execution_mode_t execution_mode;
  turbo_tool_idempotency_t idempotency;
  uint64_t max_input_bytes;
  uint64_t max_output_bytes;
  uint32_t default_timeout_ms;
  const char *const *capabilities;
  size_t capability_count;
} turbo_tool_runtime_tool_v2_t;
```

兼容默认值：旧工具全部视为 `SEQUENTIAL + NONE + cooperative_limited`。只有显式声明 `PARALLEL_SAFE` 的工具才并行。

`EXCLUSIVE` 在其前后建立 batch barrier，适合 workspace patch、git mutation 和共享工作树 build 配置等调用。

### 8.3 invoke v2 与 progress

```c
typedef struct turbo_tool_invoke_context_s {
  uint32_t struct_size;
  uint32_t abi_version;
  const turbo_execution_control_t *control; /* borrowed during invoke */
  const char *idempotency_key;
  void (*progress)(const json_value_t *event, void *user_data);
  void *progress_user_data;
} turbo_tool_invoke_context_t;

typedef turbo_tool_status_t (*turbo_tool_invoke_v2_fn)(
    void *impl,
    const turbo_tool_runtime_tool_v2_t *tool,
    const json_value_t *arguments,
    const turbo_tool_invoke_context_t *context,
    json_value_t **out_result);
```

progress callback 的 payload 必须受单事件大小和频率限制；callback 不得长期保存 borrowed DOM。

### 8.4 调度与背压

- 使用 `turbo_threadpool_t`，配置 `max_workers`、全局 queue capacity 和 per-execution max in-flight。
- 提交失败返回可区分的 FULL/CLOSED/CANCELLED/DEADLINE；不能转为无界线程。
- 一个 model tool batch 先完整验证，再构造 execution plan，避免执行到一半才发现后续调用被截断或 schema 无效。
- 连续 `PARALLEL_SAFE` 调用可成组并行；`SEQUENTIAL` 按顺序执行；`EXCLUSIVE` 等待此前任务完成并阻止后续任务启动。
- 完成顺序可以不同，但写回模型的 tool result 顺序必须与原始 tool call 顺序一致。
- executor 只在所有已启动任务进入终态后释放 batch 存储。

### 8.5 idempotency 与未知副作用

key 使用稳定 lineage：

```text
<run_id>/<turn_seq>/<tool_call_id>
```

执行日志状态：

```text
PLANNED -> STARTED -> COMMITTED
                |
                +-- crash/forced stop --> UNKNOWN
```

- COMMITTED 结果可安全重放，不重新执行工具。
- KEYED/READ_ONLY 工具只有在 backend 契约允许时自动重试。
- 非幂等工具若在外部副作用后、COMMITTED 前崩溃，必须返回 `TURBO_RUNTIME_EUNKNOWN_SIDE_EFFECT` 并进入 review，不得自动重试。
- native cooperative timeout 是 soft timeout；独立进程被可靠终止或 Wasm trap 才能标记 hard timeout。

## 9. 能力 5：coding tool pack

当前实现状态：可选 `TurboAgent::CodingTools` target 已提供 `fs.read` 与 `fs.list`。
两者只接受 workspace 相对路径，拒绝绝对路径、dot segment、符号链接/reparse
component，并限制读取字节、结果字节与目录条目数。`fs.search`、原子 patch、process、
build/test profile 和 git 工具仍未实现，不能把本只读切片描述成完整 coding tool pack。
当前路径检查是“检查后调用路径 API”，尚不是逐目录 handle/openat 模型；若 workspace
可被不可信并发进程改写，仍存在 TOCTOU 风险，因此该切片只适用于受信 workspace。

### 9.1 模块与工具

新增可选 `TurboAgent::CodingTools`：

|工具|用途|默认执行模式|主要限制|
|---|---|---|---|
|`fs.read`|读取有界文件片段。|PARALLEL_SAFE|workspace scope、字节/行数上限、binary 明示。|
|`fs.list`|列出目录或匹配文件。|PARALLEL_SAFE|结果条数/深度上限。|
|`fs.search`|通过配置的 `rg` backend 搜索。|PARALLEL_SAFE|pattern、路径、match 数与输出上限。|
|`fs.apply_patch`|原子应用统一 patch。|EXCLUSIVE|预检、workspace scope、审批、失败不留半 patch。|
|`process.exec`|以 argv 直接启动进程。|SEQUENTIAL|默认禁 shell、cwd/env allowlist、timeout、进程树终止。|
|`build.run`|执行已配置 build profile。|EXCLUSIVE|只能引用 profile，不接受任意 shell 文本。|
|`test.run`|执行已配置 test profile。|SEQUENTIAL|只能引用 profile，输出和并发有界。|
|`git.status`/`git.diff`|只读工作树检查。|PARALLEL_SAFE|无 mutation。|

`shell.exec` 若以后提供，应是独立危险 capability，不能把字符串 shell 偷渡进 `process.exec`。

### 9.2 安全边界

- 配置包含一个或多个 workspace root；所有路径先规范化，再验证归属。
- Windows reparse point/Unix symlink 必须在实际打开目标时再次验证，避免 TOCTOU 路径逃逸。
- 写操作默认进入 policy/review；只读工具仍受路径和输出配额约束。
- `process.exec` 接收 argv 数组，不由 agent core 拼接 shell 字符串。
- 环境变量采用 allowlist；密钥不进入工具结果、progress 或错误 preview。
- stdout/stderr 分别有硬上限。超限时返回 `truncated=true` 与被截断字节数；完整输出若需要保留，应写入受控 artifact store 并返回 opaque artifact ID。
- patch 流程为 parse -> scope validation -> dry-run -> conflict check -> apply -> result；任一步失败都不修改工作树。
- build/test profile 是配置事实源，包含 argv、cwd、env、timeout 和并发规则。

### 9.3 统一结果形状

```json
{
  "ok": true,
  "status": "completed",
  "exit_code": 0,
  "summary": "...",
  "stdout": "...",
  "stderr": "...",
  "truncated": false,
  "retryable": false,
  "artifacts": [],
  "metrics": {
    "duration_ms": 0,
    "output_bytes": 0
  }
}
```

该结构复用现有 action tool 的 `ok/exit_code/retryable` 语义，不在生产代码中塞入测试数据。

## 10. 能力 6：provider retry、timeout、usage 与 cost

当前实现状态：旧 transport 继续固定为单次调用；可选 transport v2 显式返回
`retryable/http_status/retry_after/provider_request_id`，配置后执行有界指数退避并在等待
阶段响应 cancel/deadline。attempt 与 provider exact usage 写入 state；未知价格明确记录为
`cost.status=unknown`。request/connect timeout 目前由 transport v2 backend 执行，agent
core 不强杀同步 callback；价格目录、跨 run 聚合、durable attempt collection 及内建 HTTP
metadata adapter 仍待实现。

### 10.1 retry 分层

retry 只有一个 owner：agent provider adapter。若底层 HTTP client 自带 retry，必须关闭或显式报告，避免 attempts 相乘。

|失败|自动重试|说明|
|---|---|---|
|连接失败、连接重置、408、429、可恢复 5xx|是|仅在未得到有效 model response 时。|
|401/403、普通 4xx、请求 schema 错误|否|配置或调用错误，fail fast。|
|context overflow|走一次 compaction 恢复|不计入普通 transient retry。|
|structured output 校验失败|沿用 structured retry|与 transport attempt 分开统计。|
|tool error|默认否|只有幂等声明和 retryable 结果同时满足才可由 tool policy 重试。|
|guardrail/review 拒绝|否|这是策略结论，不是瞬时失败。|

### 10.2 policy

```c
typedef struct turbo_agent_retry_policy_s {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t max_attempts;
  uint32_t base_delay_ms;
  uint32_t max_delay_ms;
  uint32_t max_elapsed_ms;
  uint32_t request_timeout_ms;
  uint32_t connect_timeout_ms;
  uint32_t jitter_percent;
  int honor_retry_after;
} turbo_agent_retry_policy_t;
```

- backoff 为有上限的 exponential backoff + jitter。
- `Retry-After` 只在 policy 上限内采用。
- 每次 delay 都用可取消 timed wait。
- deadline 剩余时间不足以完成下一 request timeout 时，不再发请求。
- attempt record 记录 provider、model、request ID、HTTP status/error domain、delay、start/end 和 outcome。

### 10.3 transport v2

旧 transport callback 保留；新增接收 control 和结构化 response metadata 的 v2：

```c
typedef int (*turbo_agent_transport_v2_fn)(
    const json_value_t *request,
    const turbo_execution_control_t *control,
    turbo_agent_transport_response_t *out_response,
    void *user_data);
```

`turbo_agent_transport_response_t` 至少包含 HTTP/provider status、retry-after、provider request ID、body/stream 聚合结果和 usage 原始字段。错误信息不再依赖解析一段格式化字符串来分类。

### 10.4 usage 与 cost

每次 provider response 生成 immutable usage record：

```json
{
  "schema_version": 1,
  "execution_id": "...",
  "run_id": "...",
  "turn_seq": 1,
  "operation": "model|compaction|subagent",
  "provider": "...",
  "model": "...",
  "provider_request_id": "...",
  "input_tokens": 0,
  "cached_input_tokens": null,
  "output_tokens": 0,
  "reasoning_tokens": null,
  "total_tokens": 0,
  "cost": {
    "status": "known|unknown",
    "currency": "USD",
    "amount_micros": null,
    "price_catalog_version": null
  }
}
```

规则：

- provider 原始 usage 是事实；估算值必须标 `estimated=true`，不能混入精确账单。
- 费用通过版本化 price catalog 计算，记录版本与生效时间。
- 未知模型或缺少价格时 `cost.status=unknown`，不能返回 0。
- 聚合维度包括 execution/run/thread/model/provider/operation；compaction 和 subagent 不得隐藏在主 turn 费用中。
- token 数使用 64-bit checked arithmetic；聚合溢出返回错误，不饱和为错误值。

## 11. Wasm 的代理范围

### 11.1 Wasm 代理什么

Wasm guest 只代理受能力限制的工具实现：

```text
Agent workflow
  -> registry capability metadata + Agent policy
  -> approval/guardrail
  -> tool executor
  -> Wasm tool runtime
  -> capability broker
  -> HTTP/Redis/TurboNet/FS/process 等显式 host service
```

宿主始终拥有：

- provider transport、模型密钥与 retry。
- system prompt、context projection 与 compaction。
- thread/run/checkpoint/inbox/tool journal 事实源。
- tool schema 注册、policy、review、guardrail 和最终结果提交。
- execution cancel/deadline 与 usage/cost。

### 11.2 TurboWasm 配额

`turbo_tool_runtime_wasm_config_t` 是版本化宿主适配配置，负责限制工具数量、
descriptor bytes、input bytes 与 output bytes。`turbo_wasm_policy_t` 是唯一的沙箱事实源，
负责 module root、capability manifest、linear memory、module bytes、stack、timeout、
control-flow steps、host-call 次数和 guest/host copy bytes。策略在 VM 创建时 clone/freeze，
之后 guest 或工具注册表均不能扩权。

deadline 与共享 cancel token 不属于长期 runtime 配置；它们应通过每次调用的
`turbo_tool_invoke_context_t` 传入，并在后续 runtime v2 bridge 中映射到 TurboWasm
调用边界。TurboAgent 已验证 capability、初始 linear memory、运行时 `memory.grow`、
host input/output 的拒绝路径。更强的进程级故障隔离仍可作为独立 backend，而不是绕过
同一工具策略。

### 11.3 ABI

- 使用版本化 C ABI、opaque VM 和 POD 标量。
- JSON/XML/YAML 数据均在宿主边界转换为 TurboParser JSON；guest tool ABI 通过
  TurboWasm App I/O 只收发有界 UTF-8 JSON bytes。
- 不跨 ABI 传递 guest/host 裸指针、宿主 allocator 所有权、复杂 C struct 或裸 socket。
- host capability 以 narrow operation 暴露，例如受限 `http_request`，不暴露任意系统调用。

### 11.4 WasmToolPack 决策

背景：单个 `turbo_tool_runtime_t` 已能桥接为 registry，但缺少把多个 TurboWasm module
组合为一个 Agent source registry 的所有权边界。候选方案包括自动扫描目录、让 skill
直接加载 module，以及由宿主显式组装 pack。自动扫描会扩大信任面；skill 加载会把说明
文档变成执行事实源。因此选择显式 `WasmToolPack`：宿主逐个提供 module 配置和已收紧的
TurboWasm policy，pack 只负责验证、加载、组合、容量限制与确定性销毁。

- 架构：`WasmToolPack -> RuntimeTools v3 registry adapter -> TurboWasm runtime`。v3
  metadata 自动携带 `runtime_tools`，投影与组合不得丢失；执行器在副作用前再次检查。
- 状态归属：pack 独占统一 registry；每个 registry binding 持有 runtime 引用；skill 和
  workspace selection 只借用/投影 registry，不拥有 VM。
- 失败语义：module 的所有工具必须原子加入；重复名称、容量不足、ABI/schema/policy
  失败时回滚该 module 已加入的 binding，既有 pack 状态不变。
- 并发语义：每个 module 显式声明 execution mode 与 idempotency；默认 sequential，避免
  对同一 VM 产生并发 App I/O。当前实现只接受 sequential/exclusive；在 runtime 能为并发
  调用提供独立 VM 或等价隔离前，pack fail-fast 拒绝 parallel-safe。
- 迁移：原有单 runtime bridge 保持兼容，并委托同一个 v2 adapter；现有 agent API 不变。
- 回滚：删除 pack 接入即可回到单 runtime bridge；module 文件和 skill 格式均不迁移。

## 12. 事件与可观测性

新增事件均带 `schema_version`、execution/thread/run/turn/tool lineage 和单调序号：

```text
execution.queued
execution.started
execution.cancel_requested
execution.deadline_exceeded
execution.completed|failed|cancelled|timed_out
inbox.enqueued|claimed|applied|requeued
context.budgeted
compaction.prepared|committed|failed
provider.attempt_started|retry_scheduled|attempt_finished
usage.recorded
tool.queued|started|progress|completed|unknown_side_effect
```

事件 payload 有硬上限；prompt、密钥、完整环境变量和任意大 stdout 不进入事件。日志只在错误被消费/转换的边界记录一次，高频 progress 默认采样。

## 13. 配置优先级与默认行为

配置来源保持：命令行 > 环境变量 > 配置文件 > 默认值。建议 V4 profile 使用 TOML，但内部解析后统一为 TurboParser JSON value。

兼容默认：

- 旧同步 API 行为不变，并通过新 execution 内核阻塞等待结果。
- 旧工具默认 sequential，不因 `parallel_tool_calls=1` 自动并发。
- 未配置 context window 时不启用自动 compaction；provider overflow 返回明确错误。
- 未配置 retry policy 时保留单次模型请求，避免悄然改变延迟和费用。
- coding tools 是显式注册的可选 target，不自动获得 workspace/process 权限。
- Wasm 新 capability 默认拒绝，按 manifest 开启。

## 14. 迁移计划

### Phase 0：契约与底座

- 新增 `RuntimeCore`、错误域、clock/cancel token。
- 新增可选 `runtime_store_v2` 原子能力；V1 store 明确标记 single-writer。
- 为 event、V2 vtable/config 加 `struct_size + abi_version`。
- 建立 fake clock、fake transport、blocking fake tool 测试设施。
- 不改变用户可见行为。

### Phase 1：execution + provider resilience

- 新增 async execution、wait/cancel/deadline/status。
- graph/model/backoff 加 cancel checkpoint。
- transport v2、timeout/retry、usage record。
- 旧 `turbo_agent_runtime_exec_*` 委托新内核后同步 wait。

### Phase 2：durable inbox

- 新增 inbox collections、MPSC ready queue 和 turn-boundary injection。
- 实现 crash recovery 与 inbox ID 去重。
- session destroy 使用统一 shutdown protocol。

### Phase 3：context manager

- provider token estimator strategy、budget projection。
- two-phase compaction records 和一次 overflow recovery。
- 提供从完整 history 重建 projection 的校验工具。

### Phase 4：tool executor + coding tools

- tool runtime v2 bridge、bounded threadpool、barrier、progress、journal。
- 先将现有工具以 legacy sequential adapter 接入。
- 再交付 read/list/search，随后 patch/process/build/test。

### Phase 5：Wasm hardening

- 已完成：TurboWasm 成为唯一 engine boundary；capability manifest、memory、
  control-flow、timeout、I/O 与 host-call quota 由不可变 policy 统一配置。
- 已完成：工具 ABI 改为 TurboWasm App I/O，不再跨边界读取 guest 裸指针。
- 已完成：初始 linear memory 超额与运行时 `memory.grow` 越界均有拒绝回归测试。
- 待完成：把每次 tool invocation 的共享 cancel/deadline 贯通到 TurboWasm 调用上下文。
- 只有在存在真实不可信 CPU workload 且需要进程级故障隔离时，再增加独立进程 backend。

每个 phase 都由独立 feature option 控制；下一 phase 不以未验证的上一 phase 为前提进入默认开启状态。

## 15. 兼容性、迁移成本与回滚

### 15.1 兼容性风险

|级别|风险|控制|
|---|---|---|
|HIGH|修改现有公开 vtable/配置 struct 会破坏 ABI。|只新增 V2 类型与 create_v2；V1 adapter 长期保留。|
|HIGH|并行工具会改变副作用顺序。|旧工具默认 sequential；仅显式 PARALLEL_SAFE 才并行，结果按 call 顺序提交。|
|HIGH|取消时工具可能已产生不可回滚副作用。|区分 soft/hard cancel，journal 标记 UNKNOWN 并要求 review。|
|HIGH|compaction 若成为第二历史源会造成不一致。|完整 events 为唯一事实源；summary 只作为带 source hash 的 committed projection。|
|MED|retry 改变延迟、请求次数和费用。|默认单次；显式 policy；attempt/usage 全量可观测。|
|MED|durable inbox 在无事务 store 上可能重复注入。|append-only 两阶段记录 + inbox_id event 去重与恢复审计。|
|MED|coding process/patch 扩大主机权限。|独立 target、capability、workspace scope、policy/review 和配额。|

### 15.2 迁移成本（推论）

- Phase 0–1 影响 graph、transport、runtime API 与测试，属于跨模块中高成本。
- Phase 2–3 需要新增 durable record 与恢复测试，是状态一致性风险最高部分。
- Phase 4 可通过 adapter 渐进迁移现有工具；coding tools 独立交付。
- Phase 5 不应阻塞 native tool executor，但要在启用不可信 guest 前完成。

这里不承诺人日数字；应在 API skeleton 和首个 vertical slice 完成后依据实际 diff、测试数量和 backend 数量复算。

### 15.3 回滚

- 所有新 durable record 使用新 collection 和 schema version，旧 runtime 可忽略。
- 关闭 compaction 后，从完整 history 重建原始 context projection。
- 关闭 tool executor v2 后，legacy adapter 恢复同步 sequential 调用。
- 关闭 retry 后，provider 回到单次请求。
- coding tools target 可整体不链接。
- Wasm capability manifest 失败时 fail fast，不回退到开放权限。

## 16. 验证矩阵

测试使用 TinyTest；优先 fake clock/fake transport/fake tool，避免依赖真实网络时间。

### 16.1 execution

- CREATED/QUEUED/RUNNING 每个阶段取消。
- graph node 前后、model stream、retry wait、tool queue、compaction 中取消。
- deadline 与 user cancel 竞争，验证 first reason wins。
- 多 waiter 同时 wait；timeout 不改变 execution 状态。
- session destroy 唤醒所有 waiter，且无 use-after-free。
- legacy blocking callback 被标记 cooperative_limited。

### 16.2 inbox

- `capacity`、`capacity+1`、单项 byte limit、总 byte limit。
- 多 producer/单 consumer 顺序与唯一 ID。
- STEER 在 tool batch 后注入，FOLLOW_UP 在 turn end 后注入。
- QUEUED 后内存 publish 失败的恢复。
- CLAIMED 后崩溃，有/无 matching event 两种恢复。
- shutdown 时拒绝新 item，并唤醒阻塞 enqueue。

当前 TinyTest 已覆盖 immutable payload、claim/requeue/apply、claimed 容量占用、close
唤醒、MPSC 唯一 ID、CLAIMED 有/无 matching event 的 crash recovery、STEER model
safe point、FOLLOW_UP 自动新 turn 与 APPLIED event 关联。内存 publish 故障注入、
精细 byte 边界及 sanitizer/benchmark 仍需在扩大验证阶段补齐。

### 16.3 context/compaction

- assistant tool call/result 不被拆分。
- source hash、PREPARED 未提交忽略、COMMITTED 采用。
- summary schema/size 失败不移动 context head。
- 一次 overflow -> compact -> retry success。
- 第二次 overflow fail fast，不循环。
- provider 无 window 且未配置时返回明确配置错误。
- 完整 history 能重建 projection，compaction 不删除事件。

当前 TinyTest 已覆盖 event history 不删除、最近 segment 保留、assistant tool
call/result 原子边界、COMMITTED 重启恢复、PREPARED-only 不可见、一次 overflow 后成功
重试，以及连续 overflow 最多发出两次 provider 请求。summary schema/size 的独立故障注入、
input turn lineage、request/attempt journal 与 provider limit 诊断仍待后续切片。

### 16.4 tool executor

- sequential、parallel-safe、exclusive barrier 的开始/完成顺序。
- 并发完成乱序但结果提交顺序稳定。
- queue full/closed/cancel/deadline 可区分。
- truncated/invalid tool call 在任何副作用前拒绝整个 batch。
- progress 频率/大小限制。
- COMMITTED 重放、UNKNOWN side effect 禁止自动重试。
- shutdown drain 与 payload 恰好释放一次。

当前 TinyTest 已覆盖全批预检先于副作用、显式 parallel-safe 并发与原序提交、输出上限、
callback 前取消、COMMITTED 回放、参数身份不匹配拒绝，以及 STARTED/commit 丢失后返回
UNKNOWN 且不重复调用。progress、hard timeout、独立进程终止与更细 queue 状态仍待实现。

### 16.5 coding tools/Wasm

- `..`、绝对路径、symlink/reparse point 逃逸。
- patch dry-run 失败不改文件；多文件 patch 原子性。
- argv 不经过 shell；env allowlist 和 secret redaction。
- timeout 后进程树终止；stdout/stderr 各自截断。
- Wasm memory/input/output/host-call/socket 配额。
- capability 未声明时拒绝，且不走 fallback。
- guest memory grow 后指针重新校验。

当前 CodingTools TinyTest 覆盖只读注册策略、workspace escape 拒绝及 read byte limit；
写入、进程和 Wasm quota 不在本切片的已验证范围内。

### 16.6 retry/usage

- 408/429/5xx/network reset 与 4xx/auth 分类。
- Retry-After 上限、jitter 边界、max elapsed。
- backoff 中 cancel/deadline。
- 确认不存在 HTTP 层与 provider adapter 的乘法 retry。
- structured retry、overflow recovery、transport retry 分开计数。
- usage exact/estimated、unknown price、catalog version 与 checked aggregation。

当前 TinyTest 已覆盖结构化 transient 两次失败后成功、非 retryable 单次失败、backoff
取消、attempt 次数、exact token 记录与 unknown cost。HTTP 内建 adapter 分类、fake clock
的 jitter/max-elapsed 全边界及 price catalog 尚未覆盖。

### 16.7 动态与性能验证

- ASan：execution/session/tool payload 生命周期。
- TSan（平台可用时）：MPSC inbox、status/wait/cancel 竞争。
- benchmark：tool scheduler 开销、inbox enqueue/dequeue、history segment selection；只在正确性测试通过后运行。
- 容量测试按 `items + metadata + retained payload + stdout/stderr + journal` 计算总内存，而不是只算 queue slot。

## 17. 验收标准

V4 可以宣称完成，必须同时满足：

1. 同一 execution 可被查询、等待、取消，deadline 能贯穿 model、queue、tool 和 backoff。
2. steering/follow-up 有明确 turn boundary、容量、持久化与 crash recovery 语义。
3. 长历史可在不删除 durable events 的前提下自动 compact，overflow 不会无限重试。
4. 工具并发有显式声明、硬容量、稳定结果顺序、取消与未知副作用处理。
5. coding tools 默认受 workspace、process、output、policy 和 capability 限制。
6. provider transient retry 可配置、可取消、不会乘法放大；usage/cost 可追溯且 unknown 不冒充零。
7. 旧同步 API 和旧工具在默认配置下保持 sequential、single-attempt 兼容行为。
8. 所有新增资源的 shutdown/drain/ownership 均有 TinyTest 和 sanitizer 验证。

## 18. 与 Pi 核心能力的预期差距

完成 Phase 1–4 后，TurboAgent 在核心运行能力上可补齐 Pi 当前最明显的差距：AbortSignal 类取消、steering/follow-up、自动 compaction、并行/串行工具调度、coding tools、retry 与 usage。

TurboAgent 应继续保留自己的差异化优势：durable thread/run/checkpoint、graph interrupt/review、精确工具审批、多 agent lineage、原生 C ABI 和 Wasm 工具隔离。目标不是复刻 Pi 的 TypeScript API，而是达到等价的操作能力，并在持久化、策略和隔离上保持更强边界。

Pi 一手资料：

- <https://github.com/earendil-works/pi/blob/main/packages/agent/src/agent-loop.ts>
- <https://github.com/earendil-works/pi/blob/main/packages/coding-agent/docs/compaction.md>
- <https://github.com/earendil-works/pi/blob/main/packages/coding-agent/docs/session-format.md>
- <https://github.com/earendil-works/pi/blob/main/packages/coding-agent/docs/security.md>

## 19. 推荐的首个 vertical slice

不要先同时实现六项。首个可复验切片应是：

```text
runtime_core cancel/deadline
  -> async turbo_agent_execution_t
  -> graph/model 检查点
  -> transport v2 fake backend
  -> 一个 blocking fake tool v2
  -> execution event + TinyTest
```

这个切片能先锁定最关键的所有权、状态机、ABI 和 shutdown 协议。随后 inbox、compaction、并行工具和 coding tools 都复用同一个 execution control，而不是各自发明取消机制。
