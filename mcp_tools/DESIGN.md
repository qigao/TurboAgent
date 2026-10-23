# MCP ToolPack 设计

## 背景与决策

TurboAgent 已有 `RuntimeTools` registry 和 `WasmToolPack`，但 TurboWasm 的 HTTP host
import 是低层、受限的请求能力：guest 不能注入 `Authorization`，不能跟随重定向，也不负责
MCP 工具发现、分页、JSON-RPC 校验或 SSE 消息恢复。把 OAuth token 或完整 MCP 状态机放进
WASM guest 会让凭据越过沙箱边界，并让每个工具重复实现协议。

因此采用宿主适配器：

```text
Agent / Workspace
        |
RuntimeTools composite registry
     /                    \
WasmToolPack           McpToolPack
     |                    |
TurboWasm policy       MCP 2026-07-28 + TurboHTTP
```

`McpToolPack` 是 MCP 工具目录的主事实源；registry 只是从目录生成的执行视图。`refresh()`
先在候选 registry 中完成所有分页、校验和注册，成功后一次性交换。失败时旧 registry 不变。

## 候选方案

- WASM guest 直接实现 MCP：隔离强，但会向 guest 暴露凭据，并重复实现协议；不采用。
- 把 MCP 塞进 `RuntimeTools`：调用方便，但迫使核心 registry 依赖网络；不采用。
- 独立宿主 `McpToolPack`：网络、认证和协议留在适配层，registry 与 WASM 保持独立；采用。

## 协议与错误语义

当前实现仅支持 MCP `2026-07-28` Streamable HTTP：每个 JSON-RPC 请求独立 POST，包含
`MCP-Protocol-Version`、`Mcp-Method`，`tools/call` 还包含安全编码的 `Mcp-Name`。
响应可以是 `application/json` 或请求范围内的 `text/event-stream`。旧版
initialize/session/GET-SSE 不会被静默尝试；需要旧协议时应新增显式版本适配器。

工具发现支持 `nextCursor` 分页。`x-mcp-header` 只接受由根 schema 的 `properties` 链静态
可达的 `string`、`integer`、`boolean`。不安全 UTF-8、首尾空白及 sentinel 冲突按规范使用
`=?base64?...?=`。无效注解只剔除对应工具并增加 rejected 计数；传输、JSON-RPC、分页或
目录冲突会使整个刷新失败。

协议字符串在进入 C 字符串 API 前校验完整解码长度。`resultType`、工具名及分页 cursor
包含内嵌 NUL 时刷新失败且旧 registry 保持不变，不会请求截断后的 cursor；header 名称、
header 类型或 schema 属性键包含 NUL 时，仅剔除对应工具并增加 rejected 计数；可选
description 非法时使用既有默认描述。这样 JSON 表示中的不同身份不会折叠为同一 C 名称、
参数路径或分页位置。

资源均有硬上限：工具数、页数、请求体、响应体、header 数和 header 字节数。默认
TurboHTTP 客户端禁用重定向并校验证书。自定义 transport 是借用依赖，响应由其可选
`release` 回调回收。

工具通过现有 RuntimeTools v4 同时注册文本和 JSON 入口、带上下文回调及 legacy 包装。
Agent ToolNode 的文本执行入口与直接 JSON 调用共享同一 MCP 实现，不再因缺少文本回调
而返回普通错误。带上下文调用保留 `INVALID_ARGUMENT`、`OUTPUT_LIMIT` 等类型化状态；
legacy 回调维持原有零/非零错误约定。注册的 capability、执行策略（默认 EXCLUSIVE）、idempotency
及 UNKNOWN effect 元数据不变。

带上下文回调在入口及调用 MCP 协议层前检查取消 token 和绝对 deadline；已停止调用返回
`CANCELLED` / `DEADLINE_EXCEEDED`，不发出远端请求。检查是协作边界，不是原子租约：
已经进入 transport 的请求仍使用原有超时/阻塞契约，不承诺中途打断或将其超时收紧至
Host deadline。

