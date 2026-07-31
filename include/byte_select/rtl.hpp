#pragma once

#include "byte_select/codec.hpp"

#include <cstddef>
#include <string>

namespace bsel {

std::string generate_compressor_sv(const Model& model, std::size_t set_index);
std::string generate_decompressor_sv(const Model& model, std::size_t set_index);
std::string generate_top_compressor_sv(const Model& model);
std::string generate_top_decompressor_sv(const Model& model);
void write_rtl(const Model& model, const std::string& output_directory);

}  // namespace bsel
