# TurboAgent first-party obfuscation ownership

Tracking: [vcpkg-cache PR #244](https://github.com/qigao/vcpkg-cache/pull/244) and
[issue #243](https://github.com/qigao/vcpkg-cache/issues/243).

**Status: staged Draft product adoption, not a published protected Release.**
The default SDK builds still use their existing native Release presets. Do not
claim the current MSVC/AppleClang/GCC packages are protected, and do not
publish them as the new obfuscated Release profile. The upstream shared
CMake module remains Draft and is not yet on vcpkg-cache master.

## Actual source ownership

`cmake/ProtectionTargets.cmake` is the single named list for the product's
shipped first-party compiled library/executable targets. It is included from
the existing root CMake *after* all subdirectory targets exist, only when
`TURBOAGENT_PROTECTED_RELEASE=ON`.

- Sensitive: `agent_compiler`, `runtime_tools`, `turbo_runtime`,
  `turbo_agent`, `turbo_agent_harness`, `turbo_cli_driver`, and optional
  `coding_tools`, `praktor_tools`, `wasm_tools`. They contain the product's
  tool/workflow execution, policy admission, local session/graph/knowledge
  implementation, or sandbox execution boundaries.
- Ordinary: `runtime_core`, `runtime_data`, `turbo_provider`, `turboagent`
  (the CLI entrypoint), and optional `mcp_tools`, `codex_bridge`,
  `turbo_agent_speech_input`, `turbo_agent_speech_output`. They should not
  inherit the obfuscator pass merely by linking sensitive libraries.
- Imported `Salts::...`, `CHttp::...`, `unofficial::sqlite3::sqlite3` and
  other upstream/third-party dependencies are **not** part of this list.
  No static/dynamic precompiled dependency is retroactively obfuscated.
- Existing test/example/host-tool targets are not shipped product code and
  are not included in this shipping target inventory. When a target becomes
  part of a shipped native product, update this file in the same PR.

## Intended activation

The product Release workflow first adopts the reviewed pinned SHA of
`qigao/vcpkg-cache`'s relevant `setup-llvm-native` or
`setup-llvm-mingw` action and supplies its **actual** obfuscator-llvm plugin
path. Then invoke the product's existing configure with
`-DTURBOAGENT_PROTECTED_RELEASE=ON`. The action exports
`QIGAO_OBFUSCATION_POLICY_CMAKE` and
`QIGAO_OBFUSCATOR_PLUGIN_PATH`; the normal source compilation of sensitive
targets applies the compiler's `-fpass-plugin` option. Debug remains
unobfuscated; Release/MinSizeRel/RelWithDebInfo require a supplied plugin.
No standalone Clang version tests, IR probes or synthetic consumers are added.

**Blocking follow-up:** the current TurboAgent `ci-windows-release`,
`ci-linux-release` and `ci-macos-release` presets and SDK publisher must
be migrated together to llvm-mingw/runner-native Clang with the actual plugin
before calling any resulting package a protected Release. This Draft does not
silently switch or falsely qualify those existing published SDK paths.

If source ownership moves to another first-party repository, move the
sensitive-source classification to that repository's own CMake source
target rather than relying on final `turboagent` linking. Obfuscation
increases reverse-engineering cost; it is not a substitute for server-side
secrets and authorization.

## Existing SDK publication gate

The **existing** native SDK builder now writes `source_protection=enabled`
or `source_protection=disabled` into each RID's
`turboagent-sdk-manifest.txt`, derived from the **actual product**
`TURBOAGENT_PROTECTED_RELEASE` CMakeCache entry. This is a source-build
classification receipt, not a separate Clang version test or proof of
complete reverse-engineering resistance.

The **existing tag publisher** rejects any `TurboAgent.Native` NuGet
package that lacks `source_protection=enabled` for **all** packaged
RIDs: Linux x64, Windows x64, macOS ARM64 and Android ARM64-v8a.
This gate is intentionally fail-closed: currently published/packaged
legacy SDK presets remain unprotected and therefore cannot be newly
published as compliant Release artifacts. No actual package is
published by this Draft PR.

Only after each RID's actual compiler/toolchain and product-owned
first-party plugin have been migrated and the normal build compiles
successfully may its Release manifest set `source_protection=enabled`.
No new compiler-validation workflow, CMake probe, consumer project
or fallback is allowed.

## SDK compiler paths in existing CI

Current PR CI presets explicitly choose the shared `RunnerClang` selector
on Linux/macOS and `LlvmMingw` on Windows x64. The action checkout is pinned
to [vcpkg-cache PR #244](https://github.com/qigao/vcpkg-cache/pull/244)
commit `35dc0f8ac0c7e880a5126fac516d7258b6fea15b`, which provides the new distinct MinGW triplets.
Android retains its own NDK Clang. This does not rewrite old published MSVC,
GCC or AppleClang dependency SDKs, and old ABI artifacts cannot substitute for
MinGW/Clang Release packages. Missing correct ABI packages remain a real build
blocker, not a reason to reenable MSVC or GCC. No new Clang validation jobs.
