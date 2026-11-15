#include "assets/tpf.h"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
void u32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        bytes.at(offset + i) = static_cast<uint8_t>(value >> (8 * i));
    }
}

void runTests() {
    constexpr size_t dds = 36;
    std::vector<uint8_t> bytes(dds + 156);
    std::copy_n("TPF\0", 4, bytes.begin());
    u32(bytes, 8, 1);
    u32(bytes, 16, dds);
    u32(bytes, 20, 156);
    std::copy_n("DDS ", 4, bytes.begin() + dds);
    u32(bytes, dds + 4, 124);
    u32(bytes, dds + 12, 2);
    u32(bytes, dds + 16, 3);
    u32(bytes, dds + 76, 32);
    std::copy_n("DX10", 4, bytes.begin() + dds + 84);
    u32(bytes, dds + 128, 72);
    u32(bytes, dds + 132, 3);
    u32(bytes, dds + 140, 1);
    u32(bytes, dds + 148, 0x07E0F800);  // Red, then green.
    u32(bytes, dds + 152, 0b00111001);  // Green, two blends, red, then zero indices.

    const auto texture = sekiro::assets::readTpfBc1(bytes);
    const std::vector<uint8_t> expected{
        0, 255, 0, 255, 170, 85, 0, 255, 85, 170, 0, 255,
        255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255,
    };
    assert(texture.width == 3 && texture.height == 2 && texture.rgba == expected);

    bytes.pop_back();
    u32(bytes, 20, 155);
    try {
        (void)sekiro::assets::readTpfBc1(bytes);
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error("Truncated mip was accepted");
}
}

int main(int argc, char** argv) {
    runTests();
    std::cout << "BC1 palette, partial blocks and truncated mip passed\n";
    if (argc == 2) {
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) {
            throw std::runtime_error("Could not open TPF");
        }
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        const auto texture = sekiro::assets::readTpfBc1(bytes);
        std::cout << texture.width << 'x' << texture.height << ", " << texture.rgba.size()
                  << " decoded RGBA bytes\n";
    }
}
