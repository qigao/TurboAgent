# TurboAgent.Native

TurboAgent 的 native SDK package，由 `qigao/TurboAgent` 自己构建、验证和发布。

TurboAgent.Native **不发布 NuGet dependency metadata**，因此 package 本身不保留
Salts / SaltsUtils / CHttp / Praktor 的任何版本号。发布 CI 与仓库 CI 始终从各依赖仓库
恢复当前 latest released SDK，再通过 CMake `find_dependency(... CONFIG)` 验证兼容性。

TurboAgent 自身的第三方 C/C++ 依赖（例如 OpenSSL / sqlite）仍由项目的
vcpkg manifest/toolchain 解析，也不写入 Native package 的版本依赖元数据。

## SDK platforms

- `sdk/linux-x64`
- `sdk/windows-x64`
- `sdk/macos-arm64`
- `sdk/android-arm64-v8a`

Praktor 0.4.x 当前不发布 Windows SDK，因此：

- Linux / macOS / Android SDK 包含 `TurboAgent::PraktorTools`
- Windows SDK 不包含 `TurboAgent::PraktorTools`
- 其它 TurboAgent runtime/tool/harness modules 仍可在 Windows 使用

## Consume

还原 NuGet graph 后，为目标平台设置：

- `SALTS_ROOT`
- `SALTS_UTILS_ROOT`
- `CHTTP_ROOT`
- Linux/macOS/Android 如需 PraktorTools，再设置 `PRAKTOR_ROOT` 和可发现的 TurboScript SDK

然后使用 vcpkg toolchain 配置消费者：

```cmake
find_package(TurboAgent CONFIG REQUIRED)

target_link_libraries(my_app PRIVATE
  TurboAgent::RuntimeCore
  TurboAgent::RuntimeData
  TurboAgent::RuntimeTools
  TurboAgent::LangChain)
```

Praktor workflow tools：

```cmake
target_link_libraries(my_app PRIVATE TurboAgent::PraktorTools)
```

TurboAgent 1.1.0 包含 harness-native Praktor WorkflowPlan 集成：

- immutable reviewed WorkflowPlan identity
- generated workflow schema
- conservative effect → policy capability mapping
- cancellation/deadline + thread/run/turn/tool-call lineage
- workflow/task event stream
- full-detail durable journal path
- compact `agent_output` for model context

PR 只做 qualification。正式 package / GitHub Release 只由与项目版本一致的
`v*` tag 发布。
