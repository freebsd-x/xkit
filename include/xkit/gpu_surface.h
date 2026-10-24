#pragma once

// XGpuSurface — Direct GPU rendering for FreeBSD-X apps
//
// Allows an app to render frames using OpenGL ES (via EGL) directly into a
// DMA-buf backed buffer, which is then zero-copy composited by xserver.
//
// Usage:
//   auto gpu = XGpuSurface::create(surface_id, width, height);
//   gpu->beginFrame();
//   // ... OpenGL ES draw calls ...
//   gpu->endFrame(xbus_client); // sends DMA-buf fd to compositor
//
// Thread safety: not thread-safe; call from one render thread only.

#include <cstdint>
#include <memory>
#include <functional>

// Forward-declare EGL/GBM types to avoid header pollution.
typedef void*  EGLDisplay;
typedef void*  EGLContext;
typedef void*  EGLSurface;
typedef void*  EGLConfig;
struct gbm_device;
struct gbm_bo;

namespace fbx::xkit {

class XBusClient;

class XGpuSurface {
public:
    // Create a GPU surface backed by a GBM buffer object.
    // `drm_render_node` is the DRM render node path, e.g. "/dev/dri/renderD128".
    [[nodiscard]] static std::unique_ptr<XGpuSurface> create(
        uint32_t surface_id,
        uint32_t width,
        uint32_t height,
        const char* drm_render_node = "/dev/dri/renderD128") noexcept;

    ~XGpuSurface() noexcept;

    XGpuSurface(const XGpuSurface&) = delete;
    XGpuSurface& operator=(const XGpuSurface&) = delete;

    // Make the EGL context current and bind the framebuffer for drawing.
    // Must be called before any OpenGL ES commands.
    bool beginFrame() noexcept;

    // Flush GL commands, export the buffer as a DMA-buf fd, and send a
    // FrameReady message to xserver via the IPC socket at `sock_path`.
    // Returns true on success. The fd is consumed by xserver.
    bool endFrame(int ipc_fd) noexcept;

    [[nodiscard]] uint32_t width()  const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }
    [[nodiscard]] EGLContext context() const noexcept { return egl_ctx_; }

private:
    XGpuSurface() noexcept = default;

    uint32_t    surface_id_{0};
    uint32_t    width_{0};
    uint32_t    height_{0};

    int         drm_fd_{-1};
    gbm_device* gbm_{nullptr};
    gbm_bo*     bo_{nullptr};        // single GBM buffer object (swap via fence later)

    EGLDisplay  egl_dpy_{nullptr};
    EGLContext  egl_ctx_{nullptr};
    [[maybe_unused]] EGLSurface  egl_surf_{nullptr};  // EGL_KHR_surfaceless_context; null in that mode
    uint32_t    fbo_{0};             // GL framebuffer wrapping the GBM texture
    uint32_t    tex_{0};             // GL texture imported from GBM BO via EGL image
};

} // namespace fbx::xkit
