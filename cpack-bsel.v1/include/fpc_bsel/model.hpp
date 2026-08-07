#pragma once

#include "fpc_bsel/bsel.hpp"

#include <string>

namespace fpc_bsel {

struct Model {
    BselModel prefix;
    BselModel residual;
};

Model train_model(const std::vector<Bytes>& blocks, std::size_t max_patterns);
void save_model(const Model& model, const std::string& path);
Model load_model(const std::string& path);

}
