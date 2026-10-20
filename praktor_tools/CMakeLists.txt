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
# JsonParser is an installed STATIC SDK archive and calls CSTL's
# hash_bytes/hash_key_equal. The Salts SDK also supplies shared libraries,
# so an ordinary PRIVATE archive reference may be treated as already
# satisfied by a different DSO. That leaves an UND hash_bytes relocation
# in THIS DSO and an ELF hidden-symbol collision at final executable link.
#
# Force the precise hidden CSTL archive to be included in the producer
# SHARED image on Linux. This intentionally preserves image-local data
# ownership and never exports or redefines hash_bytes or changes public ABI.
set(_praktor_cstl_owner Salts::CSTL)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_praktor_cstl_owner "$<LINK_LIBRARY:WHOLE_ARCHIVE,Salts::CSTL>")
endif()
target_link_libraries(
  praktor_tools
  PUBLIC TurboAgent::RuntimeTools Salts::JsonParser
  PRIVATE Praktor::Praktor Salts::Core ${_praktor_cstl_owner})
unset(_praktor_cstl_owner)

# A shared-object build is not valid if it relies on an executable to
# provide an unresolved CSTL implementation. Check at the owning DSO
# link, including Linux CI's parent --allow-shlib-undefined flags.
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
