#pragma once

#include <array>
#include <vulkan/vulkan.h>

inline VkRect2D safeViewport(VkExtent2D extent, const std::array<uint32_t, 4>& insets) {
    const bool fitsWidth = static_cast<uint64_t>(insets[0]) + insets[2] < extent.width;
    const bool fitsHeight = static_cast<uint64_t>(insets[1]) + insets[3] < extent.height;
    return {{fitsWidth ? static_cast<int32_t>(insets[0]) : 0,
             fitsHeight ? static_cast<int32_t>(insets[1]) : 0},
            {extent.width - (fitsWidth ? insets[0] + insets[2] : 0),
             extent.height - (fitsHeight ? insets[1] + insets[3] : 0)}};
}
