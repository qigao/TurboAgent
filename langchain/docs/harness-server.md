# TurboAgent Harness Server 控制面

## 决策背景

`TurboAgent::Harness` 已封装单个 App/Session 的异步执行生命周期，但它不是面向 IDE、CLI、
桌面端或远程宿主的控制协议。宿主仍缺少连接握手、thread/turn 路由、流式事件恢复、审批响应和
多客户端隔离。

本设计借鉴 Codex App Server 的分层与 `thread / turn / item` 协议模型，新增传输无关的
Harness Server。它不追求 Codex wire compatibility；协议字段只承诺本文和公开头文件定义的
TurboAgent v1 语义。

## 候选方案

### 直接扩展单 Harness

优点是对象少。缺点是一个 Session 保存 `thread_id`、`last_run_id` 和
`last_checkpoint_id` 的派生缓存，无法安全表达多个并行 thread；同时会让执行生命周期对象承担
连接和协议职责。

### 在 Runtime Remote 上增加 Agent 方法

现有 Runtime Remote 已有 JSON-RPC 编解码模式，但它面向同步、无会话的 Runtime 数据面。
把连接初始化、异步 turn、事件背压和审批放进去，会让持久化事实源与控制面生命周期相互穿透。

### 独立 Harness Server 控制面

采用此方案。Server 通过 factory 为每个加载的 thread 创建独立 Harness，并把 JSON 协议适配、
连接状态和事件 journal 保持在 Harness/Runtime 之外。

## 分层与状态归属

```text
stdio / WebSocket / Unix socket adapter      （宿主，可替换）
                    |
Harness Server connection + protocol         （本模块）
                    |
thread record -> one Harness -> App/Session   （执行适配层）
                    |
Runtime Store                                （唯一持久化事实源）
                    |
Tool Registry / MCP / Wasm                   （能力与隔离）
```

- Server 只保存“当前进程已加载哪些 thread”和“当前逻辑 turn”的控制状态。
- Runtime Store 唯一保存 thread、run、checkpoint 和历史事件；Server 不复制这些记录。
- 每个已加载 thread 拥有一个 Harness，所以 Session 的最近-ID缓存不会跨 thread 竞争。
- 一个 thread 同时最多一个逻辑 turn；不同 thread 是否并行由 factory 创建的 bounded executor
  决定。
- Connection 保存初始化协商结果和一个有界事件 journal，不保存 Agent 领域状态。
- `turn/start` 的 connection 拥有该 active turn 的 steer、interrupt 与 approval 权限；其他
  connection 可读取 thread/turn 状态，但不可劫持其控制操作或消费其 journal。

## 公开协议 v1

线上的消息采用省略 `"jsonrpc":"2.0"` 的 JSON-RPC 风格对象。请求包含 `method`、`id` 和可选
`params`；响应回显 `id` 并携带 `result` 或 `error`。客户端 notification 没有 `id`，成功时没有
响应。

稳定方法：

- `initialize`，随后客户端发送 `initialized` notification
- `thread/start`、`thread/resume`、`thread/get`、`thread/list`
- `turn/start`、`turn/steer`、`turn/interrupt`、`turn/get`
- `approval/respond`
- `event/replay`、`event/ack`

v1 的 `turn/start.input` 和 `turn/steer.input` 接受一个或多个 `{type:"text", text:"..."}`；
其他 input item 类型 fail fast。`turn/start.interruptBeforeNodes` 可选，最多 64 个非空节点名，用于
宿主明确请求 human-in-the-loop 检查点；`deadlineMonoMs` 是可选的单调时钟绝对截止时间。
`thread/fork` 需要“不执行 turn 即复制历史”的 Runtime 原语，
当前没有公开为稳定方法，capability 中明确报告 `supportsThreadFork=false`，避免用一次隐式 graph
执行伪装 Codex 的历史分叉语义。

事件 notification 使用单调 `sequence`：

- `thread/started`
- `turn/started`
- `item/event`，其中 `item` 是 TurboParser JSON-native runtime event
- `item/review/requestApproval`
- `item/tool/requestApproval`
- `serverRequest/resolved`
- `turn/completed`

`item/event` 是 TurboAgent 的原生 item envelope，不把已有 runtime event 猜测性转换成 Codex
内部 item 类型。

## 事件消费与 JSONL 传输适配

`turbo_agent_harness_connection_wait_event_json_value()` 是 journal 的阻塞消费入口。它返回最旧的
未确认 notification clone，并把精确 `sequence` 写入 `params.sequence`；零超时执行非阻塞检查，
`UINT64_MAX` 表示无限等待。事件在显式调用
`turbo_agent_harness_connection_ack_events()` 前保持可重放。一个 connection 同时只能有一个事件
消费者：不得并发执行 wait、`event/replay`、`event/ack` 或多个 pump；请求 dispatcher 可与消费者
并行。

`turbo_agent_harness_transport.h` 提供传输无关的 JSON-lines Adapter：

