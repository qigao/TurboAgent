# TurboAgent Harness 适配层

## 决策背景

TurboAgent 已分别提供 App/Session、durable runtime、workflow graph、tool schema、启动诊断、事件 sink 和异步 Execution。宿主在接入时仍需自行组合这些接口，并自行处理 graph 生命周期、取消令牌、Session 运行摘要同步和单写者约束。

本设计增加 `TurboAgent::Harness` 作为嵌入式宿主入口。它是薄适配层，不新增 Agent 状态、工具状态或持久化副本，也不改变既有 App/Session API。

## 候选方案

### 直接扩展 `turbo_agent_app_t`

优点是调用层级少。缺点是 App 同时承担配置、同步 convenience API、异步调度和执行句柄管理，进一步扩大公开接口职责；异步生命周期也会侵入现有同步行为。

### 让宿主直接组合 Session 与 Execution

无需新增 API，但每个宿主都必须复制 runtime options 转换、preset graph 生命周期和 Session summary capture，容易造成 run/checkpoint 缓存与持久化状态不一致。

### 独立 Harness 适配器

采用此方案。Harness 拥有 App，借用 bounded executor，并返回独立的 retained execution handle。底层仍以 Runtime Store 为 thread/run/checkpoint 的唯一事实源。

## 公开边界

`TurboAgent::Harness` 是拥有嵌入式适配器、Server 和 Transport 的独立动态库，公开链接 `TurboAgent::Agent`。嵌入式适配器通过 `turbo_agent_session_execution.h` 的 `turbo_agent_session_submit()` 提交操作，不再包含 Agent 私有头文件。Session 摘要同步、inbox 提交与后续轮次仍由 Agent 内部完成，错误先写入执行结果再发布终态。两者不新增状态 owner。拆分边界见 [module separation](module-separation.md)。

公开对象：

- `turbo_agent_harness_t`：拥有一个 App；借用调用方 executor。
- `turbo_agent_harness_execution_t`：拥有 workflow graph，retain Harness 与底层 Execution。
- `turbo_agent_harness_run_options_t`：描述 graph options、resume/fork 语义、deadline 和事件 sink。

输入、输出、诊断、工具 schema 与事件均使用 TurboParser `json_value_t`。Harness 不引入其他 JSON 数据模型。

## 所有权与并发协议

| 项目 | 契约 |
|---|---|
| 数据单元 | 一个 retained execution handle；输入 JSON 在 submit 返回前由 Execution 深拷贝 |
| 事实源 | Runtime Store 保存 thread、run、checkpoint；Session 仅缓存最近标识 |
| Harness 所有权 | Harness 创建并销毁 App；executor 由调用方拥有 |
| Execution 所有权 | caller 与 worker 各持一份 wrapper 引用；Harness active 槽位持一份底层 Execution 引用 |
| Graph 生命周期 | wrapper 从 submit 到最终 release 独占 graph，保证 worker 和 completion hook 使用期间有效 |
| 拓扑 | 每个 Harness 为单 producer control plane、最多一个 active execution；executor 可为 MPMC，但同一 Runtime Store 不并发写 |
| 容量 | executor 必须配置非零有界 queue；每个 Harness 的 active execution 上限固定为 1 |
| 背压 | executor 满或已有非终态执行时立即返回 `TURBO_EBUSY`，不阻塞、不丢弃、不无界扩容 |
| 取消 | `cancel` 协作式触发；graph 在安全边界生成 `cancelled` 或 `timed_out` summary 与可恢复 checkpoint |
| 关闭 | Harness execution 必须到达终态；wrapper retain Harness，因此调用方可先 release 自己的 Harness 引用。executor 必须最后销毁 |
| 事件 | sink 在 worker 线程同步调用；`event_sink_user_data` 必须保持到 execution 终态，不得在回调中销毁 Harness/Execution |

Harness 本身不承诺 App accessor 与 worker 并发访问的线程安全。宿主应通过 event sink、execution status/result 和终态后的 App inspect API 观察运行。

### 默认模型 HTTP 客户端的线程归属

CHttp/CNet 在初始化时固定 progress owner。Harness 可以在控制线程创建，再把
运行提交到不同的 executor worker，因此默认模型客户端不能在 Agent 创建时初始化。
默认 transport 在每次 HTTP 调用的执行线程初始化客户端，完成 POST 后在同一线程
停止、drain 并销毁其实现，再发布模型结果。Agent 仅持有客户端外壳；请求、响应和
网络容量沿用已有硬上限，Session 仍为单写者。初始化、请求和关闭失败都返回
transport 错误，关闭失败不发布成功，也不重新初始化尚未释放的客户端。

选择请求作用域，是因为延迟初始化一次仍会在后续运行换 worker 时失败；为每个
Agent 另建固定 HTTP 线程会增加队列、关闭协议和常驻线程。代价是默认路径不再跨
模型调用复用连接；未声称性能收益。初始化错误在首次 HTTP 调用报告。调用方显式
注入的 `http_client` 仍由调用方拥有，必须在其固定 owner 上使用，不由 Agent
停止或销毁；需要自定义调度时使用已有 transport 接口。

