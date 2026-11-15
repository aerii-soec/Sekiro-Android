#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <string_view>
#include <vector>

namespace fixture {
inline constexpr uint32_t boneTable = 0x80 + 2 * 0x20;

inline void write(std::vector<uint8_t>& bytes, size_t offset, uint32_t value, size_t count = 4) {
    for (size_t i = 0; i < count; ++i) {
        bytes.at(offset + i) = static_cast<uint8_t>(value >> (i * 8));
    }
}

inline uint32_t append(std::vector<uint8_t>& bytes, uint32_t length) {
    const auto offset = static_cast<uint32_t>(bytes.size());
    bytes.resize(offset + length);
    return offset;
}

inline uint32_t name(std::vector<uint8_t>& bytes, std::u16string_view value) {
    const auto offset = append(bytes, static_cast<uint32_t>((value.size() + 1) * 2));
    for (size_t i = 0; i < value.size(); ++i) {
        write(bytes, offset + i * 2, value[i], 2);
    }
    return offset;
}

inline uint32_t numbers(std::vector<uint8_t>& bytes, std::initializer_list<uint32_t> values) {
    const auto offset = append(bytes, static_cast<uint32_t>(values.size()) * 4);
    auto cursor = offset;
    for (auto value : values) {
        write(bytes, cursor, value);
        cursor += 4;
    }
    return offset;
}

inline std::vector<uint8_t> flver() {
    constexpr uint32_t material = 0x80;
    constexpr uint32_t mesh = boneTable + 2 * 0x80;
    constexpr uint32_t face = mesh + 2 * 0x30;
    constexpr uint32_t stream = face + 2 * 0x20;
    constexpr uint32_t layout = stream + 4 * 0x20;
    constexpr uint32_t texture = layout + 2 * 0x10;
    std::vector<uint8_t> bytes(texture + 0x20);
    constexpr std::array<uint8_t, 8> magic{'F', 'L', 'V', 'E', 'R', 0, 'L', 0};
    for (size_t i = 0; i < magic.size(); ++i) {
        bytes[i] = magic[i];
    }
    write(bytes, 8, 0x2001A);
    write(bytes, 0x18, 2);
    write(bytes, 0x1C, 2);
    write(bytes, 0x20, 2);
    write(bytes, 0x24, 4);
    bytes[0x49] = 1;
    write(bytes, 0x50, 2);
    write(bytes, 0x54, 2);
    write(bytes, 0x58, 1);

    const auto mtd = name(bytes, u"fixture.mtd");
    write(bytes, material, name(bytes, u"Textured foreground"));
    write(bytes, material + 4, mtd);
    write(bytes, material + 8, 1);
    write(bytes, material + 32, name(bytes, u"Untextured background"));
    write(bytes, material + 36, mtd);
    write(bytes, texture, name(bytes, u"n:\\TEST\\PreviewAlbedo.tif"));
    write(bytes, texture + 4, name(bytes, u"g_DiffuseTexture"));
    write(bytes, texture + 8, std::bit_cast<uint32_t>(1.0f));
    write(bytes, texture + 12, std::bit_cast<uint32_t>(1.0f));
    for (uint32_t i = 0; i < 2; ++i) {
        const auto row = boneTable + i * 0x80;
        write(bytes, row + 12, name(bytes, i ? u"child" : u"root"));
        write(bytes, row + 28, i ? 0 : 0xFFFF, 2);
        for (uint32_t channel = 0; channel < 3; ++channel) {
            write(bytes, row + 32 + channel * 4, std::bit_cast<uint32_t>(1.0f));
        }
    }

    write(bytes, layout, 1);
    write(bytes, layout + 12, numbers(bytes, {0, 0, 2, 0, 0}));
    write(bytes, layout + 16, 2);
    write(bytes, layout + 28, numbers(bytes, {0, 0, 0x11, 3, 0, 0, 4, 0x15, 5, 0}));
    for (uint32_t i = 0; i < 2; ++i) {
        const auto row = mesh + i * 0x30;
        write(bytes, row + 4, i);
        write(bytes, row + 32, 1);
        write(bytes, row + 36, numbers(bytes, {i}));
        write(bytes, row + 40, 2);
        write(bytes, row + 44, numbers(bytes, {i * 2, i * 2 + 1}));
    }

    const auto data = static_cast<uint32_t>(bytes.size());
    write(bytes, 12, data);
    for (uint32_t i = 0; i < 2; ++i) {
        const auto indices = i == 0 ? std::vector<uint16_t>{0, 1, 2, 3, 65535, 0, 0, 1}
                                    : std::vector<uint16_t>{0, 1, 2, 2, 1, 3};
        const auto buffer = append(bytes, static_cast<uint32_t>(indices.size() * 2));
        for (size_t j = 0; j < indices.size(); ++j) {
            write(bytes, buffer + j * 2, indices[j], 2);
        }
        const auto faceRow = face + i * 32;
        bytes[faceRow + 4] = static_cast<uint8_t>(i == 0);
        write(bytes, faceRow + 8, static_cast<uint32_t>(indices.size()));
        write(bytes, faceRow + 12, buffer - data);
        write(bytes, faceRow + 24, 16);

        const auto positions = append(bytes, 4 * 12);
        const auto attributes = append(bytes, 4 * 8);
        for (uint32_t v = 0; v < 4; ++v) {
            const std::array point{v & 1 ? 1.0f : -1.0f, v >> 1 ? -1.0f : 1.0f, i ? -0.25f : 0.25f};
            for (size_t axis = 0; axis < 3; ++axis) {
                write(bytes, positions + v * 12 + axis * 4, std::bit_cast<uint32_t>(point[axis]));
            }
            write(bytes, attributes + v * 8, 0x00FE7F7F);
            write(bytes, attributes + v * 8 + 4, v & 1 ? 2048 : 0, 2);
            write(bytes, attributes + v * 8 + 6, v >> 1 ? 2048 : 0, 2);
        }
        for (uint32_t j = 0; j < 2; ++j) {
            const auto streamRow = stream + (i * 2 + j) * 32;
            write(bytes, streamRow, j);
            write(bytes, streamRow + 4, j);
            write(bytes, streamRow + 8, j ? 8 : 12);
            write(bytes, streamRow + 12, 4);
            write(bytes, streamRow + 28, (j ? attributes : positions) - data);
        }
    }
    write(bytes, 16, static_cast<uint32_t>(bytes.size()) - data);
    return bytes;
}
}
