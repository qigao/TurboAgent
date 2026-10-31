# TurboAgent: obfuscation follows the compiler Setup Action

Tracking: vcpkg-cache Draft PR #244 and TurboAgent Draft PR #312.

The former source target inventory `ProtectionTargets.cmake` and
`TURBOAGENT_PROTECTED_RELEASE` CMake option are **removed**.
No downstream target enumeration is required. The product native SDK
workflow selects the existing LLVM/Clang Action with:

```yaml
with:
  build-type: toolchain-only
  obfuscator: "true"
```

Windows uses `setup-llvm-mingw` with `target: x64`; Linux/macOS use
`setup-llvm-native` with their runner Clang. Both compiler setup Actions
prepare the source-pinned upstream obfuscator-llvm host plugin when enabled,
or accept an existing host `plugin-path`. LLVM does not have a global
version restriction. Windows llvm-mingw tag is independent of this plugin.

Windows compiler setup exports separate product and dependency toolchain files.
The shared vcpkg triplets chainload the dependency file, while the product
entry point activates the launcher. The host plugin compiler follows the
runner CPU, independently of the product target. Compiler Actions and the
shared cache contract are pinned to the same revision.

The ordinary product CMake toolchain wrapper activates
`QigaoCompilerObfuscation.cmake`. It installs C/C++ compiler launchers
for *all* product source compilations in Release/MinSizeRel/RelWithDebInfo.
Debug is left unobfuscated. There is no target allowlist, special runtime,
consumer project, Clang test workflow, ABI/IR probe, or bypass.

**Boundary:** third-party vcpkg dependencies build through their own
port/toolchain and remain separate binaries and cache ABIs. Bundled
third-party sources compiled directly in TurboAgent's CMake graph will
receive the same pass; move such packages to the vcpkg dependency graph
if they should remain unmodified. Precompiled imported SDK libraries
cannot be obfuscated by linking the final App.

The product's existing native SDK packaging reads
`QIGAO_COMPILER_OBFUSCATION_ENABLED:BOOL=ON` from its *actual*
optimized Release CMake cache and writes `source_protection=enabled`
to the RID manifest only then. Tag publication rejects any RID
without a source-protection receipt. This is admission based on the
ordinary product compilation, not proof of irreversibility.

Remaining blockers: a host LLVM development package is needed to
source-build the native pass. In particular, Windows llvm-mingw may
not contain LLVMConfig.cmake; the normal workflow must fail
unless an appropriate host plugin is provided. Existing Salts/CHttp
SDKS and vcpkg packages compiled under old ABIs cannot be relabeled
as native Clang/llvm-mingw. Android NDK Clang has not yet been
connected to the opt-in obfuscator action, so Android remains
blocked for protected publication. No SDK is published by this Draft.
