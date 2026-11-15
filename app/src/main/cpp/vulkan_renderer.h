#pragma once

#include "preview_texture.h"

#include <android/asset_manager.h>
#include <android/native_window.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

class VulkanRenderer {
public:
    VulkanRenderer() = default;
    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;
    ~VulkanRenderer();

    void initialize(ANativeWindow* window, AAssetManager* assets, const sekiro::assets::Texture* texture);
    void invalidateSwapchain() { needsSwapchain_ = true; }
    bool draw();

private:
    struct FrameImage {
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkSemaphore rendered = VK_NULL_HANDLE;
    };

    void chooseDevice();
    bool createSwapchain();
    void destroySwapchain();

    ANativeWindow* window_ = nullptr;
    const sekiro::assets::Texture* texture_ = nullptr;
    AAssetManager* assets_ = nullptr;
    std::unique_ptr<PreviewTexture> preview_;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkSemaphore imageAvailable_ = VK_NULL_HANDLE;
    VkFence frameFinished_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    std::vector<FrameImage> frames_;
    bool needsSwapchain_ = true;
    bool loggedPresent_ = false;
};
