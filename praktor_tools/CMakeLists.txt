cmake_minimum_required(VERSION 3.20)
project(praktor_tools LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(CmakeUtils)

cmake_add_source(SOURCES)

add_library(praktor_tools SHARED ${SOURCES})
target_compile_definitions(praktor_tools PRIVATE SHARED_CXX)
target_include_directories(
  praktor_tools PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                       $<INSTALL_INTERFACE:include>)
target_link_libraries(
  praktor_tools
  PUBLIC TurboAgent::RuntimeTools Salts::JsonParser
  # PraktorTools must own CSTL callbacks linked into this DSO, not resolve
  # hash_bytes from the executable or another module's hidden static archive.
  PRIVATE Salts::CSTL Praktor::Praktor Salts::Core)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # The callback is reached through a dependency DSO, which does not always
  # pull its PRIVATE static archive member into this shared image. Force only
  # this CSTL implementation into PraktorTools, retaining hidden visibility.
  target_link_options(praktor_tools PRIVATE
    "LINKER:-u,hash_bytes"
    "LINKER:-z,defs")
endif()

cmake_config_target(praktor_tools FOLDER "Praktor Tools" EXPORT_NAME PraktorTools
                    ALIAS TurboAgent::PraktorTools)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION include)

if(BUILD_TESTS)
  file(GLOB TEST_SOURCES "tests/test_*.c")
  cmake_add_test(
    SOURCES
    ${TEST_SOURCES}
    LIBS
    praktor_tools
    agent_compiler
    Salts::Core
    Salts::TinyTest
    INCLUDES
    include
    FOLDER
    "Praktor Tools/tests")

  # Verify that linking the imported static CSTL really put hash_bytes
  # inside this image, without leaving a dynamic import or public export.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(
      NAME turbo_praktor_tools_private_cstl_symbols
      COMMAND "${CMAKE_COMMAND}"
        "-DNATIVE_IMAGE=$<TARGET_FILE:praktor_tools>"
        "-DNM_TOOL=${CMAKE_NM}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ci/verify-praktor-cstl-image.cmake")
  endif()
endif()
