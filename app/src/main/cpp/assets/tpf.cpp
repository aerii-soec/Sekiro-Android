#include "assets/tpf.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace sekiro::assets {
namespace {
using Color = std::array<uint8_t, 4>;

uint32_t readU32(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
           static_cast<uint32_t>(bytes[offset + 1]) << 8 |
           static_cast<uint32_t>(bytes[offset + 2]) << 16 |
           static_cast<uint32_t>(bytes[offset + 3]) << 24;
}

Color rgb565(uint16_t code) {
    const unsigned r = (code >> 11) & 31;
    const unsigned g = (code >> 5) & 63;
    const unsigned b = code & 31;
    return {static_cast<uint8_t>((r << 3) | (r >> 2)),
            static_cast<uint8_t>((g << 2) | (g >> 4)),
            static_cast<uint8_t>((b << 3) | (b >> 2)), 255};
}

Color mix(const Color& a, const Color& b, unsigned wa, unsigned wb) {
    Color color{};
    for (size_t i = 0; i < color.size(); ++i) {
        color[i] = static_cast<uint8_t>((a[i] * wa + b[i] * wb) / (wa + wb));
    }
    return color;
}
}

Texture readTpfBc1(std::span<const uint8_t> bytes) {
    constexpr uint32_t ddsHeaderSize = 148;
    if (bytes.size() < 36 || !std::equal(bytes.begin(), bytes.begin() + 4, "TPF\0") ||
        readU32(bytes, 8) != 1 || bytes[12] != 0 || bytes[27] != 0) {
        throw std::runtime_error("Expected a single-texture PC TPF");
    }
    const auto offset = readU32(bytes, 16);
    const auto size = readU32(bytes, 20);
    if (offset < 36 || offset > bytes.size() || size < ddsHeaderSize || size > bytes.size() - offset) {
        throw std::runtime_error("TPF texture exceeds package bounds");
    }
    const auto dds = bytes.subspan(offset, size);
    if (!std::equal(dds.begin(), dds.begin() + 4, "DDS ") || readU32(dds, 4) != 124 ||
        readU32(dds, 76) != 32 || readU32(dds, 84) != 0x30315844 || readU32(dds, 128) != 72 ||
        readU32(dds, 132) != 3 || readU32(dds, 136) != 0 || readU32(dds, 140) != 1) {
        throw std::runtime_error("Expected a single 2D DX10 BC1 sRGB DDS");
    }
    const auto width = readU32(dds, 16);
    const auto height = readU32(dds, 12);
    constexpr uint32_t maxDimension = 4096;
    if (width == 0 || height == 0 || width > maxDimension || height > maxDimension) {
        throw std::runtime_error("DDS dimensions exceed preview limits");
    }
    const uint32_t blocksWide = (width + 3) / 4;
    const uint32_t blocksHigh = (height + 3) / 4;
    if (blocksWide * blocksHigh * 8 > size - ddsHeaderSize) {
        throw std::runtime_error("Truncated DDS first mip");
    }

    Texture texture{width, height, std::vector<uint8_t>(width * height * 4)};
    size_t position = ddsHeaderSize;
    for (uint32_t by = 0; by < blocksHigh; ++by) {
        for (uint32_t bx = 0; bx < blocksWide; ++bx) {
            const auto endpoints = readU32(dds, position);
            const auto indices = readU32(dds, position + 4);
            position += 8;
            const auto c0 = static_cast<uint16_t>(endpoints);
            const auto c1 = static_cast<uint16_t>(endpoints >> 16);
            const Color a = rgb565(c0);
            const Color b = rgb565(c1);
            const std::array<Color, 4> palette{a, b,
                c0 > c1 ? mix(a, b, 2, 1) : mix(a, b, 1, 1),
                c0 > c1 ? mix(a, b, 1, 2) : Color{}};
            for (uint32_t y = 0; y < 4 && by * 4 + y < height; ++y) {
                for (uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x) {
                    const auto& color = palette[(indices >> (2 * (y * 4 + x))) & 3];
                    const size_t pixel = ((by * 4 + y) * width + bx * 4 + x) * 4;
                    std::copy(color.begin(), color.end(), texture.rgba.data() + pixel);
                }
            }
        }
    }
    return texture;
}
}
