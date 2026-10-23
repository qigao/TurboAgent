if(NOT DEFINED WORKER OR NOT EXISTS "${WORKER}" OR
   NOT DEFINED ROOT OR NOT IS_ABSOLUTE "${ROOT}" OR
   NOT CUT MATCHES "^(before_planned|after_planned|before_admitted|after_admitted|before_started|after_started|after_effect|before_committed|after_committed|before_delivery)$")
  message(FATAL_ERROR "Invalid process-crash test configuration")
endif()

# Never reuse or remove a caller's database. Only remove this newly created,
# private case directory after every native verification process succeeds.
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef nonce)
set(work "${ROOT}/${CUT}-${nonce} with spaces")
if(EXISTS "${work}")
  message(FATAL_ERROR "Process-crash case directory already exists: ${work}")
endif()
file(MAKE_DIRECTORY "${work}")

function(run_stage stage expected)
  execute_process(COMMAND "${WORKER}" "${stage}" "${CUT}" "${work}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
  if(NOT "${result}" STREQUAL "${expected}")
    message(FATAL_ERROR
      "${CUT}/${stage}: expected exit ${expected}, got ${result}\n${output}${error}\nEvidence retained at ${work}")
  endif()
endfunction()

run_stage(init 0)
# WILL_FAIL would accept unrelated failures; only the chosen _Exit(86) counts.
run_stage(crash 86)
run_stage(inspect 0)
run_stage(denied 0)
if(NOT CUT STREQUAL "before_planned")
  run_stage(mismatch 0)
endif()
run_stage(recover 0)
run_stage(replay 0)
file(REMOVE_RECURSE "${work}")
