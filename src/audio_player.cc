#include "xkit/audio_player.h"
#include <iostream>
#include <cmath>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

namespace fbx::xkit {

AudioPlayer::AudioPlayer(XBusClient& xbus) noexcept : xbus_(xbus) {}

AudioPlayer::~AudioPlayer() noexcept {
    if (stream_id_ != 0) {
        shared::AudioStreamRequest req{};
        std::memcpy(&req, &stream_id_, sizeof(stream_id_)); // Payload is just the ID
        std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&stream_id_), sizeof(stream_id_));
        xbus_.Call("com.freebsdx.Audio", 0x02, payload, nullptr);
    }
    if (mapped_buffer_) {
        munmap(mapped_buffer_, buffer_size_);
    }
    if (shm_fd_ >= 0) {
        ::close(shm_fd_);
    }
}

std::expected<void, std::error_code> AudioPlayer::playTone(float frequency, float duration_sec) noexcept {
    const uint32_t sample_rate = 44100;
    const uint32_t channels = 2;
    const uint32_t num_samples = static_cast<uint32_t>(sample_rate * duration_sec);
    
    buffer_size_ = num_samples * channels * sizeof(int16_t);

    // Create anonymous shared memory via shm_open (POSIX)
    // FreeBSD specific: shm_open(SHM_ANON, ...)
    shm_fd_ = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
    if (shm_fd_ < 0) {
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }

    if (ftruncate(shm_fd_, buffer_size_) < 0) {
        ::close(shm_fd_);
        shm_fd_ = -1;
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }

    mapped_buffer_ = mmap(nullptr, buffer_size_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
    if (mapped_buffer_ == MAP_FAILED) {
        ::close(shm_fd_);
        shm_fd_ = -1;
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }

    // Generate Sine Wave
    int16_t* samples = static_cast<int16_t*>(mapped_buffer_);
    for (uint32_t i = 0; i < num_samples; ++i) {
        float time = static_cast<float>(i) / sample_rate;
        float value = std::sin(2.0f * M_PI * frequency * time);
        int16_t int_val = static_cast<int16_t>(value * 10000.0f); // Amplitude
        samples[i * 2] = int_val;     // Left
        samples[i * 2 + 1] = int_val; // Right
    }

    // Request Audio Broker to play
    shared::AudioStreamRequest req{
        .sample_rate_ = sample_rate,
        .channels_ = channels,
        .bit_depth_ = 16,
        .buffer_size_ = buffer_size_
    };

    std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&req), sizeof(req));
    xbus_.Call("com.freebsdx.Audio", 0x01, payload, nullptr, xserver::UniqueFd{shm_fd_});
    shm_fd_ = -1; // ownership transferred to UniqueFd

    return {};
}

std::expected<void, std::error_code> AudioPlayer::playChime() noexcept {
    // Release any previous buffer
    if (mapped_buffer_) {
        munmap(mapped_buffer_, buffer_size_);
        mapped_buffer_ = nullptr;
    }
    if (shm_fd_ >= 0) {
        ::close(shm_fd_);
        shm_fd_ = -1;
    }

    const uint32_t sample_rate = 44100;
    const uint32_t channels    = 2;

    // Two-note chime: A5 (880 Hz, 90 ms) → 20 ms silence → D6 (1174.66 Hz, 140 ms)
    const uint32_t n0     = static_cast<uint32_t>(sample_rate * 0.090f); // A5
    const uint32_t n_gap  = static_cast<uint32_t>(sample_rate * 0.020f); // silence
    const uint32_t n1     = static_cast<uint32_t>(sample_rate * 0.140f); // D6
    const uint32_t total  = n0 + n_gap + n1;
    const uint32_t fade   = static_cast<uint32_t>(sample_rate * 0.012f); // 12 ms fade

    buffer_size_ = total * channels * sizeof(int16_t);

    shm_fd_ = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
    if (shm_fd_ < 0)
        return std::unexpected(std::make_error_code(std::errc::io_error));

    if (ftruncate(shm_fd_, buffer_size_) < 0) {
        ::close(shm_fd_); shm_fd_ = -1;
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }

    mapped_buffer_ = mmap(nullptr, buffer_size_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
    if (mapped_buffer_ == MAP_FAILED) {
        ::close(shm_fd_); shm_fd_ = -1;
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }

    int16_t* buf = static_cast<int16_t*>(mapped_buffer_);

    auto write_note = [&](uint32_t offset, uint32_t count, float freq) {
        for (uint32_t i = 0; i < count; ++i) {
            float t    = static_cast<float>(i) / static_cast<float>(sample_rate);
            float env  = 1.0f;
            if (i < fade)
                env = static_cast<float>(i) / static_cast<float>(fade);
            else if (i >= count - fade)
                env = static_cast<float>(count - i) / static_cast<float>(fade);
            float val  = env * std::sin(2.0f * static_cast<float>(M_PI) * freq * t);
            int16_t s  = static_cast<int16_t>(val * 12000.0f);
            buf[(offset + i) * 2]     = s;
            buf[(offset + i) * 2 + 1] = s;
        }
    };

    // A5 note
    write_note(0, n0, 880.0f);
    // silence gap
    std::memset(buf + n0 * channels, 0, n_gap * channels * sizeof(int16_t));
    // D6 note
    write_note(n0 + n_gap, n1, 1174.66f);

    shared::AudioStreamRequest req{
        .sample_rate_ = sample_rate,
        .channels_    = channels,
        .bit_depth_   = 16,
        .buffer_size_ = buffer_size_
    };

    std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&req), sizeof(req));
    xbus_.Call("com.freebsdx.Audio", 0x01, payload, nullptr, xserver::UniqueFd{shm_fd_});
    shm_fd_ = -1; // ownership transferred

    return {};
}

} // namespace fbx::xkit
