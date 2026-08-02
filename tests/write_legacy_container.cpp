#include <cstdint>
#include <fstream>
#include <stdexcept>

namespace {

template <typename T>
void write_integer(std::ostream& output, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        output.put(static_cast<char>((value >> (i * 8U)) & 0xffU));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    std::ofstream output(argv[1], std::ios::binary);
    if (!output) {
        return 3;
    }

    output.write("BSEL", 4);
    write_integer<std::uint32_t>(output, 1);
    write_integer<std::uint64_t>(output, 4);
    write_integer<std::uint32_t>(output, 4);
    write_integer<std::uint64_t>(output, 1);
    output.put(1);
    write_integer<std::uint32_t>(output, 0);
    write_integer<std::uint32_t>(output, 0);
    write_integer<std::uint32_t>(output, 2);
    output.put('A');
    output.put('B');
    write_integer<std::uint32_t>(output, 0);
    return output ? 0 : 4;
}
