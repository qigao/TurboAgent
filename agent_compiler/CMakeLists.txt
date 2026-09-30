cmake_minimum_required(VERSION 3.20)
project(agent_compiler LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(CmakeUtils)

cmake_add_source(SOURCES)

add_library(agent_compiler SHARED ${SOURCES})
target_compile_definitions(agent_compiler PRIVATE SHARED_CXX)
target_include_directories(
  agent_compiler PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                        $<INSTALL_INTERFACE:include>)
target_link_libraries(
  agent_compiler
  PUBLIC TurboAgent::RuntimeTools TurboAgent::RuntimeData Salts::DataBind)

cmake_config_target(agent_compiler FOLDER "Agent Compiler" EXPORT_NAME AgentCompiler
                    ALIAS TurboAgent::AgentCompiler)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION include)

if(BUILD_TESTS)
  file(GLOB TEST_SOURCES "tests/test_*.c")
  cmake_add_test(
    SOURCES
    ${TEST_SOURCES}
    LIBS
    agent_compiler
    Salts::TinyTest
    INCLUDES
    include
    FOLDER
    "Agent Compiler/tests")
endif()
