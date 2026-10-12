cmake_minimum_required(VERSION 3.20)
project(praktor_tools LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(CmakeUtils)

cmake_add_source(SOURCES)

add_library(praktor_tools SHARED ${SOURCES})
target_compile_definitions(praktor_tools PRIVATE SHARED_CXX)
target_include_directories(
  praktor_tools PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                       $<INSTALL_INTERFACE:include>)
target_link_libraries(
  praktor_tools
  PUBLIC TurboAgent::RuntimeTools TurboAgent::AgentCompiler TurboAgent::Runtime TurboAgent::Agent Salts::ComponentPlugin Salts::JsonParser
  PRIVATE Praktor::Praktor Salts::Core)

cmake_config_target(praktor_tools FOLDER "Praktor Tools" EXPORT_NAME PraktorTools
                    ALIAS TurboAgent::PraktorTools)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION include)

if(BUILD_TESTS)
  file(GLOB TEST_SOURCES "tests/test_*.c")
  # This DSO+Harness integration is a Linux-only CTest with its own fixture,
  # not an unrelated unit-test source or an Android/Windows placeholder.
  list(FILTER TEST_SOURCES EXCLUDE REGEX
    "test_turbo_praktor_dso_compiled_harness\\.c$")
  # Real DSO race conformance starts on Linux/Praktor ON; other profiles
  # retain their existing tests rather than compiling a placeholder fixture.
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(FILTER TEST_SOURCES EXCLUDE REGEX "test_turbo_praktor_dso_replacement\\.c$")
  endif()
  cmake_add_test(
    SOURCES
    ${TEST_SOURCES}
    LIBS
    praktor_tools
    agent_compiler
    Salts::Core
    Salts::TinyTest
    INCLUDES
    include
    FOLDER
    "Praktor Tools/tests")

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Two distinct native Provider images. The ComponentPlugin registry,
    # not the test executable, owns their actual dlopen/dlclose lifecycle.
    add_library(praktor_dso_fixture_n SHARED tests/praktor_dso_provider.c)
    add_library(praktor_dso_fixture_n1 SHARED tests/praktor_dso_provider.c)
    foreach(fixture IN ITEMS praktor_dso_fixture_n praktor_dso_fixture_n1)
      target_link_libraries(${fixture}
        PRIVATE Salts::ComponentPluginABI Salts::PluginABI)
      target_include_directories(${fixture} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
      set_target_properties(${fixture} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
        FOLDER "Praktor Tools/tests/fixtures")
    endforeach()
    target_compile_definitions(praktor_dso_fixture_n PRIVATE
      PRAKTOR_DSO_EPOCH=41
      "PRAKTOR_DSO_PLUGIN_ID=\"test.turboagent.praktor.provider.N\"")
    target_compile_definitions(praktor_dso_fixture_n1 PRIVATE
      PRAKTOR_DSO_EPOCH=42
      "PRAKTOR_DSO_PLUGIN_ID=\"test.turboagent.praktor.provider.N1\"")
    target_include_directories(test_turbo_praktor_dso_replacement PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(test_turbo_praktor_dso_replacement PRIVATE
      "PRAKTOR_DSO_N_PATH=\"$<TARGET_FILE:praktor_dso_fixture_n>\""
      "PRAKTOR_DSO_N1_PATH=\"$<TARGET_FILE:praktor_dso_fixture_n1>\"")
    add_dependencies(test_turbo_praktor_dso_replacement
      praktor_dso_fixture_n praktor_dso_fixture_n1)
    set_tests_properties(test_turbo_praktor_dso_replacement PROPERTIES TIMEOUT 60)

    # The compiled Harness callback reaches a *real* Managed ComponentProvider
    # vtable while a one-thread executor queue is deliberately parked.
    # Its Host-held Generation Scope and Plugin Lease must prevent dlclose.
    add_library(praktor_dso_fixture_async_managed SHARED
      tests/praktor_dso_provider.c)
    target_include_directories(praktor_dso_fixture_async_managed PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(praktor_dso_fixture_async_managed PRIVATE
      Salts::ComponentPluginABI Salts::PluginABI)
    target_compile_definitions(praktor_dso_fixture_async_managed PRIVATE
      PRAKTOR_DSO_MANAGED_LIFECYCLE=1 PRAKTOR_DSO_EPOCH=45
      "PRAKTOR_DSO_PLUGIN_ID=\"test.turboagent.praktor.async.M45\"")
    set_target_properties(praktor_dso_fixture_async_managed PROPERTIES
      C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
      FOLDER "Praktor Tools/tests/fixtures")

    cmake_add_test(SOURCES
      tests/test_turbo_praktor_dso_compiled_harness.c
      LIBS TurboAgent::Harness praktor_tools
        Salts::ComponentPlugin Salts::Core Salts::TinyTest
      INCLUDES include
      FOLDER "Praktor Tools/tests")
    target_include_directories(test_turbo_praktor_dso_compiled_harness PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(test_turbo_praktor_dso_compiled_harness PRIVATE
      "PRAKTOR_DSO_ASYNC_MANAGED_PATH=\"$<TARGET_FILE:praktor_dso_fixture_async_managed>\"")
    add_dependencies(test_turbo_praktor_dso_compiled_harness
      praktor_dso_fixture_async_managed)
    set_tests_properties(test_turbo_praktor_dso_compiled_harness PROPERTIES
      TIMEOUT 60)
    # The separate #119 source-instrumented TurboAgent graph must preserve
    # an actual executable linker map, not merely compile unused sanitizer
    # objects. The normal Native SDK/production build gets no extra flags.
    if(ENABLE_SANITIZER_THREAD OR ENABLE_SANITIZER_ADDRESS)
      target_link_options(test_turbo_praktor_dso_compiled_harness PRIVATE
        "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/queued_dso_sanitizer.map")
      # Also audit static Salts::Plugin / Salts::ComponentPlugin objects that
      # can be extracted inside this shared Praktor integration target.
      target_link_options(praktor_tools PRIVATE
        "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/praktor_tools_sanitizer.map")
    endif()
  endif()
endif()
