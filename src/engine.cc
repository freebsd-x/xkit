#include "xkit/engine.h"
#include <unordered_map>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include "xkit/widget.h"
#include "xkit/render_node.h"
#include "shared/logger.h"
#include "xrender/image_decoder.h"
#include "xrender/font_manager.h"
#include "xrender/text_shaper.h"
#include <vulkan/vulkan.h>
#include <iostream>
#include <vector>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <string>
#include <climits>
#include <unistd.h>
#include <fcntl.h>
#if defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

namespace fbx::xkit {

namespace {

// Directory containing the running executable, or "" if it can't be resolved.
std::string executable_dir() {
#if defined(__FreeBSD__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    char buf[PATH_MAX];
    size_t len = sizeof(buf);
    if (sysctl(mib, 4, buf, &len, nullptr, 0) == 0 && len > 0) {
        std::string p(buf);
        auto slash = p.find_last_of('/');
        if (slash != std::string::npos) return p.substr(0, slash);
    }
#elif defined(__linux__)
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string p(buf);
        auto slash = p.find_last_of('/');
        if (slash != std::string::npos) return p.substr(0, slash);
    }
#endif
    return {};
}

// Locate the directory holding the compiled UI shaders (ui_*.spv). Resolution
// order: $FREEBSDX_SHADER_DIR, then next to the executable, then a couple of
// dev build layouts relative to the cwd — so the client works no matter where
// it is launched from instead of hardcoding "build/".
std::string resolve_shader_dir() {
    auto has_shaders = [](const std::string& d) {
        return access((d + "/ui_solid.vert.spv").c_str(), R_OK) == 0;
    };
    if (const char* env = std::getenv("FREEBSDX_SHADER_DIR")) {
        if (env[0] && has_shaders(env)) return env;
    }
    std::vector<std::string> candidates;
    std::string exe = executable_dir();
    if (!exe.empty()) {
        candidates.push_back(exe + "/freebsdx.p");
        candidates.push_back(exe + "/../freebsdx.p");
    }
    candidates.push_back("build-root/freebsdx.p");  // dev fallback
    candidates.push_back("build/freebsdx.p");       // dev fallback
    for (const auto& d : candidates)
        if (has_shaders(d)) return d;
    return {};
}

} // namespace

VulkanEngine::VulkanEngine() {}

VulkanEngine::~VulkanEngine() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        if (render_fence_) vkDestroyFence(device_, render_fence_, nullptr);
        for (int b = 0; b < kBufferCount; ++b) {
            if (framebuffer_[b]) vkDestroyFramebuffer(device_, framebuffer_[b], nullptr);
            if (render_view_[b]) vkDestroyImageView(device_, render_view_[b], nullptr);
            if (render_target_[b]) vkDestroyImage(device_, render_target_[b], nullptr);
            if (render_memory_[b]) vkFreeMemory(device_, render_memory_[b], nullptr);
        }
        if (command_buffer_) vkFreeCommandBuffers(device_, command_pool_, 1, &command_buffer_);
        if (command_pool_) vkDestroyCommandPool(device_, command_pool_, nullptr);
        if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
        
        if (solid_pipeline_) vkDestroyPipeline(device_, solid_pipeline_, nullptr);
        if (image_pipeline_) vkDestroyPipeline(device_, image_pipeline_, nullptr);
        if (text_pipeline_) vkDestroyPipeline(device_, text_pipeline_, nullptr);
        if (solid_pipeline_layout_) vkDestroyPipelineLayout(device_, solid_pipeline_layout_, nullptr);
        if (image_pipeline_layout_) vkDestroyPipelineLayout(device_, image_pipeline_layout_, nullptr);
        if (text_pipeline_layout_) vkDestroyPipelineLayout(device_, text_pipeline_layout_, nullptr);
        
        if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
        if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
        if (image_set_layout_) vkDestroyDescriptorSetLayout(device_, image_set_layout_, nullptr);

        if (atlas_view_) vkDestroyImageView(device_, atlas_view_, nullptr);
        if (atlas_memory_) vkFreeMemory(device_, atlas_memory_, nullptr);
        if (atlas_image_) vkDestroyImage(device_, atlas_image_, nullptr);

        for (auto& [key, u] : uploaded_) {
            if (u.view) vkDestroyImageView(device_, u.view, nullptr);
            if (u.image) vkDestroyImage(device_, u.image, nullptr);
            if (u.memory) vkFreeMemory(device_, u.memory, nullptr);
        }

        vkDestroyDevice(device_, nullptr);
    }
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

