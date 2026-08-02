if(NOT DEFINED BSEL OR NOT DEFINED INPUT OR NOT DEFINED MODEL)
    message(FATAL_ERROR "BSEL, INPUT, and MODEL must be specified")
endif()

execute_process(
    COMMAND "${BSEL}" train "${INPUT}" "${MODEL}"
            --block-size 64 --threshold 1 --paper-config bsel-1024-1024-128
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "training failed: ${output}${error}")
endif()

execute_process(
    COMMAND "${BSEL}" patterns "${MODEL}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "patterns failed: ${output}${error}")
endif()

foreach(expected IN ITEMS
    "pattern_set index=0 target=32 metadata_bytes=2 dictionary=30 metadata_tag_bits=1 metadata_tag=0 pattern_id_bits=15 patterns=1"
    "pattern_set index=1 target=16 metadata_bytes=2 dictionary=14 metadata_tag_bits=1 metadata_tag=0 pattern_id_bits=15 patterns=1"
    "pattern_set index=2 target=8 metadata_bytes=1 dictionary=7 metadata_tag_bits=1 metadata_tag=1 pattern_id_bits=7 patterns=1"
    "digits=0111111111111111111111111111111111111111111111111111111111111111"
)
    string(FIND "${output}" "${expected}" found_index)
    if(found_index EQUAL -1)
        message(FATAL_ERROR "patterns output missing '${expected}': ${output}")
    endif()
endforeach()

# --limit N prints at most N patterns per set.
execute_process(
    COMMAND "${BSEL}" patterns "${MODEL}" --limit 1
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "patterns --limit failed: ${output}${error}")
endif()
string(FIND "${output}" "pid=128" tagged_index)
if(tagged_index EQUAL -1)
    message(FATAL_ERROR "tagged 8-byte pattern did not carry pid 128: ${output}")
endif()
string(FIND "${output}" "pattern index=1" second_index)
if(NOT second_index EQUAL -1)
    message(FATAL_ERROR "--limit 1 unexpectedly printed a second pattern: ${output}")
endif()
