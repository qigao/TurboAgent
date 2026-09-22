cmake_minimum_required(VERSION 3.20)
project(mcp_tools LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(CmakeUtils)

cmake_add_source(SOURCES)

add_library(mcp_tools SHARED ${SOURCES})
target_compile_definitions(mcp_tools PRIVATE SHARED_CXX)
target_include_directories(
  mcp_tools PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                   $<INSTALL_INTERFACE:include>)
target_link_libraries(mcp_tools PUBLIC TurboAgent::RuntimeTools
                                       Salts::JsonParser
                                       Salts::UriParser
                                       CHttp::Client)

cmake_config_target(mcp_tools FOLDER "MCP Tools" EXPORT_NAME McpTools
                    ALIAS TurboAgent::McpTools)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION include)

if(BUILD_TESTS)
  file(GLOB TEST_SOURCES "tests/test_*.c")
  cmake_add_test(
    SOURCES
    ${TEST_SOURCES}
    LIBS
    mcp_tools
    Salts::TinyTest
    INCLUDES
    include
    FOLDER
    "MCP Tools/tests")
endif()
