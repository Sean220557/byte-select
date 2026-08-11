cmake_minimum_required(VERSION 3.16)
project(byte_select LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(byte_select
    src/pattern.cpp
    src/trainer.cpp
    src/codec.cpp
    src/mcc.cpp
    src/baseline.cpp
    src/paper_config.cpp
    src/model_io.cpp
    src/rtl.cpp
)
target_include_directories(byte_select PUBLIC include)

if(MSVC)
    target_compile_options(byte_select PRIVATE /W4)
else()
    target_compile_options(byte_select PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(bsel src/main.cpp)
target_link_libraries(bsel PRIVATE byte_select)

enable_testing()
add_executable(byte_select_tests tests/tests.cpp)
target_link_libraries(byte_select_tests PRIVATE byte_select)
add_test(NAME byte_select_tests COMMAND byte_select_tests)
add_executable(byte_select_legacy_fixture tests/write_legacy_container.cpp)

option(BSEL_ENABLE_RTL_SIM "Run generated RTL simulations when Icarus Verilog is available" ON)
if(BSEL_ENABLE_RTL_SIM)
    find_program(BSEL_IVERILOG_EXECUTABLE iverilog)
    find_program(BSEL_VVP_EXECUTABLE vvp)
    if(BSEL_IVERILOG_EXECUTABLE AND BSEL_VVP_EXECUTABLE)
        add_test(
            NAME byte_select_rtl_simulation
            COMMAND "${CMAKE_COMMAND}"
                    "-DBSEL=$<TARGET_FILE:bsel>"
                    "-DIVERILOG=${BSEL_IVERILOG_EXECUTABLE}"
                    "-DVVP=${BSEL_VVP_EXECUTABLE}"
                    "-DOUTPUT_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/rtl-simulation"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_rtl_simulation.cmake"
        )
    else()
        message(STATUS "Icarus Verilog not found; generated RTL simulation test is disabled")
    endif()
endif()

set(BSEL_TRAIN_LIST_MODEL "${CMAKE_CURRENT_BINARY_DIR}/train-list.model")
add_test(
    NAME byte_select_train_list
    COMMAND bsel train-list "${BSEL_TRAIN_LIST_MODEL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/trace-a.bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/trace-b.bin"
            --block-size 5 --threshold 1 --holdout-middle 50 --target 4:4:1
)
set_tests_properties(
    byte_select_train_list
    PROPERTIES PASS_REGULAR_EXPRESSION
               "training_traces=2 holdout_middle_percent=50 training_blocks=4 holdout_blocks=4"
)
add_test(
    NAME byte_select_train_list_analyze
    COMMAND bsel analyze "${BSEL_TRAIN_LIST_MODEL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/trace-a.bin"
            --ideal-metadata-bytes 1
)
set_tests_properties(byte_select_train_list_analyze PROPERTIES DEPENDS byte_select_train_list)
add_test(
    NAME byte_select_evaluate_list
    COMMAND bsel evaluate-list "${BSEL_TRAIN_LIST_MODEL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/trace-a.bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/trace-b.bin"
)
set_tests_properties(
    byte_select_evaluate_list
    PROPERTIES DEPENDS byte_select_train_list
               PASS_REGULAR_EXPRESSION "trace_mean count=2"
)
add_test(
    NAME byte_select_mcc
    COMMAND bsel mcc "${BSEL_TRAIN_LIST_MODEL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/trace-a.bin"
            --base 4096 --alignment 4 --limit 2
)
set_tests_properties(
    byte_select_mcc
    PROPERTIES DEPENDS byte_select_train_list
               PASS_REGULAR_EXPRESSION "mcc blocks=.*padding_bytes=.*physical_bytes=.*metadata_regions=.*quantized_ratio=.*mcc_entry logical_block=0 physical_address=4096.*metadata_region=0"
)
add_test(
    NAME byte_select_evaluate_groups
    COMMAND bsel evaluate-groups "${BSEL_TRAIN_LIST_MODEL}"
            --group alpha "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/trace-a.bin"
            --group beta "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/trace-b.bin"
)
set_tests_properties(
    byte_select_evaluate_groups
    PROPERTIES DEPENDS byte_select_train_list
               PASS_REGULAR_EXPRESSION "byte_select_mean_of_group_means sample_count=2"
)
add_test(
    NAME byte_select_analyze_list
    COMMAND bsel analyze-list "${BSEL_TRAIN_LIST_MODEL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/trace-a.bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/trace-b.bin"
            --ideal-metadata-bytes 1
)
set_tests_properties(
    byte_select_analyze_list
    PROPERTIES DEPENDS byte_select_train_list
               PASS_REGULAR_EXPRESSION "upper_bound_mean count=2"
)
add_test(
    NAME byte_select_analyze_groups
    COMMAND bsel analyze-groups "${BSEL_TRAIN_LIST_MODEL}"
            --group alpha "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/trace-a.bin"
            --group beta "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/trace-b.bin"
            --ideal-metadata-bytes 1
)
set_tests_properties(
    byte_select_analyze_groups
    PROPERTIES DEPENDS byte_select_train_list
               PASS_REGULAR_EXPRESSION "upper_bound_mean_of_group_means sample_count=2"
)
set(BSEL_COMPARE_MODEL "${CMAKE_CURRENT_BINARY_DIR}/compare.model")
add_test(
    NAME byte_select_compare_train
    COMMAND bsel train "${CMAKE_CURRENT_SOURCE_DIR}/src/codec.cpp" "${BSEL_COMPARE_MODEL}"
            --block-size 64 --threshold 1 --target 32:16:1
)
add_test(
    NAME byte_select_compare
    COMMAND bsel compare "${BSEL_COMPARE_MODEL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/codec.cpp"
            --baseline fpc --baseline bdi --baseline hybrid
            --baseline cpack --baseline bpc --baseline huffman
)
set_tests_properties(byte_select_compare PROPERTIES DEPENDS byte_select_compare_train)
add_test(
    NAME byte_select_mcc_compare
    COMMAND bsel mcc-compare "${BSEL_COMPARE_MODEL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/codec.cpp"
            --base 4096 --alignment 8
)
set_tests_properties(
    byte_select_mcc_compare
    PROPERTIES DEPENDS byte_select_compare_train
               PASS_REGULAR_EXPRESSION "mcc_compare algorithm=bsel.*after_quantized_ratio="
)
add_test(
    NAME byte_select_compare_groups
    COMMAND bsel compare-groups "${BSEL_COMPARE_MODEL}"
            --group codec "${CMAKE_CURRENT_SOURCE_DIR}/src/codec.cpp"
            --group baseline "${CMAKE_CURRENT_SOURCE_DIR}/src/baseline.cpp"
            --baseline fpc --baseline bdi --baseline cpack --baseline bpc
            --baseline huffman
)
set_tests_properties(
    byte_select_compare_groups
    PROPERTIES DEPENDS byte_select_compare_train
               PASS_REGULAR_EXPRESSION "byte_select_mean_of_group_means sample_count=2"
)
set(BSEL_PAPER_PRESET_MODEL "${CMAKE_CURRENT_BINARY_DIR}/paper-preset.model")
add_test(
    NAME byte_select_paper_preset
    COMMAND bsel train "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/trace-uniform-64.bin" "${BSEL_PAPER_PRESET_MODEL}"
            --block-size 64 --threshold 1 --paper-config bsel-1024-1024-128
)
set_tests_properties(
    byte_select_paper_preset
    PROPERTIES PASS_REGULAR_EXPRESSION "paper_config=bsel-1024-1024-128 targets=3"
)
add_test(
    NAME byte_select_patterns
    COMMAND "${CMAKE_COMMAND}"
            "-DBSEL=$<TARGET_FILE:bsel>"
            "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/trace-uniform-64.bin"
            "-DMODEL=${CMAKE_CURRENT_BINARY_DIR}/patterns.model"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_patterns_output.cmake"
)
set_tests_properties(byte_select_patterns PROPERTIES DEPENDS byte_select_paper_preset)
add_test(
    NAME byte_select_container_v2_round_trip
    COMMAND "${CMAKE_COMMAND}"
            "-DBSEL=$<TARGET_FILE:bsel>"
            "-DMODEL=${BSEL_PAPER_PRESET_MODEL}"
            "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/data/trace-uniform-64.bin"
            "-DOUTPUT_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/container-v2"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_container_v2_round_trip.cmake"
)
set_tests_properties(
    byte_select_container_v2_round_trip
    PROPERTIES DEPENDS byte_select_paper_preset
)
add_test(
    NAME byte_select_container_v1_compatibility
    COMMAND "${CMAKE_COMMAND}"
            "-DBSEL=$<TARGET_FILE:bsel>"
            "-DFIXTURE_WRITER=$<TARGET_FILE:byte_select_legacy_fixture>"
            "-DOUTPUT_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/container-v1"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_container_v1_compatibility.cmake"
)
add_test(
    NAME byte_select_paper_preset_rtl_metadata_tag
    COMMAND "${CMAKE_COMMAND}"
            "-DBSEL=$<TARGET_FILE:bsel>"
            "-DMODEL=${BSEL_PAPER_PRESET_MODEL}"
            "-DOUTPUT_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/paper-preset-rtl"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_paper_preset_rtl_tag.cmake"
)
set_tests_properties(
    byte_select_paper_preset_rtl_metadata_tag
    PROPERTIES DEPENDS byte_select_paper_preset
)
add_test(
    NAME byte_select_paper_preset_rejects_target_override
    COMMAND "${CMAKE_COMMAND}"
            "-DBSEL=$<TARGET_FILE:bsel>"
            "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/src/codec.cpp"
            "-DMODEL=${BSEL_PAPER_PRESET_MODEL}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_paper_config_conflict.cmake"
)
add_test(
    NAME byte_select_compare_list
    COMMAND bsel compare-list "${BSEL_COMPARE_MODEL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/codec.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/baseline.cpp"
            --baseline fpc --baseline bdi --baseline hybrid
            --baseline cpack --baseline bpc --baseline huffman
)
set_tests_properties(
    byte_select_compare_list
    PROPERTIES DEPENDS byte_select_compare_train
               PASS_REGULAR_EXPRESSION "baseline_mean algorithm=fpc count=2"
)
