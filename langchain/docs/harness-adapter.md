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

构建目标为 `TurboAgent::Harness`。它是链接到 `TurboAgent::LangChain` 的 interface target，不引入第二个动态库或重复状态 owner。

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

若需回滚，只需停止消费 `TurboAgent::Harness` 与 `turbo_agent_harness.h`，继续链接 `TurboAgent::LangChain` 并调用原 App/Session API。Runtime Store 数据格式没有变化，无需数据迁移。

## 验证范围

- capability 与 startup diagnostics
- 异步 start、事件 sink、结果所有权与 Session 最近 run 同步
- 同一 Harness 并发提交拒绝
- cooperative cancel 与 resumable terminal summary
- review workflow 的 resume/fork command 语义
- deadline 传播
- Execution、App、Session 相邻回归与完整 CTest
