cmake_minimum_required(VERSION 3.20)
project(TurboAgentCITransportTests LANGUAGES NONE)
enable_testing()
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
add_test(NAME ci_github_packages_transport
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_github_packages.py")
add_test(NAME ci_job_file_conformance_guards
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_job_file_conformance.py")
set_tests_properties(ci_job_file_conformance_guards PROPERTIES
  ENVIRONMENT "JOB_FILE_TEST_CMAKE=${CMAKE_COMMAND}")
