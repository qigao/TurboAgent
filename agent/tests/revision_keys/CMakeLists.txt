cmake_minimum_required(VERSION 3.20)

# This exact test/source pair also runs without SDKs or header stubs. It checks
# the private identity format only, not TurboDB persistence or Agent ACLs.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(TurboAgentRevisionKeys LANGUAGES C)
  enable_testing()
endif()

add_executable(test_turbo_agent_revision_keys
  revision_keys_test.c ../../src/turbo_agent_revision_keys.c)
target_include_directories(test_turbo_agent_revision_keys PRIVATE ../../src)
set_target_properties(test_turbo_agent_revision_keys PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
  target_compile_options(test_turbo_agent_revision_keys PRIVATE /W4 /WX)
else()
  target_compile_options(test_turbo_agent_revision_keys PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Werror)
endif()
add_test(NAME test_turbo_agent_revision_keys COMMAND test_turbo_agent_revision_keys)
set_tests_properties(test_turbo_agent_revision_keys PROPERTIES TIMEOUT 20)
