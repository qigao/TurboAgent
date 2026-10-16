# Test-only REAL pinned M1/M2 SpeechInput-to-M4.2 Host contract integration.
# The ASR/Capture bridge is compiled directly from exact PR #136 commit.
# Nothing is copied into or installed from the M4 production SDK.
if(NOT BUILD_TESTING)
  message(FATAL_ERROR "M4.2 host handoff requires BUILD_TESTING")
endif()
if(NOT DEFINED SPEECH_M2_SOURCE_ROOT OR
   NOT EXISTS "${SPEECH_M2_SOURCE_ROOT}/speech_input/CMakeLists.txt" OR
   NOT EXISTS "${SPEECH_M2_SOURCE_ROOT}/speech_input/capture/include/turbo_agent_speech_capture.h")
  message(FATAL_ERROR "M4.2 Host must consume independently checked-out pinned M1/M2 source")
endif()
if(TARGET turbo_agent_speech_input OR TARGET turbo_agent_speech_capture_bridge)
  message(FATAL_ERROR "M4.2 Host must not reuse an unverified SpeechInput target")
endif()
set(ENABLE_SPEECH_CAPTURE_BRIDGE ON CACHE BOOL "" FORCE)
set(ENABLE_SPEECH_CAPTURE_NATIVE OFF CACHE BOOL "" FORCE)
add_subdirectory("${SPEECH_M2_SOURCE_ROOT}/speech_input"
                 "${CMAKE_BINARY_DIR}/m42-pinned-m2-speech-input")

find_package(Threads REQUIRED)
add_executable(test_tts_m42_real_m2_host
  test_real_m2_host_handoff.c)
target_compile_features(test_tts_m42_real_m2_host PRIVATE c_std_11)
set_target_properties(test_tts_m42_real_m2_host PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_link_libraries(test_tts_m42_real_m2_host PRIVATE
  turbo_tts_voice_coordinator turbo_agent_speech_capture_bridge Threads::Threads)
target_compile_options(test_tts_m42_real_m2_host PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
add_test(NAME tts_m42_real_m2_host_handoff
  COMMAND test_tts_m42_real_m2_host)
set_tests_properties(tts_m42_real_m2_host_handoff
  PROPERTIES TIMEOUT 30)
