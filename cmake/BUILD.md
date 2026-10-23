# Praktor build contract

The repository `.env` supplies host-specific paths to Praktor. Praktor passes
them to child processes; `CMakeUserPresets.json` reads them with `$penv{...}`.
The versioned presets remain the authority for compiler settings, features,
triplets, build directories, SDK profile roots and install destinations.
This contract applies to Windows, Linux and macOS hosts, and Android targets.

The Praktor workflows are usable by both developers and CI runners with an
installed Praktor CLI. `build.yml` consumes installed SDKs; `sdk-build.yml`
restores published Release SDKs first. Both delegate configure, build, test and
install to the same `cmake/workflows/` pipelines. Compiler flags, feature
selection and the generated build graph remain owned by CMake presets.

## Setup

Install a runnable Praktor CLI, CMake, Ninja and Git on the host's `PATH`.
Praktor must be able to find its own runtime libraries before it can read `.env`.
On Windows this includes its Salts/Chttp DLL dependencies; use the installed
CLI's runtime environment rather than copying DLLs into TurboAgent.

Copy `.env.example` to `.env`, or add the example's non-secret keys to an
existing `.env`. Keep credentials in the ignored `.env` or the parent process.
`GITHUB_TOKEN` needs `read:packages` for the shared NuGet feed; `cmake/ci/restore-native-sdk.sh` uses curl with
`GH_TOKEN`, then `GITHUB_TOKEN`, for GitHub Release access.
Do not print credentials or enable verbose environment dumps.

Use literal, absolute, forward-slash paths. Praktor's dotenv parser does not
expand `$VAR`, `${VAR}`, `~` or CMake macros. Later `.env` values override the
inherited process environment; task-specific environment overrides those values.
CMake does not load `.env` itself. Direct preset invocation needs these same
variables already set in the parent environment.

The common environment keys are:

| Key | Owner and purpose |
| --- | --- |
| `PKG_ROOT` | Locally installed first-party SDKs; presets derive Debug/Release and Android roots |
| `VSDEVCMD_BAT` | Windows Visual Studio developer-command batch file called by the Praktor Windows workflow |
| `VCPKG_ROOT` | vcpkg installation using the shared tool/scripts identity |
| `VCPKG_CACHE_REPOSITORY_ROOT` | Checkout of `qigao/vcpkg-cache`, containing the toolchain and overlays |
| `VCPKG_BINARY_SOURCES` | Shared read-only NuGet feed plus writable local binary cache; use the absolute NuGet config path for this checkout |
| `RE2C_ROOT` | Restored `Qigao.Re2c.Binary` host payload: `<packages>/qigao.re2c.binary/<version>/tools/<host-rid>` |
| `SDK_RESTORE_ROOT` | Dedicated disposable package restore directory, e.g. this repository's `build/restored-sdks` |
| `BASH_EXECUTABLE` | Windows Git Bash executable for SDK restoration; use its absolute path to avoid the WSL launcher |
| `ANDROID_NDK_HOME` | NDK root, required when targeting Android |

On Linux, typical existing roots are `PKG_ROOT=/opt`, `VCPKG_ROOT=/opt/vcpkg`
and `VCPKG_CACHE_REPOSITORY_ROOT=/home/your-user/.cache/qigao/vcpkg-cache`.
macOS uses its own installed host paths. Neither OS consumes Windows paths.
Both need the host compiler, re2c and the NuGet runtime required by the shared
cache setup. SDK restoration on every host needs Python 3.9+, `curl`, `jq` and `unzip`;
Windows uses Git Bash and its `cygpath` path converter. Install `jq` on its
inherited `PATH` and set `BASH_EXECUTABLE` as shown in `.env.example`.

Initialize the cache checkout at the chosen location before using its toolchain:

```sh
git clone https://github.com/qigao/vcpkg-cache.git /absolute/path/to/vcpkg-cache
```

Follow that checkout's setup contract for the pinned vcpkg tool/scripts and
re2c tools. Updating the cache repository is separate from restoring binary
packages. vcpkg owns ABI matching and restores packages using the existing
read-only GitHub Packages feed and writable local binary cache; the product
`vcpkg.json` continues to own its dependency graph.

### Shared re2c host package

