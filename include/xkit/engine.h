#pragma once

#include "xkit/widget.h"
#include "xkit/theme.h"
#include "xkit/render_node.h"
#include "xserver/unique_fd.h"
#include <memory>
#include <vector>
#include <cstdint>
#include <gbm.h>
#include <vulkan/vulkan.h>
#include "xrender/font_atlas.h"
#include "xrender/text_shaper.h"

namespace fbx::xkit {

class VisualRenderBox {
public:
    VisualRenderBox(Color c) : color_(c) {}
    void setBorderRadius(float r) { radius_ = r; }
    void setDropShadow(float blur, float alpha) { blur_ = blur; alpha_ = alpha; }
    void setSize(float w, float h) { width_ = w; height_ = h; }

private:
    [[maybe_unused]] Color color_;
    float radius_{0};
    float blur_{0};
    float alpha_{0};
    float width_{0};
    float height_{0};
};

class RenderResult {
public:
    RenderResult(int fd) : damage_x_(0), damage_y_(0), damage_width_(0), damage_height_(0), dmabuf_fd_(fd) {}
    RenderResult(int fd, float dx, float dy, float dw, float dh) 
        : damage_x_(dx), damage_y_(dy), damage_width_(dw), damage_height_(dh), dmabuf_fd_(fd) {}
    
    xserver::UniqueFd exportDmabuf() {
        if (dmabuf_fd_ < 0) return xserver::UniqueFd(-1);
        return xserver::UniqueFd(dup(dmabuf_fd_));
    }
    void waitForCommitFence() {}

    float damage_x_{0}, damage_y_{0}, damage_width_{0}, damage_height_{0};
    // Which of the engine's two alternating render buffers this frame used.
    // Carried to the compositor in FrameReady.buffer_index_ so it composites
    // the buffer the client just finished, not the one it's about to draw into.
    uint32_t buffer_index_{0};

private:
    int dmabuf_fd_{-1};
};

class VulkanEngine {
public:
    VulkanEngine();
    ~VulkanEngine();

    bool init(int w, int h);
    RenderResult paint(std::shared_ptr<RenderNode> root);
    uint32_t getStride() const noexcept;

    // When true, paint() clears the surface to fully transparent instead of the
    // opaque window background. The desktop shell uses this so its empty desktop
    // area lets app windows (which the compositor stacks below the shell) show
    // through; only its panels/overlays draw opaque pixels. Apps keep the
    // default opaque clear so their own background is solid.
    void setTransparentClear(bool t) noexcept { transparent_clear_ = t; }

    // Copy the last-painted render target back to CPU memory as packed pixels
    // (BGRA8, so a uint32_t reads as 0xAARRGGBB on little-endian). Inspection /
    // test hook: paint() must have run first. Returns an empty vector on failure
    // (no device, or the buffer/copy could not be created).
    std::vector<uint32_t> readbackPixels(uint32_t w, uint32_t h);

    uint32_t window_w_{1920};
    uint32_t window_h_{1080};

private:
    int drm_fd_{-1};
    struct gbm_device* gbm_dev_{nullptr};
    struct gbm_bo* gbm_bo_{nullptr};
    int dmabuf_fd_[2]{-1, -1};  // exported fd for each double-buffered render target

    std::unique_ptr<fbx::xrender::FontAtlas> font_atlas_;
    std::unique_ptr<fbx::xrender::TextShaper> text_shaper_;

    // Vulkan Handles
    VkInstance instance_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue queue_{VK_NULL_HANDLE};
    uint32_t queue_family_index_{0};
    // Double-buffered render targets. The client draws into buffer
    // current_buffer_ and hands that fd to the compositor, then flips the index
    // so the next frame draws into the OTHER buffer while the compositor is
    // still sampling this one — no shared buffer is read and written at once,
    // which is what caused tearing on the live 60Hz scanout. render_pass_ is
    // format-compatible with both framebuffers, so it stays single.
    static constexpr int kBufferCount = 2;
    VkImage render_target_[kBufferCount]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory render_memory_[kBufferCount]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView render_view_[kBufferCount]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkRenderPass render_pass_{VK_NULL_HANDLE};
    VkFramebuffer framebuffer_[kBufferCount]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    int current_buffer_{0};       // buffer the NEXT paint() will draw into
    int last_painted_buffer_{0};  // buffer the LAST paint() drew into (readback)
    bool transparent_clear_{false}; // clear to transparent (desktop shell) vs opaque
    VkCommandPool command_pool_{VK_NULL_HANDLE};
    
    // UI Pipelines
    VkPipeline solid_pipeline_{VK_NULL_HANDLE};
    VkPipelineLayout solid_pipeline_layout_{VK_NULL_HANDLE};
    
    VkPipeline image_pipeline_{VK_NULL_HANDLE};
    VkPipelineLayout image_pipeline_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout image_set_layout_{VK_NULL_HANDLE};
    
    VkPipeline text_pipeline_{VK_NULL_HANDLE};
    VkPipelineLayout text_pipeline_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout text_set_layout_{VK_NULL_HANDLE};

    VkSampler sampler_{VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};

    // Font Atlas Vulkan Resources
    VkImage atlas_image_{VK_NULL_HANDLE};
    VkDeviceMemory atlas_memory_{VK_NULL_HANDLE};
    VkImageView atlas_view_{VK_NULL_HANDLE};
    VkDescriptorSet atlas_descriptor_set_{VK_NULL_HANDLE};

    // Tracking images to descriptors
    std::unordered_map<const uint8_t*, VkDescriptorSet> image_descriptors_;
    std::unordered_map<const uint8_t*, VkImage> uploaded_images_;
    std::unordered_map<const uint8_t*, VkDeviceMemory> uploaded_image_memories_;
    std::unordered_map<const uint8_t*, VkImageView> uploaded_image_views_;

    bool initVulkan(int w, int h);
    void uploadFontAtlas();
    // Re-copy the (grown) CPU atlas page into the existing GPU atlas image.
    // Called when new glyphs were added mid-session (font_atlas_->isDirty()).
    void reuploadFontAtlas();
    VkDescriptorSet getOrCreateImageDescriptor(const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t size);
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    VkCommandBuffer command_buffer_{VK_NULL_HANDLE};
    VkFence render_fence_{VK_NULL_HANDLE};
};

} // namespace fbx::xkit
