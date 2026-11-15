#pragma once

#include "mesh_preview.h"

#include <android/asset_manager.h>
#include <android/native_window.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

class VulkanRenderer {
public:
    VulkanRenderer() = default;
    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;
    ~VulkanRenderer();

    void initialize(ANativeWindow* window, AAssetManager* assets, const sekiro::assets::Texture* texture,
                    const sekiro::assets::FlverModel* model, std::span<const sekiro::assets::ModelTextureView> materials);
    void invalidateSwapchain() { needsSwapchain_ = true; }
    void setCutout(std::array<uint32_t, 4> insets) { cutout_ = insets; }
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
    const sekiro::assets::FlverModel* model_ = nullptr;
    std::span<const sekiro::assets::ModelTextureView> materials_;
    std::unique_ptr<MeshPreview> mesh_;
    std::unique_ptr<DepthImage> depth_;
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
    std::array<uint32_t, 4> cutout_{};
    std::vector<FrameImage> frames_;
    bool needsSwapchain_ = true;
    bool loggedPresent_ = false;
};
