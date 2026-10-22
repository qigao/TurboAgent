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

This qualifies the installed TurboAgent tree against those exact selected
SDKs. It does not unpack/reconsume the final NuGet, relocate dependencies,
publish a release, select an unpublished SDK, provide a device runner, or
replace #69 provider provenance, #122 corrected SDK or #123 sanitizer gates.
