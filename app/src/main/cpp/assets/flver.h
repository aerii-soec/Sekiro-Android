#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sekiro::assets {

struct FlverVertex {
    std::array<float, 3> position;
    std::array<float, 3> normal;
    std::array<float, 2> uv;
};

struct FlverMaterial {
    std::u16string name;
    std::u16string mtd;
    std::u16string diffuse;
    std::array<float, 2> diffuseScale{1.0f, 1.0f};
};

struct FlverBone {
    std::u16string name;
    std::array<float, 3> translation;
    std::array<float, 3> rotation;
    std::array<float, 3> scale;
    int16_t parent;
};

struct FlverSurface {
    uint32_t firstIndex;
    uint32_t indexCount;
    uint32_t material;
    bool hasUv;
};

struct FlverModel {
    std::vector<FlverVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<FlverSurface> surfaces;
    std::vector<FlverMaterial> materials;
    std::vector<FlverBone> bones;
    std::array<float, 3> minimum;
    std::array<float, 3> maximum;
};

// Sekiro little-endian Unicode FLVER2, base LOD. Throws on unsupported geometry.
FlverModel readFlver2(std::span<const uint8_t> bytes);

// Match original file references without depending on their PC path prefixes.
std::u16string assetStem(std::u16string_view name);

}
