#pragma once

#include "fpc_bsel/bsel.hpp"

#include <string>

namespace fpc_bsel {

struct Model {
    BselModel prefix;
    BselModel residual;
};

Model train_model(const std::vector<Bytes>& blocks, std::size_t max_patterns);
Model train_model_stream(const std::string& path, std::size_t max_patterns,
                         std::size_t chunk_bytes, std::uint64_t* block_count = nullptr,
                         std::size_t codebook_budget_bytes = 0,
                         std::uint64_t offset_bytes = 0,
                         std::uint64_t length_bytes = 0);
void save_model(const Model& model, const std::string& path);
Model load_model(const std::string& path);

}
