# M4.2 test-only half-duplex coordinator stacked on M4.1.
if(NOT TARGET turbo_tts_playback_bridge)
  message(FATAL_ERROR "M4.2 requires explicit M4.1 playback opt-in")
endif()
add_library(turbo_tts_voice_coordinator STATIC
  src/turbo_agent_voice_coordinator.c)
add_library(TurboAgent::VoiceCoordinator ALIAS turbo_tts_voice_coordinator)
target_compile_features(turbo_tts_voice_coordinator PUBLIC c_std_11)
set_target_properties(turbo_tts_voice_coordinator PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(turbo_tts_voice_coordinator PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:include>")
target_link_libraries(turbo_tts_voice_coordinator PUBLIC
  turbo_tts_playback_bridge)
if(MSVC)
  target_compile_options(turbo_tts_voice_coordinator PRIVATE /W4)
else()
  target_compile_options(turbo_tts_voice_coordinator PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()
if(BUILD_TESTING)
  add_executable(test_tts_voice_coordinator_fake
    tests/test_voice_coordinator.c)
  target_link_libraries(test_tts_voice_coordinator_fake PRIVATE
    turbo_tts_voice_coordinator)
  add_test(NAME tts_voice_coordinator_fake
    COMMAND test_tts_voice_coordinator_fake)
endif()
include(GNUInstallDirs)
install(TARGETS turbo_tts_voice_coordinator
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/turbo_agent_voice_coordinator.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