bool VulkanEngine::init(int w, int h) {
    window_w_ = w;
    window_h_ = h;
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "xapp", 1, "xkit", 1, VK_API_VERSION_1_3};
    const char* ext[] = { VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME };
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &ai, 0, nullptr, 2, ext};
    if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) return false;

    uint32_t dc = 0; vkEnumeratePhysicalDevices(instance_, &dc, nullptr);
    std::vector<VkPhysicalDevice> devices(dc); vkEnumeratePhysicalDevices(instance_, &dc, devices.data());
    physical_device_ = devices[0];

    uint32_t qfc = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qfc, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qfc); vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qfc, qf.data());
    int qfi = -1; for(int i=0; i<(int)qfc; ++i) if(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }
    
    float qp = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, (uint32_t)qfi, 1, &qp};
    const char* dext[] = { VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, "VK_KHR_external_memory_fd" };
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &qci, 0, nullptr, 4, dext, nullptr};
    if (vkCreateDevice(physical_device_, &dci, nullptr, &device_) != VK_SUCCESS) return false;
    vkGetDeviceQueue(device_, qfi, 0, &queue_);

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, (uint32_t)qfi};
    vkCreateCommandPool(device_, &cpci, nullptr, &command_pool_);
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, command_pool_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    vkAllocateCommandBuffers(device_, &cbai, &command_buffer_);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    vkCreateFence(device_, &fci, nullptr, &render_fence_);

    // Render pass is format-compatible with both framebuffers — create it once.
    VkAttachmentDescription ca{0, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference cr{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, 1, &cr, nullptr, nullptr, 0, nullptr};
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &ca, 1, &sub, 0, nullptr};
    vkCreateRenderPass(device_, &rpi, nullptr, &render_pass_);

    auto fp = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(device_, "vkGetMemoryFdKHR");
    VkComponentMapping ident = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};

    // Create both double-buffered render targets (image + exported memory + fd +
    // view + framebuffer). Each is an independent BGRA8 OPTIMAL image backed by
    // exported (dmabuf) memory the compositor imports once.
    for (int b = 0; b < kBufferCount; ++b) {
        VkExternalMemoryImageCreateInfo ei{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ei, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, {(uint32_t)w, (uint32_t)h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr, VK_IMAGE_LAYOUT_UNDEFINED};
        vkCreateImage(device_, &ii, nullptr, &render_target_[b]);

        VkMemoryRequirements mr; vkGetImageMemoryRequirements(device_, render_target_[b], &mr);
        VkExportMemoryAllocateInfo ex{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ex, mr.size, findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        vkAllocateMemory(device_, &mai, nullptr, &render_memory_[b]);
        vkBindImageMemory(device_, render_target_[b], render_memory_[b], 0);

        VkMemoryGetFdInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr, render_memory_[b], VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
        int fd = -1; if (!fp || fp(device_, &gi, &fd) != VK_SUCCESS) return false;
        dmabuf_fd_[b] = fd;

        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, render_target_[b], VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, ident, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCreateImageView(device_, &vi, nullptr, &render_view_[b]);

        VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, render_pass_, 1, &render_view_[b], (uint32_t)w, (uint32_t)h, 1};
        vkCreateFramebuffer(device_, &fbi, nullptr, &framebuffer_[b]);
    }

    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 1, &b};
    vkCreateDescriptorSetLayout(device_, &lci, nullptr, &image_set_layout_);
    // Freeable sets: image textures come and go (evictUnusedImages).
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr,
                                    VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, 256, 1, &ps};
    vkCreateDescriptorPool(device_, &dpci, nullptr, &descriptor_pool_);
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, nullptr, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0, VK_FALSE, 0, VK_FALSE, VK_COMPARE_OP_NEVER, 0, 0, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_FALSE};
    vkCreateSampler(device_, &si, nullptr, &sampler_);

    auto createPL = [&](uint32_t s) { VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, s}; VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,0,0,1,&image_set_layout_,1,&pcr}; VkPipelineLayout pl; vkCreatePipelineLayout(device_,&pli,0,&pl); return pl; };
    solid_pipeline_layout_ = createPL(52); image_pipeline_layout_ = createPL(48); text_pipeline_layout_ = createPL(56);
    
    auto createP = [&](const char* v, const char* f, VkPipelineLayout l, VkPipeline* p) {
        std::ifstream fv(v, std::ios::ate|std::ios::binary); std::ifstream ff(f, std::ios::ate|std::ios::binary);
        if (!fv.is_open() || !ff.is_open()) return false;
        size_t sv = fv.tellg(); size_t sf = ff.tellg(); std::vector<char> bv(sv); std::vector<char> bf(sf); fv.seekg(0); ff.seekg(0); fv.read(bv.data(),sv); ff.read(bf.data(),sf);
        auto createM = [&](const std::vector<char>& b) { VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,0,0,b.size(),(const uint32_t*)b.data()}; VkShaderModule m; vkCreateShaderModule(device_,&ci,0,&m); return m; };
        VkShaderModule mv = createM(bv); VkShaderModule mf = createM(bf);
        VkPipelineShaderStageCreateInfo ss[] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,0,0,VK_SHADER_STAGE_VERTEX_BIT,mv,"main",0},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,0,0,VK_SHADER_STAGE_FRAGMENT_BIT,mf,"main",0}};
        VkPipelineVertexInputStateCreateInfo vis{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,0,0,0,0,0,0};
        VkPipelineInputAssemblyStateCreateInfo ias{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,0,0,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,0};
        VkViewport vp{0,0,1920,1080,0,1}; VkRect2D sc{{0,0},{1920,1080}}; VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,0,0,1,&vp,1,&sc};
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,0,0,0,0,VK_POLYGON_MODE_FILL,VK_CULL_MODE_NONE,VK_FRONT_FACE_CLOCKWISE,0,0,0,0,1};
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,0,0,VK_SAMPLE_COUNT_1_BIT,0,0,0,0,0};
        // Alpha composites "over" too (ONE, ONE_MINUS_SRC_ALPHA). It used to
        // be ONE, ZERO — every draw *replaced* the surface's alpha with its
        // own, so a button shadow, an anti-aliased edge or a text quad left a
        // see-through hole in an opaque window, and the compositor (which
        // honours per-pixel alpha) showed the wallpaper through it.
        VkPipelineColorBlendAttachmentState ba{1,VK_BLEND_FACTOR_SRC_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,VK_BLEND_OP_ADD,VK_BLEND_FACTOR_ONE,VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,VK_BLEND_OP_ADD,0xf};
        VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,0,0,0,VK_LOGIC_OP_COPY,1,&ba,{0,0,0,0}};
        VkDynamicState ds[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR }; VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,0,0,2,ds};
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,0,0,2,ss,&vis,&ias,0,&vps,&rs,&ms,0,&bs,&dsi,l,render_pass_,0,0,0};
        vkCreateGraphicsPipelines(device_, 0, 1, &pi, 0, p); vkDestroyShaderModule(device_,mv,0); vkDestroyShaderModule(device_,mf,0); return true;
    };
    std::string shader_dir = resolve_shader_dir();
    if (shader_dir.empty()) {
        LOG_ERROR("Could not locate UI shaders (ui_*.spv). Set FREEBSDX_SHADER_DIR to the directory containing them.");
        return false;
    }
    auto shp = [&](const char* name) { return shader_dir + "/" + name; };
    if (!createP(shp("ui_solid.vert.spv").c_str(), shp("ui_solid.frag.spv").c_str(), solid_pipeline_layout_, &solid_pipeline_)) {
        LOG_ERROR("Failed to load ui_solid shaders from {}", shader_dir); return false;
    }
    if (!createP(shp("ui_image.vert.spv").c_str(), shp("ui_image.frag.spv").c_str(), image_pipeline_layout_, &image_pipeline_)) {
        LOG_ERROR("Failed to load ui_image shaders from {}", shader_dir); return false;
    }
    if (!createP(shp("ui_text.vert.spv").c_str(), shp("ui_text.frag.spv").c_str(), text_pipeline_layout_, &text_pipeline_)) {
        LOG_ERROR("Failed to load ui_text shaders from {}", shader_dir); return false;
    }

    // --- Font System ---
    xrender::FontManager::init();
    std::string font_path;
    auto font_path_res = xrender::FontManager::findFontPath("DejaVu Sans", false, false);
    if (font_path_res) {
        font_path = *font_path_res;
    } else {
        // Common FreeBSD fallback paths
        const char* fallbacks[] = {
            "/usr/local/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/local/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
        };
        for (const char* p : fallbacks) {
            if (access(p, R_OK) == 0) { font_path = p; break; }
        }
    }
    font_atlas_ = xrender::GlyphCache::create();
    if (font_atlas_) {
        text_shaper_ = std::make_unique<xrender::TextShaper>();
        uploadFontAtlas();
        xrender::FontManager::setTextProvider(font_atlas_.get(), text_shaper_.get());
        if (!font_path.empty())
            LOG_INFO("Font system ready: {}", font_path);
        else
            LOG_INFO("Font system ready.");
    } else {
        LOG_INFO("Font atlas generation failed — text will render as blocks.");
    }

    return true;
}

