#include "vulkan_renderer.h"

#include <android/log.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
constexpr char logTag[] = "SekiroNative";

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " returned " + std::to_string(result));
    }
}

bool rebuildNeeded(VkResult result) {
    return result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR;
}
}

void VulkanRenderer::initialize(ANativeWindow* window, const sekiro::assets::Texture* texture) {
    window_ = window;
    texture_ = texture;
    const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,
                                "Sekiro Native Development", 1, nullptr, 0, VK_API_VERSION_1_0};
    constexpr std::array instanceExtensions{VK_KHR_SURFACE_EXTENSION_NAME,
                                            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = instanceExtensions.size();
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();
    check(vkCreateInstance(&instanceInfo, nullptr, &instance_), "vkCreateInstance");

    VkAndroidSurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    surfaceInfo.window = window;
    check(vkCreateAndroidSurfaceKHR(instance_, &surfaceInfo, nullptr, &surface_),
          "vkCreateAndroidSurfaceKHR");
    chooseDevice();

    constexpr std::array deviceExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = deviceExtensions.size();
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    check(vkCreateDevice(physicalDevice_, &deviceInfo, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;
    check(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = commandPool_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device_, &allocate, &commandBuffer_), "vkAllocateCommandBuffers");
    const VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailable_), "vkCreateSemaphore");
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    check(vkCreateFence(device_, &fenceInfo, nullptr, &frameFinished_), "vkCreateFence");
}

void VulkanRenderer::chooseDevice() {
    uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "vkEnumeratePhysicalDevices");
    if (count == 0) {
        throw std::runtime_error("No Vulkan physical devices found");
    }
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "vkEnumeratePhysicalDevices");
    devices.resize(count);
    auto selected = std::find_if(devices.begin(), devices.end(), [this](VkPhysicalDevice device) {
        uint32_t extensionsCount = 0;
        check(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionsCount, nullptr),
              "vkEnumerateDeviceExtensionProperties");
        std::vector<VkExtensionProperties> extensions(extensionsCount);
        check(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionsCount, extensions.data()),
              "vkEnumerateDeviceExtensionProperties");
        extensions.resize(extensionsCount);
        const auto swapchain = std::find_if(extensions.begin(), extensions.end(), [](const auto& extension) {
            return std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
        });
        if (swapchain == extensions.end()) {
            return false;
        }

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
        for (uint32_t index = 0; index < familyCount; ++index) {
            VkBool32 present = VK_FALSE;
            check(vkGetPhysicalDeviceSurfaceSupportKHR(device, index, surface_, &present),
                  "vkGetPhysicalDeviceSurfaceSupportKHR");
            if (present && (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                queueFamily_ = index;
                return true;
            }
        }
        return false;
    });
    if (selected == devices.end()) {
        throw std::runtime_error("No Vulkan queue supports graphics and Android presentation");
    }
    physicalDevice_ = *selected;
}

bool VulkanRenderer::createSwapchain() {
    VkSurfaceCapabilitiesKHR capabilities{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &capabilities),
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    extent_ = capabilities.currentExtent;
    if (extent_.width == UINT32_MAX) {
        const int32_t width = ANativeWindow_getWidth(window_);
        const int32_t height = ANativeWindow_getHeight(window_);
        if (width <= 0 || height <= 0) {
            return false;
        }
        extent_ = {
            std::clamp(static_cast<uint32_t>(width), capabilities.minImageExtent.width,
                       capabilities.maxImageExtent.width),
            std::clamp(static_cast<uint32_t>(height), capabilities.minImageExtent.height,
                       capabilities.maxImageExtent.height),
        };
    }
    if (extent_.width == 0 || extent_.height == 0) {
        return false;
    }
    if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
        throw std::runtime_error("Surface does not support color attachment images");
    }

    uint32_t formatCount = 0;
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr),
          "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (formatCount == 0) {
        throw std::runtime_error("Surface has no Vulkan image formats");
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data()),
          "vkGetPhysicalDeviceSurfaceFormatsKHR");
    formats.resize(formatCount);
    const auto srgb = std::find_if(formats.begin(), formats.end(), [](const auto& candidate) {
        return candidate.format == VK_FORMAT_R8G8B8A8_SRGB || candidate.format == VK_FORMAT_B8G8R8A8_SRGB;
    });
    VkSurfaceFormatKHR format = srgb == formats.end() ? formats.front() : *srgb;
    if (format.format == VK_FORMAT_UNDEFINED) {
        format.format = VK_FORMAT_R8G8B8A8_UNORM;
    }

    const bool bgra = format.format == VK_FORMAT_B8G8R8A8_SRGB || format.format == VK_FORMAT_B8G8R8A8_UNORM;
    const bool rgba = format.format == VK_FORMAT_R8G8B8A8_SRGB || format.format == VK_FORMAT_R8G8B8A8_UNORM;
    const bool canPreview = (rgba || bgra) && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                            (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
    if (texture_ && canPreview) {
        try {
            preview_ = std::make_unique<PreviewBuffer>(device_);
            preview_->upload(physicalDevice_, *texture_, extent_, bgra);
        } catch (const std::exception& error) {
            __android_log_print(ANDROID_LOG_WARN, logTag, "Texture preview unavailable: %s", error.what());
            preview_.reset();
        }
    } else if (texture_) {
        __android_log_write(ANDROID_LOG_WARN, logTag, "Surface does not support the texture preview");
    }

    constexpr std::array alphaModes{VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                    VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                    VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                                    VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
    const auto alpha = std::find_if(alphaModes.begin(), alphaModes.end(), [&](auto mode) {
        return (capabilities.supportedCompositeAlpha & mode) != 0;
    });
    if (alpha == alphaModes.end()) {
        throw std::runtime_error("Surface has no supported composite alpha mode");
    }

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount != 0) {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = imageCount;
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (preview_) {
        info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = *alpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    const VkResult creation = vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_);
    if (creation == VK_ERROR_OUT_OF_DATE_KHR) {
        return false;
    }
    check(creation, "vkCreateSwapchainKHR");

    VkAttachmentDescription color{};
    color.format = format.format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    const VkAttachmentReference attachment{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &attachment;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &color;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    check(vkCreateRenderPass(device_, &renderPassInfo, nullptr, &renderPass_), "vkCreateRenderPass");

    check(vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr), "vkGetSwapchainImagesKHR");
    std::vector<VkImage> images(imageCount);
    check(vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, images.data()), "vkGetSwapchainImagesKHR");
    frames_.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        auto& frame = frames_[i];
        frame.image = images[i];
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device_, &viewInfo, nullptr, &frame.view), "vkCreateImageView");

        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = renderPass_;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &frame.view;
        framebufferInfo.width = extent_.width;
        framebufferInfo.height = extent_.height;
        framebufferInfo.layers = 1;
        check(vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &frame.framebuffer), "vkCreateFramebuffer");

        const VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &frame.rendered), "vkCreateSemaphore");
    }
    return true;
}

