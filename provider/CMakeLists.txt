cmake_minimum_required(VERSION 3.20)
project(provider LANGUAGES C)

include(CmakeUtils)
cmake_add_source(SOURCES)
add_library(turbo_provider SHARED ${SOURCES})
target_compile_definitions(turbo_provider PRIVATE SHARED_CXX)
target_include_directories(turbo_provider PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(turbo_provider PUBLIC TurboAgent::RuntimeData
  PRIVATE Salts::Mustache)
cmake_config_target(turbo_provider FOLDER "Provider" EXPORT_NAME Provider
                    ALIAS TurboAgent::Provider)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION include)

if(BUILD_TESTS)
  file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "tests/test_*.c")
  cmake_add_test(SOURCES ${TEST_SOURCES}
    LIBS TurboAgent::Provider Salts::TinyTest FOLDER "Provider/tests")
endif()
