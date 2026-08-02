#include "byte_select/model_io.hpp"

#include <fstream>
#include <stdexcept>
#include <string>

namespace bsel {

void save_model(const Model& model, const std::string& path) {
    validate_model(model);
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("cannot open model for writing: " + path);
    }
    output << "BSEL_MODEL 2\n";
    output << "block_size " << model.block_size << '\n';
    output << "sets " << model.sets.size() << '\n';
    for (const auto& set : model.sets) {
        output << "set " << set.target_size << ' ' << set.metadata_bytes << ' '
               << set.metadata_tag_bits << ' ' << set.metadata_tag_value << ' '
               << set.patterns.size() << '\n';
        for (const auto& pattern : set.patterns) {
            output << "pattern " << pattern.to_string() << '\n';
        }
    }
}

Model load_model(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open model for reading: " + path);
    }
    std::string token;
    int version = 0;
    input >> token >> version;
    if (token != "BSEL_MODEL" || (version != 1 && version != 2)) {
        throw std::runtime_error("unsupported model format");
    }
    Model model;
    std::size_t set_count = 0;
    input >> token >> model.block_size;
    if (token != "block_size") {
        throw std::runtime_error("missing block_size");
    }
    input >> token >> set_count;
    if (token != "sets") {
        throw std::runtime_error("missing sets");
    }
    std::string line;
    std::getline(input, line);
    for (std::size_t i = 0; i < set_count; ++i) {
        PatternSet set;
        std::size_t pattern_count = 0;
        input >> token >> set.target_size >> set.metadata_bytes;
        if (version == 2) {
            input >> set.metadata_tag_bits >> set.metadata_tag_value;
        }
        input >> pattern_count;
        if (token != "set") {
            throw std::runtime_error("missing set");
        }
        std::getline(input, line);
        for (std::size_t j = 0; j < pattern_count; ++j) {
            std::getline(input, line);
            const std::string prefix = "pattern ";
            if (line.rfind(prefix, 0) != 0) {
                throw std::runtime_error("missing pattern");
            }
            Pattern pattern = Pattern::parse(line.substr(prefix.size()));
            if (pattern.size() != model.block_size) {
                throw std::runtime_error("model pattern has incorrect block size");
            }
            set.patterns.push_back(std::move(pattern));
        }
        model.sets.push_back(std::move(set));
    }
    input >> std::ws;
    if (!input.eof()) {
        throw std::runtime_error("unexpected trailing model data");
    }
    validate_model(model);
    return model;
}

}  // namespace bsel
