if(NOT DEFINED WORKER OR NOT EXISTS "${WORKER}" OR NOT DEFINED ROOT OR NOT IS_ABSOLUTE "${ROOT}")
  message(FATAL_ERROR "Invalid report CLI configuration")
endif()
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef nonce)
set(work "${ROOT}/cli-${nonce} with spaces")
file(MAKE_DIRECTORY "${work}")
foreach(id IN ITEMS "bad/id" valid)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env
      --unset=REPORT_A_MODEL --unset=REPORT_B_MODEL --unset=REPORT_PLAN_MODEL --unset=REPORT_EXEC_MODEL
      "REPORT_AUDIT_DIR=${work}" "REPORT_AUDIT_ID=${id}"
      "${WORKER}" host-parent goal brief
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
  if(NOT "${result}" STREQUAL "1" OR NOT "${output}" STREQUAL "" OR
     NOT "${error}" MATCHES "Report stopped at configuration")
    message(FATAL_ERROR "CLI id=${id}: expected configuration failure without output, got ${result}\n${output}${error}\nEvidence: ${work}")
  endif()
endforeach()
foreach(kind IN ITEMS missing_argument invalid_id missing_database)
  if(kind STREQUAL "missing_argument")
    set(args --inspect "${work}" valid)
  elseif(kind STREQUAL "invalid_id")
    set(args --inspect "${work}" bad/id host-parent)
  else()
    set(args --inspect "${work}/absent directory" valid host-parent)
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env
      --unset=REPORT_A_MODEL --unset=REPORT_B_MODEL --unset=REPORT_PLAN_MODEL --unset=REPORT_EXEC_MODEL
      "${WORKER}" ${args}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
  if(NOT "${result}" STREQUAL "1" OR NOT "${output}" STREQUAL "")
    message(FATAL_ERROR "Inspection ${kind}: expected failure without output, got ${result}\n${output}${error}\nEvidence: ${work}")
  endif()
endforeach()
if(EXISTS "${work}/absent directory/host.db")
  message(FATAL_ERROR "Inspection created a missing database: ${work}")
endif()
if(NOT EXISTS "${work}/host.db" OR EXISTS "${work}/draft.db")
  message(FATAL_ERROR "Expected only Host store initialization before missing-model rejection: ${work}")
endif()
file(REMOVE_RECURSE "${work}")
