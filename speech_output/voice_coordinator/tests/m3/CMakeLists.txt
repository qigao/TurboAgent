# Exact immutable original M3 C11 Provider under the SAME sanitizer C flags as
# M4.2 and original M1. No model weights are fetched, uploaded, or bundled.
# The original #145 fake-engine fixture is used unchanged against the true
# reviewed v1.9.5 public Whisper/GGML C ABI headers.
if(NOT BUILD_TESTING OR NOT TARGET turbo_agent_speech_input)
  message(FATAL_ERROR "M3 original source sanitizer requires real pinned M1 and BUILD_TESTING")
endif()
if(NOT DEFINED SPEECH_M3_SOURCE_ROOT OR
   NOT EXISTS "${SPEECH_M3_SOURCE_ROOT}/speech_input/whisper/src/turbo_agent_speech_whisper.c" OR
   NOT EXISTS "${SPEECH_M3_SOURCE_ROOT}/speech_input/whisper/tests/test_turbo_agent_speech_whisper_mock.c" OR
   NOT DEFINED SPEECH_M3_WHISPER_HEADER_ROOT OR
   NOT EXISTS "${SPEECH_M3_WHISPER_HEADER_ROOT}/include/whisper.h" OR
   NOT EXISTS "${SPEECH_M3_WHISPER_HEADER_ROOT}/ggml/include/ggml.h")
  message(FATAL_ERROR "M3/Whisper exact source snapshots absent — no fallback")
endif()
if(NOT SPEECH_M3_PINNED_SHA STREQUAL "4614360e6ee2e174d96cbdd9ce41c5388c81d064" OR
   NOT SPEECH_M3_WHISPER_PINNED_SHA STREQUAL
       "d1be6fde11ac6e0407606b4e42fe72d34add8037")
  message(FATAL_ERROR "M3/Whisper exact source identity mismatch")
endif()
if(NOT DEFINED ENV{SALTS_ROOT} OR NOT IS_DIRECTORY "$ENV{SALTS_ROOT}")
  message(FATAL_ERROR "M3 sanitized consumer requires explicit published Salts SDK root")
endif()
find_package(Salts CONFIG REQUIRED PATHS "$ENV{SALTS_ROOT}"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT TARGET Salts::Core)
  message(FATAL_ERROR "M3 sanitized source requires original Salts::Core SHA-256")
endif()
find_package(Threads REQUIRED)
add_executable(test_tts_m42_original_m3_whisper_sanitized
  "${SPEECH_M3_SOURCE_ROOT}/speech_input/whisper/src/turbo_agent_speech_whisper.c"
  "${SPEECH_M3_SOURCE_ROOT}/speech_input/whisper/tests/test_turbo_agent_speech_whisper_mock.c")
target_compile_features(test_tts_m42_original_m3_whisper_sanitized PRIVATE c_std_11)
set_target_properties(test_tts_m42_original_m3_whisper_sanitized PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(test_tts_m42_original_m3_whisper_sanitized PRIVATE
  "${SPEECH_M3_SOURCE_ROOT}/speech_input/whisper/include"
  "${SPEECH_M3_WHISPER_HEADER_ROOT}/include"
  "${SPEECH_M3_WHISPER_HEADER_ROOT}/ggml/include")
target_link_libraries(test_tts_m42_original_m3_whisper_sanitized PRIVATE
  turbo_agent_speech_input Salts::Core Threads::Threads m)
target_compile_options(test_tts_m42_original_m3_whisper_sanitized PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
add_test(NAME tts_m42_original_m3_whisper_sanitized
  COMMAND test_tts_m42_original_m3_whisper_sanitized)
set_tests_properties(tts_m42_original_m3_whisper_sanitized
  PROPERTIES TIMEOUT 60)
