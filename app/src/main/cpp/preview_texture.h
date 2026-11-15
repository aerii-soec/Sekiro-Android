#pragma once

#include "assets/tpf.h"

#include <vulkan/vulkan.h>

class PreviewTexture {
public:
    explicit PreviewTexture(VkDevice device) : device_(device) {}
    PreviewTexture(const PreviewTexture&) = delete;
    PreviewTexture& operator=(const PreviewTexture&) = delete;
    ~PreviewTexture();

    void upload(VkPhysicalDevice physicalDevice, VkQueue queue, VkCommandBuffer commands,
                const sekiro::assets::Texture& texture, bool srgbAttachment, bool repeating = false);
    void createPipeline(VkRenderPass renderPass, std::span<const uint32_t> vertex,
                        std::span<const uint32_t> fragment, bool mesh = false);
    void draw(VkCommandBuffer commands, VkRect2D area) const;

    VkDescriptorSet descriptor() const { return descriptor_; }
    VkPipeline pipeline() const { return pipeline_; }
    VkPipelineLayout pipelineLayout() const { return pipelineLayout_; }

private:
    VkDevice device_;
    VkExtent2D size_{};
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};
