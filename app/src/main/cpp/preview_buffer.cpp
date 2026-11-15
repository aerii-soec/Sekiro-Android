#include "preview_buffer.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace {
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " returned " + std::to_string(result));
    }
}

void fillPreview(uint8_t* destination, VkExtent2D extent, const sekiro::assets::Texture& texture,
                 bool bgra) {
    uint32_t width = extent.width;
    uint32_t height = extent.height;
    if (static_cast<uint64_t>(width) * texture.height > static_cast<uint64_t>(height) * texture.width) {
        width = std::max(1u, static_cast<uint32_t>(static_cast<uint64_t>(height) * texture.width /
                                                   texture.height));
    } else {
        height = std::max(1u, static_cast<uint32_t>(static_cast<uint64_t>(width) * texture.height /
                                                    texture.width));
    }
    const uint32_t left = (extent.width - width) / 2;
    const uint32_t top = (extent.height - height) / 2;
    const size_t pixelCount = static_cast<size_t>(extent.width) * extent.height;
    for (size_t i = 0; i < pixelCount; ++i) {
        const size_t channel = i * 4;
        destination[channel] = destination[channel + 1] = destination[channel + 2] = 24;
        destination[channel + 3] = 255;
    }
    for (uint32_t y = 0; y < height; ++y) {
        const size_t sourceRow = static_cast<uint64_t>(y) * texture.height / height * texture.width;
        for (uint32_t x = 0; x < width; ++x) {
            const size_t sourceColumn = static_cast<uint64_t>(x) * texture.width / width;
            const uint8_t* source = texture.rgba.data() + (sourceRow + sourceColumn) * 4;
            uint8_t* pixel = destination + ((static_cast<size_t>(top) + y) * extent.width + left + x) * 4;
            // BC1 has one-bit alpha. Leave transparent texels as the preview background.
            if (source[3]) {
                pixel[0] = source[bgra ? 2 : 0];
                pixel[1] = source[1];
                pixel[2] = source[bgra ? 0 : 2];
            }
        }
    }
}
}

void PreviewBuffer::upload(VkPhysicalDevice physicalDevice, const sekiro::assets::Texture& texture,
                           VkExtent2D extent, bool bgra) {
    extent_ = extent;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = byteCount;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device_, &info, nullptr, &buffer_), "vkCreateBuffer");

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer_, &requirements);
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    constexpr auto desired = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t index = 0;
    while (index < memoryProperties.memoryTypeCount) {
        if ((requirements.memoryTypeBits & (1u << index)) &&
            (memoryProperties.memoryTypes[index].propertyFlags & desired) == desired) {
            break;
        }
        ++index;
    }
    if (index == memoryProperties.memoryTypeCount) {
        throw std::runtime_error("No host-coherent Vulkan memory for texture preview");
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = index;
    check(vkAllocateMemory(device_, &allocate, nullptr, &memory_), "vkAllocateMemory");
    check(vkBindBufferMemory(device_, buffer_, memory_, 0), "vkBindBufferMemory");
    void* mapped = nullptr;
    check(vkMapMemory(device_, memory_, 0, byteCount, 0, &mapped), "vkMapMemory");
    fillPreview(static_cast<uint8_t*>(mapped), extent, texture, bgra);
    vkUnmapMemory(device_, memory_);
}

void PreviewBuffer::copyTo(VkCommandBuffer commandBuffer, VkImage image) const {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {extent_.width, extent_.height, 1};
    vkCmdCopyBufferToImage(commandBuffer, buffer_, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
}

PreviewBuffer::~PreviewBuffer() {
    vkDestroyBuffer(device_, buffer_, nullptr);
    vkFreeMemory(device_, memory_, nullptr);
}
