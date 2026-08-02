#include "byte_select/rtl.hpp"

#include <cerrno>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace bsel {
namespace {

std::vector<std::size_t> first_positions(const Pattern& pattern) {
    std::vector<std::size_t> result(pattern.rank(), pattern.size());
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const auto symbol = pattern.symbols()[i];
        if (result[symbol] == pattern.size()) {
            result[symbol] = i;
        }
    }
    return result;
}

std::string module_suffix(std::size_t index) {
    return "_s" + std::to_string(index);
}

void ensure_directory(const std::string& path) {
#ifdef _WIN32
    const int result = _mkdir(path.c_str());
#else
    const int result = mkdir(path.c_str(), 0755);
#endif
    if (result != 0 && errno != EEXIST) {
        throw std::runtime_error("cannot create RTL output directory: " + path);
    }
}

}  // namespace

std::string generate_compressor_sv(const Model& model, std::size_t set_index) {
    validate_model(model);
    if (set_index >= model.sets.size()) {
        throw std::invalid_argument("RTL set index out of range");
    }
    const auto& set = model.sets[set_index];
    const auto id_bits = set.metadata_bytes * 8;
    std::ostringstream out;
    out << "module bsel_compressor" << module_suffix(set_index) << " (\n"
        << "  input  logic [" << model.block_size * 8 - 1 << ":0] block_i,\n"
        << "  output logic success_o,\n"
        << "  output logic [" << id_bits - 1 << ":0] pattern_id_o,\n"
        << "  output logic [" << set.dictionary_size() * 8 - 1
        << ":0] dictionary_o\n"
        << ");\n"
        << "  always_comb begin\n"
        << "    success_o = 1'b0;\n"
        << "    pattern_id_o = '0;\n"
        << "    dictionary_o = '0;\n";
    for (std::size_t p = 0; p < set.patterns.size(); ++p) {
        const auto& pattern = set.patterns[p];
        const auto first = first_positions(pattern);
        out << "    if (!success_o && (1'b1";
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            const auto representative = first[pattern.symbols()[i]];
            if (i != representative) {
                out << " && (block_i[" << i * 8 << " +: 8] == block_i["
                    << representative * 8 << " +: 8])";
            }
        }
        out << ")) begin\n"
            << "      success_o = 1'b1;\n"
            << "      pattern_id_o = " << id_bits << "'d"
            << encode_pattern_metadata(set, p) << ";\n";
        for (std::size_t symbol = 0; symbol < pattern.rank(); ++symbol) {
            out << "      dictionary_o[" << symbol * 8
                << " +: 8] = block_i[" << first[symbol] * 8 << " +: 8];\n";
        }
        out << "    end\n";
    }
    out << "  end\nendmodule\n";
    return out.str();
}

std::string generate_decompressor_sv(const Model& model, std::size_t set_index) {
    validate_model(model);
    if (set_index >= model.sets.size()) {
        throw std::invalid_argument("RTL set index out of range");
    }
    const auto& set = model.sets[set_index];
    const auto id_bits = set.metadata_bytes * 8;
    std::ostringstream out;
    out << "module bsel_decompressor" << module_suffix(set_index) << " (\n"
        << "  input  logic [" << id_bits - 1 << ":0] pattern_id_i,\n"
        << "  input  logic [" << set.dictionary_size() * 8 - 1
        << ":0] dictionary_i,\n"
        << "  output logic [" << model.block_size * 8 - 1 << ":0] block_o,\n"
        << "  output logic valid_o\n"
        << ");\n"
        << "  always_comb begin\n"
        << "    block_o = '0;\n"
        << "    valid_o = 1'b1;\n"
        << "    case (pattern_id_i)\n";
    for (std::size_t p = 0; p < set.patterns.size(); ++p) {
        out << "      " << id_bits << "'d" << encode_pattern_metadata(set, p)
            << ": begin\n";
        const auto& pattern = set.patterns[p];
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            out << "        block_o[" << i * 8 << " +: 8] = dictionary_i["
                << pattern.symbols()[i] * 8 << " +: 8];\n";
        }
        out << "      end\n";
    }
    out << "      default: valid_o = 1'b0;\n"
        << "    endcase\n"
        << "  end\nendmodule\n";
    return out.str();
}

