cmake_minimum_required(VERSION 3.20)
project(codex_bridge LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(CmakeUtils)

cmake_add_source(SOURCES)

add_library(codex_bridge SHARED ${SOURCES})
target_compile_definitions(codex_bridge PRIVATE SHARED_CXX)
target_include_directories(
  codex_bridge PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                      $<INSTALL_INTERFACE:include>)
target_link_libraries(codex_bridge PUBLIC TurboAgent::RuntimeTools
                                          TurboParser::Parser
                                          TurboUtils::Core)

cmake_config_target(codex_bridge FOLDER "Codex Bridge" EXPORT_NAME CodexBridge
                    ALIAS TurboAgent::CodexBridge)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION include)

if(BUILD_TESTS)
  file(GLOB TEST_SOURCES "tests/test_*.c")
  cmake_add_test(SOURCES ${TEST_SOURCES} LIBS codex_bridge TurboUtils::TinyTest
                 INCLUDES include FOLDER "Codex Bridge/tests")
endif()
