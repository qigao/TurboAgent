if(NOT DEFINED WORKER OR NOT EXISTS "${WORKER}" OR
   NOT DEFINED ROOT OR NOT IS_ABSOLUTE "${ROOT}" OR
   NOT CUT MATCHES "^(before_claim|after_claim|before_run_started|after_run_started|in_callback|before_committed|after_committed|after_run_committed|before_delivery)$")
  message(FATAL_ERROR "Invalid compiled Inspect crash test configuration")
endif()

if(DEFINED MODE AND NOT MODE STREQUAL "job")
  message(FATAL_ERROR "Unsupported Inspect execution mode")
endif()
set(mode_args)
if(MODE STREQUAL "job")
  list(APPEND mode_args job)
endif()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef nonce)
set(work "${ROOT}/${CUT}-${nonce} with spaces")
if(EXISTS "${work}")
  message(FATAL_ERROR "Compiled Inspect case already exists: ${work}")
endif()
file(MAKE_DIRECTORY "${work}")

function(run_stage stage expected)
  execute_process(COMMAND "${WORKER}" "${stage}" "${CUT}" "${work}" ${mode_args}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
  if(NOT "${result}" STREQUAL "${expected}")
    message(FATAL_ERROR
      "${CUT}/${stage}: expected exit ${expected}, got ${result}\n${output}${error}\nEvidence retained at ${work}")
  endif()
endfunction()

run_stage(init 0)
run_stage(crash 86)
run_stage(inspect 0)
run_stage(denied 0)
if(NOT CUT STREQUAL "before_claim")
  if(NOT MODE STREQUAL "job")
    run_stage(mismatch 0)
  endif()
  run_stage(stale 0)
endif()
run_stage(recover 0)
run_stage(replay 0)
# Only this newly created private directory is removed, and only on success.
file(REMOVE_RECURSE "${work}")