Use `Qigao.Re2c.Binary` published by `qigao/vcpkg-cache`. It contains the host
executable and `share/re2c/stdlib`; it is separate from vcpkg library archives
and the first-party SDKs under `PKG_ROOT`. TurboAgent and http-services can use
the same restored payload through `RE2C_ROOT`.

For local setup, create a reusable `re2c.csproj` outside either source tree:

```xml
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
    <RestorePackagesWithLockFile>false</RestorePackagesWithLockFile>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Qigao.Re2c.Binary" Version="*" />
  </ItemGroup>
</Project>
```

With .NET SDK 8 and `GITHUB_TOKEN` in the process environment, restore from
this repository root (replace both absolute paths for the current host):

```sh
dotnet restore /absolute/re2c-restore/re2c.csproj --packages /absolute/shared/nuget-packages --configfile cmake/vcpkg-cache.nuget.config --no-cache --force-evaluate
```

Read the resolved `Qigao.Re2c.Binary/<version>` from the restore project's
`obj/project.assets.json`. Set the same literal `RE2C_ROOT` in both `.env` files
to `<packages>/qigao.re2c.binary/<version>/tools/<host-rid>`, then run
`praktor -f build.yml -i TASK=refresh` to discard old cached tool paths.
The `.env.example` version is illustrative, not a version constraint.
Choose the host RID even for Android/iOS targets. Ordinary builds reuse this
installation; restoring/upgrading the host package is an explicit setup step.

GitHub Actions already uses
`qigao/vcpkg-cache/.github/actions/setup-re2c-tools@master`, which installs .NET,
restores the latest stable package and exports `RE2C_ROOT` and `PATH`.

## Preset composition and path rules

`CMakeUserPresets.json` keeps the public configure/build/test/install names and
the project path rules. Hidden presets share dependency roots, platform runtime
paths and CI directory layout. The `TURBOAGENT_*` variables below are selected by
presets; they do not add required `.env` settings.

`CMakePresets.json` owns the hidden project build policies: local test/sanitizer
selection, CI feature selection, and CI platform/compiler/ABI combinations.
The CI policies also select the stable profile name and SDK RID; directory
expressions remain in the user presets. It includes the shared definitions
under `presets/`:

- `BasePresets.json` composes platform/compiler profiles and the common Ninja
  generator with vcpkg manifest mode.
- `ConfigurePresets.json` composes native compiler/configuration profiles;
  `AndroidPresets.json` owns Android toolchain, ABI and configuration policies.
- `BuildPresets.json` owns the shared `base-install` operation;
  `TestPresets.json` owns `base-test` execution policy.

Project SDK roots, machine environment paths and public user entry points remain
in `CMakeUserPresets.json`. Existing shared preset names remain available.
The public CI entries compose their environment with the corresponding
`base-ci-*-release` policy. Environment presets precede policy presets so the
CI build directory takes precedence over the generic Android build directory.
Neither the Praktor entry workflows nor GitHub Actions need a preset-name or
command change for this separation.

| Scope | Path rule |
| --- | --- |
| Local SDKs and installation | `PKG_ROOT/<package><TURBOAGENT_SDK_SUFFIX>/<TURBOAGENT_CONFIG>`; suffix is empty for native SDKs and `-android` for Android; configuration is `debug` or `release` |
| Native vcpkg prefix | `<source>/vcpkg_installed/<TURBOAGENT_HOST>/<triplet>` with `/debug` for Debug runtime libraries |
| Windows-hosted Android vcpkg tree | `<source>/vcpkg_installed/<ANDROID_BUILD_VARIANT>`; existing ABI/configuration names are retained |
| Local Android build tree | `<source>/build/<ANDROID_BUILD_VARIANT>`; the Android profile defines the variant once for both build and vcpkg paths |
| CI build and vcpkg trees | `<source>/build/<TURBOAGENT_PROFILE>` and `<source>/vcpkg_installed/<TURBOAGENT_PROFILE>` |
| CI installation | `<source>/build/sdk/<TURBOAGENT_SDK_RID>` |

CI SDK roots still come directly from the restore environment. They do not use
the local `PKG_ROOT` layout. Windows shares one runtime `PATH` rule; Linux and
macOS share the SDK library-directory list while retaining their respective
`LD_LIBRARY_PATH` and `DYLD_LIBRARY_PATH` variables. Android does not add target
libraries to a host runtime search path.

