#include "xkit/gpu_surface.h"
#include "shared/protocol.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>
#include <iostream>

// GBM / EGL / GL headers
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <libdrm/drm_fourcc.h>
#include <xf86drm.h>

namespace fbx::xkit {

// ─── EGL extension function pointers ──────────────────────────────────────────

using PFN_eglCreateImageKHR     = EGLImageKHR (*)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*);
using PFN_eglDestroyImageKHR    = EGLBoolean  (*)(EGLDisplay, EGLImageKHR);
using PFN_glEGLImageTargetTex2D = void        (*)(GLenum, GLeglImageOES);
using PFN_eglGetPlatformDisplay = EGLDisplay  (*)(EGLenum, void*, const EGLAttrib*);

static PFN_eglCreateImageKHR     s_eglCreateImage    = nullptr;
static PFN_eglDestroyImageKHR    s_eglDestroyImage   = nullptr;
static PFN_glEGLImageTargetTex2D s_glEGLImageTarget  = nullptr;
static PFN_eglGetPlatformDisplay s_eglGetPlatformDpy = nullptr;

static bool load_extensions() noexcept {
    s_eglCreateImage    = reinterpret_cast<PFN_eglCreateImageKHR>(
        eglGetProcAddress("eglCreateImageKHR"));
    s_eglDestroyImage   = reinterpret_cast<PFN_eglDestroyImageKHR>(
        eglGetProcAddress("eglDestroyImageKHR"));
    s_glEGLImageTarget  = reinterpret_cast<PFN_glEGLImageTargetTex2D>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    s_eglGetPlatformDpy = reinterpret_cast<PFN_eglGetPlatformDisplay>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    return s_eglCreateImage && s_eglDestroyImage && s_glEGLImageTarget;
}

// ─── XGpuSurface::create ─────────────────────────────────────────────────────

std::unique_ptr<XGpuSurface> XGpuSurface::create(
    uint32_t surface_id, uint32_t width, uint32_t height,
    const char* drm_render_node) noexcept
{
    auto s = std::unique_ptr<XGpuSurface>(new XGpuSurface());
    s->surface_id_ = surface_id;
    s->width_      = width;
    s->height_     = height;

    // 1. Open DRM render node
    s->drm_fd_ = open(drm_render_node, O_RDWR | O_CLOEXEC);
    if (s->drm_fd_ < 0) {
        std::cerr << "[gpu_surface] Failed to open DRM render node: " << drm_render_node << "\n";
        return nullptr;
    }

    // 2. Create GBM device + buffer object (XRGB8888, scanout+texture)
    s->gbm_ = gbm_create_device(s->drm_fd_);
    if (!s->gbm_) {
        std::cerr << "[gpu_surface] gbm_create_device failed\n";
        return nullptr;
    }

    s->bo_ = gbm_bo_create(s->gbm_, width, height, GBM_FORMAT_XRGB8888,
                            GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!s->bo_) {
        std::cerr << "[gpu_surface] gbm_bo_create failed\n";
        return nullptr;
    }

    if (!load_extensions()) {
        std::cerr << "[gpu_surface] Required EGL extensions not available\n";
        return nullptr;
    }

    // 3. Create EGL display from GBM device
    EGLDisplay dpy = EGL_NO_DISPLAY;
    if (s_eglGetPlatformDpy) {
        dpy = s_eglGetPlatformDpy(EGL_PLATFORM_GBM_KHR,
                                   reinterpret_cast<void*>(s->gbm_), nullptr);
    }
    if (dpy == EGL_NO_DISPLAY) {
        dpy = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(s->gbm_));
    }
    if (dpy == EGL_NO_DISPLAY) {
        std::cerr << "[gpu_surface] eglGetDisplay failed\n";
        return nullptr;
    }
    s->egl_dpy_ = dpy;

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        std::cerr << "[gpu_surface] eglInitialize failed\n";
        return nullptr;
    }

    // 4. Choose EGL config (OpenGL ES 2.0, RGBA8)
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,   8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint num = 0;
    if (!eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &num) || num == 0) {
        std::cerr << "[gpu_surface] eglChooseConfig failed\n";
        return nullptr;
    }

    // 5. Create context
    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    s->egl_ctx_ = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (s->egl_ctx_ == EGL_NO_CONTEXT) {
        std::cerr << "[gpu_surface] eglCreateContext failed\n";
        return nullptr;
    }

    // 6. Make current (surfaceless via EGL_NO_SURFACE — requires EGL_KHR_surfaceless_context)
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s->egl_ctx_)) {
        std::cerr << "[gpu_surface] eglMakeCurrent (surfaceless) failed\n";
        return nullptr;
    }

    // 7. Import GBM BO into GL via EGLImage → GL texture → FBO
    EGLImageKHR image = s_eglCreateImage(dpy, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR,
        reinterpret_cast<EGLClientBuffer>(s->bo_), nullptr);
    if (image == EGL_NO_IMAGE_KHR) {
        std::cerr << "[gpu_surface] eglCreateImageKHR failed\n";
        return nullptr;
    }

    glGenTextures(1, &s->tex_);
    glBindTexture(GL_TEXTURE_2D, s->tex_);
    s_glEGLImageTarget(GL_TEXTURE_2D, reinterpret_cast<GLeglImageOES>(image));
    s_eglDestroyImage(dpy, image); // GL holds a reference

    glGenFramebuffers(1, &s->fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex_, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[gpu_surface] GL framebuffer incomplete\n";
        return nullptr;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    std::cout << "[gpu_surface] Created " << width << "×" << height
              << " EGL/GBM surface for surface_id=" << surface_id << "\n";
    return s;
}

