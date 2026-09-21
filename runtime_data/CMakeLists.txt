cmake_minimum_required(VERSION 3.20)
project(runtime_data LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(CmakeUtils)

cmake_add_source(SOURCES)

add_library(runtime_data SHARED ${SOURCES})
target_compile_definitions(runtime_data PRIVATE SHARED_CXX)
target_include_directories(
  runtime_data PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                      $<INSTALL_INTERFACE:include>)
target_link_libraries(runtime_data PUBLIC Salts::JsonParser Salts::Core)

cmake_config_target(runtime_data FOLDER "Runtime Data" EXPORT_NAME RuntimeData
                    ALIAS TurboAgent::RuntimeData)
add_library(TurboNet::RuntimeData ALIAS runtime_data)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION include)

if(BUILD_TESTS)
  file(GLOB TEST_SOURCES "tests/test_*.c")
  cmake_add_test(
    SOURCES
    ${TEST_SOURCES}
    LIBS
    runtime_data
    Salts::CYamlJsonAdapter
    Salts::XmlParser
    Salts::TinyTest
    INCLUDES
    include
    FOLDER
    "Runtime Data/tests")
endif()
