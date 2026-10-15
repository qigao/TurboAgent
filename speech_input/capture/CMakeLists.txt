cmake_minimum_required(VERSION 3.20)

add_library(turbo_agent_speech_capture_bridge STATIC
  src/turbo_agent_speech_capture_bridge.c)
add_library(TurboAgent::SpeechCaptureBridge ALIAS
  turbo_agent_speech_capture_bridge)
target_compile_features(turbo_agent_speech_capture_bridge PUBLIC c_std_11)
set_target_properties(turbo_agent_speech_capture_bridge PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
  POSITION_INDEPENDENT_CODE ON EXPORT_NAME SpeechCaptureBridge)
target_include_directories(turbo_agent_speech_capture_bridge PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(turbo_agent_speech_capture_bridge
  PUBLIC turbo_agent_speech_input)
if(MSVC)
  target_compile_options(turbo_agent_speech_capture_bridge PRIVATE
    /experimental:c11atomics)
endif()

if(NOT TURBO_SPEECH_STANDALONE)
  include(GNUInstallDirs)
  install(TARGETS turbo_agent_speech_capture_bridge EXPORT TurboAgentTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES include/turbo_agent_speech_capture.h DESTINATION include)
endif()

if(BUILD_TESTING)
  add_executable(test_turbo_agent_speech_capture_bridge
    tests/test_turbo_agent_speech_capture_bridge.c)
  target_link_libraries(test_turbo_agent_speech_capture_bridge PRIVATE
    turbo_agent_speech_capture_bridge)
  add_test(NAME turbo_agent_speech_capture_bridge_core
    COMMAND test_turbo_agent_speech_capture_bridge)
  set_tests_properties(turbo_agent_speech_capture_bridge_core
    PROPERTIES TIMEOUT 20)
endif()

if(BUILD_TESTING AND UNIX)
  find_package(Threads REQUIRED)
  add_executable(test_turbo_agent_speech_capture_concurrent
    tests/test_turbo_agent_speech_capture_concurrent.c)
  target_compile_features(test_turbo_agent_speech_capture_concurrent PRIVATE c_std_11)
  target_link_libraries(test_turbo_agent_speech_capture_concurrent PRIVATE
    turbo_agent_speech_capture_bridge Threads::Threads)
  add_test(NAME turbo_agent_speech_capture_bridge_concurrent
    COMMAND test_turbo_agent_speech_capture_concurrent)
  set_tests_properties(turbo_agent_speech_capture_bridge_concurrent
    PROPERTIES TIMEOUT 30)
endif()

# Salts Capture is an explicit optional native provider, not a required
# dependency of TurboAgent::SpeechInput or its portable capture bridge.
# A requested but absent Salts::Capture target is a hard configuration error.
if(ENABLE_SPEECH_CAPTURE_NATIVE)
  if(TURBO_SPEECH_STANDALONE)
    message(FATAL_ERROR
      "Native Capture requires an installed SaltsUtils SDK and top-level TurboAgent")
  endif()
  if(NOT TARGET Salts::Capture)
    message(FATAL_ERROR
      "ENABLE_SPEECH_CAPTURE_NATIVE requires an installed Salts::Capture target; no backend fallback")
  endif()
  add_library(turbo_agent_speech_capture_salts STATIC
    src/turbo_agent_speech_capture_salts.c)
  add_library(TurboAgent::SpeechCaptureSalts ALIAS
    turbo_agent_speech_capture_salts)
  target_compile_features(turbo_agent_speech_capture_salts PUBLIC c_std_11)
  set_target_properties(turbo_agent_speech_capture_salts PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
    EXPORT_NAME SpeechCaptureSalts POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(turbo_agent_speech_capture_salts
    PUBLIC turbo_agent_speech_capture_bridge
    PRIVATE Salts::Capture)
  if(MSVC)
    target_compile_options(turbo_agent_speech_capture_salts PRIVATE
      /experimental:c11atomics)
  endif()
  install(TARGETS turbo_agent_speech_capture_salts EXPORT TurboAgentTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
