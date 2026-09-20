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
  PUBLIC TurboAgent::RuntimeTools TurboParser::Parser
  PRIVATE Praktor::Praktor TurboUtils::Core)

cmake_config_target(praktor_tools FOLDER "Praktor Tools" EXPORT_NAME PraktorTools
                    ALIAS TurboAgent::PraktorTools)
add_library(TurboNet::PraktorTools ALIAS praktor_tools)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION include)

if(BUILD_TESTS)
  file(GLOB TEST_SOURCES "tests/test_*.c")
  cmake_add_test(
    SOURCES
    ${TEST_SOURCES}
    LIBS
    praktor_tools
    TurboUtils::TinyTest
    INCLUDES
    include
    FOLDER
    "Praktor Tools/tests")
endif()
