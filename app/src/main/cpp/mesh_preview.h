#pragma once

#include "assets/model_materials.h"
#include "preview_texture.h"

#include <memory>
#include <optional>

class DepthImage {
public:
    explicit DepthImage(VkDevice device) : device_(device) {}
    DepthImage(const DepthImage&) = delete;
    DepthImage& operator=(const DepthImage&) = delete;
    ~DepthImage();

    void create(VkPhysicalDevice physical, VkExtent2D size);
    VkFormat format() const { return format_; }
    VkImageView view() const { return view_; }

private:
    VkDevice device_;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
};

class MeshPreview {
public:
    explicit MeshPreview(VkDevice device) : device_(device), fallback_(device) {}
    MeshPreview(const MeshPreview&) = delete;
    MeshPreview& operator=(const MeshPreview&) = delete;
    ~MeshPreview();

    void initialize(VkPhysicalDevice physical, VkQueue queue, VkCommandBuffer commands,
                    VkRenderPass renderPass, const sekiro::assets::FlverModel& model,
                    std::span<const sekiro::assets::ModelTextureView> textures,
                    bool srgb, std::span<const uint32_t> vert, std::span<const uint32_t> frag);
    void draw(VkCommandBuffer commands, VkRect2D area) const;
    size_t texturedSurfaces() const;

private:
    VkDevice device_;
    PreviewTexture fallback_;
    std::vector<std::unique_ptr<PreviewTexture>> materialTextures_;
    VkBuffer geometry_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize indexOffset_ = 0;
    const sekiro::assets::FlverModel* model_ = nullptr;
    std::vector<std::optional<size_t>> surfaceTextures_;
};
