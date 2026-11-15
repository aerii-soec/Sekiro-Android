#include "assets/model_materials.h"

#include <algorithm>
#include <stdexcept>

namespace sekiro::assets {
namespace {
class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    std::span<const uint8_t> take(size_t len) {
        if (len > bytes_.size()) {
            throw std::runtime_error("Truncated bundled material pack");
        }
        const auto data = bytes_.first(len);
        bytes_ = bytes_.subspan(len);
        return data;
    }

    uint32_t word() {
        const auto data = take(4);
        return static_cast<uint32_t>(data[0]) | static_cast<uint32_t>(data[1]) << 8 |
               static_cast<uint32_t>(data[2]) << 16 | static_cast<uint32_t>(data[3]) << 24;
    }

    std::u16string stem(uint32_t length) {
        if (length == 0 || length > 128) {
            throw std::runtime_error("Invalid bundled material name length");
        }
        std::u16string result;
        for (auto byte : take(length)) {
            if (!((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '_')) {
                throw std::runtime_error("Invalid bundled material name");
            }
            result.push_back(static_cast<char16_t>(byte));
        }
        return result;
    }

    bool empty() const { return bytes_.empty(); }

private:
    std::span<const uint8_t> bytes_;
};
}

std::vector<ModelTexture> readModelMaterials(std::span<const uint8_t> bytes, FlverModel& model) {
    Reader reader(bytes);
    const auto header = reader.take(4);
    if (!std::equal(header.begin(), header.end(), "SKMP") || reader.word() != 1) {
        throw std::runtime_error("Unsupported bundled material pack");
    }
    const auto textureCount = reader.word();
    const auto bindingCount = reader.word();
    if (textureCount == 0 || textureCount > 16 || bindingCount > 64) {
        throw std::runtime_error("Bundled material pack table exceeds limits");
    }

    std::vector<ModelTexture> textures;
    textures.reserve(textureCount);
    for (uint32_t i = 0; i < textureCount; ++i) {
        const auto length = reader.word();
        const auto width = reader.word();
        const auto height = reader.word();
        const auto stem = reader.stem(length);
        if (width == 0 || height == 0 || width > 4096 || height > 4096) {
            throw std::runtime_error("Invalid bundled base-color dimensions");
        }
        const auto data = reader.take(static_cast<size_t>(width) * height * 4);
        textures.push_back({stem, {width, height, {data.begin(), data.end()}}});
    }

    std::vector<std::pair<std::u16string, uint32_t>> bindings;
    for (uint32_t i = 0; i < bindingCount; ++i) {
        const auto length = reader.word();
        const auto index = reader.word();
        if (index >= textures.size()) {
            throw std::runtime_error("Bundled material binding index exceeds texture list");
        }
        bindings.emplace_back(reader.stem(length), index);
    }
    if (!reader.empty()) {
        throw std::runtime_error("Unexpected data in bundled material pack");
    }

    for (auto& material : model.materials) {
        if (!material.diffuse.empty()) {
            continue;
        }
        const auto mtd = assetStem(material.mtd);
        const auto match = std::find_if(bindings.begin(), bindings.end(), [&](const auto& entry) {
            return entry.first == mtd;
        });
        if (match != bindings.end()) {
            material.diffuse = textures[match->second].stem;
        }
    }
    return textures;
}
}
