# This is evaluated at the end of the root real TurboAgent project configure.
if(NOT TARGET TurboAgent::Harness OR NOT TARGET TurboAgent::Agent)
  message(FATAL_ERROR "Original full-source real Harness/Agent must be built")
endif()
if(NOT TARGET Salts::JsonParser OR NOT TARGET Salts::Playback)
  message(FATAL_ERROR "Actual released Salts JsonParser/Playback targets required")
endif()
if(NOT EXISTS "${VOICE_M42_PREBUILT_ROOT}/libturbo_agent_speech_output.a" OR
   NOT EXISTS "${VOICE_M42_PREBUILT_ROOT}/playback/libturbo_tts_playback_bridge.a" OR
   NOT EXISTS "${VOICE_M42_PREBUILT_ROOT}/voice_coordinator/libturbo_tts_voice_coordinator.a")
  message(FATAL_ERROR "Current-head M4.0/M4.1/M4.2 concrete archives missing")
endif()

add_executable(test_tts_m42_actual_harness
  "${CMAKE_CURRENT_SOURCE_DIR}/test_real_harness.c")
target_compile_features(test_tts_m42_actual_harness PRIVATE c_std_11)
set_target_properties(test_tts_m42_actual_harness PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(test_tts_m42_actual_harness PRIVATE
  "${VOICE_M42_WORKSPACE}/speech_output/include"
  "${VOICE_M42_WORKSPACE}/speech_output/playback/include"
  "${VOICE_M42_WORKSPACE}/speech_output/voice_coordinator/include"
  "${VOICE_M42_WORKSPACE}/speech_output/voice_coordinator/tests/harness/include"
  "$ENV{SALTS_UTILS_ROOT}/include"
  "$ENV{SALTS_ROOT}/include")
target_compile_options(test_tts_m42_actual_harness PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(test_tts_m42_actual_harness PRIVATE
  "${VOICE_M42_PREBUILT_ROOT}/voice_coordinator/tests/harness/libturbo_voice_harness_final_gate.a"
  "${VOICE_M42_PREBUILT_ROOT}/voice_coordinator/libturbo_tts_voice_coordinator.a"
  "${VOICE_M42_PREBUILT_ROOT}/playback/libturbo_tts_playback_bridge.a"
  "${VOICE_M42_PREBUILT_ROOT}/libturbo_agent_speech_output.a"
  TurboAgent::Harness
  Salts::JsonParser
  Threads::Threads)
add_test(NAME tts_m42_actual_harness
  COMMAND test_tts_m42_actual_harness)
set_tests_properties(tts_m42_actual_harness PROPERTIES TIMEOUT 90)
