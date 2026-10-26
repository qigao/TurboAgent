cmake_minimum_required(VERSION 3.20)
project(harness LANGUAGES C)

include(CmakeUtils)
add_library(turbo_agent_harness SHARED
  src/turbo_agent_job.c
  src/turbo_agent_harness.c
  src/turbo_agent_harness_server.c
  src/turbo_agent_harness_transport.c)
target_compile_definitions(turbo_agent_harness PRIVATE SHARED_CXX)
target_include_directories(turbo_agent_harness PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(turbo_agent_harness
  PUBLIC TurboAgent::Agent TurboAgent::AgentCompiler
  PRIVATE Salts::CSTL)
cmake_config_target(turbo_agent_harness FOLDER "Harness" EXPORT_NAME Harness
                    ALIAS TurboAgent::Harness)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION include)

if(BUILD_TESTS)
  cmake_add_test(SOURCES tests/test_turbo_agent_job.c tests/test_turbo_agent_harness_store.c tests/test_turbo_agent_harness.c tests/test_turbo_agent_harness_server.c tests/test_turbo_agent_harness_compiled.c
    tests/test_turbo_document_render_example.c
    tests/test_turbo_agent_public_api.c
    LIBS TurboAgent::Harness Salts::TinyTest FOLDER "Harness/tests")
  if(NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(tests/process_crash)
  endif()
endif()
