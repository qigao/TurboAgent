# Relocated installed SDK consumers

Issue #71's native consumption gate runs after the complete producer build,
CTest and install. `cmake/ci/verify-installed-sdk.cmake` moves the entire
TurboAgent prefix to `build/relocated-sdk/<RID>` so the old install location
is absent during configuration, linking and execution. The producer's entire
build tree is also temporarily renamed, preventing an installed library from
silently loading a dependency from its former build RPATH. An independent project
uses `find_package(TurboAgent CONFIG REQUIRED)` and imported module targets.
It has no source/header include directories or build-tree library paths.

The C11 consumers reuse the existing public API, Harness lifecycle and local
Remote dispatcher tests. The separate C++17 consumer includes public
Agent/Runtime/Harness/Remote headers and executes object ownership, quiescent
authority invalidation and Harness ABI initialization checks. Every executable
is registered with CTest and the complete consumer graph is built once.

Linux and Windows run this gate in ordinary SDK CI. Their compilers match
the producer (GCC/G++ and MSVC). Dynamic loader paths contain the relocated
TurboAgent SDK, the selected published dependency SDKs and the producer's
installed vcpkg ports; consumer build RPATH is disabled. CMake user/system
package registries and system/environment package search are disabled after
compiler detection. The package must expose the direct TurboAgent module
targets without a retired LangChain alias. Missing dependencies fail configure.

To reproduce after the normal native profile's full build/test/install:

```sh
SDK_RID=linux-x64 cmake -P cmake/ci/verify-installed-sdk.cmake
```

On Windows set `SDK_RID=windows-x64`, then invoke the script through
`cmake/ci/run-vsdevcmd.cmd`. Compilers and the installed vcpkg triplet/prefix
come from the producer's actual CMake cache. Existing SDK-root
environment variables must still identify the selected installed releases.
The driver restores the prefix on success or a failed subprocess. Abrupt
runner termination can leave the disposable CI prefix relocated.

The final NuGet gate runs the same four consumers again in fresh Linux and
Windows jobs after packaging. `cmake/ci/verify-nuget-sdk.py stage` exports the
producer's installed vcpkg triplet and selected dependency SDK directories as
a temporary test context. It records the actual producer cache, dependency
manifest and SHA-256 inventory of every installed TurboAgent file. The context
is separate from the public NuGet and expires after seven days; it does not
add dependencies to the shipped package or rebuild/reselect them.

The `consume` action checks the final `.nupkg` identity, package version,
repository commit, all four RID manifests/configs, and every selected RID file
against the producer inventory. It unpacks the actual final package and test
context into fresh locations, with no producer build/install tree available.
Both native jobs reuse `run-sdk-consumers.cmake` for the complete independent
configure/build/CTest graph. The CI/release reusable workflow fails if either
post-pack consumer fails; packing alone no longer completes the workflow.

This qualifies the final package's selected C11/C++17 consumers against those
exact dependencies, including their relocation in this test context. It does
not provide full symbol/ABI closure, prove arbitrary dependency/toolchain
compatibility, publish a release, select an unpublished SDK, provide a device
runner, or replace #69 provider provenance, #122 corrected SDK or #123 sanitizer
gates. macOS and Android retain build/install coverage; they do not run these
native package consumers.

The first Linux execution (CI #1008) passed the existing 140-entry suite and
configured the relocated package, then reproduced a public C++ include-order
defect: `turbo_agent.h` included extensions inside its `extern "C"` block,
placing the transitive `<atomic>` templates under C linkage. The umbrella now
closes its own linkage block before including independently guarded extensions.
The C++ consumer deliberately includes `turbo_agent.h` first, before TinyTest
or a standard C++ header, to keep that regression observable. No C symbol,
structure layout, function signature or policy lifecycle contract changes.
