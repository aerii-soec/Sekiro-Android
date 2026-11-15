#pragma once

#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string>

inline void checkVulkan(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " returned VkResult " + std::to_string(result));
    }
}

inline VkDeviceMemory allocateGpuMemory(VkDevice device, VkPhysicalDevice physical,
                                        const VkMemoryRequirements& requirements, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & flags) == flags) {
            const VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
                                            requirements.size, i};
            VkDeviceMemory memory = VK_NULL_HANDLE;
            checkVulkan(vkAllocateMemory(device, &info, nullptr, &memory), "vkAllocateMemory");
            return memory;
        }
    }
    throw std::runtime_error("No compatible Vulkan memory type");
}