- 宿主 reader 把一个不含 CR/LF 的完整 JSON 请求交给 `dispatch_line()`。
- 宿主 writer thread 调用 `pump_events()`，等待首个事件后，以非阻塞方式批量排空最多
  `max_event_batch` 个事件。
- Adapter 为请求响应和事件共用同一个输出锁，writer callback 每次收到恰好一个以 LF 结尾、内部
  不含行分隔符的完整 frame，因此并发 dispatcher 和 event pump 不会交错写入字节。
- 只有 writer callback 成功后才确认事件；失败时当前 sequence 仍在 connection journal，下一次
  pump 会重放同一事件。

stdio 宿主可用一个阻塞 reader loop 调用 `dispatch_line()`，另用一个 writer loop 调用
`pump_events(UINT64_MAX)`；WebSocket 宿主把每个 text message 视为一条不带 LF 的输入 frame，并在
writer callback 中把 Adapter 的完整 JSONL frame 映射为一个 WebSocket text message（可在发送前
去掉末尾 LF）。Adapter 不拥有文件描述符、socket、event loop 或 TLS，因此这些资源的关闭与线程
join 仍由宿主控制。销毁顺序为：停止输入、关闭 connection 以唤醒 pump、join reader/writer、销毁
Adapter，最后销毁 Server。Adapter 在创建时 retain connection，因此 `close()` 与刚启动的 pump
并发也不会释放其底层内存；Adapter 本身不隐式关闭 connection。

## 数据路径协议

| 项目 | 契约 |
|---|---|
| 数据单元 | journal entry：`sequence + owned json_value_t + serialized_bytes` |
| 事实源 | connection journal 是未确认传输事件的唯一内存事实源；Runtime 历史仍在 Runtime Store |
| 所有权 | enqueue 成功后 connection 拥有 JSON；replay/wait 返回 deep clone；ack 后释放原对象 |
| 生命周期 | replay 结果独立；borrowed event 只在 Harness sink 回调期间有效并立即包装/clone |
| 拓扑 | Harness worker 为 producer，connection transport 为 consumer；多个 thread 可向同一 connection 生产 |
| 顺序 | 每 connection 全局递增 sequence；成功入 journal 的事件保持 FIFO |
| 容量 | `max_event_count`、`max_event_bytes`、`max_single_event_bytes` 均为硬上限 |
| 背压 | worker 在 journal 满时等待 ack；为 approval/resolved/completed 保留 3 个控制事件槽及等额字节预算，并额外保证一个最大数据事件可进入；connection close 唤醒并停止投递；单事件过大使 stream fail fast |
| 失败 | enqueue 前失败仍由 producer 拥有；stream failure 可由 replay/turn 状态观察，不静默扩容 |
| 关闭 | 先关闭 connection 并唤醒 producer，再取消/等待 active turn，最后销毁 Server |

容量计算由宿主按峰值事件速率与最长未 ack 时间配置：

```text
required_entries >= peak_events_per_second * worst_ack_stall_seconds + maximum_event_batch
required_bytes   >= required_entries * aligned_average_event_bytes + largest_event_bytes
```

这是内存预算，不是吞吐保证。Server 在创建时预留 thread 表和 journal 容量，运行期不使用无界
增长作为背压。

## 审批协议

底层工具 policy 仍通过 review checkpoint 请求审批。Harness Server 在 execution 到达
`interrupted + pending_action=review` 后发出 `item/review/requestApproval`；若 checkpoint 带有工具
调用描述，则发出 `item/tool/requestApproval`。两者都保持同一个逻辑 turn：

```text
inProgress -> approvalPending -> approve_once -> resumed execution -> terminal
                              \-> deny         -> declined terminal
```

v1 只接受 `approve_once` 和 `deny`。批准时 Server 用 thread-scoped `approve_review` command 恢复
Harness；拒绝时不执行工具并结束逻辑 turn。工具权限的最终判定仍由 Agent Policy、Tool Registry
capability 和 TurboWasm sandbox 共同执行，Server 不绕过策略。

## 兼容性、迁移与回滚

这是新增 ABI，不修改 Harness、App、Session、Execution 或 Runtime 的既有签名与持久化格式。
已有嵌入式调用方无需迁移。已有轮询 `event/replay` 客户端仍可保持原行为；新宿主可使用 wait API
或 JSONL Adapter 接入 stdio/WebSocket。不得让旧 replay consumer 与新 pump 同时消费同一
connection；迁移时应在连接粒度切换。

回滚只需停止创建 Harness Server connection，继续直接使用 `TurboAgent::Harness`。Runtime Store
没有迁移或双写，因此不需要数据回滚。

## 验证范围

- initialize / initialized 顺序、重复初始化与未知方法
- thread start/resume/get/list 与 factory 绑定校验
- 每 thread 单 turn、跨 thread 独立执行
- text input 校验、start、steer、interrupt
- event FIFO、cursor replay、阻塞 wait 唤醒、ack、容量背压和 close 唤醒
- JSONL 单帧输出、notification 无响应、事件批量排空与 writer 失败后重放
- review approval request、approve_once 恢复、deny 终止
- Harness、Execution、Inbox、Runtime Remote 相邻回归与完整 CTest
