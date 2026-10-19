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
# JsonParser is an installed static archive whose CSTL calls must be owned
# inside this shared object; ordinary linkage can leave a hidden hash_bytes
# reference crossing the DSO boundary on Linux.
set(_praktor_cstl_owner Salts::CSTL)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_praktor_cstl_owner "$<LINK_LIBRARY:WHOLE_ARCHIVE,Salts::CSTL>")
endif()
target_link_libraries(
  praktor_tools
  PUBLIC TurboAgent::RuntimeTools Salts::JsonParser
  PRIVATE Praktor::Praktor Salts::Core ${_praktor_cstl_owner})
unset(_praktor_cstl_owner)

# Fail at the producer DSO link rather than deferring unresolved symbols to
# consumers/executables (CI may otherwise allow undefined shared-library refs).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(praktor_tools PRIVATE "LINKER:-z,defs")
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
endif()
