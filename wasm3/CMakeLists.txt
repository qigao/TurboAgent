cmake_minimum_required(VERSION 3.20)
project(turbo_wasm3 LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(CmakeUtils)

find_package(unofficial-sqlite3 CONFIG REQUIRED)

set(WASM3_BUILD_EXAMPLES_DEFAULT ON)
if(DEFINED BUILD_EXAMPLES)
  set(WASM3_BUILD_EXAMPLES_DEFAULT ${BUILD_EXAMPLES})
endif()
set(WASM3_BUILD_TESTS_DEFAULT ${BUILD_TESTING})
if(DEFINED BUILD_TESTS)
  set(WASM3_BUILD_TESTS_DEFAULT ${BUILD_TESTS})
endif()

option(WASM3_BUILD_EXAMPLES "Build TurboNet wasm3 examples"
       ${WASM3_BUILD_EXAMPLES_DEFAULT})
option(WASM3_BUILD_TESTS "Build TurboNet wasm3 tests"
       ${WASM3_BUILD_TESTS_DEFAULT})

if(NOT TARGET m3)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../vendor/wasm3
                   ${CMAKE_CURRENT_BINARY_DIR}/vendor/wasm3)
endif()

set(TURBO_WASM3_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(TURBO_WASM3_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(TURBO_WASM3_MODULE_SOURCES
    ${TURBO_WASM3_SRC_DIR}/turbo_wasm3_core.c
    ${TURBO_WASM3_SRC_DIR}/turbo_wasm3_socket.c
    ${TURBO_WASM3_SRC_DIR}/turbo_wasm3_db.c
    ${TURBO_WASM3_SRC_DIR}/turbo_wasm3_http.c
    ${TURBO_WASM3_SRC_DIR}/turbo_wasm3_redis.c
    ${TURBO_WASM3_SRC_DIR}/turbo_wasm3_parser.c)

set(TURBO_WASM3_PUBLIC_HEADERS
    ${TURBO_WASM3_INCLUDE_DIR}/turbo_wasm3.h
    ${TURBO_WASM3_INCLUDE_DIR}/turbo_wasm3_core.h
    ${TURBO_WASM3_INCLUDE_DIR}/turbo_wasm3_socket.h
    ${TURBO_WASM3_INCLUDE_DIR}/turbo_wasm3_db.h
    ${TURBO_WASM3_INCLUDE_DIR}/turbo_wasm3_http.h
    ${TURBO_WASM3_INCLUDE_DIR}/turbo_wasm3_redis.h
    ${TURBO_WASM3_INCLUDE_DIR}/turbo_wasm3_parser.h
    ${TURBO_WASM3_INCLUDE_DIR}/turbo_wasm3_guest_abi.h)

add_library(turbo_wasm3 SHARED
            ${TURBO_WASM3_MODULE_SOURCES}
            ${TURBO_WASM3_PUBLIC_HEADERS}
            ${TURBO_WASM3_SRC_DIR}/turbo_wasm3_internal.h)

set(TURBO_WASM3_CONFIG_ARGS FOLDER "wasm3" EXPORT_NAME Wasm3)
if(NOT TARGET TurboHttp::Wasm3)
  list(APPEND TURBO_WASM3_CONFIG_ARGS ALIAS TurboHttp::Wasm3)
endif()
cmake_config_target(turbo_wasm3 ${TURBO_WASM3_CONFIG_ARGS})

target_include_directories(
  turbo_wasm3
  PUBLIC $<BUILD_INTERFACE:${TURBO_WASM3_INCLUDE_DIR}>
         $<INSTALL_INTERFACE:include/wasm3>
  PRIVATE ${TURBO_WASM3_SRC_DIR})

target_compile_definitions(turbo_wasm3 PRIVATE SHARED_CXX)

target_link_libraries(
  turbo_wasm3
  PUBLIC m3 TurboNet::CoroNet TurboNet::Utils
  PRIVATE unofficial::sqlite3::sqlite3 TurboHttp::HttpClient TurboNet::Redis)

install(FILES ${TURBO_WASM3_PUBLIC_HEADERS}
        DESTINATION include/wasm3)

if(WASM3_BUILD_EXAMPLES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt)
  add_subdirectory(examples)
endif()

if(WASM3_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
  add_subdirectory(tests)
endif()