bool VulkanRenderer::draw() {
    if (needsSwapchain_) {
        check(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");
        destroySwapchain();
        if (!createSwapchain()) {
            return false;
        }
        needsSwapchain_ = false;
    }

    constexpr uint64_t frameWaitNs = 16'000'000;
    const VkResult ready = vkWaitForFences(device_, 1, &frameFinished_, VK_TRUE, frameWaitNs);
    if (ready == VK_TIMEOUT) {
        return false;
    }
    check(ready, "vkWaitForFences");
    uint32_t image = 0;
    // Use a bounded acquire so a suspended surface cannot stall the native event loop.
    const VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_, frameWaitNs,
                                                    imageAvailable_, VK_NULL_HANDLE, &image);
    if (acquired == VK_TIMEOUT || acquired == VK_NOT_READY) {
        return false;
    }
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        needsSwapchain_ = true;
        return false;
    }
    if (acquired != VK_SUBOPTIMAL_KHR) {
        check(acquired, "vkAcquireNextImageKHR");
    }
    const auto& frame = frames_.at(image);

    check(vkResetCommandBuffer(commandBuffer_, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer_, &begin), "vkBeginCommandBuffer");
    if (preview_) {
        preview_->copyTo(commandBuffer_, frame.image);
    } else {
        constexpr VkClearValue clear{{{0.025f, 0.07f, 0.085f, 1.0f}}};
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = renderPass_;
        pass.framebuffer = frame.framebuffer;
        pass.renderArea.extent = extent_;
        pass.clearValueCount = 1;
        pass.pClearValues = &clear;
        vkCmdBeginRenderPass(commandBuffer_, &pass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(commandBuffer_);
    }
    check(vkEndCommandBuffer(commandBuffer_), "vkEndCommandBuffer");

    const VkPipelineStageFlags waitStage = preview_ ? VK_PIPELINE_STAGE_TRANSFER_BIT
                                                   : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAvailable_;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &frame.rendered;
    check(vkResetFences(device_, 1, &frameFinished_), "vkResetFences");
    check(vkQueueSubmit(queue_, 1, &submit, frameFinished_), "vkQueueSubmit");

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &frame.rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &image;
    const VkResult presented = vkQueuePresentKHR(queue_, &present);
    if (!rebuildNeeded(presented)) {
        check(presented, "vkQueuePresentKHR");
    }
    needsSwapchain_ = acquired == VK_SUBOPTIMAL_KHR || rebuildNeeded(presented);
    if (!loggedPresent_ && presented != VK_ERROR_OUT_OF_DATE_KHR) {
        __android_log_print(ANDROID_LOG_INFO, logTag, "Vulkan %s frame queued at %ux%u",
                            preview_ ? "texture" : "clear", extent_.width, extent_.height);
        loggedPresent_ = true;
    }
    return presented != VK_ERROR_OUT_OF_DATE_KHR;
}

void VulkanRenderer::destroySwapchain() {
    preview_.reset();
    for (const auto& frame : frames_) {
        vkDestroySemaphore(device_, frame.rendered, nullptr);
        vkDestroyFramebuffer(device_, frame.framebuffer, nullptr);
        vkDestroyImageView(device_, frame.view, nullptr);
    }
    frames_.clear();
    vkDestroyRenderPass(device_, renderPass_, nullptr);
    renderPass_ = VK_NULL_HANDLE;
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

VulkanRenderer::~VulkanRenderer() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        vkDestroyFence(device_, frameFinished_, nullptr);
        vkDestroySemaphore(device_, imageAvailable_, nullptr);
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        vkDestroyInstance(instance_, nullptr);
    }
}