// The face a text command asks for (family, weight, style), at the weight asked
// for to the nearest hundred: what a web page's layout measured with. (A face first
// wanted inside an app's Capsicum sandbox opens through the font directories
// XApplication::Run kept, so no weight has to be snapped to a preloaded one.) A face
// that can't be opened falls back to the default. Used both to warm the glyph atlas
// and to draw, so the lookups match.
// A face's own weight, from its OS/2 table (400 if it doesn't say).
static int faceWeight(const xrender::FontFace* f) noexcept {
    static std::unordered_map<uint32_t, int> known;
    if (!f || !f->ft_face_) return 400;
    if (const auto it = known.find(f->face_id_); it != known.end()) return it->second;
    int w = (f->ft_face_->style_flags & FT_STYLE_FLAG_BOLD) ? 700 : 400;
    if (const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(f->ft_face_, FT_SFNT_OS2)); os2 && os2->version != 0xFFFF && os2->usWeightClass > 0)
        w = os2->usWeightClass;
    known[f->face_id_] = w;
    return w;
}

static xrender::FontFace* faceFor(const fbx::shared::TextData* td) noexcept {
    std::string family(td->font_family_, strnlen(td->font_family_, sizeof(td->font_family_)));
    const int w = td->font_weight_ ? td->font_weight_ : 400;
    const auto weight = static_cast<xrender::FontWeight>(std::clamp((w + 50) / 100 * 100, 100, 900));
    auto style = td->font_style_ ? xrender::FontStyle::Italic : xrender::FontStyle::Normal;
    if (auto* f = xrender::FontManager::getFace(family, weight, style)) return f;
    return xrender::FontManager::getFace();
}

