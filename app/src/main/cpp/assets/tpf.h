#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sekiro::assets {

struct Texture {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

// Decodes the first mip of a single-texture PC TPF with a 2D BC1 sRGB DDS.
Texture readTpfBc1(std::span<const uint8_t> bytes);
std::u16string tpfTextureName(std::span<const uint8_t> bytes);

}
