# TurboAgent.Native

TurboAgent 的 native SDK package，由 `qigao/TurboAgent` 自己构建、验证和发布。

TurboAgent.Native **不发布 NuGet dependency metadata**，因此 package 本身不保留
Salts / SaltsUtils / CHttp / TurboWasm / Praktor 的任何版本号。发布 CI 与仓库 CI 从
[`native-sdk-versions.json`](../../cmake/ci/native-sdk-versions.json) 选择发布包，
再通过正式构建和 CTest 验证兼容性。该清单固定本次指定的六个版本；未纳入本次升级的
TurboScript / Praktor 保持显式 `latest` 策略。缺失清单项或指定发布包时直接失败。

TurboAgent 自身的第三方 C/C++ 依赖（sqlite）仍由项目的
vcpkg manifest/toolchain 解析，也不写入 Native package 的版本依赖元数据。

## CI 与发布构建入口

仓库 CI 和 tag 发布共同调用
[`native-sdk-build.yml`](../../.github/workflows/native-sdk-build.yml)，
执行版本解析、四个平台的完整构建与安装，以及同一轮工件的 NuGet 打包。
PR、main 和手动 CI 只生成工件；`native-sdk-release.yml` 仅在 `v*` tag push
时发布包和 GitHub Release，写权限限定在 publish job。

此前两条流程分别维护 CMake 参数、SDK restore 和安装检查，Windows 还绕过
manifest。现在平台配置的事实源为
[`CMakeUserPresets.json`](../../CMakeUserPresets.json)：

| Configure / build preset | SDK | 正式测试 | 可选后端 |
| --- | --- | --- | --- |
| `ci-linux-release` | linux-x64 | CTest 全量 | Wasm、Praktor |
| `ci-windows-release` | windows-x64 | CTest 全量 | Wasm |
| `ci-macos-release` | macos-arm64 | 保持关闭 | Wasm、Praktor |
| `ci-android-release` | android-arm64-v8a | 无设备 runner，关闭 | Wasm、Praktor；关闭 CodexBridge |

每个 profile 都通过根 `vcpkg.json` 配置完整 build graph；安装使用
`install-<preset>` build preset，产物进入 `build/sdk/<rid>`。Windows/Linux
测试使用同名 test preset，真实模型测试保持显式 opt-in。
macOS 延续原发布流程的 build/install 覆盖，不将它描述为测试通过。
普通 CI 因此也覆盖 macOS 和 NuGet 打包；Linux 的 Praktor 配置覆盖完整 graph，
Android 与发布配置统一启用 Wasm/Praktor，代价是 CI 构建量增加。

SDK restore 脚本提供当前 profile 的 `SALTS_ROOT`、`SALTS_UTILS_ROOT`、
`CHTTP_ROOT`、`TURBOWASM_ROOT`，启用 Praktor 时还提供 `TURBOSCRIPT_ROOT`、
`PRAKTOR_ROOT`。第一方 package 从这些根精确查找；`CMAKE_PREFIX_PATH` 只含
该 profile 的 vcpkg 前缀。Android 沿用同一发布包中的 Linux host IDLC 和 Salts
runtime，宿主路径由 `TURBOAGENT_HOST_IDLC`、`TURBOAGENT_HOST_SALTS_ROOT`
传入，NDK 通过外层 vcpkg toolchain chainload。

共享 cache setup 提供工具链、只读远端缓存和可写本地缓存；Windows CI 的
target/host triplet 从其 `QIGAO_VCPKG_WINDOWS_TRIPLET` 和
`QIGAO_VCPKG_WINDOWS_HOST_TRIPLET` 接收，路径也由 preset 从 triplet 派生。
本地复现 CI preset 时需显式提供这些父环境、SDK 根和 `RE2C_ROOT`，Windows
先进入匹配的 `VsDevCmd.bat`。开发机仍可使用原来的 `win-release-user` 等入口。

保留既有独立工作流会继续产生配置漂移；只抽取参数仍会重复维护构建和打包
顺序，因此选择共享 workflow 与 presets。行为验证归正式 CTest，移除临时
consumer 工程和 CMake 导出文本断言。迁移不改变 SDK 格式；回滚此构建编排
提交即可恢复先前流程，Agent 重构单独提交。本地和远端的验证范围分别记录，
本地通过不代表所有 runner 已通过。

本轮本地验证：`ci-windows-release` configure、全量 build、同名 CTest 和
`install-ci-windows-release` 均通过；95 项离线测试通过，1 项 live 默认跳过
（10.99 秒）。使用本机 MSVC 与 `x64-windows` target/host triplet、清单选定
SDK；未执行托管 runner 的 `qigao-x64-windows-v145` 配置。
三个 workflow 的 `actionlint` 通过。Linux、macOS、Android、远端 Actions、
完整四平台 NuGet 打包与真实模型调用尚未在本轮执行。

## SDK platforms

- `sdk/linux-x64`
- `sdk/windows-x64`
- `sdk/macos-arm64`
- `sdk/android-arm64-v8a`

TurboWasm-backed `TurboAgent::WasmTools` 当前在四个平台的 Native SDK 中都发布。
因此使用当前完整 TurboAgent package 执行 `find_package(TurboAgent CONFIG REQUIRED)`
时，需要让清单选定、对应平台的 TurboWasm SDK 可发现。

Praktor 当前不发布 Windows SDK，因此：