RenderResult VulkanEngine::paint(std::shared_ptr<RenderNode> root) {
    uint32_t w = window_w_, h = window_h_;
    // Draw into the buffer the compositor is NOT currently sampling.
    const int buf_idx = current_buffer_;
    vkWaitForFences(device_, 1, &render_fence_, VK_TRUE, UINT64_MAX); vkResetFences(device_, 1, &render_fence_);
    evictUnusedImages();   // the last frame's GPU work is done: safe to free what it didn't use
    // What is behind this window's own painting, for readable unstyled text:
    // the clear colour below, or for the transparent shell the (blue) desktop
    // wallpaper. Matches the layout audit's contrast check.
    // Light theme: adapt apps' hard-coded dark colours (render_node.h, adapt::).
    adapt::active() = !ThemeManager::getInstance().isDarkMode();
    RenderNode::paintBackdrop() = transparent_clear_ ? Color{0.06f, 0.16f, 0.38f, 1.0f}
                                                     : ThemeManager::getInstance().windowBackground();
    fbx::xrender::DisplayList dl; root->paint(dl, {0,0});

    // Ensure every glyph this frame's text needs is present in the atlas, then
    // re-upload the atlas texture if it grew. The atlas is sampled inside the
    // render pass below, so this must happen first. Without it the atlas stays
    // as uploaded at init (empty) and all glyph lookups miss — text renders
    // invisibly. The atlas stabilises after the first few frames (static UI
    // text), so re-uploads stop once every glyph has been seen once.
    if (font_atlas_ && text_shaper_) {
        for (const auto& cmd : dl.getCommands()) {
            if (cmd.type_ != fbx::shared::CommandType::DrawText) continue;
            const auto* td = reinterpret_cast<const fbx::shared::TextData*>(
                dl.getDataBuffer().data() + cmd.data_offset_);
            const char* str = reinterpret_cast<const char*>(td + 1);
            xrender::FontManager::ensureGlyphs(std::string(str, td->string_length_),
                                               faceFor(td), *font_atlas_);
        }
        if (font_atlas_->isDirty()) {
            reuploadFontAtlas();
            font_atlas_->clearDirty();
        }
    }

    static int client_loop_count = 0;
    if (client_loop_count++ % 60 == 0) std::cout << "[xkit] Painting frame with " << dl.getCommands().size() << " commands.\n" << std::flush;
    vkResetCommandBuffer(command_buffer_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,0,VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,0}; 
    vkBeginCommandBuffer(command_buffer_, &bi);
    
    VkClearValue cv;
    if (transparent_clear_) {
        // Desktop shell: transparent so app windows show through the empty desktop.
        cv.color.float32[0] = 0.0f; cv.color.float32[1] = 0.0f; cv.color.float32[2] = 0.0f; cv.color.float32[3] = 0.0f;
    } else {
        const Color bg = ThemeManager::getInstance().windowBackground();
        cv.color.float32[0] = bg.r; cv.color.float32[1] = bg.g; cv.color.float32[2] = bg.b; cv.color.float32[3] = 1.0f;
    }
    VkRenderPassBeginInfo rpb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,0,render_pass_,framebuffer_[buf_idx],{{0,0},{w,h}},1,&cv};
    vkCmdBeginRenderPass(command_buffer_, &rpb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)w, (float)h, 0, 1}; vkCmdSetViewport(command_buffer_, 0, 1, &vp);
    VkRect2D scissor = {{0,0},{w,h}}; vkCmdSetScissor(command_buffer_, 0, 1, &scissor);
    
    for (const auto& cmd : dl.getCommands()) {
        if (cmd.type_ == fbx::shared::CommandType::SetScissor) {
            const auto* sd = reinterpret_cast<const fbx::shared::ScissorData*>(
                dl.getDataBuffer().data() + cmd.data_offset_);
            VkRect2D sc = {
                { static_cast<int32_t>(sd->x_), static_cast<int32_t>(sd->y_) },
                { static_cast<uint32_t>(sd->width_), static_cast<uint32_t>(sd->height_) }
            };
            vkCmdSetScissor(command_buffer_, 0, 1, &sc);
        } else if (cmd.type_ == fbx::shared::CommandType::ResetScissor) {
            VkRect2D sc = {{0, 0}, {w, h}};
            vkCmdSetScissor(command_buffer_, 0, 1, &sc);
        } else if (cmd.type_ == fbx::shared::CommandType::DrawRect) {
            const auto* r = reinterpret_cast<const fbx::shared::RectData*>(dl.getDataBuffer().data() + cmd.data_offset_);
            vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, solid_pipeline_);
            // Layout (52 bytes / 13 floats, std430):
            //   pos(2) size(2) screen_size(2) border_radius(1) shadow_blur(1) color(4) shadow_alpha(1)
            float pc[13] = {
                r->x_, r->y_, r->width_, r->height_,
                (float)w, (float)h,
                r->border_radius_, r->shadow_blur_,
                r->r_, r->g_, r->b_, r->a_,
                r->shadow_alpha_
            };
            vkCmdPushConstants(command_buffer_, solid_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 52, pc);
            vkCmdDraw(command_buffer_, 4, 1, 0, 0);
        } else if (cmd.type_ == fbx::shared::CommandType::DrawImage) {
            const auto* id = reinterpret_cast<const fbx::shared::ImageData*>(dl.getDataBuffer().data() + cmd.data_offset_);
            if (id->pixel_ptr_) {
                const uint64_t key = id->texture_id_ ? (uint64_t{1} << 63) | id->texture_id_
                                                     : reinterpret_cast<uintptr_t>(id->pixel_ptr_);
                VkDescriptorSet set = getOrCreateImageDescriptor(key, id->pixel_ptr_, id->pixel_width_, id->pixel_height_, id->pixel_width_ * id->pixel_height_ * 4);
                if (!set) continue;
                vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, image_pipeline_);
                vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, image_pipeline_layout_, 0, 1, &set, 0, nullptr);
                // Push constants must match ui_image.vert's std430 layout exactly:
                //   pos(2) size(2) screen_size(2) uv0(2) uv1(2) — two vec2 UV pairs,
                //   NOT a vec4 (which std430 would pad to a 16-byte boundary).
                float pc[12] = { id->x_, id->y_, id->width_, id->height_, (float)w, (float)h, id->uv_x0_, id->uv_y0_, id->uv_x1_, id->uv_y1_, 0.0f, 0.0f };
                vkCmdPushConstants(command_buffer_, image_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, pc);
                vkCmdDraw(command_buffer_, 4, 1, 0, 0);
            }
        } else if (cmd.type_ == fbx::shared::CommandType::DrawText) {
            const auto* td = reinterpret_cast<const fbx::shared::TextData*>(
                dl.getDataBuffer().data() + cmd.data_offset_);

            if (!font_atlas_ || !text_shaper_ || !atlas_descriptor_set_) {
                // Font system not ready — draw a colored block as placeholder
                const char* fb_str = reinterpret_cast<const char*>(td + 1);
                float text_w = xrender::FontManager::measureText(
                    std::string(fb_str, td->string_length_), td->font_size_);
                vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, solid_pipeline_);
                float pc[13] = { td->x_, td->y_, text_w, td->font_size_,
                    (float)w, (float)h, 0.0f, 0.0f, td->r_, td->g_, td->b_, td->a_, 0.0f };
                vkCmdPushConstants(command_buffer_, solid_pipeline_layout_,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 52, pc);
                vkCmdDraw(command_buffer_, 4, 1, 0, 0);
            } else {
                // Extract the inline UTF-8 string that follows the TextData struct
                const char* str = reinterpret_cast<const char*>(td + 1);
                std::string text(str, td->string_length_);

                auto* face  = faceFor(td);   // the family / weight / style asked for
                // Bold asked of a family with no bold face (a page's one webfont): drawn
                // twice, the second a little to the right, as browsers make it up.
                const int wanted_weight = td->font_weight_ ? td->font_weight_ : 400;
                const bool fake_bold = wanted_weight >= 600 && faceWeight(face) < 600;
                const float bold_shift = std::clamp(td->font_size_ / 24.0f, 0.5f, 2.0f);
                auto* emoji = xrender::FontManager::getEmojiFace();
                auto glyphs = (face && text_shaper_)
                    ? text_shaper_->shapeText(text, face->hb_font_, face->face_id_,
                                              emoji ? emoji->hb_font_ : nullptr,
                                              emoji ? emoji->face_id_ : 0)
                    : std::vector<xrender::ShapedGlyph>{};

                // Atlas was built at 48px; scale to requested font_size_
                float scale = td->font_size_ / 48.0f;
                constexpr float atlas_w = static_cast<float>(xrender::GlyphCache::kPageSize);
                constexpr float atlas_h = static_cast<float>(xrender::GlyphCache::kPageSize);

                vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, text_pipeline_);
                vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    text_pipeline_layout_, 0, 1, &atlas_descriptor_set_, 0, nullptr);

                float pen_x = td->x_;
                // pen_y is the baseline. The text box is font_size tall, so place
                // the baseline ~0.8*font_size below the box top (approx ascent) —
                // otherwise glyphs (positioned as baseline - bearing_y) render
                // entirely above the box, so text sits too high and never centers
                // against adjacent elements.
                float pen_y = td->y_ + td->font_size_ * 0.8f;

                for (const auto& glyph : glyphs) {
                    const auto* metric = font_atlas_
                        ? font_atlas_->lookup(glyph.face_id_, glyph.glyph_index_)
                        : nullptr;
                    if (!metric || metric->width_ == 0.0f) {
                        pen_x += glyph.x_advance_ * scale;
                        continue;
                    }

                    float gx = pen_x + (metric->bearing_x_ + glyph.x_offset_) * scale;
                    float gy = pen_y - (metric->bearing_y_ + glyph.y_offset_) * scale;
                    float gw = metric->width_  * scale;
                    float gh = metric->height_ * scale;

                    float uv_x = metric->atlas_u_;
                    float uv_y = metric->atlas_v_;
                    float uv_w = metric->width_  / atlas_w;
                    float uv_h = metric->height_ / atlas_h;

                    // push_constant layout (56 bytes, matches ui_text.vert/frag):
                    //   color(4) pos(2) size(2) screen_size(2) uv_pos(2) uv_size(2)
                    // color is first so its vec4 is 16-byte aligned with no std430
                    // padding shifting the rest (see the shaders' comment).
                    float pc[14] = {
                        td->r_, td->g_, td->b_, td->a_,
                        gx, gy, gw, gh,
                        (float)w, (float)h,
                        uv_x, uv_y, uv_w, uv_h
                    };
                    vkCmdPushConstants(command_buffer_, text_pipeline_layout_,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 56, pc);
                    vkCmdDraw(command_buffer_, 4, 1, 0, 0);
                    if (fake_bold) {
                        pc[4] = gx + bold_shift;
                        vkCmdPushConstants(command_buffer_, text_pipeline_layout_,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 56, pc);
                        vkCmdDraw(command_buffer_, 4, 1, 0, 0);
                    }

                    pen_x += glyph.x_advance_ * scale;
                }
            }
        }
    }
    vkCmdEndRenderPass(command_buffer_);
    
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,render_target_[buf_idx],{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 1, &barrier);

    vkEndCommandBuffer(command_buffer_);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO,0,0,0,0,1,&command_buffer_,0,0};
    vkQueueSubmit(queue_, 1, &si, render_fence_); vkQueueWaitIdle(queue_);

    // This buffer is now fully rendered; hand it to the compositor and flip so
    // the next paint() draws into the other buffer.
    last_painted_buffer_ = buf_idx;
    current_buffer_ = (buf_idx + 1) % kBufferCount;
    RenderResult rr(dmabuf_fd_[buf_idx], 0, 0, w, h);
    rr.buffer_index_ = static_cast<uint32_t>(buf_idx);
    return rr;
}

