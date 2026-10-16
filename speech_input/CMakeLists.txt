cmake_minimum_required(VERSION 3.20)

# Configuring this directory alone provides a dependency-free C11 test path.
# Full TurboAgent only includes this target when ENABLE_SPEECH_INPUT=ON.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(TurboAgentSpeechStandalone LANGUAGES C CXX)
  include(CTest)
  enable_testing()
  set(TURBO_SPEECH_STANDALONE ON)
endif()

add_library(turbo_agent_speech_input STATIC src/turbo_agent_speech_input.c)
add_library(TurboAgent::SpeechInput ALIAS turbo_agent_speech_input)
target_compile_features(turbo_agent_speech_input PUBLIC c_std_11)
if(MSVC)
  # Match the repository's C11 baseline: MSVC requires this for stdatomic.h.
  target_compile_options(turbo_agent_speech_input PRIVATE /experimental:c11atomics)
endif()
set_target_properties(turbo_agent_speech_input PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
  EXPORT_NAME SpeechInput POSITION_INDEPENDENT_CODE ON)
target_include_directories(turbo_agent_speech_input PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

if(NOT TURBO_SPEECH_STANDALONE)
  include(GNUInstallDirs)
  install(TARGETS turbo_agent_speech_input EXPORT TurboAgentTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES include/turbo_agent_speech_input.h DESTINATION include)
endif()

if(BUILD_TESTING)
  add_executable(test_turbo_agent_speech_input
    tests/test_turbo_agent_speech_input.c)
  target_link_libraries(test_turbo_agent_speech_input
    PRIVATE turbo_agent_speech_input)
  target_compile_features(test_turbo_agent_speech_input PRIVATE c_std_11)
  add_test(NAME turbo_agent_speech_input_core
    COMMAND test_turbo_agent_speech_input)
endif()

# Unix uses pthreads for a real concurrent capture-producer stress / TSan
# gate. The production module itself uses only standard C11 atomics.
if(BUILD_TESTING AND UNIX)
  find_package(Threads REQUIRED)
  add_executable(test_turbo_agent_speech_concurrent
    tests/test_turbo_agent_speech_concurrent.c)
  target_link_libraries(test_turbo_agent_speech_concurrent
    PRIVATE turbo_agent_speech_input Threads::Threads)
  target_compile_features(test_turbo_agent_speech_concurrent PRIVATE c_std_11)
  add_test(NAME turbo_agent_speech_input_concurrent
    COMMAND test_turbo_agent_speech_concurrent)
  set_tests_properties(turbo_agent_speech_input_concurrent
    PROPERTIES TIMEOUT 20)
endif()

# This is a consumer test only. The production target remains strict C11.
if(BUILD_TESTING)
  add_executable(test_turbo_agent_speech_cpp17
    tests/test_turbo_agent_speech_cpp17.cpp)
  target_link_libraries(test_turbo_agent_speech_cpp17
    PRIVATE turbo_agent_speech_input)
  target_compile_features(test_turbo_agent_speech_cpp17 PRIVATE cxx_std_17)
  add_test(NAME turbo_agent_speech_input_cpp17
    COMMAND test_turbo_agent_speech_cpp17)
endif()
