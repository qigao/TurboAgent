if(NOT DEFINED WORKER OR NOT EXISTS "${WORKER}" OR
   NOT DEFINED ROOT OR NOT IS_ABSOLUTE "${ROOT}" OR
   NOT DEFINED CASE OR NOT CASE MATCHES "^(normal|crash_after_effect)$")
  message(FATAL_ERROR "Invalid cross-process Tool Journal owner test")
endif()
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef nonce)
set(work "${ROOT}/${CASE}-owner-${nonce} with spaces")
if(EXISTS "${work}")
  message(FATAL_ERROR "Private case directory already exists: ${work}")
endif()
file(MAKE_DIRECTORY "${work}")
set(db "${work}/journal.db")
set(effects "${work}/external-effects.marker")
set(flag "${work}/contender-checked.marker")

function(stage name)
  execute_process(COMMAND "${WORKER}" "${name}" "${db}" "${effects}" "${flag}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 15)
  if(NOT "${rc}" STREQUAL "0")
    message(FATAL_ERROR "Stage ${name} failed: ${rc}\n${out}${err}\nEvidence: ${work}")
  endif()
endfunction()

stage(seed)
# CMake executes both commands at once. Producer stdout becomes consumer
# stdin, so the contender cannot attempt SQLite open until holder reports READY.
# Verify *both* exit codes instead of accepting only the last process status.
# The crash profile only accepts the selected _Exit(86) after an actual Tool
# effect; a different fault, timeout or ordinary success is a test failure.
if(CASE STREQUAL "crash_after_effect")
  set(holder hold_crash)
  set(expected "86;0")
else()
  set(holder hold)
  set(expected "0;0")
endif()
execute_process(
  COMMAND "${WORKER}" "${holder}" "${db}" "${effects}" "${flag}"
  COMMAND "${WORKER}" contend "${db}" "${effects}" "${flag}"
  RESULTS_VARIABLE results OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 25)
if(NOT "${results}" STREQUAL "${expected}")
  message(FATAL_ERROR "SQLite ${CASE}: expected ${expected}, got '${results}'\n${out}${err}\nEvidence: ${work}")
endif()
if(CASE STREQUAL "crash_after_effect")
  stage(reopen_crash)
else()
  stage(reopen)
endif()
file(REMOVE_RECURSE "${work}")