std::vector<uint32_t> VulkanEngine::readbackPixels(uint32_t w, uint32_t h) {
    VkImage src = render_target_[last_painted_buffer_];  // inspect what paint() last drew
    if (!device_ || !src || w == 0 || h == 0) return {};
    VkDeviceSize s = static_cast<VkDeviceSize>(w) * h * 4;

    // Host-visible staging buffer to receive the copied image.
    VkBuffer sb = VK_NULL_HANDLE; VkDeviceMemory sm = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,0,0,s,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_SHARING_MODE_EXCLUSIVE,0,0};
    if (vkCreateBuffer(device_, &bci, nullptr, &sb) != VK_SUCCESS) return {};
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(device_, sb, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,0,mr.size,
        findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    vkAllocateMemory(device_, &ai, nullptr, &sm); vkBindBufferMemory(device_, sb, sm, 0);

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,0,command_pool_,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};
    VkCommandBuffer cmd = VK_NULL_HANDLE; vkAllocateCommandBuffers(device_, &cai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,0,VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,0};
    vkBeginCommandBuffer(cmd, &bi);
    // paint() leaves render_target_ in GENERAL layout; make its colour writes
    // visible to the upcoming transfer read.
    VkImageMemoryBarrier tb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,src,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &tb);
    VkBufferImageCopy r{0,0,0,{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{0,0,0},{w,h,1}};
    vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_GENERAL, sb, 1, &r);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO,0,0,0,0,1,&cmd,0,0};
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(queue_);

    void* d = nullptr; vkMapMemory(device_, sm, 0, s, 0, &d);
    std::vector<uint32_t> p(static_cast<size_t>(w) * h);
    if (d) std::memcpy(p.data(), d, static_cast<size_t>(s));
    vkUnmapMemory(device_, sm);
    vkDestroyBuffer(device_, sb, nullptr); vkFreeMemory(device_, sm, nullptr);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
    return p;
}

