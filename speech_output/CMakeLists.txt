cmake_minimum_required(VERSION 3.20)

# Can be configured by itself: no Salts/Whisper/Agent/model dependency for
# deterministic M4.0 tests. Root includes it only behind ENABLE_SPEECH_OUTPUT.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(TurboAgentSpeechOutputStandalone LANGUAGES C CXX)
  include(CTest)
  enable_testing()
  set(TURBO_TTS_STANDALONE ON)
endif()

add_library(turbo_agent_speech_output STATIC
  src/turbo_agent_speech_output.c)
add_library(TurboAgent::SpeechOutput ALIAS turbo_agent_speech_output)
set_target_properties(turbo_agent_speech_output PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
  EXPORT_NAME SpeechOutput POSITION_INDEPENDENT_CODE ON)
target_compile_features(turbo_agent_speech_output PUBLIC c_std_11)
target_include_directories(turbo_agent_speech_output PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

include(GNUInstallDirs)
if(TURBO_TTS_STANDALONE)
  # Make the public C11 ABI independently consumable in M4.0 without
  # installing any Agent, Salts, playback-device or model dependency.
  install(TARGETS turbo_agent_speech_output
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
else()
  install(TARGETS turbo_agent_speech_output EXPORT TurboAgentTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(FILES include/turbo_agent_speech_output.h DESTINATION include)

if(BUILD_TESTING)
  add_executable(test_turbo_agent_speech_output
    tests/test_turbo_agent_speech_output.c
    tests/fake_tts_provider.c)
  target_include_directories(test_turbo_agent_speech_output PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(test_turbo_agent_speech_output
    PRIVATE turbo_agent_speech_output)
  target_compile_features(test_turbo_agent_speech_output PRIVATE c_std_11)
  add_test(NAME turbo_agent_speech_output_core
    COMMAND test_turbo_agent_speech_output)
  set_tests_properties(turbo_agent_speech_output_core
    PROPERTIES TIMEOUT 15)

  add_executable(test_turbo_agent_speech_output_cpp17
    tests/test_turbo_agent_speech_output_cpp17.cpp)
  target_link_libraries(test_turbo_agent_speech_output_cpp17
    PRIVATE turbo_agent_speech_output)
  target_compile_features(test_turbo_agent_speech_output_cpp17 PRIVATE cxx_std_17)
  add_test(NAME turbo_agent_speech_output_cpp17
    COMMAND test_turbo_agent_speech_output_cpp17)
  set_tests_properties(turbo_agent_speech_output_cpp17
    PROPERTIES TIMEOUT 15)
endif()
