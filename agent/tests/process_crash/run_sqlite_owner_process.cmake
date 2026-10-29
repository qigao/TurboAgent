if(NOT DEFINED WORKER OR NOT EXISTS "${WORKER}" OR
   NOT DEFINED ROOT OR NOT IS_ABSOLUTE "${ROOT}")
  message(FATAL_ERROR "Invalid cross-process Tool Journal owner test")
endif()
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef nonce)
set(work "${ROOT}/owner-${nonce} with spaces")
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
execute_process(
  COMMAND "${WORKER}" hold "${db}" "${effects}" "${flag}"
  COMMAND "${WORKER}" contend "${db}" "${effects}" "${flag}"
  RESULTS_VARIABLE results OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 25)
if(NOT "${results}" STREQUAL "0;0")
  message(FATAL_ERROR "SQLite holder/contender: expected 0;0, got '${results}'\n${out}${err}\nEvidence: ${work}")
endif()
stage(reopen)
file(REMOVE_RECURSE "${work}")