void VulkanEngine::releaseImage(UploadedImage& u) noexcept {
    if (u.set) vkFreeDescriptorSets(device_, descriptor_pool_, 1, &u.set);
    if (u.view) vkDestroyImageView(device_, u.view, nullptr);
    if (u.image) vkDestroyImage(device_, u.image, nullptr);
    if (u.memory) vkFreeMemory(device_, u.memory, nullptr);
    u = {};
}

void VulkanEngine::evictUnusedImages() noexcept {
    for (auto it = uploaded_.begin(); it != uploaded_.end();) {
        if (it->second.last_frame < frame_no_) { releaseImage(it->second); it = uploaded_.erase(it); }
        else ++it;
    }
    ++frame_no_;
}

VkDescriptorSet VulkanEngine::getOrCreateImageDescriptor(uint64_t key, const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t size) {
    if (auto it = uploaded_.find(key); it != uploaded_.end()) {
        it->second.last_frame = frame_no_;
        return it->second.set;
    }
    if (!pixels || w == 0 || h == 0) return VK_NULL_HANDLE;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,0,0,VK_IMAGE_TYPE_2D, (size == w*h ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM), {w,h,1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img; vkCreateImage(device_, &ii, 0, &img);
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(device_, img, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,0,mr.size, findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory mem; vkAllocateMemory(device_, &ai, 0, &mem); vkBindImageMemory(device_, img, mem, 0);
    VkBuffer sb; VkDeviceMemory sm; VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,0,0,size,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_SHARING_MODE_EXCLUSIVE,0,0};
    vkCreateBuffer(device_, &bci, 0, &sb); vkGetBufferMemoryRequirements(device_, sb, &mr);
    VkMemoryAllocateInfo sai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,0,mr.size,findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    vkAllocateMemory(device_, &sai, 0, &sm); vkBindBufferMemory(device_, sb, sm, 0);
    void* data; vkMapMemory(device_, sm, 0, size, 0, &data); std::memcpy(data, pixels, size); vkUnmapMemory(device_, sm);
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,0,command_pool_,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};
    VkCommandBuffer cmd; vkAllocateCommandBuffers(device_, &cbai, &cmd);
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,0,VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,0};
    vkBeginCommandBuffer(cmd, &cbi);
    VkImageMemoryBarrier tb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,0,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,img,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &tb);
    VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyBufferToImage(cmd, sb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    tb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; tb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    tb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; tb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, 0, 0, 0, 1, &tb);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO,0,0,0,0,1,&cmd,0,0};
    vkQueueSubmit(queue_, 1, &si, 0); vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd); vkDestroyBuffer(device_, sb, 0); vkFreeMemory(device_, sm, 0);
    VkComponentMapping ident = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,0,0,img,VK_IMAGE_VIEW_TYPE_2D,ii.format,ident,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkImageView view; vkCreateImageView(device_, &vi, 0, &view);
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,0,descriptor_pool_,1,&image_set_layout_};
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_, &dsai, &set) != VK_SUCCESS) {
        // Pool exhausted (more than 255 distinct images in one frame): skip it.
        vkDestroyImageView(device_, view, nullptr); vkDestroyImage(device_, img, nullptr); vkFreeMemory(device_, mem, nullptr);
        return VK_NULL_HANDLE;
    }
    VkDescriptorImageInfo dii{sampler_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,0,set,0,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&dii,0,0};
    vkUpdateDescriptorSets(device_, 1, &wds, 0, 0);
    uploaded_[key] = {set, img, mem, view, frame_no_};
    return set;
}