- Linux / macOS / Android SDK 包含 `TurboAgent::PraktorTools`
- Windows SDK 不包含 `TurboAgent::PraktorTools`
- 其它 TurboAgent runtime/tool/harness modules 仍可在 Windows 使用

## Consume

按版本清单还原 native SDK graph 后，为目标平台设置环境变量：

- `SALTS_ROOT`
- `SALTS_UTILS_ROOT`
- `CHTTP_ROOT`
- `TURBOWASM_ROOT`
- Linux/macOS/Android 如需 PraktorTools，再设置 `PRAKTOR_ROOT`、`TURBOSCRIPT_ROOT`

第一方根目录只在对应 user preset 的环境中定义，不能用旧的 `-D*_ROOT` cache 参数
覆盖。`CMAKE_PREFIX_PATH` 只用于当前 vcpkg profile；缺少根目录时不能退回系统 SDK。

然后使用 vcpkg toolchain 配置消费者：

```cmake
find_package(TurboAgent CONFIG REQUIRED)

target_link_libraries(my_app PRIVATE
  TurboAgent::RuntimeCore
  TurboAgent::RuntimeData
  TurboAgent::RuntimeTools
  TurboAgent::Agent)
```

TurboWasm sandbox tools：

```cmake
target_link_libraries(my_app PRIVATE TurboAgent::WasmTools)
```

`WasmTools` 通过 installed `TurboWasm::Runtime` target 链接；TurboAgent 不 vendor
TurboWasm，也不通过 source checkout 或另一个发布版本兜底。

Praktor workflow tools：

```cmake
target_link_libraries(my_app PRIVATE TurboAgent::PraktorTools)
```

TurboAgent harness-native Praktor WorkflowPlan 集成包括：

- immutable reviewed WorkflowPlan identity
- generated workflow schema
- conservative effect → policy capability mapping
- workflow-config ABI v3 的 borrowed `approved_host_tools` projection
- Praktor HostTool ABI 2.5 plan-wide preflight
- resolved HostTool arguments 的 RuntimeTools canonical schema validation
- HostTool capability union；非 HostTool unknown effect 继续 fail-closed
- cancellation/deadline + thread/run/turn/tool-call lineage
- workflow/task event stream
- full-detail durable journal path
- compact `agent_output` for model context

HostTool execution始终通过 approved RuntimeTools projection；Praktor 不持有完整
registry，也不知道实际 backend 是 Native、WasmToolPack/TurboWasm 或 MCP。
PraktorTools 需要 latest released Praktor SDK 提供 ABI 2.5+
`PRAKTOR_CAPABILITY_HOST_TOOL`，不存在旧 SDK/source/SHA fallback。

PR 只做 qualification。正式 package / GitHub Release 只由与项目版本一致的
`v*` tag 发布。

## 2026-10-10 SDK migration

