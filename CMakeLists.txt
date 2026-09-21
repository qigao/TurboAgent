cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0144)
  cmake_policy(SET CMP0144 OLD)
endif()
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()


project(
  TurboAgent
  VERSION 1.0.0
  LANGUAGES C CXX)

# Project configuration - output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Normalize install prefix (remove trailing slashes)
get_filename_component(CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}" ABSOLUTE)

# Project description
set(PROJECT_DESCRIPTION "a LLM agent library")
set(PROJECT_HOMEPAGE_URL "https://github.com/qigao/turbo-agent")

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

find_package(OpenSSL REQUIRED)
find_package(base64 CONFIG REQUIRED)
find_package(TurboUtils CONFIG REQUIRED PATHS "${TURBOUTILS_ROOT}")
find_package(TurboParser CONFIG REQUIRED PATHS "${TURBOPARSER_ROOT}")
if(ENABLE_MCP_TOOLS OR ENABLE_LANGCHAIN)
  find_package(TurboHttp CONFIG REQUIRED PATHS "${TURBOHTTP_ROOT}")
endif()
if(ENABLE_LLM_SANDBOX)
  find_package(TurboWasm CONFIG REQUIRED PATHS "${TURBOWASM_ROOT}")
endif()

if(ENABLE_PRAKTOR_TOOLS)
  find_package(Praktor CONFIG REQUIRED PATHS "${PRAKTOR_ROOT}")
endif()
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(STC REQUIRED IMPORTED_TARGET stc)

# Additional components
add_subdirectory(tools)
include(FindTools)
add_subdirectory(vendor)
add_subdirectory(runtime_core)
add_subdirectory(runtime_data)
add_subdirectory(runtime_tools)
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

if(ENABLE_LANGCHAIN)
  add_subdirectory(langchain)
endif()
if(ENABLE_LLM_SANDBOX)
  add_subdirectory(llm_sandbox)
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
