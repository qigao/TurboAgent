cmake_minimum_required(VERSION 3.20)
project(harness LANGUAGES C)

include(CmakeUtils)
add_library(turbo_agent_harness SHARED
  src/turbo_agent_job_observation.c
  src/turbo_agent_job_execution.c
  src/turbo_agent_job_file_verifier.c
  src/turbo_agent_job_delivery.c
  src/turbo_agent_job_acceptance.c
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
  PRIVATE Salts::CSTL Salts::Core)
cmake_config_target(turbo_agent_harness FOLDER "Harness" EXPORT_NAME Harness
                    ALIAS TurboAgent::Harness)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION include)

if(BUILD_TESTS)
  cmake_add_test(SOURCES tests/test_turbo_agent_job_execution.c tests/test_turbo_agent_job_file_verifier.c tests/test_turbo_agent_job.c tests/test_turbo_agent_harness_store.c tests/test_turbo_agent_harness.c tests/test_turbo_agent_harness_server.c tests/test_turbo_agent_harness_compiled.c
    tests/test_turbo_document_render_example.c
    tests/test_turbo_agent_public_api.c
    LIBS TurboAgent::Harness Salts::TinyTest FOLDER "Harness/tests")
  set_tests_properties(test_turbo_agent_harness_compiled PROPERTIES TIMEOUT 60)
  set_tests_properties(test_turbo_agent_job_execution PROPERTIES TIMEOUT 60 LABELS "harness;job;async;cancellation")
elseif(TURBOAGENT_JOB_FILE_CONFORMANCE)
  cmake_add_test(SOURCES tests/test_turbo_agent_job_file_verifier.c
    LIBS TurboAgent::Harness Salts::TinyTest FOLDER "Harness/tests")
endif()
if(BUILD_TESTS OR TURBOAGENT_JOB_FILE_CONFORMANCE)
  set_tests_properties(test_turbo_agent_job_file_verifier PROPERTIES TIMEOUT 60 LABELS "harness;job;file-verification")
  if(NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(tests/process_crash)
  endif()
endif()