`TURBOAGENT_PROFILE` has an explicit, stable value so an `install-*` entry uses
the same paths as its configure/build entry. Environment expressions containing
`${presetName}` would instead be expanded in the consuming preset's context;
see [CMake preset macro expansion](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html#macro-expansion).
Each install entry inherits its matching build entry and the shared
`base-install` preset, which declares the `install` target once.

## Local build with installed SDKs

Praktor is the cross-platform entry point and reads `.env` itself. On Windows,
its workflow calls `VSDEVCMD_BAT`, then runs each CMake or CTest command in that
same `cmd.exe` process. Unix workflows invoke CMake directly.
The Windows helper preserves the configured `VCPKG_ROOT` across `VsDevCmd.bat`
and derives `VCPKG_VISUAL_STUDIO_PATH` from the selected VS instance. Native CI
uses the same helper, so Visual Studio's bundled vcpkg cannot replace that root.
Explicit triplet toolset constraints still apply.

```sh
praktor -f build.yml
praktor -f build.yml -i PRESET=win-dev-user
praktor -f build.yml -i TASK=refresh
praktor -f build.yml -i TASK=build
praktor -f build.yml -i TASK=test
praktor -f build.yml -i INSTALL=1
praktor -f build.yml -i CACHE_ACTION=sync
```

Linux uses the same workflow and its existing user preset:

```sh
praktor -f build.yml -i HOST=unix -i PRESET=linux-release-user
praktor -f build.yml -i HOST=unix -i PRESET=linux-release-user -i INSTALL=1
praktor -f build.yml -i HOST=unix -i PRESET=linux-sdk-package -i TEST=0 -i INSTALL=1
```

`TASK` selects `configure`, `refresh`, `build`, `test`, `install` or local `run`;
its default `all` runs configure, the complete build graph and matching CTest. `build`
includes configure, while `test` includes configure and build. `refresh` runs
only `cmake --fresh --preset`; `FRESH=1` keeps the same recovery behavior for
the `all`, `configure`, `build`, `test`, `install` and `run` paths.
Installation is opt-in. `CACHE_ACTION=clone` asks Git to create the cache checkout
at the configured path; an existing destination fails instead of being replaced.
Cache synchronization is opt-in with `CACHE_ACTION=sync` and runs
`git pull --ff-only` against the configured checkout; failures stop the build.
It does not reset local changes or fetch the entire remote binary feed.

Each selected profile must provide a configure preset and a build preset with
the same name. Testing additionally requires the matching test preset and tests
enabled at configure time; `TEST=1` only runs CTest, it does not enable tests in
CMake. Installation uses the matching `install-<preset>` build preset. The local
Windows/Linux Release profiles enable tests and exclude benchmarks. Use
`TEST=0` for package-only or Android profiles without host-runnable tests.

If the selected compiler/toolchain changed or the existing build cache is
damaged, request CMake's preset recovery explicitly with `-i FRESH=1`:

```sh
praktor -f build.yml -i FRESH=1
```

Normal builds preserve the cache. Recovery clears only CMake's selected build
cache using `--fresh`; it does not delete installed packages or source files.

### Build one target, select tests, or run a program locally

`build.yml` accepts optional `TARGET`, `TEST_REGEX`, `RUN_PROGRAM` and `RUN_ARGS`
inputs. Empty `TARGET` keeps the complete build graph; a nonempty value selects
one CMake target and its dependencies. `TEST_REGEX` selects CTest names, which
need not match build target names. Neither input creates or enables a target/test
that the selected configure preset excludes.

```sh
# Configure and build just this executable and its dependencies.
praktor -f build.yml -i TASK=build -i TARGET=turboagent

# Build one test target, then execute the matching registered CTest test.
praktor -f build.yml -i TASK=test -i TARGET=test_turbo_runtime_control -i 'TEST_REGEX=^test_turbo_runtime_control$'

# Build the complete graph, then execute a selected group of tests.
praktor -f build.yml -i TASK=test -i 'TEST_REGEX=^test_turbo_(runtime_control|diagram)$'

# Build and run a native program with the selected preset's runtime environment.
# Without a prompt, the read-only CLI exits when this noninteractive runner sends EOF.
praktor -f build.yml -i TASK=run -i TARGET=turboagent -i RUN_PROGRAM=build/Msvc-Release/bin/turboagent.exe -i 'RUN_ARGS=["--read-only"]'
```

For Linux, add `-i HOST=unix -i PRESET=linux-release-user`; the corresponding
program path is `build/linux-gcc-release/bin/turboagent`. `RUN_PROGRAM` is an
explicit absolute path or a path relative to the repository root. `RUN_ARGS`
is a JSON array of strings, not a shell command line. Single-quoted input examples
work in PowerShell and POSIX shells. Program execution uses the repository root
as its working directory and is noninteractive, with a 30-minute timeout.

`TASK=run` configures/builds before running; it does not run tests or install.
The shared `run-program` target invokes `cmake/RunLocal.cmake` under the selected
build preset, retaining its runtime library environment and, on Windows,
`VsDevCmd.bat`. The helper passes program arguments directly as argv, preserving
empty arguments, spaces and shell metacharacters. It does not infer a program
path from the target name. Cross-compiled programs require a separate device
runner and are rejected by this host execution path. Run test executables through
`TASK=test` and CTest instead.

When `TARGET` is set and testing is requested, `TEST_REGEX` is required; use
`TASK=build` or `TEST=0` for build-only work. Every selected test and its runtime
fixtures must be built by the chosen target, or leave `TARGET` empty. A filter
matching no tests fails. Target selection cannot be combined with installation,
which requires the complete graph. Invalid input combinations fail before
configure, and build/test/program failures stop the workflow.
The existing `turboagent --help` command returns 1; the runner reports that exit
as a failure rather than overriding the executable's exit status.

These controls belong to local `build.yml`. `sdk-build.yml` exposes no target
selection or program runner; its default CI/package path builds the complete
graph and runs the profile's CTest suite. Both public workflows reject unknown
input names rather than silently ignoring them.

## Build with freshly restored Release SDKs

`sdk-build.yml` calls the shared Bash restore script through Praktor's
`program` runner. Supply the exact published `SALTS_RELEASE_TAG`,
`SALTS_UTILS_RELEASE_TAG`, `CHTTP_RELEASE_TAG`, and `TURBOWASM_RELEASE_TAG`
in `.env` or the parent environment; Unix builds also require
`TURBOSCRIPT_RELEASE_TAG` and `PRAKTOR_RELEASE_TAG`. See `.env.example` for the
current CI tag set. `gh` checks that each Release is published and downloads
its exact versioned Native package. Missing tags, assets, or RID configs fail
immediately. The script records tags, asset names, and SHA-256 digests under
`build/release-provenance/<RID>.txt`, as used by CI packaging.
The restore directory is disposable: each selected package subdirectory is
replaced. Do not point `SDK_RESTORE_ROOT` at installed SDKs or user data.

The script returns SDK roots as JSON. Praktor forwards them into the common
CMake workflow, which uses the existing `ci-*` presets. A child process cannot
change its parent's environment, and local execution does not interpret
`GITHUB_ENV`. Existing GitHub Actions calls retain their default `GITHUB_ENV`
output; JSON mode is explicitly selected by this workflow. Windows CI uses
the same script via its `bash` shell. `restore-native-sdk.ps1` has been removed.
The workflow uses only Praktor's program executor, so a core-only CLI with
the TurboScript engine disabled is sufficient.

| Host / target | RID | Preset | Tests |
| --- | --- | --- | --- |
| Windows / Windows | `windows-x64` | `ci-windows-release` | CTest |
| Linux / Linux | `linux-x64` | `ci-linux-release` | CTest |
| macOS / macOS | `macos-arm64` | `ci-macos-release` | Disabled by existing profile |
| Linux / Android | `android-arm64-v8a` | `ci-android-release` | No device runner; disabled |

```sh
praktor -f sdk-build.yml
```

```sh
praktor -f sdk-build.yml -i HOST=unix -i RID=linux-x64 -i PRESET=ci-linux-release
praktor -f sdk-build.yml -i HOST=unix -i RID=macos-arm64 -i PRESET=ci-macos-release -i TEST=0
praktor -f sdk-build.yml -i HOST=unix -i RID=android-arm64-v8a -i PRESET=ci-android-release -i TEST=0
```

Windows `ci-*` presets require the target/host triplets provided by the shared
cache setup or configured in `.env`; they must match the installed compiler.
The restored Windows SDK graph excludes PraktorTools, as existing releases do
not contain a Windows Praktor SDK. The build orchestrator itself runs on the
host and is independent of that optional TurboAgent backend.

Android keeps the outer vcpkg toolchain and chainloads the NDK via
`VCPKG_CHAINLOAD_TOOLCHAIN_FILE`. The Unix restore script supplies the Linux host
IDLC and Salts roots beside the Android target SDK. Windows-hosted Android local
presets keep their existing ABI directories and require a compatible installed
host IDLC; this workflow does not introduce Windows Android SDK restoration or
a device runner.

## Using the same pipeline in CI

Once Praktor and the host toolchain are available, a runner can execute the same
entry point with its environment and matrix values. For example, the Linux SDK
job's restore/configure/build/test/install sequence is:

```sh
praktor -f sdk-build.yml -i HOST=unix -i RID=linux-x64 -i PRESET=ci-linux-release -i TEST=1 -i INSTALL=1
```

Windows uses `HOST=windows`, `RID=windows-x64` and `PRESET=ci-windows-release`;
the common Windows pipeline enters `VSDEVCMD_BAT` for every CMake/CTest command.
macOS and Android use their existing RID/preset pairs with `HOST=unix` and
`TEST=0`. Android uses the Linux host CLI and target SDKs, not an Android CLI.
CI supplies the environment directly; it does not need to create a `.env` file.

These invocations build the complete configured graph and propagate failures
before installation. `FRESH=1` is explicit cache recovery, not the default CI
behavior. Artifact upload, NuGet packaging and publishing remain owned by the
existing GitHub workflow. The current Actions jobs still invoke CMake directly;
switching them to the command above requires provisioning Praktor on the runner,
which is outside this usage change.

## Scope and rollback

Machine paths formerly embedded in presets now come from `.env` or the parent
environment. Windows/Linux local profiles share their SDK root declarations,
while Android retains separate target roots. Public preset names, compiler
options, dependency selection, SDK package formats and CI entry commands stay
the same for CMake. Windows Actions now restore SDKs through the shared Bash
script; existing Actions do not require Praktor or a `.env` file.

This keeps configuration in CMake and gives Praktor only orchestration and
environment loading. A second build configuration or package downloader would
duplicate those owners. Reverting this change restores the previous machine
path definitions and leaves downloaded SDKs and installed packages untouched.
Other repositories can adopt the same environment keys without being modified
by this change.

## Verification on 2026-10-14

Using the installed Praktor CLI and its runtime `PATH`, Praktor completed the
local `win-release-user` workflow, including explicit `FRESH=1`
recovery: configure and the full build passed; CTest passed 103 tests and skipped
the opt-in live test. The workflows passed Praktor validation; Bash syntax,
Actionlint and `git diff --check` passed.

The `sdk-build.yml` workflow restored current published SDKs through `gh`, parsed
the returned JSON and completed `ci-windows-release` configure/full build.
The selected releases were Salts `v2.3.0-rc.16`, SaltsUtils `v4.3.0-rc.11`,
Chttp `v2.1.0-rc.6` and TurboWasm `v0.2.1`; these are observations, not version locks.

After replacing Windows restoration with Git Bash, `sdk-build.yml` completed
restoration, configure, the full build and CTest: 103 passed and the opt-in live
test was skipped. Praktor validation, `bash -n`, Actionlint and
`git diff --check` also passed for the final workflows.

**MED:** Two earlier full SDK test runs failed the deadline assertion in
`cli/tests/test_turbo_cli_tool_cancel.c:230`: 102 passed, 1 failed, 1 skipped.
An isolated CTest rerun and the final full SDK run passed. The intermittent
failure's cause is unresolved; no runtime/test code was changed. Praktor
propagated the earlier failures and did not run the requested install.
The final pass does not establish that this timing-sensitive test is stable.

Linux/macOS/Android builds, the install branch, clone/sync mutations and a fresh
remote NuGet cache restore have not been executed. The default local build is
validated; other-platform commands document the shared contract and retain the
existing profile/test boundaries.

## Verification on 2026-10-16

After consolidating preset inheritance and path variables, and again after
moving shared policies into `presets/`, `praktor -f build.yml` completed Windows
Release configure, the complete build graph and CTest: 103 tests passed and the
opt-in live test was skipped.

`praktor -f sdk-build.yml -i INSTALL=1` also completed SDK restoration,
`ci-windows-release` configure, the complete build graph, CTest (103 passed,
one opt-in live test skipped) and installation to `build/sdk/windows-x64`.
The restored SDK versions matched the observations above. Both workflows
reported success, confirming the shared runtime paths and inherited install
presets on Windows. The SDK pipeline also passed after the `presets/` changes.

Both entry workflows and both common CMake workflows passed `praktor validate`;
`git diff --check` passed. Linux/macOS/Android builds and a GitHub Actions
invocation were not executed in this follow-up. The earlier intermittent
deadline-test failure remains unresolved.

The local target/run extension was then exercised on Windows:

- `TASK=test`, `TARGET=test_turbo_runtime_control` and an exact `TEST_REGEX`
  built the selected target and passed its single CTest test.
- A regex containing `|` selected and passed two existing CTest tests; an
  unmatched regex failed with CTest exit code 8 and stopped the workflow.
- `TASK=run` built `turboagent` and ran it with `RUN_ARGS=["--read-only"]`;
  the program exited successfully on EOF. Its existing `--help` exit code 1
  propagated as a workflow failure.
- Target selection without a test filter, non-string program arguments and
  passing `TARGET` to `sdk-build.yml` were rejected before configuration.
- The default `praktor -f build.yml` still completed the full graph and CTest:
  103 passed, one opt-in live test skipped. All four workflow files passed
  Praktor validation, and `git diff --check` passed.

SDK restoration/installation and non-Windows execution were not rerun for this
local target/run extension.

After moving the project policies from user presets into `CMakePresets.json`,
`praktor -f build.yml` and `praktor -f sdk-build.yml -i INSTALL=1` both completed
Windows configure and the full build graph. Each CTest run passed 103 tests
and skipped the opt-in live test; the SDK workflow also installed successfully
to `build/sdk/windows-x64`. The first SDK attempt stopped during SaltsUtils
download with a TLS transport error, before CMake configuration; one retry
completed the full workflow. `git diff --check` passed. Non-Windows builds and
GitHub Actions execution were not run for this policy separation.

## CI downloads without Mono

The local `setup-ci-tools` action checks out the shared cache infrastructure at
an exact commit and consumes its tool/scripts identities, contract hash,
overlays and canonical materialization paths. It does not run the shared
NuGet/Mono setup action. `github-packages.py` uses curl for GitHub Packages
NuGet V3 downloads and published SDK Release assets. re2c retains latest-stable
selection; its resolved version and SHA-256 are logged. SDK Release tags remain
pinned in the workflows; published status, exact asset name, GitHub asset digest
when supplied, and the existing SDK provenance inventory are checked.

Before the normal configure, `restore-vcpkg-cache.py <ci-preset>` runs the real
preset/toolchain in a separate configure-only directory. The internal
`TURBOAGENT_CACHE_PREFLIGHT` switch requires vcpkg `--dry-run`; the pinned vcpkg
tool computes ABIs and writes `--x-write-nuget-packages-config`. No port is built
in this phase. Windows runs both phases through `run-vsdevcmd.cmd` so the
compiler owner is unchanged; Android retains its NDK chainload and host triplet.

curl downloads those exact package references. Admission verifies the nuspec
identity/version and CONTROL package/triplet/ABI, rejects unsafe archive entries,
and copies the payload into vcpkg's native files-cache ZIP layout, removing only
the NuGet envelope. The normal configure uses `--only-binarycaching` and a
read-only files provider. Missing packages, invalid payloads or compiler ABI
mismatches fail the job; no source-build fallback or guessed ABI is used. The
normal configure and build still cover the complete configured graph.

Credentials travel to curl on stdin and are never persisted in a NuGet config.
Redirects require HTTPS and do not forward credentials to a different host.
.NET is used only to create the final native package; its zero-PackageReference
project restores with an empty package-source list. Offline transport admission
tests run through CTest from `cmake/ci/tests`; native and installed/final-package
consumer checks remain in their existing build/test phases.
