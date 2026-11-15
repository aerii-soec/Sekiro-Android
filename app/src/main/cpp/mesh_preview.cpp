#include "mesh_preview.h"
#include "vulkan_memory.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ranges>

namespace {
using Vec3 = std::array<float, 3>;

float dot(Vec3 a, Vec3 b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 unit(Vec3 a) {
    const float length = std::sqrt(dot(a, a));
    return {a[0] / length, a[1] / length, a[2] / length};
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

struct Push {
    std::array<float, 16> camera;
    std::array<float, 2> uvScale;
};
static_assert(sizeof(Push) == 18 * sizeof(float));

std::array<float, 16> inspectionCamera(const sekiro::assets::FlverModel& model, float aspect) {
    Vec3 center{};
    Vec3 delta{};
    for (size_t i = 0; i < 3; ++i) {
        center[i] = (model.maximum[i] + model.minimum[i]) * 0.5f;
        delta[i] = (model.maximum[i] - model.minimum[i]) * 0.5f;
    }
    const float radius = std::max(0.001f, std::sqrt(dot(delta, delta)));
    const Vec3 forward = unit({-1.5f, -0.9f, -3.0f});
    const Vec3 right = unit(cross(forward, {0.0f, 1.0f, 0.0f}));
    const Vec3 up = cross(right, forward);
    constexpr float tangent = 0.45f;
    const float limitingTangent = tangent * std::min(1.0f, aspect);
    const float distance = radius * 1.12f * std::sqrt(1.0f + limitingTangent * limitingTangent) /
                           limitingTangent;
    const Vec3 eye{center[0] - forward[0] * distance, center[1] - forward[1] * distance,
                   center[2] - forward[2] * distance};
    const float near = std::max(radius * 0.02f, distance - radius * 1.2f);
    const float far = distance + radius * 1.2f;
    const float z = far / (far - near);
    const float x = 1.0f / (aspect * tangent);
    const float y = -1.0f / tangent;
    const float forwardOffset = -dot(forward, eye);
    return {right[0] * x, up[0] * y, forward[0] * z, forward[0],
            right[1] * x, up[1] * y, forward[1] * z, forward[1],
            right[2] * x, up[2] * y, forward[2] * z, forward[2],
            -dot(right, eye) * x, -dot(up, eye) * y, forwardOffset * z - near * z, forwardOffset};
}

struct StagingBuffer {
    VkDevice device;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    ~StagingBuffer() {
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, memory, nullptr);
    }
};
}

void DepthImage::create(VkPhysicalDevice physical, VkExtent2D size) {
    for (auto candidate : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(physical, candidate, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            format_ = candidate;
            break;
        }
    }
    if (format_ == VK_FORMAT_UNDEFINED) {
        throw std::runtime_error("No supported Vulkan depth image format");
    }
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = format_;
    image.extent = {size.width, size.height, 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    checkVulkan(vkCreateImage(device_, &image, nullptr, &image_), "vkCreateImage (depth)");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, image_, &requirements);
    memory_ = allocateGpuMemory(device_, physical, requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    checkVulkan(vkBindImageMemory(device_, image_, memory_, 0), "vkBindImageMemory (depth)");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format_;
    view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    checkVulkan(vkCreateImageView(device_, &view, nullptr, &view_), "vkCreateImageView (depth)");
}

DepthImage::~DepthImage() {
    vkDestroyImageView(device_, view_, nullptr);
    vkDestroyImage(device_, image_, nullptr);
    vkFreeMemory(device_, memory_, nullptr);
}

void MeshPreview::initialize(VkPhysicalDevice physical, VkQueue queue, VkCommandBuffer commands,
                             VkRenderPass pass, const sekiro::assets::FlverModel& model,
                             std::span<const sekiro::assets::ModelTextureView> textures,
                             bool srgb, std::span<const uint32_t> vert, std::span<const uint32_t> frag) {
    model_ = &model;
    materialTextures_.resize(textures.size());
    for (const auto& surface : model.surfaces) {
        const auto stem = sekiro::assets::assetStem(model.materials[surface.material].diffuse);
        const auto match = std::ranges::find(textures, stem, &sekiro::assets::ModelTextureView::stem);
        if (!surface.hasUv || stem.empty() || match == textures.end()) {
            surfaceTextures_.push_back(std::nullopt);
            continue;
        }
        const auto index = static_cast<size_t>(match - textures.begin());
        if (!materialTextures_[index]) {
            auto image = std::make_unique<PreviewTexture>(device_);
            image->upload(physical, queue, commands, *match->image, srgb, true);
            materialTextures_[index] = std::move(image);
        }
        surfaceTextures_.push_back(index);
    }
    fallback_.upload(physical, queue, commands, {1, 1, {230, 230, 230, 255}}, srgb);
    fallback_.createPipeline(pass, vert, frag, true);
    indexOffset_ = model.vertices.size() * sizeof(sekiro::assets::FlverVertex);
    const VkDeviceSize size = indexOffset_ + model.indices.size() * sizeof(uint32_t);

    const VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_SHARING_MODE_EXCLUSIVE};
    checkVulkan(vkCreateBuffer(device_, &create, nullptr, &geometry_), "vkCreateBuffer (mesh)");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, geometry_, &requirements);
    memory_ = allocateGpuMemory(device_, physical, requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    checkVulkan(vkBindBufferMemory(device_, geometry_, memory_, 0), "vkBindBufferMemory (mesh)");

    StagingBuffer staging{device_};
    auto stageInfo = create;
    stageInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    checkVulkan(vkCreateBuffer(device_, &stageInfo, nullptr, &staging.buffer), "vkCreateBuffer (mesh staging)");
    vkGetBufferMemoryRequirements(device_, staging.buffer, &requirements);
    staging.memory = allocateGpuMemory(device_, physical, requirements,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    checkVulkan(vkBindBufferMemory(device_, staging.buffer, staging.memory, 0), "vkBindBufferMemory (mesh staging)");
    void* mapped = nullptr;
    checkVulkan(vkMapMemory(device_, staging.memory, 0, size, 0, &mapped), "vkMapMemory (mesh staging)");
    std::memcpy(mapped, model.vertices.data(), indexOffset_);
    std::memcpy(static_cast<uint8_t*>(mapped) + indexOffset_, model.indices.data(), size - indexOffset_);
    vkUnmapMemory(device_, staging.memory);

    checkVulkan(vkResetCommandBuffer(commands, 0), "vkResetCommandBuffer (mesh upload)");
    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                         VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    checkVulkan(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer (mesh upload)");
    const VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(commands, staging.buffer, geometry_, 1, &copy);
    const VkBufferMemoryBarrier visible{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, geometry_, 0, size};
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                         0, 0, nullptr, 1, &visible, 0, nullptr);
    checkVulkan(vkEndCommandBuffer(commands), "vkEndCommandBuffer (mesh upload)");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    checkVulkan(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit (mesh upload)");
    checkVulkan(vkQueueWaitIdle(queue), "vkQueueWaitIdle (mesh upload)");
}

void MeshPreview::draw(VkCommandBuffer commands, VkRect2D area) const {
    const VkViewport viewport{static_cast<float>(area.offset.x), static_cast<float>(area.offset.y),
                              static_cast<float>(area.extent.width), static_cast<float>(area.extent.height),
                              0.0f, 1.0f};
    Push push{inspectionCamera(*model_, viewport.width / viewport.height), {}};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, fallback_.pipeline());
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &area);
    constexpr VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commands, 0, 1, &geometry_, &offset);
    vkCmdBindIndexBuffer(commands, geometry_, indexOffset_, VK_INDEX_TYPE_UINT32);
    const auto layout = fallback_.pipelineLayout();
    for (size_t i = 0; i < model_->surfaces.size(); ++i) {
        const auto& surface = model_->surfaces[i];
        const auto image = surfaceTextures_[i];
        push.uvScale = image ? model_->materials[surface.material].diffuseScale :
                                      std::array{1.0f, 1.0f};
        const auto set = image ? materialTextures_[*image]->descriptor() : fallback_.descriptor();
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push), &push);
        vkCmdDrawIndexed(commands, surface.indexCount, 1, surface.firstIndex, 0, 0);
    }
}

size_t MeshPreview::texturedSurfaces() const {
    return static_cast<size_t>(std::ranges::count_if(surfaceTextures_, [](const auto& item) { return item.has_value(); }));
}

MeshPreview::~MeshPreview() {
    vkDestroyBuffer(device_, geometry_, nullptr);
    vkFreeMemory(device_, memory_, nullptr);
}
