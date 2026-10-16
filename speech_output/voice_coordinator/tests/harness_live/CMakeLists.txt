# This is evaluated at the end of the root real TurboAgent project configure.
# Additional opt-in M3 source / vendor header pins. These are ORIGINAL
# separately reviewed source snapshots; no library, weights or model fallback.
if(NOT DEFINED VOICE_M42_M3_SOURCE_ROOT OR
   NOT DEFINED VOICE_M42_WHISPER_HEADER_ROOT OR
   NOT EXISTS "${VOICE_M42_M3_SOURCE_ROOT}/speech_input/whisper/src/turbo_agent_speech_whisper.c" OR
   NOT EXISTS "${VOICE_M42_M3_SOURCE_ROOT}/speech_input/whisper/tests/whisper_fake_engine_image.c" OR
   NOT EXISTS "${VOICE_M42_WHISPER_HEADER_ROOT}/include/whisper.h")
  message(FATAL_ERROR "Missing exact M3 C11 original source or pinned upstream v1.9.5 Whisper API")
endif()
if(NOT VOICE_M42_M3_PINNED_SHA STREQUAL
       "4614360e6ee2e174d96cbdd9ce41c5388c81d064" OR
   NOT VOICE_M42_WHISPER_PINNED_SHA STREQUAL
       "d1be6fde11ac6e0407606b4e42fe72d34add8037")
  message(FATAL_ERROR "M3/Whisper public C ABI source digest pin mismatch")
endif()

if(NOT TARGET TurboAgent::Harness OR NOT TARGET TurboAgent::Agent)
  message(FATAL_ERROR "Original full-source real Harness/Agent must be built")
endif()
if(NOT TARGET turbo_agent_speech_input OR
   NOT TARGET turbo_agent_speech_capture_bridge)
  message(FATAL_ERROR "Original pinned M1/M2 production targets are required")
endif()
if(ENABLE_SPEECH_CAPTURE_NATIVE)
  message(FATAL_ERROR "No real device may be opened by this test")
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
  "${VOICE_M42_WORKSPACE}/speech_output/voice_coordinator/tests/harness_live/test_real_harness.c"
  "${VOICE_M42_M3_SOURCE_ROOT}/speech_input/whisper/src/turbo_agent_speech_whisper.c"
  "${VOICE_M42_M3_SOURCE_ROOT}/speech_input/whisper/tests/whisper_fake_engine_image.c")
target_compile_features(test_tts_m42_actual_harness PRIVATE c_std_11)
set_target_properties(test_tts_m42_actual_harness PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(test_tts_m42_actual_harness PRIVATE
  "${VOICE_M42_WORKSPACE}/speech_output/include"
  "${VOICE_M42_WORKSPACE}/speech_output/playback/include"
  "${VOICE_M42_WORKSPACE}/speech_output/voice_coordinator/include"
  "${VOICE_M42_WORKSPACE}/speech_output/voice_coordinator/tests/harness/include"
  "${VOICE_M42_M3_SOURCE_ROOT}/speech_input/whisper/include"
  "${VOICE_M42_WHISPER_HEADER_ROOT}/include"
  "${VOICE_M42_WHISPER_HEADER_ROOT}/ggml/include"
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
  turbo_agent_speech_capture_bridge
  Salts::JsonParser
  Salts::Core
  Threads::Threads)
add_test(NAME tts_m42_actual_harness
  COMMAND test_tts_m42_actual_harness)
set_tests_properties(tts_m42_actual_harness PROPERTIES TIMEOUT 90)

# M4.3 opt-in REAL upstream Whisper v1.9.5 CPU runtime with NO model weights.
# Do not link the original test-only fake engine into this second executable.
# Public M1/M2, actual Harness/Session and current-head M4 still use the
# identical published/source ABI. This tests only real CPU/model rejection.
if(NOT DEFINED ENV{TURBO_WHISPER_SDK_ROOT} OR
   NOT IS_DIRECTORY "$ENV{TURBO_WHISPER_SDK_ROOT}")
  message(FATAL_ERROR "Genuine Whisper runtime negative requires explicit CPU SDK root")
endif()
find_package(whisper 1.9.5 EXACT CONFIG REQUIRED
  PATHS "$ENV{TURBO_WHISPER_SDK_ROOT}"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT TARGET whisper)
  message(FATAL_ERROR "Pinned genuine Whisper CPU SDK does not export target whisper")
endif()
add_executable(test_tts_m42_real_whisper_cpu_no_weights
  "${VOICE_M42_WORKSPACE}/speech_output/voice_coordinator/tests/harness_live/test_real_harness.c"
  "${VOICE_M42_M3_SOURCE_ROOT}/speech_input/whisper/src/turbo_agent_speech_whisper.c")
target_compile_features(test_tts_m42_real_whisper_cpu_no_weights PRIVATE c_std_11)
set_target_properties(test_tts_m42_real_whisper_cpu_no_weights PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_compile_definitions(test_tts_m42_real_whisper_cpu_no_weights PRIVATE
  M42_REAL_VENDOR_NEGATIVE=1)
target_include_directories(test_tts_m42_real_whisper_cpu_no_weights PRIVATE
  "${VOICE_M42_WORKSPACE}/speech_output/include"
  "${VOICE_M42_WORKSPACE}/speech_output/playback/include"
  "${VOICE_M42_WORKSPACE}/speech_output/voice_coordinator/include"
  "${VOICE_M42_WORKSPACE}/speech_output/voice_coordinator/tests/harness/include"
  "${VOICE_M42_M3_SOURCE_ROOT}/speech_input/whisper/include"
  "$ENV{SALTS_UTILS_ROOT}/include"
  "$ENV{SALTS_ROOT}/include")
target_compile_options(test_tts_m42_real_whisper_cpu_no_weights PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(test_tts_m42_real_whisper_cpu_no_weights PRIVATE
  "${VOICE_M42_PREBUILT_ROOT}/voice_coordinator/tests/harness/libturbo_voice_harness_final_gate.a"
  "${VOICE_M42_PREBUILT_ROOT}/voice_coordinator/libturbo_tts_voice_coordinator.a"
  "${VOICE_M42_PREBUILT_ROOT}/playback/libturbo_tts_playback_bridge.a"
  "${VOICE_M42_PREBUILT_ROOT}/libturbo_agent_speech_output.a"
  turbo_agent_speech_capture_bridge
  TurboAgent::Harness
  Salts::JsonParser
  Salts::Core
  whisper
  Threads::Threads)
add_test(NAME tts_m42_real_whisper_cpu_no_weights
  COMMAND test_tts_m42_real_whisper_cpu_no_weights)
set_tests_properties(tts_m42_real_whisper_cpu_no_weights
  PROPERTIES TIMEOUT 30)
