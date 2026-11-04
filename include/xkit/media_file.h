#pragma once
// Read media from a descriptor. A sandboxed app can't open a path, so what it has
// is the descriptor it was handed (XFileDialog::pickToOpen) or opened before the
// sandbox; FFmpeg gets at it through a custom reader.

#include <cstdint>
#include <string>
#include <vector>

struct AVFormatContext;

namespace fbx::xkit {

// Open an FFmpeg input on `fd`. The caller keeps `fd` open and owned until
// closeMediaFd. Reads use pread, so several inputs may share one descriptor.
// `name` is only a hint for detecting the format. Null on failure.
[[nodiscard]] AVFormatContext* openMediaFd(int fd, const std::string& name = {}) noexcept;
void closeMediaFd(AVFormatContext*& ctx) noexcept;

// Decode the first frame of an image file to RGBA8. Anything FFmpeg reads: PNG,
// JPEG, GIF, WebP, BMP, TIFF and so on. Larger images are scaled down to fit
// max_dim. On failure, returns false and says why in `error`.
[[nodiscard]] bool decodeImageFd(int fd, const std::string& name, std::vector<uint8_t>& rgba,
                                 uint32_t& width, uint32_t& height, std::string& error,
                                 uint32_t max_dim = 4096) noexcept;

// The whole file; false on a read error. Stops at max_bytes and fails.
[[nodiscard]] bool readAllFd(int fd, std::string& out, size_t max_bytes = 256u << 20) noexcept;

} // namespace fbx::xkit
