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
      "PRAKTOR_DSO_PLUGIN_ID=\\\"test.turboagent.praktor.provider.N\\\"")
    target_compile_definitions(praktor_dso_fixture_n1 PRIVATE
      PRAKTOR_DSO_EPOCH=42
      "PRAKTOR_DSO_PLUGIN_ID=\\\"test.turboagent.praktor.provider.N1\\\"")
    target_include_directories(test_turbo_praktor_dso_replacement PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(test_turbo_praktor_dso_replacement PRIVATE
      "PRAKTOR_DSO_N_PATH=\\\"$<TARGET_FILE:praktor_dso_fixture_n>\\\""
      "PRAKTOR_DSO_N1_PATH=\\\"$<TARGET_FILE:praktor_dso_fixture_n1>\\\"")
    add_dependencies(test_turbo_praktor_dso_replacement
      praktor_dso_fixture_n praktor_dso_fixture_n1)
    set_tests_properties(test_turbo_praktor_dso_replacement PROPERTIES TIMEOUT 60)
  endif()
endif()
