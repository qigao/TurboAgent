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
