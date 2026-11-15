#pragma once

#include "assets/tpf.h"

#include <vulkan/vulkan.h>

class PreviewBuffer {
public:
    explicit PreviewBuffer(VkDevice device) : device_(device) {}
    PreviewBuffer(const PreviewBuffer&) = delete;
    PreviewBuffer& operator=(const PreviewBuffer&) = delete;
    ~PreviewBuffer();

    void upload(VkPhysicalDevice physicalDevice, const sekiro::assets::Texture& texture,
                VkExtent2D extent, bool bgra);
    void copyTo(VkCommandBuffer commandBuffer, VkImage image) const;

private:
    VkDevice device_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
};
