#pragma once

#include "xkit/xbus_client.h"
#include <memory>
#include <expected>
#include <system_error>

namespace fbx::xkit {

class AudioPlayer {
public:
    explicit AudioPlayer(XBusClient& xbus) noexcept;
    ~AudioPlayer() noexcept;

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    std::expected<void, std::error_code> playTone(float frequency, float duration_sec) noexcept;

    // Play a two-note notification chime (A5 → D6, ~230 ms, with fade envelope).
    std::expected<void, std::error_code> playChime() noexcept;

private:
    XBusClient& xbus_;
    int shm_fd_{-1};
    void* mapped_buffer_{nullptr};
    uint32_t stream_id_{0};
    uint32_t buffer_size_{0};
};

} // namespace fbx::xkit