uint32_t VulkanEngine::findMemoryType(uint32_t f, VkMemoryPropertyFlags p) const {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physical_device_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((f & (1 << i)) && (mp.memoryTypes[i].propertyFlags & p) == p) return i;
    return 0;
}

uint32_t VulkanEngine::getStride() const noexcept { return window_w_ * 4; }

void VulkanEngine::uploadFontAtlas() {
    if (!font_atlas_ || font_atlas_->getPageCount() == 0) return;

    constexpr uint32_t aw = xrender::GlyphCache::kPageSize;
    constexpr uint32_t ah = xrender::GlyphCache::kPageSize;
    const auto& bitmap = font_atlas_->getPageBitmap(0);
    VkDeviceSize atlas_size = static_cast<VkDeviceSize>(aw) * ah; // R8_UNORM: 1 byte/pixel

    // GPU image (R8_UNORM, device-local)
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D,
        VK_FORMAT_R8_UNORM, {aw, ah, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, nullptr, VK_IMAGE_LAYOUT_UNDEFINED};
    vkCreateImage(device_, &ii, nullptr, &atlas_image_);
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(device_, atlas_image_, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size,
        findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    vkAllocateMemory(device_, &ai, nullptr, &atlas_memory_);
    vkBindImageMemory(device_, atlas_image_, atlas_memory_, 0);

    // Staging buffer (host-visible)
    VkBuffer sb; VkDeviceMemory sm;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, atlas_size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
    vkCreateBuffer(device_, &bci, nullptr, &sb);
    VkMemoryRequirements bmr; vkGetBufferMemoryRequirements(device_, sb, &bmr);
    VkMemoryAllocateInfo sai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, bmr.size,
        findMemoryType(bmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    vkAllocateMemory(device_, &sai, nullptr, &sm);
    vkBindBufferMemory(device_, sb, sm, 0);
    void* mapped; vkMapMemory(device_, sm, 0, atlas_size, 0, &mapped);
    std::memcpy(mapped, bitmap.data(), static_cast<size_t>(atlas_size));
    vkUnmapMemory(device_, sm);

    // Upload
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, command_pool_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd; vkAllocateCommandBuffers(device_, &cbai, &cmd);
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, 0};
    vkBeginCommandBuffer(cmd, &cbi);

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0,
        0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
        atlas_image_, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &barrier);

    VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {aw, ah, 1}};
    vkCmdCopyBufferToImage(cmd, sb, atlas_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, 0, 0, 0, 1, &barrier);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cmd, 0, 0};
    vkQueueSubmit(queue_, 1, &si, nullptr); vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
    vkDestroyBuffer(device_, sb, nullptr); vkFreeMemory(device_, sm, nullptr);

    // Image view — swizzle R into all channels so the shader reads .r correctly
    VkComponentMapping swizzle{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R};
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, atlas_image_,
        VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8_UNORM, swizzle, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCreateImageView(device_, &vi, nullptr, &atlas_view_);

    // Descriptor set (reuse image_set_layout_ — same binding as text_pipeline_layout_)
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, descriptor_pool_, 1, &image_set_layout_};
    vkAllocateDescriptorSets(device_, &dsai, &atlas_descriptor_set_);
    VkDescriptorImageInfo dii{sampler_, atlas_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, atlas_descriptor_set_, 0, 0, 1,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii, 0, 0};
    vkUpdateDescriptorSets(device_, 1, &wds, 0, 0);

    LOG_INFO("Font atlas uploaded to GPU: {}x{} R8_UNORM", aw, ah);
}

