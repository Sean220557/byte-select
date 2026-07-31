#pragma once

#include "byte_select/codec.hpp"

#include <string>

namespace bsel {

void save_model(const Model& model, const std::string& path);
Model load_model(const std::string& path);

}  // namespace bsel
