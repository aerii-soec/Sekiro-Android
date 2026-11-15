#pragma once

#include "assets/flver.h"
#include "assets/tpf.h"

namespace sekiro::assets {

struct ModelTexture {
    std::u16string stem;
    Texture image;
};

struct ModelTextureView {
    std::u16string_view stem;
    const Texture* image;
};

// Reads a staged base-color pack and resolves missing FLVER references by MTD stem.
std::vector<ModelTexture> readModelMaterials(std::span<const uint8_t> bytes, FlverModel& model);

}
