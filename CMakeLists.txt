cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0144)
  cmake_policy(SET CMP0144 OLD)
endif()
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()


project(
  TurboAgent
  VERSION 1.2.0
  LANGUAGES C CXX)

# Project configuration - output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Normalize install prefix (remove trailing slashes)
get_filename_component(CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}" ABSOLUTE)

# Project description
set(PROJECT_DESCRIPTION "a LLM agent library")
set(PROJECT_HOMEPAGE_URL "https://github.com/qigao/TurboAgent")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
# Language standards - unified approach
set(CMAKE_CXX_STANDARD 17) # C++17 for compatibility
set(CMAKE_C_STANDARD 11) # C17 to ensure C11 atomic support
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  add_compile_options(/experimental:c11atomics)
endif()
# Enable folder organization in IDEs
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "cmake")

# Enable testing
include(CTest)
enable_testing()

include(CMakeOptions.cmake)
include(Sanitizers)
include(CmakeUtils)

set(_turboagent_dependency_roots SALTS_ROOT SALTS_UTILS_ROOT CHTTP_ROOT)
if(ENABLE_WASM_TOOLS)
  list(APPEND _turboagent_dependency_roots TURBOWASM_ROOT)
endif()
if(ENABLE_PRAKTOR_TOOLS)
  list(APPEND _turboagent_dependency_roots TURBOSCRIPT_ROOT PRAKTOR_ROOT)
endif()
foreach(_root IN LISTS _turboagent_dependency_roots)
  if(NOT DEFINED ENV{${_root}} OR NOT IS_DIRECTORY "$ENV{${_root}}")
    message(FATAL_ERROR "${_root} must name the installed SDK for this profile")
  endif()
endforeach()
unset(_root)
unset(_turboagent_dependency_roots)

# Reconfiguration must not retain a package directory from another SDK set.
unset(Salts_DIR CACHE)
unset(Salts_DIR)
unset(SaltsUtils_DIR CACHE)
unset(SaltsUtils_DIR)
find_package(Salts CONFIG REQUIRED PATHS "$ENV{SALTS_ROOT}"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(SaltsUtils CONFIG REQUIRED PATHS "$ENV{SALTS_UTILS_ROOT}"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

unset(Chttp_DIR CACHE)
unset(Chttp_DIR)
find_package(Chttp CONFIG REQUIRED PATHS "$ENV{CHTTP_ROOT}"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(ENABLE_WASM_TOOLS)
  unset(TurboWasm_DIR CACHE)
  unset(TurboWasm_DIR)
  find_package(TurboWasm CONFIG REQUIRED PATHS "$ENV{TURBOWASM_ROOT}"
               NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif()
if(ENABLE_PRAKTOR_TOOLS)
  # Praktor imports TurboScript; select its SDK before resolving that dependency.
  unset(TurboScript_DIR CACHE)
  unset(TurboScript_DIR)
  find_package(TurboScript CONFIG REQUIRED PATHS "$ENV{TURBOSCRIPT_ROOT}"
               NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  unset(Praktor_DIR CACHE)
  unset(Praktor_DIR)
  find_package(Praktor CONFIG REQUIRED PATHS "$ENV{PRAKTOR_ROOT}"
               NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif()
find_package(Threads REQUIRED)

# Additional components
add_subdirectory(tools)
include(FindTools)
add_subdirectory(runtime_core)
add_subdirectory(runtime_data)
add_subdirectory(runtime_tools)
add_subdirectory(agent_compiler)
if(ENABLE_MCP_TOOLS)
  add_subdirectory(mcp_tools)
endif()

if(ENABLE_PRAKTOR_TOOLS)
  add_subdirectory(praktor_tools)
endif()

if(ENABLE_CODEX_BRIDGE)
  add_subdirectory(codex_bridge)
endif()

if(ENABLE_CODING_TOOLS)
  add_subdirectory(coding_tools)
endif()

add_subdirectory(runtime)
add_subdirectory(provider)
add_subdirectory(agent)
# Installed CHttp + installed SaltsUtils IDLC conformance, never a second RPC owner.
if(BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  add_subdirectory(agent/tests/rpc_generated)
endif()
add_subdirectory(harness)
add_subdirectory(cli)
if(ENABLE_WASM_TOOLS)
  add_subdirectory(llm_sandbox)
endif()
# Compile and link direct C11/C++17 consumers of the installed CHttp Owner
# RPC ABI. The existing TurboAgent HTTP/remote runtime tests exercise real
# request/cancel/drain behavior against loopback Servers.
if(BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  add_executable(turboagent_chttp_rc5_c11 cmake/ci/chttp-rc5-api-smoke.c)
  target_link_libraries(turboagent_chttp_rc5_c11 PRIVATE CHttp::Server CHttp::Client)
  add_test(NAME turboagent.chttp_rc5_c11 COMMAND turboagent_chttp_rc5_c11)

  add_executable(turboagent_chttp_rc5_cxx17 cmake/ci/chttp-rc5-api-smoke.cpp)
  target_link_libraries(turboagent_chttp_rc5_cxx17 PRIVATE CHttp::Server CHttp::Client)
  add_test(NAME turboagent.chttp_rc5_cxx17 COMMAND turboagent_chttp_rc5_cxx17)
  set_tests_properties(turboagent.chttp_rc5_c11 turboagent.chttp_rc5_cxx17
                       PROPERTIES TIMEOUT 20)
endif()

# Package Configuration
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(TURBO_AGENT_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/TurboAgent")
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TurboAgentConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/TurboAgentConfig.cmake"
  INSTALL_DESTINATION "${TURBO_AGENT_INSTALL_CMAKEDIR}")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/TurboAgentConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)

install(
  EXPORT TurboAgentTargets
  FILE TurboAgentTargets.cmake
  NAMESPACE TurboAgent::
  DESTINATION "${TURBO_AGENT_INSTALL_CMAKEDIR}")
install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/TurboAgentConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/TurboAgentConfigVersion.cmake"
  DESTINATION "${TURBO_AGENT_INSTALL_CMAKEDIR}")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_agent_api.h"
        DESTINATION include)
