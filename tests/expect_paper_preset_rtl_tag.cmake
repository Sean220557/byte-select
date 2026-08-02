if(NOT DEFINED BSEL OR NOT DEFINED MODEL OR NOT DEFINED OUTPUT_DIRECTORY)
    message(FATAL_ERROR "BSEL, MODEL, and OUTPUT_DIRECTORY must be specified")
endif()

execute_process(
    COMMAND "${BSEL}" generate-rtl "${MODEL}" "${OUTPUT_DIRECTORY}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "RTL generation failed: ${output}${error}")
endif()

file(READ "${OUTPUT_DIRECTORY}/bsel_compressor_s2.sv" compressor)
file(READ "${OUTPUT_DIRECTORY}/bsel_decompressor_s2.sv" decompressor)
string(FIND "${compressor}" "pattern_id_o = 8'd128" compressor_tag_index)
string(FIND "${decompressor}" "8'd128: begin" decompressor_tag_index)
if(compressor_tag_index EQUAL -1 OR decompressor_tag_index EQUAL -1)
    message(FATAL_ERROR "8-byte preset target did not encode its high metadata tag")
endif()
