#include "vulkan_renderer.h"
#include "viewport.h"

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
        throw std::runtime_error(std::string(operation) + " returned VkResult " + std::to_string(result));
    }
}

std::vector<uint32_t> readShader(AAssetManager* manager, const char* path) {
    std::unique_ptr<AAsset, decltype(&AAsset_close)> asset(
        AAssetManager_open(manager, path, AASSET_MODE_BUFFER), &AAsset_close);
    if (!asset) {
        throw std::runtime_error(std::string("Could not open packaged shader ") + path);
    }
    const auto size = AAsset_getLength(asset.get());
    if (size < 20 || size % 4 != 0) {
        throw std::runtime_error(std::string("Invalid SPIR-V size for ") + path);
    }
    std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
    if (AAsset_read(asset.get(), words.data(), size) != size || words.front() != 0x07230203) {
        throw std::runtime_error(std::string("Could not read valid SPIR-V from ") + path);
    }
    __android_log_print(ANDROID_LOG_INFO, logTag, "Loaded shader %s (%ld bytes)", path, static_cast<long>(size));
    return words;
}

VKAPI_ATTR VkBool32 VKAPI_CALL validationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const auto priority = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
        ? ANDROID_LOG_ERROR : ANDROID_LOG_WARN;
    __android_log_print(priority, "SekiroVulkan", "%s: %s",
                        data->pMessageIdName ? data->pMessageIdName : "Vulkan", data->pMessage);
    return VK_FALSE;
}

bool rebuildNeeded(VkResult result) {
    return result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR;
}
}

