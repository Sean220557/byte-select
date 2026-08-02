if(NOT DEFINED BSEL OR NOT DEFINED FIXTURE_WRITER OR NOT DEFINED OUTPUT_DIRECTORY)
    message(FATAL_ERROR "BSEL, FIXTURE_WRITER, and OUTPUT_DIRECTORY must be specified")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
set(model "${OUTPUT_DIRECTORY}/legacy.model")
set(container "${OUTPUT_DIRECTORY}/legacy-v1.bsel")
set(restored "${OUTPUT_DIRECTORY}/restored.bin")
file(WRITE "${model}" "BSEL_MODEL 1\nblock_size 4\nsets 1\nset 3 1 1\npattern 0 0 1 0\n")

execute_process(
    COMMAND "${FIXTURE_WRITER}" "${container}"
    RESULT_VARIABLE result
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "v1 fixture generation failed: ${error}")
endif()

execute_process(
    COMMAND "${BSEL}" decompress "${model}" "${container}" "${restored}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "v1 decompression failed: ${output}${error}")
endif()

file(READ "${restored}" restored HEX)
string(TOLOWER "${restored}" restored)
if(NOT restored STREQUAL "41414241")
    message(FATAL_ERROR "v1 container decoded to ${restored}, expected AABA")
endif()
