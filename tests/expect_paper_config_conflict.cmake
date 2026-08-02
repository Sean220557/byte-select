if(NOT DEFINED BSEL OR NOT DEFINED INPUT OR NOT DEFINED MODEL)
    message(FATAL_ERROR "BSEL, INPUT, and MODEL must be specified")
endif()

execute_process(
    COMMAND "${BSEL}" train "${INPUT}" "${MODEL}"
            --block-size 64 --paper-config bsel-256 --target 32:256:1
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(result EQUAL 0)
    message(FATAL_ERROR "mixed preset and target options unexpectedly succeeded: ${output}")
endif()
string(FIND "${error}" "--target cannot be combined with --paper-config" error_index)
if(error_index EQUAL -1)
    message(FATAL_ERROR "unexpected error for mixed preset and target options: ${error}")
endif()
