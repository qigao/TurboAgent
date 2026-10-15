cmake_minimum_required(VERSION 3.27)
project(TurboAgentM4InstalledPlaybackConsumer LANGUAGES C)
if(NOT DEFINED ENV{SALTS_ROOT} OR NOT DEFINED ENV{SALTS_UTILS_ROOT})
  message(FATAL_ERROR "Exact SALTS_ROOT and SALTS_UTILS_ROOT required")
endif()
find_package(SaltsUtils CONFIG REQUIRED
  PATHS "$ENV{SALTS_UTILS_ROOT}"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT TARGET Salts::Playback)
  message(FATAL_ERROR "Published SaltsUtils SDK lacks Salts::Playback")
endif()
add_executable(tts_playback_release_link verify_installed_playback.c)
target_compile_features(tts_playback_release_link PRIVATE c_std_11)
target_compile_options(tts_playback_release_link PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
target_include_directories(tts_playback_release_link PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/../../include"
  "${CMAKE_CURRENT_LIST_DIR}/../../../include")
target_link_libraries(tts_playback_release_link PRIVATE Salts::Playback)
