# PraktorTools is a native DSO and must not import another image's hidden
# Salts::CSTL hash_bytes or export that private static callback globally.
if(NOT DEFINED NATIVE_IMAGE OR NOT EXISTS "${NATIVE_IMAGE}")
  message(FATAL_ERROR "PraktorTools final shared image missing: ${NATIVE_IMAGE}")
endif()
if(NOT DEFINED NM_TOOL OR NM_TOOL STREQUAL "")
  message(FATAL_ERROR "An exact native nm tool is required")
endif()
foreach(_mode IN ITEMS --undefined-only --defined-only)
  execute_process(
    COMMAND "${NM_TOOL}" -D "${_mode}" "${NATIVE_IMAGE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _symbols ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "PraktorTools ELF symbol audit failed (${_mode}): ${_err}")
  endif()
  string(REPLACE "\n" ";" _lines "${_symbols}")
  foreach(_line IN LISTS _lines)
    if(_line MATCHES "^[ \t]*([0-9a-fA-F]+[ \t]+)?[A-Za-z][ \t]+hash_bytes(@.*)?$")
      message(FATAL_ERROR
        "PraktorTools must resolve image-local CSTL, got dynamic ${_mode}: ${_line}")
    endif()
  endforeach()
endforeach()
message(STATUS "PraktorTools CSTL hash_bytes resolved privately inside its DSO")
