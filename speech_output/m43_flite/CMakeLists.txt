cmake_minimum_required(VERSION 3.25)
project(TurboAgentM43FliteProcess LANGUAGES C)
include(CTest)
enable_testing()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "M4.3 Flite process adapter is currently Linux-only")
endif()

add_library(turbo_tts_m43_flite_process STATIC
  src/turbo_agent_tts_flite_process.c)
target_compile_features(turbo_tts_m43_flite_process PUBLIC c_std_11)
set_target_properties(turbo_tts_m43_flite_process PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(turbo_tts_m43_flite_process PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_include_directories(turbo_tts_m43_flite_process PRIVATE src)

add_library(turbo_tts_m43_source STATIC
  ../src/turbo_agent_speech_output.c)
target_compile_features(turbo_tts_m43_source PUBLIC c_std_11)
set_target_properties(turbo_tts_m43_source PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(turbo_tts_m43_source PUBLIC ../include)

add_executable(m43_process_fixture tests/process_fixture.c)
target_include_directories(m43_process_fixture PRIVATE src)
add_executable(test_m43_process tests/test_m43_process.c)
target_link_libraries(test_m43_process PRIVATE
  turbo_tts_m43_flite_process turbo_tts_m43_source)
target_compile_definitions(test_m43_process PRIVATE
  TEST_M43_HELPER_PATH="$<TARGET_FILE:m43_process_fixture>")
add_dependencies(test_m43_process m43_process_fixture)

foreach(t turbo_tts_m43_flite_process turbo_tts_m43_source
          m43_process_fixture test_m43_process)
  set_target_properties(${t} PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endforeach()
add_test(NAME m43_process_source_provider
  COMMAND test_m43_process)
set_tests_properties(m43_process_source_provider
  PROPERTIES TIMEOUT 30)

# Exact real dlopen/dlclose: the shared Provider contains the entire process
# adapter, with no unresolved SpeechOutput symbols supplied by the host.
# Still uses only deterministic child fixture PCM (no Flite/model/voice).
add_library(turbo_tts_m43_flite_provider_dso SHARED
  src/turbo_agent_tts_flite_process.c
  src/turbo_agent_tts_flite_dso.c)
set_target_properties(turbo_tts_m43_flite_provider_dso PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
  POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
target_include_directories(turbo_tts_m43_flite_provider_dso PRIVATE
  src include ../include)
target_link_options(turbo_tts_m43_flite_provider_dso PRIVATE "LINKER:-z,defs")
target_compile_options(turbo_tts_m43_flite_provider_dso PRIVATE
  -Wall -Wextra -Wpedantic -Werror)

add_executable(test_m43_dso tests/test_m43_dso.c)
set_target_properties(test_m43_dso PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(test_m43_dso PRIVATE include ../include)
target_link_libraries(test_m43_dso PRIVATE
  turbo_tts_m43_source "${CMAKE_DL_LIBS}")
target_compile_definitions(test_m43_dso PRIVATE
  TEST_M43_HELPER_PATH="$<TARGET_FILE:m43_process_fixture>"
  TEST_M43_DSO_PATH="$<TARGET_FILE:turbo_tts_m43_flite_provider_dso>")
target_compile_options(test_m43_dso PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
add_dependencies(test_m43_dso m43_process_fixture
  turbo_tts_m43_flite_provider_dso)
add_test(NAME m43_process_dlopen_provider COMMAND test_m43_dso)
set_tests_properties(m43_process_dlopen_provider PROPERTIES TIMEOUT 30)

function(m43_set_reviewed_pcm_oracle target sample_count fnv64_hex)
  target_compile_definitions(${target} PRIVATE
    M43_APPROVED_PCM_SAMPLES=${sample_count}u
    M43_APPROVED_PCM_FNV64=0x${fnv64_hex}ULL)
endfunction()

# Compile the exact same oracle-definition helper even when Flite and its
# licensed voice are completely disabled. Do not fake a real audio result.
add_executable(test_m43_oracle_definitions
  tests/test_m43_oracle_definitions.c)
target_compile_features(test_m43_oracle_definitions PRIVATE c_std_11)
m43_set_reviewed_pcm_oracle(test_m43_oracle_definitions
  80 1122334455667788)
target_compile_options(test_m43_oracle_definitions PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
add_test(NAME m43_process_oracle_definitions
  COMMAND test_m43_oracle_definitions)
# The normal build, default speech paths and CI NEVER download Flite, an
# engine binary, a voice, a model or license-gated data. For an authorized
# local real-CPU build, provide each EXACT released library + SHA-256.
option(M43_ENABLE_REAL_FLITE "Build real offline Flite 2.2 helper (manual)" OFF)
option(M43_VENDOR_LICENSE_APPROVED "Explicit human voice/license approval" OFF)
set(M43_FLITE_INCLUDE_DIR "" CACHE PATH "Exact installed Flite 2.2 headers")
set(M43_FLITE_LIBRARIES "" CACHE STRING "Four absolute Flite libraries, voice first")
set(M43_FLITE_LIBRARY_SHA256 "" CACHE STRING "Four exact 64-char SHA-256 digests")
set(M43_FLITE_RELEASE_TAG "" CACHE STRING "Manually reviewed Flite release tag")
set(M43_FLITE_HEADER_SHA256 "" CACHE STRING "SHA-256 of exact installed flite.h")
set(M43_FLITE_LICENSE_FILE "" CACHE FILEPATH "Absolute reviewed Flite core COPYING")
set(M43_FLITE_LICENSE_SHA256 "" CACHE STRING "SHA-256 of reviewed Flite core COPYING")
set(M43_FLITE_VOICE_LICENSE_FILE "" CACHE FILEPATH "Absolute reviewed CMU Kal16 voice license/notice")
set(M43_FLITE_VOICE_LICENSE_SHA256 "" CACHE STRING "SHA-256 of reviewed CMU Kal16 voice license/notice")
# Manual real-voice acceptance is separate from the model-free CI tests.
# In addition to license and exact vendor digests, it requires a real
# reviewed PCM oracle and the exact installed published Playback header.
option(M43_REAL_VOICE_ACCEPTANCE "Run authorized real Flite → VoiceCoordinator C11 acceptance" OFF)
set(M43_REAL_EXPECTED_PCM_SAMPLES "" CACHE STRING "Reviewed real voice sample count")
set(M43_REAL_EXPECTED_FNV64 "" CACHE STRING "Reviewed 16-digit real PCM FNV64")
if(M43_REAL_VOICE_ACCEPTANCE AND NOT M43_ENABLE_REAL_FLITE)
  message(FATAL_ERROR "Real voice acceptance requires explicitly approved Flite")
endif()
if(M43_ENABLE_REAL_FLITE)
  if(NOT M43_VENDOR_LICENSE_APPROVED)
    message(FATAL_ERROR "Real Flite requires explicit vendor/voice license approval")
  endif()
  if(NOT IS_ABSOLUTE "${M43_FLITE_INCLUDE_DIR}" OR
     NOT EXISTS "${M43_FLITE_INCLUDE_DIR}/flite.h")
    message(FATAL_ERROR "A pinned installed Flite 2.2 header directory is required")
  endif()
  if(NOT M43_FLITE_RELEASE_TAG STREQUAL "v2.2")
    message(FATAL_ERROR "Real helper requires exact upstream Flite tag v2.2")
  endif()
  if(NOT IS_ABSOLUTE "${M43_FLITE_LICENSE_FILE}" OR
     NOT EXISTS "${M43_FLITE_LICENSE_FILE}")
    message(FATAL_ERROR "Exact installed Flite core license evidence required")
  endif()
  if(NOT IS_ABSOLUTE "${M43_FLITE_VOICE_LICENSE_FILE}" OR
     NOT EXISTS "${M43_FLITE_VOICE_LICENSE_FILE}")
    message(FATAL_ERROR "Exact installed CMU Kal16 voice license evidence required")
  endif()
  # These digests are independently supplied by the reviewer. They prove
  # local bytes, NOT by themselves that a binary came from the declared tag.
  foreach(label IN ITEMS HEADER LICENSE VOICE_LICENSE)
    if(label STREQUAL "HEADER")
      set(file "${M43_FLITE_INCLUDE_DIR}/flite.h")
      set(want "${M43_FLITE_HEADER_SHA256}")
    elseif(label STREQUAL "LICENSE")
      set(file "${M43_FLITE_LICENSE_FILE}")
      set(want "${M43_FLITE_LICENSE_SHA256}")
    else()
      set(file "${M43_FLITE_VOICE_LICENSE_FILE}")
      set(want "${M43_FLITE_VOICE_LICENSE_SHA256}")
    endif()
    string(LENGTH "${want}" digest_len)
    if(NOT digest_len EQUAL 64 OR NOT want MATCHES "^[0-9a-fA-F]+$")
      message(FATAL_ERROR "Missing valid independent Flite ${label} SHA-256")
    endif()
    # CMake's SHA256 output is lowercase; normalize accepted uppercase input
    # before comparing so the documented hexadecimal case-insensitive form
    # behaves as advertised.
    string(TOLOWER "${want}" want)
    file(SHA256 "${file}" actual)
    if(NOT actual STREQUAL want)
      message(FATAL_ERROR "Flite ${label} provenance digest mismatch")
    endif()
  endforeach()
  unset(file)
  unset(want)
  unset(actual)
  list(LENGTH M43_FLITE_LIBRARIES nlibs)
  list(LENGTH M43_FLITE_LIBRARY_SHA256 nsha)
  if(NOT nlibs EQUAL 4 OR NOT nsha EQUAL 4)
    message(FATAL_ERROR "Provide four exact Flite libraries and four digests")
  endif()
  foreach(i RANGE 0 3)
    list(GET M43_FLITE_LIBRARIES ${i} lib)
    list(GET M43_FLITE_LIBRARY_SHA256 ${i} sha)
    string(LENGTH "${sha}" sha_length)
    if(NOT IS_ABSOLUTE "${lib}" OR NOT EXISTS "${lib}" OR
       NOT sha_length EQUAL 64 OR NOT sha MATCHES "^[0-9a-fA-F]+$")
      message(FATAL_ERROR "Flite library/digest ${i} invalid")
    endif()
    # Accept uppercase or lowercase hex consistently with the validation
    # above and the documented SHA-256 input format.
    string(TOLOWER "${sha}" sha)
    file(SHA256 "${lib}" actual)
    if(NOT actual STREQUAL sha)
      message(FATAL_ERROR "Flite exact library SHA-256 mismatch for ${lib}")
    endif()
  endforeach()
  add_executable(turbo_m43_flite_real_helper src/flite_native_helper.c)
  target_compile_features(turbo_m43_flite_real_helper PRIVATE c_std_11)
  set_target_properties(turbo_m43_flite_real_helper PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  target_include_directories(turbo_m43_flite_real_helper PRIVATE
    src include ../include "${M43_FLITE_INCLUDE_DIR}")
  target_link_libraries(turbo_m43_flite_real_helper PRIVATE
    ${M43_FLITE_LIBRARIES} m)
  target_compile_options(turbo_m43_flite_real_helper PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

  if(M43_REAL_VOICE_ACCEPTANCE)
    if(NOT IS_ABSOLUTE "${SALTS_PLAYBACK_INCLUDE_DIR}" OR
       NOT EXISTS "${SALTS_PLAYBACK_INCLUDE_DIR}/salts_playback.h" OR
       NOT EXISTS "${SALTS_PLAYBACK_INCLUDE_DIR}/salts_playback_export.h")
      message(FATAL_ERROR "Manual voice acceptance requires exact installed Salts Playback headers")
    endif()
    if(NOT M43_REAL_EXPECTED_PCM_SAMPLES MATCHES "^[1-9][0-9]*$")
      message(FATAL_ERROR "Missing reviewed real PCM sample count")
    endif()
    if(M43_REAL_EXPECTED_PCM_SAMPLES GREATER 720000)
      message(FATAL_ERROR "Reviewed PCM sample count exceeds bounded duration")
    endif()
    string(LENGTH "${M43_REAL_EXPECTED_FNV64}" fnv_digits)
    if(NOT fnv_digits EQUAL 16 OR
       NOT M43_REAL_EXPECTED_FNV64 MATCHES "^[0-9a-fA-F]+$")
      message(FATAL_ERROR "Missing reviewed 16-digit real PCM hash")
    endif()

    add_executable(test_m43_manual_real_voice
      tests/test_m43_real_voice.c
      ../playback/src/turbo_agent_tts_playback_bridge.c
      ../voice_coordinator/src/turbo_agent_voice_coordinator.c)
    set_target_properties(test_m43_manual_real_voice PROPERTIES
      C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    target_include_directories(test_m43_manual_real_voice PRIVATE
      include ../include ../playback/include ../voice_coordinator/include
      "${SALTS_PLAYBACK_INCLUDE_DIR}")
    target_link_libraries(test_m43_manual_real_voice PRIVATE
      turbo_tts_m43_source "${CMAKE_DL_LIBS}")
    target_compile_definitions(test_m43_manual_real_voice PRIVATE
      TEST_M43_REAL_HELPER_PATH="$<TARGET_FILE:turbo_m43_flite_real_helper>"
      TEST_M43_DSO_PATH="$<TARGET_FILE:turbo_tts_m43_flite_provider_dso>")
    m43_set_reviewed_pcm_oracle(test_m43_manual_real_voice
      ${M43_REAL_EXPECTED_PCM_SAMPLES} ${M43_REAL_EXPECTED_FNV64})
    target_compile_options(test_m43_manual_real_voice PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
    add_dependencies(test_m43_manual_real_voice
      turbo_m43_flite_real_helper turbo_tts_m43_flite_provider_dso)
    add_test(NAME m43_manual_approved_flite_voice
      COMMAND test_m43_manual_real_voice)
    set_tests_properties(m43_manual_approved_flite_voice
      PROPERTIES TIMEOUT 40)
  endif()
endif()