XGpuSurface::~XGpuSurface() noexcept {
    if (egl_dpy_ && egl_dpy_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (fbo_) glDeleteFramebuffers(1, &fbo_);
        if (tex_) glDeleteTextures(1, &tex_);
        if (egl_ctx_ && egl_ctx_ != EGL_NO_CONTEXT)
            eglDestroyContext(egl_dpy_, egl_ctx_);
        eglTerminate(egl_dpy_);
    }
    if (bo_)      gbm_bo_destroy(bo_);
    if (gbm_)     gbm_device_destroy(gbm_);
    if (drm_fd_ >= 0) close(drm_fd_);
}

bool XGpuSurface::beginFrame() noexcept {
    if (!eglMakeCurrent(egl_dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_ctx_)) {
        std::cerr << "[gpu_surface] beginFrame: eglMakeCurrent failed\n";
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_));
    return true;
}

bool XGpuSurface::endFrame(int ipc_fd) noexcept {
    // Flush all pending GL commands
    glFlush();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Export GBM BO as a PRIME DMA-buf fd
    int dmabuf_fd = gbm_bo_get_fd(bo_);
    if (dmabuf_fd < 0) {
        std::cerr << "[gpu_surface] gbm_bo_get_fd failed\n";
        return false;
    }

    // Build a FrameReady RenderMessage
    fbx::shared::RenderMessage msg{};
    msg.surface_id_ = surface_id_;
    msg.status_     = fbx::shared::RenderMessageStatus::FrameReady;
    msg.width_      = width_;
    msg.height_     = height_;
    msg.stride_     = gbm_bo_get_stride(bo_);

    // Send message + DMA-buf fd via SCM_RIGHTS
    struct iovec iov { const_cast<fbx::shared::RenderMessage*>(&msg), sizeof(msg) };
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct msghdr mh{};
    mh.msg_iov    = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control    = cmsg_buf;
    mh.msg_controllen = sizeof(cmsg_buf);
    struct cmsghdr* cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type  = SCM_RIGHTS;
    cm->cmsg_len   = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cm), &dmabuf_fd, sizeof(int));

    // MSG_EOR delimits the SEQPACKET record — without it this send coalesces
    // with adjacent messages on the compositor socket (see IpcSocket::sendMessage).
    ssize_t sent = sendmsg(ipc_fd, &mh, MSG_EOR);
    close(dmabuf_fd); // kernel keeps a reference once passed via SCM_RIGHTS

    if (sent < 0) {
        std::cerr << "[gpu_surface] sendmsg failed: " << strerror(errno) << "\n";
        return false;
    }
    return true;
}

} // namespace fbx::xkit
