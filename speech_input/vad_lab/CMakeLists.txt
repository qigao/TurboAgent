cmake_minimum_required(VERSION 3.20)
project(TurboAgentStrictC11TriStateVadLab LANGUAGES C)
include(CTest)

# Separate diagnostic subtree, not included by speech_input/CMakeLists,
# not installed/exported, with no production Agent/Whisper/SDK dependency.
add_library(ta_speech_vad_lab STATIC src/ta_speech_vad_lab.c)
target_include_directories(ta_speech_vad_lab PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(ta_speech_vad_lab PUBLIC c_std_11)
set_target_properties(ta_speech_vad_lab PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
  C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(ta_speech_vad_lab PRIVATE /W4 /WX)
else()
  target_compile_options(ta_speech_vad_lab PRIVATE
    -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow)
  if(UNIX)
    target_link_libraries(ta_speech_vad_lab PUBLIC m)
  endif()
endif()

if(BUILD_TESTING)
  add_executable(test_ta_speech_vad_lab
    tests/test_ta_speech_vad_lab.c)
  target_link_libraries(test_ta_speech_vad_lab PRIVATE ta_speech_vad_lab)
  set_target_properties(test_ta_speech_vad_lab PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(NOT MSVC)
    target_compile_options(test_ta_speech_vad_lab PRIVATE
      -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow)
  endif()
  add_test(NAME ta_speech_vad_lab_c11_bounded_owner
    COMMAND test_ta_speech_vad_lab)
  set_tests_properties(ta_speech_vad_lab_c11_bounded_owner
    PROPERTIES TIMEOUT 30)

  # Independent deterministic frequency-domain diagnostic controls, not
  # production voice-rejection admission.
  add_executable(test_ta_speech_vad_multiband
    tests/test_ta_speech_vad_multiband.c)
  target_link_libraries(test_ta_speech_vad_multiband PRIVATE ta_speech_vad_lab)
  set_target_properties(test_ta_speech_vad_multiband PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(NOT MSVC)
    target_compile_options(test_ta_speech_vad_multiband PRIVATE
      -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow)
  endif()
  add_test(NAME ta_speech_vad_multiband_synthetic_controls
    COMMAND test_ta_speech_vad_multiband)
  set_tests_properties(ta_speech_vad_multiband_synthetic_controls
    PROPERTIES TIMEOUT 30)
endif()

# Only the private CLI needs POSIX nofollow/open functions. The C11 library
# API remains platform-neutral; no external DSP or ASR model is needed.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(ta_speech_vad_lab_raw_probe
    tools/ta_speech_vad_lab_raw_probe.c)
  target_link_libraries(ta_speech_vad_lab_raw_probe PRIVATE ta_speech_vad_lab)
  set_target_properties(ta_speech_vad_lab_raw_probe PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_compile_options(ta_speech_vad_lab_raw_probe PRIVATE
    -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow)
endif()
