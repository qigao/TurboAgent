cmake_minimum_required(VERSION 3.20)
project(runtime_codec LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(CmakeUtils)

cmake_add_source(SOURCES)

add_library(runtime_codec SHARED ${SOURCES})
target_compile_definitions(runtime_codec PRIVATE SHARED_CXX)
target_include_directories(
  runtime_codec PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                       $<INSTALL_INTERFACE:include>)
target_include_directories(runtime_codec PRIVATE ${CMAKE_SOURCE_DIR}/vendor/mir)
target_link_libraries(runtime_codec PUBLIC TurboNet::RuntimeData PRIVATE mir_static)

cmake_config_target(runtime_codec FOLDER "Runtime Codec" ALIAS TurboNet::RuntimeCodec)

if(BUILD_TESTS)
  file(GLOB TEST_SOURCES "tests/test_*.c")
  cmake_add_test(
    SOURCES
    ${TEST_SOURCES}
    LIBS
    runtime_codec
    TurboNet::TinyTest
    INCLUDES
    include
    FOLDER
    "Runtime Codec/tests")
endif()
