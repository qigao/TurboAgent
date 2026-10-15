# M4.1 opt-in bridge compiled against a real pinned installed Salts SDK.
if(NOT DEFINED SALTS_PLAYBACK_INCLUDE_DIR OR
   NOT EXISTS "${SALTS_PLAYBACK_INCLUDE_DIR}/salts_playback.h" OR
   NOT EXISTS "${SALTS_PLAYBACK_INCLUDE_DIR}/salts_playback_export.h")
  message(FATAL_ERROR "M4.1 requires the pinned installed Playback header")
endif()

add_library(turbo_tts_playback_bridge STATIC
  src/turbo_agent_tts_playback_bridge.c)
add_library(TurboAgent::SpeechOutputPlaybackBridge
  ALIAS turbo_tts_playback_bridge)
target_compile_features(turbo_tts_playback_bridge PUBLIC c_std_11)
set_target_properties(turbo_tts_playback_bridge PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
  POSITION_INDEPENDENT_CODE ON)
target_link_libraries(turbo_tts_playback_bridge PUBLIC turbo_agent_speech_output)
target_include_directories(turbo_tts_playback_bridge PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${SALTS_PLAYBACK_INCLUDE_DIR}>"
  "$<INSTALL_INTERFACE:include>")
if(MSVC)
  target_compile_options(turbo_tts_playback_bridge PRIVATE /W4)
else()
  target_compile_options(turbo_tts_playback_bridge PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()

# TEST-ONLY fake sink implementations are compiled against the unmodified
# installed header. No fake playback is shipped in production.
if(BUILD_TESTING)
  add_executable(test_tts_playback_bridge_mock
    tests/test_playback_bridge_mock.c)
  target_link_libraries(test_tts_playback_bridge_mock
    PRIVATE turbo_tts_playback_bridge)
  add_test(NAME tts_playback_bridge_mock
    COMMAND test_tts_playback_bridge_mock)
endif()

include(GNUInstallDirs)
install(TARGETS turbo_tts_playback_bridge ARCHIVE DESTINATION
  ${CMAKE_INSTALL_LIBDIR})
install(FILES include/turbo_agent_tts_playback_bridge.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
