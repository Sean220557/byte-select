if(NOT DEFINED BSEL OR NOT DEFINED IVERILOG OR NOT DEFINED VVP OR
   NOT DEFINED OUTPUT_DIRECTORY)
    message(FATAL_ERROR "BSEL, IVERILOG, VVP, and OUTPUT_DIRECTORY must be specified")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
set(model "${OUTPUT_DIRECTORY}/rtl-simulation.model")
set(testbench "${OUTPUT_DIRECTORY}/rtl-simulation.sv")
set(simulator "${OUTPUT_DIRECTORY}/rtl-simulation.out")
file(WRITE "${model}" "BSEL_MODEL 2\nblock_size 4\nsets 2\nset 3 1 0 0 2\npattern 0 0 1 0\npattern 0 1 0 1\nset 2 1 1 1 1\npattern 0 0 0 0\n")

execute_process(
    COMMAND "${BSEL}" generate-rtl "${model}" "${OUTPUT_DIRECTORY}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "RTL generation failed: ${output}${error}")
endif()

file(WRITE "${testbench}" [=[
module rtl_simulation_tb;
  logic [31:0] block_i;
  logic [15:0] invalid_target_size_i;
  logic [7:0] invalid_pattern_id_i;
  logic [15:0] invalid_dictionary_i;

  wire success_s0;
  wire [7:0] pattern_id_s0;
  wire [15:0] dictionary_s0;
  wire [31:0] block_s0;
  wire valid_s0;
  wire success_s1;
  wire [7:0] pattern_id_s1;
  wire [7:0] dictionary_s1;
  wire [31:0] block_s1;
  wire valid_s1;
  wire top_success;
  wire [7:0] top_pattern_id;
  wire [15:0] top_dictionary;
  wire [15:0] top_target_size;
  wire [31:0] top_block;
  wire top_valid;
  wire invalid_valid;

  bsel_compressor_s0 compressor_s0 (
    .block_i(block_i), .success_o(success_s0), .pattern_id_o(pattern_id_s0),
    .dictionary_o(dictionary_s0)
  );
  bsel_decompressor_s0 decompressor_s0 (
    .pattern_id_i(pattern_id_s0), .dictionary_i(dictionary_s0),
    .block_o(block_s0), .valid_o(valid_s0)
  );
  bsel_compressor_s1 compressor_s1 (
    .block_i(block_i), .success_o(success_s1), .pattern_id_o(pattern_id_s1),
    .dictionary_o(dictionary_s1)
  );
  bsel_decompressor_s1 decompressor_s1 (
    .pattern_id_i(pattern_id_s1), .dictionary_i(dictionary_s1),
    .block_o(block_s1), .valid_o(valid_s1)
  );
  bsel_compressor_top compressor_top (
    .block_i(block_i), .success_o(top_success), .pattern_id_o(top_pattern_id),
    .dictionary_o(top_dictionary), .target_size_o(top_target_size)
  );
  bsel_decompressor_top decompressor_top (
    .target_size_i(top_target_size), .pattern_id_i(top_pattern_id),
    .dictionary_i(top_dictionary), .block_o(top_block), .valid_o(top_valid)
  );
  bsel_decompressor_top invalid_decompressor_top (
    .target_size_i(invalid_target_size_i), .pattern_id_i(invalid_pattern_id_i),
    .dictionary_i(invalid_dictionary_i), .block_o(), .valid_o(invalid_valid)
  );

  task check_expect(input logic condition, input [8*96-1:0] message);
    begin
      if (!condition) begin
        $display("RTL simulation failure: %0s", message);
        $fatal(1);
      end
    end
  endtask

  initial begin
    invalid_target_size_i = 16'd2;
    invalid_pattern_id_i = 8'd0;
    invalid_dictionary_i = '0;

    // 0010 is the first matching rank-two pattern for AABA.
    block_i = 32'h41424141;
    #1;
    check_expect(success_s0 && pattern_id_s0 == 8'd0 && dictionary_s0 == 16'h4241,
           "first matching pattern must win and select A,B");
    check_expect(valid_s0 && block_s0 == block_i, "set 0 round trip for AABA");
    check_expect(!success_s1, "tagged uniform target must reject AABA");
    check_expect(top_success && top_target_size == 16'd3 && top_pattern_id == 8'd0 &&
           top_valid && top_block == block_i, "top selects 3-byte target for AABA");
    check_expect(!invalid_valid, "wrong tagged target metadata must be invalid");

    // 0101 is the only rank-two match for ABAB.
    block_i = 32'h42414241;
    #1;
    check_expect(success_s0 && pattern_id_s0 == 8'd1 && dictionary_s0 == 16'h4241,
           "second pattern selects A,B for ABAB");
    check_expect(valid_s0 && block_s0 == block_i, "set 0 round trip for ABAB");
    check_expect(top_success && top_target_size == 16'd3 && top_pattern_id == 8'd1 &&
           top_valid && top_block == block_i, "top selects second 3-byte pattern");

    // Uniform data reaches the smaller tagged target and its metadata value 128.
    block_i = 32'h41414141;
    #1;
    check_expect(success_s0 && pattern_id_s0 == 8'd0, "set 0 retains priority on overlap");
    check_expect(success_s1 && pattern_id_s1 == 8'd128 && dictionary_s1 == 8'h41,
           "tagged set emits metadata value 128");
    check_expect(valid_s1 && block_s1 == block_i, "tagged set round trip for uniform data");
    check_expect(top_success && top_target_size == 16'd2 && top_pattern_id == 8'd128 &&
           top_dictionary[7:0] == 8'h41 && top_valid && top_block == block_i,
           "top selects the smallest tagged target for uniform data");
    $display("RTL simulation passed");
    $finish;
  end
endmodule
]=])

file(GLOB rtl_sources "${OUTPUT_DIRECTORY}/bsel_*.sv")
execute_process(
    COMMAND "${IVERILOG}" -g2012 -o "${simulator}" "${testbench}" ${rtl_sources}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "RTL compilation failed: ${output}${error}")
endif()

execute_process(
    COMMAND "${VVP}" "${simulator}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0 OR NOT output MATCHES "RTL simulation passed")
    message(FATAL_ERROR "RTL simulation failed: ${output}${error}")
endif()