void VulkanEngine::reuploadFontAtlas() {
    if (!font_atlas_ || atlas_image_ == VK_NULL_HANDLE || font_atlas_->getPageCount() == 0) return;

    constexpr uint32_t aw = xrender::GlyphCache::kPageSize;
    constexpr uint32_t ah = xrender::GlyphCache::kPageSize;
    const auto& bitmap = font_atlas_->getPageBitmap(0);
    VkDeviceSize atlas_size = static_cast<VkDeviceSize>(aw) * ah;

    VkBuffer sb; VkDeviceMemory sm;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, atlas_size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
    vkCreateBuffer(device_, &bci, nullptr, &sb);
    VkMemoryRequirements bmr; vkGetBufferMemoryRequirements(device_, sb, &bmr);
    VkMemoryAllocateInfo sai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, bmr.size,
        findMemoryType(bmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    vkAllocateMemory(device_, &sai, nullptr, &sm);
    vkBindBufferMemory(device_, sb, sm, 0);
    void* mapped; vkMapMemory(device_, sm, 0, atlas_size, 0, &mapped);
    std::memcpy(mapped, bitmap.data(), static_cast<size_t>(atlas_size));
    vkUnmapMemory(device_, sm);

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, command_pool_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd; vkAllocateCommandBuffers(device_, &cbai, &cmd);
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, 0};
    vkBeginCommandBuffer(cmd, &cbi);
    // Existing atlas image is in SHADER_READ_ONLY_OPTIMAL — transition to TRANSFER_DST, copy, back.
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, atlas_image_, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &b);
    VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {aw, ah, 1}};
    vkCmdCopyBufferToImage(cmd, sb, atlas_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, 0, 0, 0, 1, &b);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cmd, 0, 0};
    vkQueueSubmit(queue_, 1, &si, nullptr); vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
    vkDestroyBuffer(device_, sb, nullptr); vkFreeMemory(device_, sm, nullptr);
}

bool VulkanEngine::initVulkan(int, int) { return true; }

} // namespace fbx::xkit
