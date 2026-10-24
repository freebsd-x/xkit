#pragma once

// XVideoSurface — Software-decoded video widget for FreeBSD-X
//
// Decodes video files with FFmpeg (libavcodec/libavformat) on a background
// thread, scales each frame to RGBA via libswscale, and presents it to the
// compositor through the standard DrawImage path (pixel_ptr_ → server-side
// texture upload).  Audio is forwarded to AudioService via SHM ring + XBus.
//
// Usage:
//   auto video = std::make_shared<XVideoSurface>(800, 450, &xbus);
//   video->open("/path/to/movie.mp4");
//   video->play();
//   // Use as a Widget inside any FlexColumn / FlexRow layout.

#include "xkit/widget.h"
#include "xkit/xbus_client.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <memory>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

namespace fbx::xkit {

// Shared pixel buffer between decode thread and render node.
struct VideoFrameBuffer {
    std::mutex              mtx;
    std::vector<uint8_t>    pixels;
    uint32_t                width{0};
    uint32_t                height{0};
    std::atomic<bool>       dirty{false};
};

class XVideoSurface : public Widget {
public:
    XVideoSurface(uint32_t view_w, uint32_t view_h,
                  XBusClient* xbus = nullptr) noexcept;
    ~XVideoSurface() noexcept;

    XVideoSurface(const XVideoSurface&) = delete;
    XVideoSurface& operator=(const XVideoSurface&) = delete;

    // Open a media file. Returns false on failure.
    bool open(const std::string& path) noexcept;
    void close() noexcept;

    // Playback control
    void play()            noexcept;
    void pause()           noexcept;
    void togglePlayPause() noexcept;
    void seek(double seconds) noexcept;

    // State queries
    [[nodiscard]] bool        isPlaying()  const noexcept { return playing_.load();  }
    [[nodiscard]] bool        isOpen()     const noexcept { return open_.load();     }
    [[nodiscard]] double      duration()   const noexcept { return duration_;        }
    [[nodiscard]] double      position()   const noexcept { return position_.load(); }
    [[nodiscard]] const std::string& filePath() const noexcept { return file_path_; }

    // Volume 0.0–1.0
    void  setVolume(float v) noexcept;
    [[nodiscard]] float volume() const noexcept { return volume_.load(); }

    // Callbacks (invoked on decode thread)
    void setOnPositionChanged(std::function<void(double pos, double dur)> cb) noexcept;
    void setOnEndOfStream(std::function<void()> cb) noexcept;
    void setOnOpened(std::function<void(double duration)> cb) noexcept;

    // Widget interface
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;

    // Exposed to VideoElement (same TU via friend)
    uint32_t view_w_, view_h_;

private:
    void decodeLoop() noexcept;
    void stopDecode() noexcept;

    XBusClient* xbus_;

    // Shared frame buffer — owned by Widget, referenced by RenderVideoFrame
    std::shared_ptr<VideoFrameBuffer> frame_buf_;

    // Playback state
    std::atomic<bool>   open_{false};
    std::atomic<bool>   running_{false};
    std::atomic<bool>   playing_{false};
    std::atomic<bool>   seek_requested_{false};
    std::atomic<double> seek_target_{0.0};
    std::atomic<double> position_{0.0};
    double              duration_{0.0};
    std::atomic<float>  volume_{1.0f};
    std::string         file_path_;

    std::thread decode_thread_;

    // Callbacks
    std::function<void(double, double)> on_position_changed_;
    std::function<void()>               on_end_of_stream_;
    std::function<void(double)>         on_opened_;

    // Opaque FFmpeg state
    struct FfCtx;
    std::unique_ptr<FfCtx> ff_;
};

} // namespace fbx::xkit
