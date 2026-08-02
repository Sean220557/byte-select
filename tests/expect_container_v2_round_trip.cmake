if(NOT DEFINED BSEL OR NOT DEFINED MODEL OR NOT DEFINED INPUT OR NOT DEFINED OUTPUT_DIRECTORY)
    message(FATAL_ERROR "BSEL, MODEL, INPUT, and OUTPUT_DIRECTORY must be specified")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
set(container "${OUTPUT_DIRECTORY}/round-trip.bsel")
set(restored "${OUTPUT_DIRECTORY}/restored.bin")

execute_process(
    COMMAND "${BSEL}" compress "${MODEL}" "${INPUT}" "${container}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "v2 compression failed: ${output}${error}")
endif()

execute_process(
    COMMAND "${BSEL}" decompress "${MODEL}" "${container}" "${restored}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "v2 decompression failed: ${output}${error}")
endif()

file(SHA256 "${INPUT}" input_sha256)
file(SHA256 "${restored}" restored_sha256)
if(NOT input_sha256 STREQUAL restored_sha256)
    message(FATAL_ERROR "v2 round trip changed the input")
endif()

file(READ "${container}" magic OFFSET 0 LIMIT 4 HEX)
file(READ "${container}" version OFFSET 4 LIMIT 4 HEX)
file(READ "${container}" first_record OFFSET 28 LIMIT 13 HEX)
string(TOLOWER "${magic}" magic)
string(TOLOWER "${version}" version)
string(TOLOWER "${first_record}" first_record)
if(NOT magic STREQUAL "4253454c" OR NOT version STREQUAL "02000000")
    message(FATAL_ERROR "container is not BSEL v2")
endif()
if(NOT first_record STREQUAL "01080000008000000000000000")
    message(FATAL_ERROR "first v2 record does not store target 8 and tagged metadata 128")
endif()
