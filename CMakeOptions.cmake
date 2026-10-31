include(CMakeDependentOption)

set(CMAKE_COLOR_DIAGNOSTICS ON)

# building the tests
option(ENABLE_TESTS "Enable the tests" ON)
set(BUILD_TESTS_DEFAULT "${ENABLE_TESTS}")

# Address Sanitizer - only enabled for Debug builds
cmake_dependent_option(
    ENABLE_ASAN "Enable Address Sanitizer" ON
    "CMAKE_BUILD_TYPE STREQUAL Debug" OFF
)

# if(MSVC)
#     add_compile_options(/bigobj)
# endif()

# Opt in only with the upstream vcpkg-cache compiler action and an
# actual source-built obfuscator-llvm plugin. A request with missing inputs is
# an error, not a silent compiler/obfuscator fallback.
option(TURBOAGENT_PROTECTED_RELEASE
  "Apply the shared first-party sensitive target policy during product Release"
  OFF)
option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_TESTS "Build test suite" ${BUILD_TESTS_DEFAULT})
option(TURBOAGENT_SQLITE_VFS_CONFORMANCE "Build native SQLite VFS recovery tests without the full suite" OFF)
option(TURBOAGENT_JOB_FILE_CONFORMANCE "Build native Job file verification tests without the full suite" OFF)
if(TURBOAGENT_JOB_FILE_CONFORMANCE AND CMAKE_CROSSCOMPILING)
  message(FATAL_ERROR "Job file conformance requires a native host")
endif()
option(ENABLE_CODING_TOOLS "Build the bounded coding tool pack" ON)
option(ENABLE_CODEX_BRIDGE "Build the Codex App Server client bridge" ON)
option(ENABLE_MCP_TOOLS "Build the MCP tool pack" ON)
option(ENABLE_PRAKTOR_TOOLS "Build the Praktor workflow tool pack" OFF)
option(ENABLE_WASM_TOOLS "Build the TurboWasm tool backend" ON)
cmake_dependent_option(
    BUILD_BENCHMARKS "Build benchmark executables" ON
    "BUILD_TESTS" OFF
)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