Session 的默认 provider 选择复用 Agent 的 resolver：显式 provider 优先，否则
遵循 `api_mode`，避免将已配置 Chat Completions 的请求错误发往 Responses。

此修复不改变配置布局、协议或持久化格式。验证覆盖离线 loopback HTTP、跨 worker
连续运行、HTTP 错误后的下一次运行，以及可选真实代理的问答和编程任务。回滚需
同时恢复固定执行 owner，不能只恢复在控制线程初始化客户端的旧行为。

### 默认 HTTP 的取消与 deadline

默认传输使用 CHttp 的 caller-driven async API，在既有执行线程内完成一次请求，
不增加 HTTP worker 或队列。阻塞 facade 无法观察执行 token；另建固定线程会
引入额外调度和关闭归属，因此采用每次请求初始化、短轮询、同线程 drain 的方案。

| 项目 | 契约 |
|---|---|
| 事实源与归属 | Agent 独占一个私有 HTTP owner；CHttp 拥有协议/网络状态；owner 的堆上 completion 存储只由当前请求线程访问 |
| 借用 | submit 成功前 CHttp 复制请求；callback 内复制模型消费的状态码和 body，绝不保留 response view；callback user 不引用 Agent、栈或执行 token |
| 容量 | 沿用 4 个 HTTP slot、16 MiB 请求、64 MiB 响应上限；同一 Agent 仍只允许一个请求，耗尽立即失败，无额外 admission/replay 队列 |
| 执行 | 请求线程 init → submit 一次 → poll → stop/drain → destroy；impl 清空后才允许下一次运行换 worker |
| 取消 | 提交前及至多 50 ms 的 poll 片段之间检查 token；先请求取消，再 drain，完成资源清理才返回取消。50 ms 不是硬实时或总关闭延迟保证 |
| 超时 | 无更早执行 deadline 时保留 60 秒请求上限；执行 deadline 通过同一 token 检查，取消不延长请求期限 |
| 失败 | init/submit/poll/copy/cancel/stop/destroy 错误显式传播；关闭最多等待 5 秒，失败后禁止复用该 owner，不发布成功或取消终态 |
| 异常保留 | stop/destroy 失败时保留 owner 和 callback 存储，防止悬空；Agent 销毁会报告保留原因，仍可能被 SDK 使用的存储留到进程退出。该 Agent 后续调用不再分配或提交 |
| 结果 | 正常关闭后将响应移动给 transport；失败详情和 attempt 数值沿用现有记录。没有隐式 POST 重试，TLS 验证和协议不变 |

模型节点收到与当前 token 一致的取消状态时，跳过响应提交、上下文溢出恢复和
结构化输出重试，将下一节点设为当前模型节点。Graph 使用原来的取消检查及
checkpoint 协议生成 `cancelled` / `timed_out` 终态，显式 resume 才重新请求模型。
同一 route 边界同时满足取消和 `interrupt_before_nodes` 时，已观察到的取消优先，
两者仍共用一次 checkpoint 写入；不会把取消覆盖成普通 workflow 暂停。
旧 transport 晚到的成功响应也在该边界丢弃；其他传输或关闭错误仍走失败路径。
显式注入的阻塞 `http_client` 与自定义 callback 不被强行中止，继续遵循其既有
owner 契约。无需改变公开配置、持久化格式或依赖版本；回滚可恢复阻塞 POST，
但必须保留每次请求在实际 worker 上初始化和清理的约束。

验证使用正式 loopback deferred-response 测试：服务端尚未回复时完成取消或
deadline，验证终态、checkpoint、单次 POST、无模型文本提交，并在另一 worker
恢复运行。关闭失败保留路径需要 SDK 故障注入才能进一步验证，不能以正常网络
测试推断其无泄漏。

### 默认 HTTP 的失败记录

默认模型 HTTP 路径复用现有 `provider_attempts` schema，记录 CHttp 返回的
HTTP 状态、传输状态、起止时间与成功/失败。状态码直接来自响应结构，不解析
错误说明字符串。请求正文和响应正文不进入 attempt 元数据；既有
`model_error_detail` 仍保留原始诊断，宿主不应直接向终端输出它。
记录先完整构造，再受检查地转移到 state，记录失败不能报告成功。

选择在默认单次 transport 旁接入现有记录器，避免为展示错误码而切换到另一套
重试机制。默认 HTTP 保留 60 秒请求上限，并遵循更早的执行取消/deadline；保持
请求线程生命周期和单次 POST，即使配置 `max_attempts > 1` 也不隐式重放。
自定义 v1 callback 保持同步调用契约；
显式 v2 adapter 继续拥有 retryable 的判定。无需新增公开 API 或迁移存储。