| SDK | Selected release |
| --- | --- |
| Salts | [2.3.0-rc.4](https://github.com/qigao/salts/releases/tag/v2.3.0-rc.4) |
| SaltsUtils | [4.3.0-rc.2](https://github.com/qigao/salts-utils/releases/tag/v4.3.0-rc.2) |
| TurboWasm | [0.2.0](https://github.com/qigao/turbowasm/releases/tag/v0.2.0) |
| TurboDB | [2.3.2](https://github.com/qigao/turbodb/releases/tag/v2.3.2) |
| SaltsNet | [1.1.0-rc.1](https://github.com/qigao/salts-net/releases/tag/v1.1.0-rc.1) |
| CHttp | [2.1.0-rc.1](https://github.com/qigao/chttp/releases/tag/v2.1.0-rc.1) |

Windows 本次将六个 NuGet 发布包的 `sdk/windows-x64/` 内容恢复到独立的
`C:/projects/cpp/external/pkgs/turboagent-sdk-20261010/<package>/release/`，
并核对 GitHub 发布资产的 SHA-256。包目录名为 `salts`、`salts-utils`、`turbowasm`、
`turbodb`、`salts-net`、`chttp`；原有共享 SDK 安装保留。TurboAgent 当前直接消费
Salts、SaltsUtils、CHttp 和 TurboWasm；TurboDB、SaltsNet 记录于清单并已恢复，
没有为本次升级新增不需要的链接依赖或声称其运行时已由 TurboAgent 测试覆盖。

线程、线程池、时钟、文件和 UUID 调用迁移到 SDK 的 `cmeta_*` 名称，文件与 UUID
头文件改为 `cmeta_fs.h`、`cmeta_uuid.h`。Execution / Harness 的公开 executor 类型
同步为 `cmeta_threadpool_t *`，保留借用关系、容量、取消和 drain 语义。消费者必须
使用整组 SDK 重新编译链接，不能混用旧头文件和 DLL。示例中的旧 `turbo_json_*`
调用改用当前 `json_*` API，释放后仍清空持有指针。

SaltsUtils 的 Unicode 能力由 Salts 唯一提供。TurboAgent 使用的 CHttp Client/Server
和 TurboWasm Runtime 接口经本次构建与测试可继续使用，没有引入旧接口兼容层。
回滚时一起恢复先前源码、preset 和完整 SDK 集，再重新构建；不要把旧 DLL 覆盖进
本次独立安装目录。

在 Visual Studio x64 Developer Command Prompt 中执行：

```bat
cmake --preset win-release-user
cmake --build --preset win-release-user --parallel 8
ctest --preset win-release-user --output-on-failure --timeout 120
cmake --build --preset install-win-release-user
```

配置前按共享 vcpkg 缓存约定提供只读包认证。`win-release-user` 启用正式测试和
现有示例，关闭 benchmark。2026-10-10 本机 Windows x64 Release 全量构建和安装通过，
64/64 CTest 通过（18.98 秒），包含 CHttp 远程调用和 TurboWasm 执行测试。
该发布包集合仅恢复 Release profile；Debug/ASan 需要独立的匹配 Debug SDK，
不能复用 Release 安装目录。本次未执行 Linux、macOS、Android、Praktor 或远端 CI。

## 2026-10-10 first module separation

以下各阶段是历史验证记录；当前构建入口与兼容层删除说明见 [LangChain removal](#langchain-removal)。

Agent 内核、通用 graph/event Runtime、Harness Server/Transport 已分别导出为
`TurboAgent::Agent`、`TurboAgent::Runtime`、`TurboAgent::Harness`；LangChain 仅保留
可关闭的兼容封装。源码、测试与 fixture 随各自归属迁移，接口头文件名不变。
新动态库布局要求消费者重新链接并部署完整 SDK。Provider 与嵌入式 Harness 的
剩余内部依赖见 [模块拆分记录](../../agent/docs/module-separation.md)。

journal/context SHA-256 改用 `cmeta_crypto.h`，移除直接 OpenSSL/BoringSSL 依赖。
固定 journal 哈希测试在旧、新实现上均通过；哈希字段分隔、数据格式和重放语义不变。

Windows x64 Release 全量构建（含示例）、64/64 CTest 与安装通过。
额外以 `cmake --preset win-release-user -DENABLE_LANGCHAIN_COMPAT=OFF` 配置，
全量构建及 62/62 非兼容层测试通过；最终恢复 `ENABLE_LANGCHAIN_COMPAT=ON`。
DLL 依赖检查确认 Runtime 不依赖 Agent/CHttp，Agent 与 Harness 不依赖 LangChain，
Agent 不再直接依赖 BoringSSL。安装导出包含上述四个独立 target。
跨平台、Debug/ASan 与远端 CI 仍未执行。

## Provider separation qualification

`TurboAgent::Provider` 已独立安装导出，包含请求 wire 编码、prompt、response/SSE
转换和 embedding helpers。Agent 通过借用的请求投影调用 Provider；既有自定义
provider callbacks、context/历史和工具权限继续由 Agent 管理。新增公开入口及
所有权说明见 [Provider](../../provider/README.md)。

Windows x64 Release 全量构建和安装通过，65/65 CTest 通过；其中 9 项相关回归
覆盖独立 Provider、旧 model-provider API、SSE、prompt、embedding、请求构造、
context 和 workflow。独立测试额外覆盖协议字段差异、借用输入生命周期、无效
schema/format 的失败输出，以及无 Agent/transport 的响应解码。
DLL 检查确认 Provider 不依赖 Agent、LangChain 或 CHttp；其安装 target 只公开
链接 Runtime。此次未扩大跨平台、Debug/ASan 或线上服务验证范围。

## Embedded Harness separation qualification

嵌入式 Harness 的实现、公开头文件和测试已迁入 `harness/`，统一由
`TurboAgent::Harness` 提供。Agent 新增版本化的 `turbo_agent_session_submit()`
公开接口，内部完成 start/resume/fork 的选项转换、摘要同步、inbox 提交和后续轮次；
Harness 不再包含或调用 Agent 私有接口。接口所有权与回调时序见
[Session execution header](../../agent/include/turbo_agent_session_execution.h)。

Windows x64 Release 全量构建、66/66 CTest 与安装通过。新测试验证输入复制、
终态发布先于通知结束、队列满/关闭/无效配置拒绝时不通知、inbox 提交失败传播，
以及调用方提前释放 Harness/执行句柄后的 worker 保活与清理。
DLL imports 验证 Harness 调用公开 Session submit，未导入旧私有提交接口。
消费者应链接 Harness 并部署更新的完整 SDK；跨平台和 Debug/ASan 仍未执行。

## StateGraph separation qualification

StateGraph 的实现、内存/文件快照存储、四个公开头文件、正式测试和两个示例迁入
`runtime/`，由 `TurboAgent::Runtime` 提供。JSON 字段复制由 RuntimeData 统一处理，
Agent 的原有私有 helper 保留错误返回约定并委托调用。快照格式与恢复/分支语义不变。
durable Agent runtime 中的命令、策略和状态投影仍留在 Agent，后续边界见
[模块拆分记录](../../agent/docs/module-separation.md)。

Windows x64 Release 全量构建（含示例）和安装通过；4/4 相关 CTest、66/66 全量
CTest 通过（全量 5.99 秒）。RuntimeData 新增两组用例覆盖借用值的独立复制、
精确整数替换，以及失败时保留输入。DLL 检查确认 Runtime 和独立 StateGraph 测试
均不依赖 Agent、Provider、Harness 或 CHttp。跨平台和 Debug/ASan 仍未执行。

## Durable record-store separation qualification

Runtime 通过 `turbo_runtime_store.h` 提供原有 runtime 句柄、后端回调、内存/文件
存储和 JSON 记录操作；公开函数与类型名保留。Agent 的 Context、Inbox、ToolExecutor
仅通过该接口读写记录。句柄布局与互斥锁归 Runtime，Agent 保留执行编排、命令、
状态投影与记录 schema；没有增加第二个存储 owner 或改变持久化格式。
所有权、并发与回滚约定见 [模块拆分记录](../../agent/docs/module-separation.md)。

五份未参与编译、也未被包含的旧 runtime shard 源码已移除。新增独立 Runtime
存储测试覆盖回调表复制、创建失败/成功的所有权、JSON 独立副本、过滤、错误输出、
并发回调串行化、旧路径中的固定 JSON 记录重开，以及损坏记录导致查询失败。
两个后端的 list 路径现在检查数组插入失败，不把部分结果报告为成功。

Windows x64 Release 全量构建（含示例）、安装通过；6/6 相关 CTest 和最终
67/67 全量 CTest 通过（全量 7.51 秒）。DLL 检查确认 Runtime 与独立存储测试
不依赖 Agent/Provider/Harness/CHttp，Agent 从 Runtime 导入记录操作和句柄生命周期。
未进行分配失败注入、跨平台、Debug/ASan 或远端 CI 验证。

## Durable segment separation qualification

`turbo_runtime_segment.h` 提供同步执行与持久化接口。Runtime 负责图执行、事件和
checkpoint 捕获、记录构造及写入；Agent 通过显式回调提供控制/工作流快照、命令
摘要和执行上下文作用域。Agent 的 start/resume/fork 复用这一入口，原有记录格式、
回调顺序、取消边界及工具审批上下文重置保持不变。checkpoint 选择/加载、命令
解释与历史查询的后续拆分见 [模块拆分记录](../../agent/docs/module-separation.md)。

新增独立 Runtime 测试包含 10 个用例，覆盖中断恢复与分支、投影和事件捕获、
取消/超时、节点错误诊断、配置拒绝、作用域清理，以及 thread/checkpoint/run/
summary 各阶段失败。验证明确保留已有部分提交语义：后续操作失败不回滚此前写入。
记录字段写入现在检查分配失败；超出 JSON 精确整数范围的序号和过长时间戳明确拒绝。

Windows x64 Release 全量构建（含示例）、安装通过，68/68 CTest 通过（7.65 秒）。
DLL 检查确认 Runtime 和独立 segment 测试不依赖 Agent/Provider/Harness/CHttp；
Agent 从 Runtime 导入 segment、记录 ID/时间戳和结果序列化接口。未执行分配失败
注入、跨平台、Debug/ASan 或远端 CI。

## Checkpoint/history separation qualification

Runtime 新增 `turbo_runtime_history.h`，提供通用记录和状态查询、latest/pending/head
选择、checkpoint 加载/排序、父链导航和原始事件历史。Agent 原头文件转发这些声明，
resume/fork 和检查视图复用下层接口；命令解释、Agent trace 和展示投影留在 Agent。
新旧 run 状态读取、时间戳相同时的后端顺序、pending 优先规则均保留。

父链查询共用循环检测，损坏或缺失链路返回失败且不交付部分历史。checkpoint
序号、版本和步数在整数转换前校验；分数、负值及超出表示范围的值明确失败。
新增独立 Runtime 的 7 个用例覆盖固定 schema-v3 记录、状态覆盖的独立所有权、
旧记录 fallback、查询顺序、循环/缺失父节点和非法数值。有效记录格式不变。

Windows x64 Release 全量构建（含示例）和安装通过；5/5 相邻回归、独立历史测试
及最终 69/69 CTest 通过（全量 8.36 秒）。DLL 检查确认 Runtime 和独立历史测试
不依赖 Agent/Provider/Harness/CHttp；Agent 从 Runtime 导入 checkpoint 和记录查询。
尚未执行分配失败注入、跨平台、Debug/ASan 或远端 CI。完整当前树的兼容层开关
资格验证和安装导出审计结果见下节。

## Final module migration qualification (before facade removal)

删除兼容层之前，当前模块拆分在 Windows x64 Release 上完成验收：兼容层关闭时
67/67 CTest 通过（18.24 秒）；开启时 69/69 通过（19.75 秒）。两组全量构建和
安装均通过。这是历史结果，旧兼容层开关已删除，当前验证入口见下节。

## LangChain removal

按明确移除要求，删除 `TurboAgent::LangChain`、`turbo_langchain.h` 和
`turbo_langchain_*` 包装函数，撤销兼容层开关。此阶段曾临时使用 `ENABLE_AGENT`，
现已按仓库定位一并删除；当前核心构建规则见 [Mandatory agent build](#mandatory-agent-build)。
原公共 API 测试迁至 Harness，直接包含各模块头文件，仅移除已删除包装符号的断言；
纯兼容层行为测试随包装实现删除。Agent/Runtime/Provider/Harness 及其持久化格式不变。

消费者应链接所属模块并使用其公开头文件；旧 Agent facade 对应 App API，而不是
简单把所有符号的 LangChain 前缀替换为 Agent。具体说明见
[迁移与回滚](../../agent/docs/module-separation.md#compatibility-migration-and-rollback)。
原有安装目录中的 `include/turbo_langchain.h`、`bin/langchain.dll` 和
`lib/langchain.lib` 必须清除，以免继续消费过期入口；CMake install 不会自动删除旧文件。

在 Visual Studio x64 开发环境中复用正式 preset：

```powershell
cmake --preset win-release-user -UENABLE_LANGCHAIN*
cmake --build --preset win-release-user --parallel 8
ctest --preset win-release-user --output-on-failure --timeout 120
cmake --build --preset install-win-release-user
```

`-U` 仅清除旧构建树中的废弃缓存项；后续配置直接使用 `cmake --preset win-release-user`。

本机 Windows x64 Release 完整构建（含示例）和安装通过，迁移后的公共 API 测试
单独通过，全量 68/68 CTest 通过（17.95 秒）。额外以 `ENABLE_AGENT=OFF` 配置，
完整构建和剩余 18/18 CTest 通过（1.29 秒），覆盖 compiler/tools 独立构建。
最终已恢复 `ENABLE_AGENT=ON`，再次完成全量构建与安装。
本次 Release 构建目录及 SDK 安装目录中的旧 LangChain 库、头文件和测试产物已清理；
安装导出不含 LangChain。源码与 CI 不再使用旧开关。未执行跨平台、Debug/ASan
或远端 CI；以上两个配置的验证均在本机 Windows 上完成。

## Mandatory agent build

仓库以构建 Agent 为核心目标。`ENABLE_AGENT` 已删除，Runtime、Provider、Agent
和 Harness 无条件加入构建；Chttp 和 SQLite 是核心及安装包的必需依赖。
可选工具后端仍保留各自开关。Praktor CI 不再关闭 Agent，并构建完整配置图。

复用旧构建树时执行一次 `cmake --preset win-release-user -UENABLE_AGENT` 清除废弃
缓存项；之后使用标准 configure/build/test/install presets，无需替代开关。
本机 Windows x64 Release 全量构建（含示例）、68/68 CTest（9.84 秒）和安装通过。
安装的包配置无条件声明 Chttp/SQLite 依赖。未执行远端 CI、Praktor、跨平台或
Debug/ASan 验证。

## Coding CLI qualification (2026-10-10)

`turboagent` 命令行入口随 SDK 安装到 `bin/`。CodingTools 通过 `salts-idlc`
生成原生参数 codec 和模型 schema，并提供 read/list/write/edit/shell 工具。
CLI 复用 Harness/Session/CHttp；用法和当前内存会话限制见
[CLI 文档](../../cli/README.md)。

本机验证使用 `win-release-user` configure/build/test preset 与
`install-win-release-user` install preset。完整构建（含示例）、71/71 CTest
和安装通过。正式测试覆盖真实文件修改、精确替换冲突、空字符串、超限、
Windows junction 拒绝、进程输出/超时/取消、部分注册回滚、CLI 可执行入口和
模拟模型驱动的多轮编码流程。未调用真实模型服务；未执行跨平台、Debug/ASan
或远端 CI。

随后 CLI 接入 SaltsUtils CmdParser 和 Agent Workspace：每轮加载根目录
`AGENTS.md`、刷新并选择 skills，复用现有能力校验与状态指令层。
同一 Windows Release preset 的完整构建、72/72 CTest（6.73 秒）和安装通过；
新增正式测试覆盖指令刷新与去重、技能重选、只读能力拒绝、缺失工具、格式错误、
文件超限和刷新失败时阻止模型调用。验证范围仍限于本机 Windows 与模拟模型。

Session 输入构造随后拆至独立实现，移除中间状态的重复整树克隆，并以无 Agent
依赖的完整历史投影替代续聊时的 Agent 浅拷贝。公开 API 与持久化格式不变。
Windows Release 完整构建、73/73 CTest（10.07 秒）及安装通过；新增测试覆盖
嵌套消息独立所有权、Session 销毁后的输入/记忆有效性、记忆加载失败、无效历史
拒绝，以及已压缩会话的完整续聊历史和原有压缩投影保持不变。未执行跨平台、
Debug/ASan、分配失败注入或真实性能测量。

Session 能力查询、工具 schema 和启动诊断随后拆至独立实现，通过临时只读视图
读取 Session 事实，保持原有报告字段、错误顺序与 provider 选择语义。
Windows Release 完整构建、74/74 CTest（5.80 秒）及安装通过；新增测试覆盖报告
独立所有权、Session/registry 销毁后的有效性、registry 更新后的新快照、provider
别名和多个诊断错误的顺序。未执行跨平台、Debug/ASan 或分配失败注入。

同步 start/resume/fork 的调度已归入 Session execution 实现，并与异步提交共用
摘要更新、inbox 提交和有界 follow-up 的完成入口，保留两类入口原有参数语义。
Windows Release 完整构建、74/74 CTest（5.78 秒）及安装通过；扩展现有 execution
测试覆盖省略 summary 时的状态更新、提交失败的输出所有权和阻止后续执行，
以及省略 summary 时的 follow-up 上限。未执行跨平台、Debug/ASan 或真实模型调用。

Supervisor 查询和 inbox override 准备已拆至独立 Session inspection 实现，复用
Runtime 返回的独立状态树，移除中间整树克隆，并检查报告字段的所有权转移。
Windows Release 完整构建、75/75 CTest（6.70 秒）及安装通过；新增测试覆盖报告
与子树独立所有权、Session 销毁后的有效性、override 不写回状态，以及缺失运行
记录时不返回部分结果。未执行跨平台、Debug/ASan、分配失败注入或性能测量。

Session memory 适配已拆至独立实现，保持 MemoryStore 的记录转换与 State 的层
构造归属，并检查 context payload 字段写入。Windows Release 完整构建、76/76
CTest（7.39 秒）及安装通过；新增测试覆盖 payload 格式与写入错误码、query
优先与失败不重试 list、加载顺序及独立所有权、后续记录失败时保留已有层与
输入工厂丢弃失败状态。未执行跨平台、Debug/ASan 或分配失败注入。

Session 的高层 start/invoke/batch 已拆至独立实现，合并执行与结果清理路径，
移除提取文本/JSON 时的整树克隆，并检查批量结果 JSON 的插入与所有权转移。
Windows Release 完整构建、77/77 CTest（8.26 秒）及安装通过；新增测试覆盖结果
独立所有权、JSON 转换失败、中断后无最终答案的清理，以及批量单项失败后继续
执行。未执行跨平台、Debug/ASan、分配失败注入或性能测量。

Session 子运行查询与检查报告已拆至独立实现，仅依赖公开 Session/State/Runtime
接口，移除 history/trace/timeline 的重复克隆，并检查报告 JSON 所有权转移。
Windows Release 完整构建、78/78 CTest（11.16 秒）及安装通过；新增 5 个用例覆盖
报告与查询结果独立所有权、Session 销毁后的有效性、可选字段、缺失 thread 的
时间线失败与空分支树、显式 checkpoint 优先且失败不重试 run，以及必需查询失败
时不返回部分报告。公开签名和数据格式不变。未执行跨平台、Debug/ASan、分配
失败注入或性能测量。

Session 的 Runtime 查询、历史回放/观察和 override 准备已拆至独立 query 实现，
统一历史来源选择，仅使用公开 Session/Runtime 接口；旧 state-patch 转发改用
行为相同的公开 prepare API。Windows Release 完整构建、79/79 CTest（7.67 秒）
及安装通过；新增 5 个用例覆盖显式/缓存来源优先级、新 run 与旧 checkpoint
并存、查询失败不重试、无来源拒绝、历史 sink 借用与回调副本生命周期，以及
默认查询和 override 独立所有权。公开签名与持久化格式不变。未执行跨平台、
Debug/ASan、分配失败注入或真实模型调用。

Session 实时 trace/observer 注册与事件桥接已拆至独立实现，通过公开 accessor
借用 Agent；预设 start/resume/fork 包装归入现有 execution 实现。Windows Release
完整构建、80/80 CTest（6.07 秒）及安装通过；新增 4 个用例覆盖注册描述符复制、
成功注册后恰好释放一次、拒绝注册不接管用户数据、独立事件副本、trace 历史留存
与实时投递分离，以及 transport 失败状态投递。保留现有字段、事件筛选和执行
语义。未执行跨平台、Debug/ASan、分配失败注入或真实模型调用。

Session 预设图工厂已拆至独立 workflow 实现，通过窄内部 const 入口借用 Session
内稳定的 context 配置；backend 指针和 limit 统一以 context 配置为事实源。
Windows Release 完整构建、81/81 CTest（5.83 秒）及安装通过；新增 4 个用例覆盖
调用方配置字符串释放后执行图节点、知识库过滤与 limit、retriever 的 scope 与
默认查询、外部 backend 所有权，以及缺少依赖和未知 preset 的拒绝。保留既有
图结构、公开签名与持久化格式。未执行跨平台、Debug/ASan、分配失败注入或
真实模型调用。

本地与远程 Session 的摘要 ID 更新已共用先复制、后提交的内部实现，修复分配
失败时旧值被释放、缓存部分更新却返回成功的问题。失败现在返回 -1 并保留该次
更新前的缓存；Runtime 可能已完成，既有输出所有权和持久化记录不回滚。
Windows Release 完整构建、82/82 CTest（6.02 秒）及安装通过；新增 4 个用例覆盖
摘要副本所有权、无效必需字段拒绝、checkpoint 保留规则和空字符串兼容性，
并通过本地/远程 Session 与 App 回归。分配失败分支已审查，未做故障注入；
未执行跨平台、Debug/ASan 或真实模型调用。

本地与远程 Session 构造现已检查所有 Session 自有字符串复制；失败清理部分对象，
不返回丢失线程或检索过滤配置的句柄。MemoryStore 仅在成功时接管，RuntimeStore
保留完整配置消费、缺失回调使用默认 store 且不消费调用方原 store 的既有约定。
远程启动在复制线程 ID 失败时不提交请求。保留 Salts `tstr_dup(NULL)` 返回空字符串
的实际行为。Windows Release 完整构建、83/83 CTest（5.95 秒）及安装通过；新增
6 个用例覆盖 store 释放次数、Agent 拒绝后的所有权、默认 store、配置字符串副本、
空值和远程客户端借用/拥有。复现使用 `win-release-user` configure/build/test
preset 和 `install-win-release-user` build preset；相关测试可通过 CTest 的
`test_turbo_agent_session_lifecycle`、`test_turbo_agent_session_workflow`、
`test_turbo_agent_session_diagnostics`、`test_turbo_agent_remote_session_api`、
`test_turbo_agent_session_api`、`test_turbo_agent_summary_ids` 筛选。
分配失败分支已审查，未做故障注入；未执行跨平台、Debug/ASan 或真实模型调用。

Agent 运行摘要、命令描述符/schema 构造和中断元数据选择已拆至独立 summary
实现。JSON 字段、数组和嵌套对象插入统一检查结果，失败清理未交付对象并返回
错误；保留字段、命令顺序和中断分类。摘要构造仍发生在 run 持久化之后，失败
不回滚已写记录。Windows Release 完整构建、84/84 CTest（8.83 秒）及安装通过；
新增 4 个用例覆盖五个父运行字段、返回树独立所有权、显式 null/空字符串、
六种状态组合的中断优先级，以及命令描述符与现有 fixture 的一致性。
复现使用 `win-release-user` configure/build/test preset 和
`install-win-release-user` build preset；相关 CTest 筛选为
`test_turbo_agent_(runtime_summary|runtime_api|session_execution|remote_session_api|session_api)$`。
分配失败清理已审查，未做故障注入；未执行跨平台、Debug/ASan 或真实模型调用。

远程 Session 的 text/message start 与四个 invoke 入口已拆至独立适配层，本地和
远程通过内部 result 实现共用最终文本/JSON 提取和失败清理，移除远程 JSON
提取前的整树克隆。保留远程必需 summary 输出、本地可省略 summary、文本
malloc/free ABI，以及转换失败不回滚 Session ID 或远程记录的语义。
Windows Release 完整构建、84/84 CTest（5.28 秒）及安装通过；现有远程测试目标
新增 4 个用例，覆盖销毁 Session/服务后的结果所有权、JSON 转换失败、中断
无最终答案、远程错误和必需输出参数。复现使用 `win-release-user`
configure/build/test preset 与 `install-win-release-user` build preset；相关
CTest 筛选为 `test_turbo_agent_(session_invoke|session_api|remote_session_api|session_lifecycle)$`。
未执行跨平台、Debug/ASan、分配失败注入、真实模型调用或性能测量。

本地 Session 与 RemoteClient 现共用独立 supervisor 报告构造器，远程报告复用
受检查的 JSON 所有权转移，移除状态和快照的重复克隆；远程 inbox/history 查询
只复制所选数组，并删除 RemoteSession 中两份无调用的旧报告构造实现。保留
查询流程、报告字段、显式 null/空数组和错误语义。Windows Release 完整构建、
84/84 CTest（5.16 秒）及安装通过；现有远程测试新增 3 个用例，覆盖本地/远程
handoff 投影一致性、返回子树独立所有权、owner 销毁后的有效性、空字段和
缺失 thread 查询失败。复现使用 `win-release-user` configure/build/test preset
及 `install-win-release-user` build preset；相关 CTest 筛选为
`test_turbo_agent_(session_inspect|remote_session_api|runtime_remote_client_api|session_api)$`。
未执行跨平台、Debug/ASan、分配失败注入、真实模型调用或性能测量。

子运行报告和父调用元数据现由 Session、RemoteSession、RemoteClient 共用内部
构造器；各适配层保留原查询顺序、checkpoint 优先级和可选观测字段策略。
结果通过受检查的 JSON 写入转移所有权，移除远程 history/trace/timeline 的
额外整树克隆；RemoteClient 编排与多 agent 外层报告也检查所有字段转移。
Windows Release 完整构建、84/84 CTest（6.48 秒）及安装通过。现有远程测试
新增 5 个用例，覆盖独立返回树与 owner 销毁、可选 null/空元数据、观测索引
不可用、显式 checkpoint 的 trace 选择，以及必需查询失败后的输出与缓存。
复现使用 `win-release-user` configure/build/test preset 和
`install-win-release-user` build preset；相关 CTest 筛选为
`test_turbo_agent_(session_child|remote_session_api|runtime_remote_client_api)`。
未执行跨平台、Debug/ASan、分配失败注入、真实模型调用或性能测量。

远程历史 replay/observer 已拆至 `turbo_agent_remote_session_observer.c`，通过
公开 RemoteSession 查询工作，复用现有 trace 类型映射；移除观察前的历史数组
克隆与终态记录的中间克隆。保留远程未知事件过滤、自定义 observer 类型、
紧凑 envelope、pending 优先和按原始 type 去重；修正公开注释对本地/远程
事件形状的描述，明确同步回调借用和 user_data 所有权。
Windows Release 完整构建、84/84 CTest（6.25 秒）及安装通过。现有远程测试
新增 5 个用例，覆盖事件顺序/过滤、回调副本的独立生命周期、原始历史回放、
interrupted/completed 终态、去重和无效入口。完成态默认无 checkpoint，测试
分别验证实际完成记录的终态合成与中断 checkpoint 的持久历史。
复现使用 `win-release-user` configure/build/test preset 和
`install-win-release-user` build preset；相关 CTest 筛选为
`test_turbo_agent_(remote_session_api|session_observer|runtime_api)$`。
未执行跨平台、Debug/ASan、分配失败注入、真实模型调用或性能测量。

新增显式启用的 `test_turbo_cli_live` CTest 集成测试，运行真实 CLI/模型/工具链，
在临时 Python fixture 中修复边界错误；先确认原始四项测试失败，再检查测试
文件未变、成功标记存在，并独立重跑测试。默认不联网，只有
`TURBOAGENT_LIVE_TESTS=1` 才运行；通过项目根目录 `.env` 与正常环境配置认证。
进程输出受限且不写入 CTest 日志，不复制凭据到 fixture。
认证更新后，真实调用暴露了默认 HTTP 客户端在控制线程初始化、在 Harness
工作线程使用时返回 `SALTS_EPERM` 的问题。默认模型 transport 现在在请求线程
初始化、停止和 drain 客户端；显式借用的客户端保持调用方 ownership。
Session 默认 provider 解析同时修复为遵循显式 `api_mode`。取舍及线程协议见
[Harness 适配层](../../agent/docs/harness-adapter.md#默认模型-http-客户端的线程归属)。

Windows Release 完整构建及安装通过；离线回归 85 项通过，1 项 live 测试跳过
（11.05 秒）。新增离线 `test_turbo_agent_http_owner` 覆盖两个 worker 与
创建线程顺序调用同一 Session，以及 401 后换 worker 恢复；测试使用本地
CHttp 服务，显式选择 Chat Completions，不需要外部模型或凭据。
真实 `http://localhost:64263/v1/chat/completions` 验证使用
`openai_compatible_chat_completions` provider，3 个用例全部通过
（26.74 秒）：同步通用问答、Harness 后台问答、CLI 修复程序。
编程用例核对测试文件未变、测试自行创建的成功标记，并独立重跑四项测试。
未执行跨平台、Debug/ASan、分配/关闭失败注入或性能测量。
复现及运行约束见 [CLI live 测试说明](../../cli/README.md#optional-live-model-verification)。

默认模型 HTTP 现将真实 HTTP/传输状态写入既有 `provider_attempts`，记录构造和
所有权转移使用受检查的 JSON API。默认 POST 仍为单次调用，原 timeout 与显式
v2 retry 契约保持不变。CLI 在失败时通过局部 tlog logger 输出固定阶段、数值
错误码及配置提示，保留原失败行和退出码；不直接输出 provider 正文或控制字符。
离线新增 9 个 CLI 诊断用例和 1 个默认 HTTP 不重试用例，并扩展已有 HTTP 成功/
失败记录断言；共享 loopback 测试配置不读取凭据。
Windows Release 完整构建与安装通过；离线 86 项 CTest 通过，1 项 live 测试
默认跳过（7.40 秒）。用户指定本地 Chat Completions 代理的 3 个真实模型用例
再次通过（23.53 秒），包括独立复核四项 Python 测试的编程闭环。
复现最小回归筛选为
`test_turbo_cli_diagnostics$|test_turbo_agent_http_owner$|test_turbo_agent_resilience$`，
使用 `win-release-user` configure/build/test preset 和
`install-win-release-user` build preset。未执行跨平台、Debug/ASan、分配/关闭
失败注入或性能测量；SDK 原始 `model_error_detail` 的内容契约未改变。

CLI 现以 Harness 执行终态决定是否输出答案并保留下一轮历史，避免将返回码为 0
的取消、超时或暂停记录当作成功。结果在 worker drain 后消费一次；非完成结果
释放，Ctrl-C 保持退出码 130，其他未完成结果退出 1。诊断增加固定枚举对应的
`execution_status`，不输出残留模型文本。新增 `test_turbo_cli_completion` 的
6 个正式用例覆盖晚到响应、取消后再运行、完成时收到中断、deadline、暂停和
结果重复消费等边界。该阶段尚未改造阻塞 HTTP；请求中取消由下节实现。
Windows Release 完整构建及安装通过；最小 CLI 回归 3 项通过（0.76 秒），
全量离线 87 项通过、1 项 live 默认跳过（10.27 秒）；用户指定本地代理的
3 个真实模型用例通过（23.76 秒），包括独立复核四项 Python 测试的编程闭环。
复现使用 `win-release-user` configure/build/test、`install-win-release-user`
build preset；最小 CTest 筛选为
`test_turbo_cli_completion$|test_turbo_cli_diagnostics$|test_turbo_cli$`。
未执行跨平台、Debug/ASan、分配失败注入或性能测量。

默认模型 HTTP 现通过 CHttp async API 在请求线程轮询执行 token，并在取消或
deadline 后同线程 drain/destroy，无需等待服务端响应。模型节点保留取消终态，
跳过该响应的提交及自动恢复重试，生成指向当前模型节点的 checkpoint；显式
resume 才重新发出 POST。Graph route 边界同时遇到取消与 workflow 暂停时，
取消优先且只写一次 checkpoint。保留默认单次 POST、60 秒请求上限、TLS 验证
及既有容量；显式借用的阻塞客户端和自定义回调仍采用协作式取消。
新增 3 个本地 deferred HTTP 取消/deadline/跨 worker 恢复用例及 1 个真实 HTTP
SSE 聚合用例；5 项相邻回归通过（2.46 秒），Graph/Execution/取消优先级回归
3 项通过（1.83 秒），全量离线 88 项通过、1 项 live 默认跳过（8.87 秒）。
本地 Chat Completions 代理的 3 个真实模型用例通过（24.79 秒），含编程修复后
独立重跑四项 Python 测试。Windows Release 完整构建及安装通过，沿用 `win-release-user` 和
`install-win-release-user`；取消测试入口为 `test_turbo_agent_http_cancel`。
正常取消路径的回收已验证；SDK 异常关闭失败会禁止复用并保留仍被借用的存储，
协议与取舍见 [HTTP owner 生命周期](../../agent/docs/harness-adapter.md#默认-http-的取消与-deadline)。
未执行关闭/分配失败注入、跨平台、Debug/ASan 或性能测量。
