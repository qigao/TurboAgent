# Source-sanitizer-only CFlow/CSTL static-owner experiment.
# The exact released Salts sources and official installed SDKs remain untouched.
# Salts #1167 / Draft PR #1169 separately qualified hidden-static CSTL on
# Linux ASan, but production ownership policy remains undecided.
# Salts #1180 follows the NEW CFlow static archive ODR collision between
# libchttp_client and libchttp_server. This is a separate candidate only.
# KEEP ASan ODR detection fully enabled, without suppressions or fallbacks.
if(NOT CMAKE_PROJECT_NAME STREQUAL "Salts")
  message(FATAL_ERROR "Static owner candidate must run inside project(Salts)")
endif()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
  set_target_properties cmeta_cstl PROPERTIES
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
  set_target_properties cmeta_cflow PROPERTIES
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