void VulkanRenderer::initialize(ANativeWindow* window, AAssetManager* assets,
                                const sekiro::assets::Texture* texture,
                                const sekiro::assets::FlverModel* model, std::u16string_view textureStem) {
    model_ = model;
    textureStem_ = textureStem;
    window_ = window;
    assets_ = assets;
    texture_ = texture;
    const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,
                                "Sekiro Native Development", 1, nullptr, 0, VK_API_VERSION_1_0};
    std::vector<const char*> instanceExtensions{VK_KHR_SURFACE_EXTENSION_NAME,
                                                VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    uint32_t extensionCount = 0;
    check(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr),
          "vkEnumerateInstanceExtensionProperties");
    std::vector<VkExtensionProperties> extensions(extensionCount);
    check(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data()),
          "vkEnumerateInstanceExtensionProperties");
    extensions.resize(extensionCount);
    const bool debugUtils = std::any_of(extensions.begin(), extensions.end(), [](const auto& item) {
        return std::strcmp(item.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
    });
    if (debugUtils) {
        instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = instanceExtensions.size();
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();
#ifdef SEKIRO_DEBUG
    constexpr const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    uint32_t layerCount = 0;
    check(vkEnumerateInstanceLayerProperties(&layerCount, nullptr), "vkEnumerateInstanceLayerProperties");
    std::vector<VkLayerProperties> layers(layerCount);
    check(vkEnumerateInstanceLayerProperties(&layerCount, layers.data()), "vkEnumerateInstanceLayerProperties");
    layers.resize(layerCount);
    const bool hasValidation = std::any_of(layers.begin(), layers.end(), [](const auto& layer) {
        return std::strcmp(layer.layerName, validationLayer) == 0;
    });
    if (hasValidation) {
        instanceInfo.enabledLayerCount = 1;
        instanceInfo.ppEnabledLayerNames = &validationLayer;
    }
    __android_log_print(ANDROID_LOG_INFO, logTag, "Khronos validation layer %s",
                        hasValidation ? "found, enabling" : "unavailable");
#endif
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = validationMessage;
    instanceInfo.pNext = debugUtils ? &debug : nullptr;
    check(vkCreateInstance(&instanceInfo, nullptr, &instance_), "vkCreateInstance");
    if (debugUtils) {
        const auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (!createMessenger || createMessenger(instance_, &debug, nullptr, &debugMessenger_) != VK_SUCCESS) {
            __android_log_write(ANDROID_LOG_WARN, logTag, "Could not register Vulkan debug messenger");
        }
    }
    __android_log_print(ANDROID_LOG_INFO, logTag, "Vulkan 1.0 instance created; debug messenger %s",
                        debugMessenger_ ? "active" : "unavailable");

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
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
    __android_log_print(ANDROID_LOG_INFO, logTag, "Vulkan device %s (API %u.%u.%u, driver 0x%x), graphics/present queue %u",
                        properties.deviceName, VK_API_VERSION_MAJOR(properties.apiVersion),
                        VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion),
                        properties.driverVersion, queueFamily_);

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
    const auto preferredFormat = std::find_if(formats.begin(), formats.end(), [](const auto& candidate) {
        return candidate.format == VK_FORMAT_R8G8B8A8_SRGB || candidate.format == VK_FORMAT_B8G8R8A8_SRGB;
    });
    VkSurfaceFormatKHR format = preferredFormat == formats.end() ? formats.front() : *preferredFormat;
    if (format.format == VK_FORMAT_UNDEFINED) {
        format.format = VK_FORMAT_R8G8B8A8_UNORM;
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
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // Let Android's compositor handle orientation for this on-demand preview.
    info.preTransform = capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
        ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : capabilities.currentTransform;
    info.compositeAlpha = *alpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    const VkResult creation = vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_);
    if (creation == VK_ERROR_OUT_OF_DATE_KHR) {
        return false;
    }
    check(creation, "vkCreateSwapchainKHR");
    __android_log_print(ANDROID_LOG_INFO, logTag,
        "Swapchain %ux%u format=%d colorSpace=%d transform=0x%x current=0x%x FIFO",
        extent_.width, extent_.height, format.format, format.colorSpace, info.preTransform,
        capabilities.currentTransform);

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
    const VkAttachmentReference depthAttachment{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    std::array<VkAttachmentDescription, 2> attachments{color, {}};
    if (model_) {
        depth_ = std::make_unique<DepthImage>(device_);
        depth_->create(physicalDevice_, extent_);
        attachments[1] = {0, depth_->format(), VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
            VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &attachment;
    subpass.pDepthStencilAttachment = depth_ ? &depthAttachment : nullptr;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (depth_) {
        constexpr auto depthTests = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.srcStageMask |= depthTests;
        dependency.dstStageMask |= depthTests;
        dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    }

    VkRenderPassCreateInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassInfo.attachmentCount = depth_ ? 2 : 1;
    renderPassInfo.pAttachments = attachments.data();
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
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device_, &viewInfo, nullptr, &frame.view), "vkCreateImageView");

        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = renderPass_;
        const std::array views{frame.view, depth_ ? depth_->view() : VK_NULL_HANDLE};
        framebufferInfo.attachmentCount = depth_ ? 2 : 1;
        framebufferInfo.pAttachments = views.data();
        framebufferInfo.width = extent_.width;
        framebufferInfo.height = extent_.height;
        framebufferInfo.layers = 1;
        check(vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &frame.framebuffer), "vkCreateFramebuffer");

        const VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &frame.rendered), "vkCreateSemaphore");
    }
    const bool srgb = format.format == VK_FORMAT_R8G8B8A8_SRGB || format.format == VK_FORMAT_B8G8R8A8_SRGB ||
                      format.format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    if (model_) {
        const auto vert = readShader(assets_, "shaders/model.vert.spv");
        const auto frag = readShader(assets_, "shaders/model.frag.spv");
        mesh_ = std::make_unique<MeshPreview>(device_);
        mesh_->initialize(physicalDevice_, queue_, commandBuffer_, renderPass_, *model_,
                          texture_, textureStem_, srgb, vert, frag);
        __android_log_print(ANDROID_LOG_INFO, logTag,
            "FLVER geometry uploaded: %zu vertices, %zu triangles, %zu surfaces, %zu matched textured surfaces",
            model_->vertices.size(), model_->indices.size() / 3, model_->surfaces.size(), mesh_->texturedSurfaces());
    } else if (texture_) {
        const auto vert = readShader(assets_, "shaders/preview.vert.spv");
        const auto frag = readShader(assets_, "shaders/preview.frag.spv");
        preview_ = std::make_unique<PreviewTexture>(device_);
        preview_->upload(physicalDevice_, queue_, commandBuffer_, *texture_, srgb);
        __android_log_print(ANDROID_LOG_INFO, logTag, "Uploaded %ux%u RGBA texture (%zu bytes) to Vulkan image",
                            texture_->width, texture_->height, texture_->rgba.size());
        preview_->createPipeline(renderPass_, vert, frag);
        __android_log_print(ANDROID_LOG_INFO, logTag,
                            "Created texture graphics pipeline and bound combined image sampler descriptor");
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
        __android_log_write(ANDROID_LOG_INFO, logTag, "Swapchain out of date on image acquire");
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
    std::array<VkClearValue, 2> clear{};
    clear[0].color = {{0.015f, 0.015f, 0.015f, 1.0f}};
    clear[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = renderPass_;
    pass.framebuffer = frame.framebuffer;
    pass.renderArea.extent = extent_;
    pass.clearValueCount = depth_ ? 2 : 1;
    pass.pClearValues = clear.data();
    vkCmdBeginRenderPass(commandBuffer_, &pass, VK_SUBPASS_CONTENTS_INLINE);
    const auto area = safeViewport(extent_, cutout_);
    if (mesh_) {
        mesh_->draw(commandBuffer_, area);
    } else if (preview_) {
        preview_->draw(commandBuffer_, area);
    }
    vkCmdEndRenderPass(commandBuffer_);
    check(vkEndCommandBuffer(commandBuffer_), "vkEndCommandBuffer (preview frame)");

    constexpr VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
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
    if (needsSwapchain_) {
        __android_log_print(ANDROID_LOG_INFO, logTag, "Swapchain rebuild pending (acquire=%d present=%d)",
                            acquired, presented);
    }
    if (!loggedPresent_ && presented != VK_ERROR_OUT_OF_DATE_KHR) {
        __android_log_print(ANDROID_LOG_INFO, logTag, "Vulkan queued %s frame at %ux%u (image=%u VkResult=%d)",
                            mesh_ ? "indexed FLVER" : preview_ ? "sampled texture" : "clear-only", extent_.width, extent_.height,
                            image, presented);
        loggedPresent_ = true;
    }
    return presented != VK_ERROR_OUT_OF_DATE_KHR;
}

void VulkanRenderer::destroySwapchain() {
    loggedPresent_ = false;
    mesh_.reset();
    preview_.reset();
    for (const auto& frame : frames_) {
        vkDestroySemaphore(device_, frame.rendered, nullptr);
        vkDestroyFramebuffer(device_, frame.framebuffer, nullptr);
        vkDestroyImageView(device_, frame.view, nullptr);
    }
    frames_.clear();
    depth_.reset();
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
        if (debugMessenger_) {
            const auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroyMessenger) {
                destroyMessenger(instance_, debugMessenger_, nullptr);
            }
        }
        vkDestroyInstance(instance_, nullptr);
    }
}