std::string generate_top_compressor_sv(const Model& model) {
    validate_model(model);
    std::size_t max_id_bits = 0;
    std::size_t max_dictionary_bytes = 0;
    for (const auto& set : model.sets) {
        max_id_bits = std::max(max_id_bits, set.metadata_bytes * 8);
        max_dictionary_bytes =
            std::max(max_dictionary_bytes, set.dictionary_size());
    }
    std::vector<std::size_t> order(model.sets.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        return model.sets[left].target_size < model.sets[right].target_size;
    });

    std::ostringstream out;
    out << "module bsel_compressor_top (\n"
        << "  input logic [" << model.block_size * 8 - 1 << ":0] block_i,\n"
        << "  output logic success_o,\n"
        << "  output logic [" << max_id_bits - 1 << ":0] pattern_id_o,\n"
        << "  output logic [" << max_dictionary_bytes * 8 - 1
        << ":0] dictionary_o,\n"
        << "  output logic [15:0] target_size_o\n"
        << ");\n";
    for (std::size_t i = 0; i < model.sets.size(); ++i) {
        const auto& set = model.sets[i];
        out << "  logic success_" << i << ";\n"
            << "  logic [" << set.metadata_bytes * 8 - 1 << ":0] pattern_id_"
            << i << ";\n"
            << "  logic [" << set.dictionary_size() * 8 - 1
            << ":0] dictionary_" << i << ";\n"
            << "  bsel_compressor" << module_suffix(i) << " compressor_" << i
            << " (.block_i(block_i), .success_o(success_" << i
            << "), .pattern_id_o(pattern_id_" << i
            << "), .dictionary_o(dictionary_" << i << "));\n";
    }
    out << "  always_comb begin\n"
        << "    success_o = 1'b0;\n"
        << "    pattern_id_o = '0;\n"
        << "    dictionary_o = '0;\n"
        << "    target_size_o = " << model.block_size << ";\n";
    for (const auto i : order) {
        const auto& set = model.sets[i];
        out << "    if (!success_o && success_" << i << ") begin\n"
            << "      success_o = 1'b1;\n"
            << "      pattern_id_o[" << set.metadata_bytes * 8 - 1
            << ":0] = pattern_id_" << i << ";\n"
            << "      dictionary_o[" << set.dictionary_size() * 8 - 1
            << ":0] = dictionary_" << i << ";\n"
            << "      target_size_o = 16'd" << set.target_size << ";\n"
            << "    end\n";
    }
    out << "  end\nendmodule\n";
    return out.str();
}

std::string generate_top_decompressor_sv(const Model& model) {
    validate_model(model);
    std::size_t max_id_bits = 0;
    std::size_t max_dictionary_bytes = 0;
    for (const auto& set : model.sets) {
        max_id_bits = std::max(max_id_bits, set.metadata_bytes * 8);
        max_dictionary_bytes =
            std::max(max_dictionary_bytes, set.dictionary_size());
    }
    std::ostringstream out;
    out << "module bsel_decompressor_top (\n"
        << "  input logic [15:0] target_size_i,\n"
        << "  input logic [" << max_id_bits - 1 << ":0] pattern_id_i,\n"
        << "  input logic [" << max_dictionary_bytes * 8 - 1
        << ":0] dictionary_i,\n"
        << "  output logic [" << model.block_size * 8 - 1 << ":0] block_o,\n"
        << "  output logic valid_o\n"
        << ");\n";
    for (std::size_t i = 0; i < model.sets.size(); ++i) {
        const auto& set = model.sets[i];
        out << "  logic [" << model.block_size * 8 - 1 << ":0] block_" << i
            << ";\n  logic valid_" << i << ";\n"
            << "  bsel_decompressor" << module_suffix(i) << " decompressor_" << i
            << " (.pattern_id_i(pattern_id_i[" << set.metadata_bytes * 8 - 1
            << ":0]), .dictionary_i(dictionary_i["
            << set.dictionary_size() * 8 - 1 << ":0]), .block_o(block_" << i
            << "), .valid_o(valid_" << i << "));\n";
    }
    out << "  always_comb begin\n"
        << "    block_o = '0;\n"
        << "    valid_o = 1'b0;\n"
        << "    case (target_size_i)\n";
    for (std::size_t i = 0; i < model.sets.size(); ++i) {
        out << "      16'd" << model.sets[i].target_size
            << ": begin block_o = block_" << i << "; valid_o = valid_" << i
            << "; end\n";
    }
    out << "      default: begin block_o = '0; valid_o = 1'b0; end\n"
        << "    endcase\n"
        << "  end\nendmodule\n";
    return out.str();
}

void write_rtl(const Model& model, const std::string& output_directory) {
    validate_model(model);
    ensure_directory(output_directory);
    for (std::size_t i = 0; i < model.sets.size(); ++i) {
        const auto suffix = module_suffix(i);
        std::ofstream compressor(output_directory + "/bsel_compressor" + suffix + ".sv");
        std::ofstream decompressor(output_directory + "/bsel_decompressor" + suffix + ".sv");
        if (!compressor || !decompressor) {
            throw std::runtime_error("cannot create RTL output files");
        }
        compressor << generate_compressor_sv(model, i);
        decompressor << generate_decompressor_sv(model, i);
    }
    std::ofstream top_compressor(output_directory + "/bsel_compressor_top.sv");
    std::ofstream top_decompressor(output_directory + "/bsel_decompressor_top.sv");
    if (!top_compressor || !top_decompressor) {
        throw std::runtime_error("cannot create RTL top-level files");
    }
    top_compressor << generate_top_compressor_sv(model);
    top_decompressor << generate_top_decompressor_sv(model);
}

}  // namespace bsel
