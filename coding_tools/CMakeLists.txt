cmake_minimum_required(VERSION 3.20)
project(coding_tools LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(CmakeUtils)
cmake_add_source(SOURCES)

add_library(coding_tools SHARED ${SOURCES})
target_compile_definitions(coding_tools PRIVATE SHARED_CXX)
target_include_directories(
  coding_tools PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                      $<INSTALL_INTERFACE:include>)
target_link_libraries(coding_tools PUBLIC TurboAgent::RuntimeTools Salts::JsonParser
                                  Salts::Core)
cmake_config_target(coding_tools FOLDER "Coding Tools" EXPORT_NAME CodingTools
                    ALIAS TurboAgent::CodingTools)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION include)

if(BUILD_TESTS)
  file(GLOB TEST_SOURCES "tests/test_*.c")
  cmake_add_test(SOURCES ${TEST_SOURCES} LIBS coding_tools Salts::TinyTest
                 INCLUDES include FOLDER "Coding Tools/tests")
endif()
