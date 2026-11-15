#include "preview_texture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char* operation) {
    if (!condition) {
        throw std::runtime_error(operation);
    }
}

void vkCheck(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " returned " + std::to_string(result));
    }
}

std::vector<uint8_t> read(const char* path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<uint32_t> shader(const char* path) {
    const auto bytes = read(path);
    require(bytes.size() >= 20 && bytes.size() % 4 == 0, "Invalid shader size");
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    require(words.front() == 0x07230203, "Invalid SPIR-V magic");
    return words;
}

struct Vulkan {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkImage output = VK_NULL_HANDLE;
    VkDeviceMemory outputMemory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;

    ~Vulkan() {
        if (device) {
            vkDeviceWaitIdle(device);
            vkDestroyBuffer(device, readback, nullptr);
            vkFreeMemory(device, readbackMemory, nullptr);
            vkDestroyFramebuffer(device, framebuffer, nullptr);
            vkDestroyImageView(device, view, nullptr);
            vkDestroyImage(device, output, nullptr);
            vkFreeMemory(device, outputMemory, nullptr);
            vkDestroyRenderPass(device, pass, nullptr);
            vkDestroyCommandPool(device, commands, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        vkDestroyInstance(instance, nullptr);
    }

    void start() {
        const VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,
                                            "Sekiro Preview Test", 1, nullptr, 0, VK_API_VERSION_1_0};
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &application;
        vkCheck(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");
        uint32_t count = 0;
        vkCheck(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        require(count > 0, "No host Vulkan device");
        std::vector<VkPhysicalDevice> devices(count);
        vkCheck(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
        physical = devices.front();
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        std::cout << "Vulkan device " << properties.deviceName << '\n';

        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        const auto graphics = std::find_if(families.begin(), families.end(), [](const auto& family) {
            return (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        });
        require(graphics != families.end(), "No host graphics queue");
        const auto index = static_cast<uint32_t>(graphics - families.begin());

        const float priority = 1.0f;
        const VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                nullptr, 0, index, 1, &priority};
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        vkCheck(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, index, 0, &queue);

        const VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                              VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, index};
        vkCheck(vkCreateCommandPool(device, &poolInfo, nullptr, &commands), "vkCreateCommandPool");
        const VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                    nullptr, commands, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        vkCheck(vkAllocateCommandBuffers(device, &allocate, &command), "vkAllocateCommandBuffers");
    }

    VkDeviceMemory allocate(VkMemoryRequirements requirements, VkMemoryPropertyFlags flags) const {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physical, &properties);
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (properties.memoryTypes[i].propertyFlags & flags) == flags) {
                const VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
                                                requirements.size, i};
                VkDeviceMemory memory = VK_NULL_HANDLE;
                vkCheck(vkAllocateMemory(device, &info, nullptr, &memory), "vkAllocateMemory");
                return memory;
            }
        }
        throw std::runtime_error("No host Vulkan test memory type");
    }

    void createTarget(VkFormat format, VkExtent2D extent) {
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {extent.width, extent.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        vkCheck(vkCreateImage(device, &imageInfo, nullptr, &output), "vkCreateImage");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, output, &requirements);
        outputMemory = allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkCheck(vkBindImageMemory(device, output, outputMemory, 0), "vkBindImageMemory");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = output;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCheck(vkCreateImageView(device, &viewInfo, nullptr, &view), "vkCreateImageView");

        const VkAttachmentDescription color{0, format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                             VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                             VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
        const VkAttachmentReference target{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &target;
        const VkSubpassDependency dependency{0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0};
        const VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0,
                                               1, &color, 1, &subpass, 1, &dependency};
        vkCheck(vkCreateRenderPass(device, &passInfo, nullptr, &pass), "vkCreateRenderPass");
        const VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr,
            0, pass, 1, &view, extent.width, extent.height, 1};
        vkCheck(vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffer), "vkCreateFramebuffer");

        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = extent.width * extent.height * 4;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkCheck(vkCreateBuffer(device, &buffer, nullptr, &readback), "vkCreateBuffer");
        vkGetBufferMemoryRequirements(device, readback, &requirements);
        readbackMemory = allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkCheck(vkBindBufferMemory(device, readback, readbackMemory, 0), "vkBindBufferMemory");
    }

    std::vector<uint8_t> render(const PreviewTexture& preview, VkExtent2D extent) const {
        vkCheck(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer");
        const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkCheck(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
        constexpr VkClearValue clear{{{0.0f, 0.0f, 0.0f, 1.0f}}};
        const VkRenderPassBeginInfo draw{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, pass,
                                         framebuffer, {{0, 0}, extent}, 1, &clear};
        vkCmdBeginRenderPass(command, &draw, VK_SUBPASS_CONTENTS_INLINE);
        preview.draw(command, extent);
        vkCmdEndRenderPass(command);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(command, output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
        vkCheck(vkEndCommandBuffer(command), "vkEndCommandBuffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        vkCheck(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
        vkCheck(vkQueueWaitIdle(queue), "vkQueueWaitIdle");

        std::vector<uint8_t> pixels(extent.width * extent.height * 4);
        void* mapped = nullptr;
        vkCheck(vkMapMemory(device, readbackMemory, 0, pixels.size(), 0, &mapped), "vkMapMemory");
        std::memcpy(pixels.data(), mapped, pixels.size());
        vkUnmapMemory(device, readbackMemory);
        return pixels;
    }
};

void verify(const sekiro::assets::Texture& input, std::span<const uint32_t> vertex,
            std::span<const uint32_t> fragment, bool srgb) {
    Vulkan vulkan;
    vulkan.start();
    const VkExtent2D extent{input.width * 2, input.height};
    vulkan.createTarget(srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, extent);
    PreviewTexture preview(vulkan.device);
    preview.upload(vulkan.physical, vulkan.queue, vulkan.command, input, srgb);
    preview.createPipeline(vulkan.pass, vertex, fragment);
    const auto output = vulkan.render(preview, extent);

    const uint32_t left = input.width / 2;
    size_t colored = 0;
    for (uint32_t y = 0; y < extent.height; ++y) {
        for (uint32_t x = 0; x < extent.width; ++x) {
            std::array<uint8_t, 4> expected{0, 0, 0, 255};
            if (x >= left && x < left + input.width) {
                const size_t source = 4 * (y * input.width + x - left);
                if (input.rgba[source + 3] != 0) {
                    std::copy_n(input.rgba.data() + source, 4, expected.begin());
                }
            }
            colored += (expected[0] || expected[1] || expected[2]);
            const size_t target = 4 * (y * extent.width + x);
            for (size_t channel = 0; channel < expected.size(); ++channel) {
                const int difference = std::abs(static_cast<int>(output[target + channel]) - expected[channel]);
                require(difference <= (srgb && channel < 3 ? 1 : 0), "Shader output differs from original texel");
            }
        }
    }
    require(colored > 0, "Original sample yielded no visible texels");
    std::cout << (srgb ? "sRGB" : "UNORM") << " Vulkan readback matched "
              << input.width << 'x' << input.height << " original texture with "
              << colored << " nonblack texels and verified side margins\n";
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 4, "Usage: test_vulkan_preview TEXTURE.TPF PREVIEW.VERT.SPV PREVIEW.FRAG.SPV");
        const auto texture = sekiro::assets::readTpfBc1(read(argv[1]));
        const auto vertex = shader(argv[2]);
        const auto fragment = shader(argv[3]);
        verify(texture, vertex, fragment, false);
        verify(texture, vertex, fragment, true);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
