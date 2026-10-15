cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
 project(TurboAgentSpeechOutput LANGUAGES C CXX)
 include(CTest)
 enable_testing()
endif()
add_library(turbo_agent_speech_output STATIC src/turbo_agent_speech_output.c)
add_library(TurboAgent::SpeechOutput ALIAS turbo_agent_speech_output)
target_compile_features(turbo_agent_speech_output PUBLIC c_std_11)
set_target_properties(turbo_agent_speech_output PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO POSITION_INDEPENDENT_CODE ON)
target_include_directories(turbo_agent_speech_output PUBLIC
 "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
 "$<INSTALL_INTERFACE:include>")
if(MSVC)
 target_compile_options(turbo_agent_speech_output PRIVATE /W4)
else()
 target_compile_options(turbo_agent_speech_output PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# Standalone opt-in export only; not installed by the main TurboAgent build.
include(GNUInstallDirs)
install(TARGETS turbo_agent_speech_output
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/turbo_agent_speech_output.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(BUILD_TESTING)
 add_executable(test_speech_output_fake tests/test_speech_output.c)
 target_link_libraries(test_speech_output_fake PRIVATE turbo_agent_speech_output)
 add_test(NAME speech_output_fake COMMAND test_speech_output_fake)
 add_executable(test_speech_output_cpp17 tests/test_speech_output_cpp17.cpp)
 target_link_libraries(test_speech_output_cpp17 PRIVATE turbo_agent_speech_output)
 target_compile_features(test_speech_output_cpp17 PRIVATE cxx_std_17)
 add_test(NAME speech_output_cpp17 COMMAND test_speech_output_cpp17)
endif()

# M4.1 is explicitly selected by a caller: never build or find Playback
# in the normal standalone M4.0 fake provider contract.
option(ENABLE_TTS_PLAYBACK_BRIDGE_TEST
  "Build opt-in playback adapter against exact installed Salts Playback" OFF)
if(ENABLE_TTS_PLAYBACK_BRIDGE_TEST)
  add_subdirectory(playback)
endif()
