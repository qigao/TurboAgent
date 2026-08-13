cmake_minimum_required(VERSION 3.20)
project(runtime_tools LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(CmakeUtils)

cmake_add_source(SOURCES)

add_library(runtime_tools SHARED ${SOURCES})
target_compile_definitions(runtime_tools PRIVATE SHARED_CXX)
target_include_directories(
  runtime_tools PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                       $<INSTALL_INTERFACE:include>)
target_link_libraries(runtime_tools PUBLIC TurboAgent::RuntimeData TurboParser::Parser)

cmake_config_target(runtime_tools FOLDER "Runtime Tools" EXPORT_NAME RuntimeTools
                    ALIAS TurboAgent::RuntimeTools)
add_library(TurboNet::RuntimeTools ALIAS runtime_tools)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION include)

if(BUILD_TESTS)
  file(GLOB TEST_SOURCES "tests/test_*.c")
  cmake_add_test(
    SOURCES
    ${TEST_SOURCES}
    LIBS
    runtime_tools
    TurboUtils::TinyTest
    INCLUDES
    include
    FOLDER
    "Runtime Tools/tests")
endif()
