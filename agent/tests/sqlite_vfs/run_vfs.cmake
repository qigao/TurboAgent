if(NOT DEFINED WORKER OR NOT EXISTS "${WORKER}" OR
   NOT DEFINED ROOT OR NOT IS_ABSOLUTE "${ROOT}" OR
   NOT PHASE MATCHES "^(started|committed)$" OR
   NOT FAULT MATCHES "^(none|journal_full|journal_write|journal_sync|database_write|database_sync)$")
  message(FATAL_ERROR "Invalid SQLite VFS test configuration")
endif()
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef nonce)
set(work "${ROOT}/${PHASE}-${FAULT}-${nonce} with spaces")
if(EXISTS "${work}")
  message(FATAL_ERROR "SQLite VFS case directory already exists: ${work}")
endif()
file(MAKE_DIRECTORY "${work}")

function(run_stage stage)
  execute_process(COMMAND "${WORKER}" "${stage}" "${PHASE}" "${FAULT}" "${work}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
  if(NOT "${result}" STREQUAL "0")
    message(FATAL_ERROR "${PHASE}/${FAULT}/${stage}: exit ${result}\n${output}${error}\nEvidence retained at ${work}")
  endif()
  message(STATUS "${output}")
endfunction()

run_stage(init)
run_stage(inject)
run_stage(inspect)
run_stage(recover)
run_stage(inspect)
run_stage(replay)
run_stage(inspect)
file(REMOVE_RECURSE "${work}")
