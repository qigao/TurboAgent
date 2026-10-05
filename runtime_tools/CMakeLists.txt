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
target_link_libraries(runtime_tools PUBLIC TurboAgent::RuntimeCore TurboAgent::RuntimeData Salts::JsonParser Salts::CMeta)
if(UNIX)
  target_link_libraries(runtime_tools PRIVATE m)
endif()

cmake_config_target(runtime_tools FOLDER "Runtime Tools" EXPORT_NAME RuntimeTools
                    ALIAS TurboAgent::RuntimeTools)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION include)

if(BUILD_TESTS)
  file(GLOB TEST_SOURCES "tests/test_*.c")
  cmake_add_test(
    SOURCES
    ${TEST_SOURCES}
    LIBS
    runtime_tools
    Salts::TinyTest
    INCLUDES
    include
    FOLDER
    "Runtime Tools/tests")

  cmake_add_test(
    SOURCES
    tests/test_turbo_tool_native_header_cpp.cpp
    LIBS
    runtime_tools
    FOLDER
    "Runtime Tools/tests")
endif()
