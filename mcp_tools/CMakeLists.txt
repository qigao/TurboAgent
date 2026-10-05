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
if(UNIX)
  target_link_libraries(mcp_tools PRIVATE m)
endif()

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
  if(TARGET test_turbo_mcp_tool_pack)
    # This suite uses only the injected in-memory transport. A wait longer than
    # one minute is a deadlock/regression, not legitimate network latency.
    set_tests_properties(test_turbo_mcp_tool_pack PROPERTIES TIMEOUT 60)
    if(WIN32)
      set(_mcp_diag_filters
        "validates its bounded"
        "discovers tools"
        "accepts request-scoped SSE"
        "keeps the previous registry")
      set(_mcp_diag_index 0)
      foreach(_mcp_filter IN LISTS _mcp_diag_filters)
        math(EXPR _mcp_diag_index "${_mcp_diag_index} + 1")
        set(_mcp_diag_name "diag_turbo_mcp_${_mcp_diag_index}")
        add_test(NAME ${_mcp_diag_name}
                 COMMAND $<TARGET_FILE:test_turbo_mcp_tool_pack>
                         --filter "${_mcp_filter}")
        set_tests_properties(${_mcp_diag_name} PROPERTIES TIMEOUT 15)
      endforeach()
      unset(_mcp_diag_filters)
      unset(_mcp_diag_index)
    endif()
  endif()
endif()
