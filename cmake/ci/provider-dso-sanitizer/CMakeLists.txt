cmake_minimum_required(VERSION 3.24)
project(TurboAgentProviderDsoSanitizer LANGUAGES C)

# This is deliberately independent of TurboAgent's main CMake graph.
# Never link the installed Salts::Plugin / Salts::ComponentPlugin runtime:
# their released static objects are not built with the requested sanitizer.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "The source-instrumented Plugin race gate currently requires Linux")
endif()
if(NOT DEFINED SALTS_SOURCE_ROOT OR NOT IS_DIRECTORY "${SALTS_SOURCE_ROOT}")
  message(FATAL_ERROR "SALTS_SOURCE_ROOT must be the exact released Salts source checkout")
endif()
if(NOT DEFINED SALTS_ROOT OR NOT IS_DIRECTORY "${SALTS_ROOT}")
  message(FATAL_ERROR "SALTS_ROOT must be the exact matching Salts Native SDK")
endif()
if(NOT DEFINED SANITIZER_KIND OR
   NOT SANITIZER_KIND MATCHES "^(thread|address)$")
  message(FATAL_ERROR "SANITIZER_KIND must be thread or address")
endif()

get_filename_component(TURBO_AGENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(_test_dir "${TURBO_AGENT_ROOT}/runtime/tests")
set(_fixture_dir "${TURBO_AGENT_ROOT}/praktor_tools/tests")
set(_salts "${SALTS_SOURCE_ROOT}")
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

# All first-party objects in this isolated graph get compiler instrumentation.
# ASan and TSan cannot coexist in one process, so use two build trees.
if(SANITIZER_KIND STREQUAL "thread")
  set(_sanitizer_flags -fsanitize=thread)
else()
  set(_sanitizer_flags -fsanitize=address,undefined)
endif()
find_package(Threads REQUIRED)
find_package(Salts CONFIG REQUIRED
  PATHS "${SALTS_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

# Include the CMeta descriptor/object functions actually used during
# Plugin manifest admission and Component projection, not just their SDK ABI.
# Salts::Core and unrelated CMeta modules remain external to this gate.
set(_runtime_sources
  "${_salts}/cmeta/src/interface.c"
  "${_salts}/cmeta/src/component.c"
  "${_salts}/cmeta/src/object.c"
  "${_salts}/plugin/src/plugin.c"
  "${_salts}/plugin/src/plugin_registry.c"
  "${_salts}/plugin/src/plugin_posix.c"
  "${_salts}/component-plugin/src/component_plugin.c"
  "${_salts}/component-plugin/src/component_plugin_runtime.c"
  "${_salts}/component/src/component.c"
  "${_salts}/platform/src/thread_posix.c"
  "${_salts}/platform/src/clock.c")
foreach(src IN LISTS _runtime_sources)
  if(NOT EXISTS "${src}")
    message(FATAL_ERROR "Missing required first-party Salts source: ${src}")
  endif()
endforeach()

add_library(provider_dso_firstparty_runtime STATIC ${_runtime_sources})
target_include_directories(provider_dso_firstparty_runtime PUBLIC
  "${_salts}/plugin/include"
  "${_salts}/component-plugin/include"
  "${_salts}/component/include"
  "${_salts}/platform/include"
  "${_salts}/cmeta/include"
  "${SALTS_ROOT}/include")
target_compile_definitions(provider_dso_firstparty_runtime PRIVATE
  _DEFAULT_SOURCE=1 _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)
target_link_libraries(provider_dso_firstparty_runtime PUBLIC
  Salts::ComponentPluginABI Salts::PluginABI Salts::Core Threads::Threads
  ${CMAKE_DL_LIBS})

add_library(provider_dso_instrumented_n SHARED
  "${_fixture_dir}/praktor_dso_provider.c")
add_library(provider_dso_instrumented_n1 SHARED
  "${_fixture_dir}/praktor_dso_provider.c")
foreach(fixture IN ITEMS provider_dso_instrumented_n provider_dso_instrumented_n1)
  target_include_directories(${fixture} PRIVATE "${_fixture_dir}")
  target_link_libraries(${fixture} PRIVATE
    Salts::ComponentPluginABI Salts::PluginABI)
endforeach()
target_compile_definitions(provider_dso_instrumented_n PRIVATE
  PRAKTOR_DSO_EPOCH=41
  "PRAKTOR_DSO_PLUGIN_ID=\"test.turboagent.sanitizer.provider.N\"")
target_compile_definitions(provider_dso_instrumented_n1 PRIVATE
  PRAKTOR_DSO_EPOCH=42
  "PRAKTOR_DSO_PLUGIN_ID=\"test.turboagent.sanitizer.provider.N1\"")

add_executable(test_turbo_component_dso_sanitized
  "${_test_dir}/test_turbo_component_dso_lifecycle.c")
target_include_directories(test_turbo_component_dso_sanitized PRIVATE
  "${_fixture_dir}")
target_compile_definitions(test_turbo_component_dso_sanitized PRIVATE
  "PRAKTOR_DSO_N_PATH=\"$<TARGET_FILE:provider_dso_instrumented_n>\""
  "PRAKTOR_DSO_N1_PATH=\"$<TARGET_FILE:provider_dso_instrumented_n1>\"")
target_link_libraries(test_turbo_component_dso_sanitized PRIVATE
  provider_dso_firstparty_runtime Salts::TinyTest Salts::Core
  Threads::Threads ${CMAKE_DL_LIBS})
target_link_options(test_turbo_component_dso_sanitized PRIVATE
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/provider_dso_sanitized.map")
add_dependencies(test_turbo_component_dso_sanitized
  provider_dso_instrumented_n provider_dso_instrumented_n1)

foreach(target IN ITEMS
    provider_dso_firstparty_runtime
    provider_dso_instrumented_n
    provider_dso_instrumented_n1
    test_turbo_component_dso_sanitized)
  target_compile_options(${target} PRIVATE
    ${_sanitizer_flags} -fno-omit-frame-pointer -g -O1)
  target_link_options(${target} PRIVATE ${_sanitizer_flags})
endforeach()

# SDK Core/CMeta/TinyTest are external ABI dependencies, not within the
# source-coverage claim. Use the release SDK's exact headers and ELF libs.
set(CMAKE_BUILD_RPATH "${SALTS_ROOT}/lib;${SALTS_ROOT}/bin")
set_target_properties(
  test_turbo_component_dso_sanitized
  provider_dso_instrumented_n provider_dso_instrumented_n1
  PROPERTIES BUILD_RPATH "${SALTS_ROOT}/lib;${SALTS_ROOT}/bin")
add_test(NAME provider_dso_full_source_sanitizer
  COMMAND test_turbo_component_dso_sanitized)
set_tests_properties(provider_dso_full_source_sanitizer PROPERTIES
  TIMEOUT 120
  ENVIRONMENT "LD_LIBRARY_PATH=${SALTS_ROOT}/lib:${SALTS_ROOT}/bin")