`tools/call` 一旦进入 transport，协议层未能取得可用结果的失败统一保守返回
`UNKNOWN_SIDE_EFFECT`，而不是普通失败或响应超限。覆盖 transport 非零返回、HTTP 错误、
响应超限、无效 JSON、错 ID、不支持的 content type、没有匹配结果的 SSE、JSON-RPC error
及缺失 result。现有 transport 的整数返回值不包含“未发送/没有效果”的证明，HTTP 或
JSON-RPC 错误也不在本适配层建立无效果保证；因此即使实际上未发送，也可能得到 UNKNOWN。
保留原始诊断，成功回调的响应仍恰好释放一次；没有更改 transport 失败时的资源契约。
JSON-RPC 版本按完整解码长度校验，不能以 `2.0` 后的内嵌 NUL 截断非法后缀；该类畸形
响应不能提交成功工具结果或替换发现目录。

本地准备/请求大小限制在 transport 前失败时仍返回原类型化状态；`tools/list` 的错误和
刷新原子性不变。有效结果保持原有 JSON 载荷行为。legacy 整数回调仍将非成功映射为
普通错误，不能承载 UNKNOWN 分类；Agent 使用带上下文的 v4 入口。

Journal 是唯一持久事实源：正常提交保存 UNKNOWN 状态，重放返回原状态而不再次发请求；
提交失败仍保留 started 并重放 UNKNOWN。晚到取消/到期不能把不确定效果改成普通停止。
没有重解释已有普通失败记录、迁移 schema、自动重试或恢复远端效果；更广的后台任务、
真实网络、进程崩溃和跨 backend 验收仍待完成。

离线 Agent/MCP 回归通过真实 ToolNode、Journal 和假 transport 验证双 capability admission、
`started` 持久化期间停止、committed 重放、失败提交保留 UNKNOWN，以及执行后取消/到期
在结果交付与重放阶段拦截成功载荷。故障矩阵还验证协议失败的 UNKNOWN、响应释放、
当前权限拒绝 UNKNOWN 重放、发送前限制的已知状态，以及协议/目录字符串不被 NUL 截断。
没有真实网络、进程或付费模型调用。

## Policy、所有权与并发

Pack 拥有 MCP client、默认 TurboHTTP client、当前 registry 和每个远端工具 binding。
registry 必须先于 pack 的网络对象销毁。registry 在下一次成功 `refresh()` 时失效；Agent、
workspace selection 或 composite registry 必须在刷新前停止使用旧视图。

客户端保存递增 request id 和最近诊断，因此不声明 `PARALLEL_SAFE`；默认工具执行模式为
`EXCLUSIVE`。Workspace 对 MCP 工具应为同一 tool name 配置两条 capability 映射：

```c
turbo_agent_workspace_tool_capability_t required[] = {
    {"mcp_docs_search", TURBO_AGENT_POLICY_CAPABILITY_RUNTIME_TOOLS},
    {"mcp_docs_search", TURBO_AGENT_POLICY_CAPABILITY_NETWORK},
};
```

同名不同 capability 采用 AND 语义。WASM 工具若启用 TurboWasm HTTP capability，也应采用
相同的双 capability 映射；不联网的 WASM 工具只需 `runtime_tools`。

多个常备工具源通过 `turbo_tool_registry_compose()` 组成借用 registry。所有源 pack 必须
比 composite、workspace selection 和 Agent 活得更久，且使用期间不得 refresh 或增删工具。

## 迁移、回滚与验证

新增 target 是 `TurboAgent::McpTools`，对已有 target 和公开行为没有默认影响。接入方先
创建/刷新 pack，再选择单独使用其 registry，或与 WasmToolPack registry compose。回滚只需
停止链接该 target 并移除 `mcp_tools` 子目录，不涉及数据格式迁移。

验证范围包括：JSON 与 SSE 响应、分页、必需 metadata/header、Base64 参数镜像、不合规
schema 剔除、失败刷新原子性、registry composition 冲突，以及 workspace 多 capability
AND 语义。真实 OAuth 获取/刷新由宿主或自定义 transport 负责，不属于本模块。

协议依据：

- <https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http>
- <https://modelcontextprotocol.io/specification/2026-07-28/basic/authorization>
- <https://modelcontextprotocol.io/specification/2026-07-28/server/tools>
