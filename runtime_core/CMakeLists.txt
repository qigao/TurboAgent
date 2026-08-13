cmake_minimum_required(VERSION 3.20)
project(runtime_core LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(CmakeUtils)

cmake_add_source(SOURCES)

add_library(runtime_core SHARED ${SOURCES})
target_compile_definitions(runtime_core PRIVATE SHARED_CXX)
target_include_directories(
  runtime_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                      $<INSTALL_INTERFACE:include>)
target_link_libraries(runtime_core PUBLIC TurboUtils::Core)

cmake_config_target(runtime_core FOLDER "Runtime Core" EXPORT_NAME RuntimeCore
                    ALIAS TurboAgent::RuntimeCore)
add_library(TurboNet::RuntimeCore ALIAS runtime_core)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION include)

if(BUILD_TESTS)
  file(GLOB TEST_SOURCES "tests/test_*.c")
  cmake_add_test(
    SOURCES
    ${TEST_SOURCES}
    LIBS
    runtime_core
    TurboUtils::TinyTest
    INCLUDES
    include
    FOLDER
    "Runtime Core/tests")
endif()
