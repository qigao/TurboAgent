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
target_link_libraries(runtime_tools PUBLIC TurboNet::RuntimeData TurboNet::Parser)

cmake_config_target(runtime_tools FOLDER "Runtime Tools" ALIAS TurboNet::RuntimeTools)

if(BUILD_TESTS)
  file(GLOB TEST_SOURCES "tests/test_*.c")
  cmake_add_test(
    SOURCES
    ${TEST_SOURCES}
    LIBS
    runtime_tools
    TurboNet::TinyTest
    INCLUDES
    include
    FOLDER
    "Runtime Tools/tests")
endif()