CLI 在失败退出前，通过局部 tlog logger 向原 diagnostics stream 输出已知阶段、
运行/HTTP/传输状态和固定提示，不安装全局 logger，也不输出 provider 文本或
任意自定义阶段。原失败行、stdout、退出码和中断行为保留。原失败行在 logger
创建失败时仍有效；详细日志属于诊断增强。验证覆盖 HTTP 错误分类、正文与控制
字符隔离、JSON 解析失败、无效地址、旧 callback 和第二轮失败。

## 状态迁移与失败语义

```text
idle -> submitting -> queued -> running -> terminal -> idle
          |             |
          +-- error ----+-- queue full -> TURBO_EBUSY
```

- submit 失败时，不发布 execution handle；输入仍由调用方拥有。
- submit 成功后，caller 必须 `turbo_agent_harness_execution_release()`。
- `take_result()` 在终态前返回 `TURBO_EBUSY`，成功后转移 summary/state 所有权，再次调用返回 `TURBO_EALREADY`。
- runtime 成功但 Session summary capture 或 inbox follow-up 失败时，execution 状态为 `FAILED`，底层 result code 保留错误；不把不一致状态报告为成功。
- completion hook 使用同一 cancel token 运行 inbox follow-up，因此 deadline/cancel 不会在后续轮次失效。

## 能力握手

`turbo_agent_harness_get_startup_diagnostics()` 保留 App 诊断，并增加 `harness` capability 对象，包括异步执行、start/resume/fork、cancel、deadline、event sink、并发上限与 executor queue capacity。宿主应在首次 submit 前要求诊断中的 `ok=true`。

## 使用示例

```c
#include "turbo_agent_harness.h"

#include <stdlib.h>
#include <string.h>

enum { HARNESS_DEADLINE_MS = 30000 };

static int example_transport(const char *request_json, char **out_response_json,
                             void *user_data) {
  const char *response =
      "{\"id\":\"resp_example\",\"output\":[{\"type\":\"message\","
      "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
      "\"text\":\"ok\"}]}]}";
  size_t size = strlen(response) + 1;
  (void)request_json;
  (void)user_data;
  if (!out_response_json) return TURBO_EINVAL;
  *out_response_json = (char *)malloc(size);
  if (!*out_response_json) return TURBO_ENOMEM;
  memcpy(*out_response_json, response, size);
  return TURBO_OK;
}

int main(void) {
turbo_threadpool_config_t pool_config = {
    .num_threads = 2,
    .queue_capacity = 16,
};
turbo_threadpool_t *pool = turbo_threadpool_create_with_config(&pool_config);

turbo_agent_session_config_t session_config = {0};
session_config.runtime_store = turbo_agent_runtime_store_memory_create();
session_config.agent_config.model = "gpt-5.4";
session_config.agent_config.transport_fn = example_transport;
session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;

turbo_agent_app_config_t app_config = {
    .session_config = &session_config,
};
turbo_agent_harness_config_t harness_config;
turbo_agent_harness_config_init(&harness_config);
harness_config.app_config = &app_config;
harness_config.executor = pool;

turbo_agent_harness_t *harness = turbo_agent_harness_create(&harness_config);
turbo_agent_harness_execution_t *execution = NULL;
turbo_agent_harness_run_options_t run_options;
turbo_agent_harness_run_options_init(&run_options);
run_options.deadline_mono_ms = turbo_monotonic_ms() + HARNESS_DEADLINE_MS;

int rc = turbo_agent_harness_start_text(harness, "inspect the repository",
                                        &run_options, &execution);
if (rc == TURBO_OK) {
  rc = turbo_agent_harness_execution_wait(execution, UINT64_MAX);
}
if (rc == TURBO_OK) {
  json_value_t *summary = NULL;
  json_value_t *state = NULL;
  rc = turbo_agent_harness_execution_take_result(execution, &summary, &state);
  turbo_runtime_json_destroy(summary);
  turbo_runtime_json_destroy(state);
}

turbo_agent_harness_execution_release(execution);
turbo_agent_harness_release(harness);
turbo_threadpool_destroy(pool);
return rc == TURBO_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
```

实际应用还应按需要初始化 tool registry、policy、WasmToolPack 与持久化 runtime store。

## 兼容性、迁移与回滚

这是新增 ABI v1，不修改原有 App/Session/Execution 函数签名与同步行为。已有调用方无需迁移；新宿主可以逐个入口替换为 Harness。

若不使用异步适配器，可直接链接 `TurboAgent::Agent` 并调用原 App/Session API。若回滚模块拆分，恢复先前完整 SDK 及匹配的宿主二进制。Runtime Store 数据格式没有变化，无需数据迁移。

## 验证范围

- capability 与 startup diagnostics
- 异步 start、事件 sink、结果所有权与 Session 最近 run 同步
- 同一 Harness 并发提交拒绝
- cooperative cancel 与 resumable terminal summary
- review workflow 的 resume/fork command 语义
- deadline 传播
- Execution、App、Session 相邻回归与完整 CTest
