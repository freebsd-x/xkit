#include "xkit/media_file.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cerrno>
#include <memory>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

namespace fbx::xkit {
namespace {

struct Reader {
    int     fd;
    int64_t pos = 0;
};

int readPacket(void* opaque, uint8_t* buf, int size) {
    auto* r = static_cast<Reader*>(opaque);
    ssize_t n;
    do {
        n = ::pread(r->fd, buf, static_cast<size_t>(size), r->pos);
    } while (n < 0 && errno == EINTR);
    if (n == 0) return AVERROR_EOF;
    if (n < 0) return AVERROR(errno);
    r->pos += n;
    return static_cast<int>(n);
}

int64_t seekTo(void* opaque, int64_t offset, int whence) {
    auto* r = static_cast<Reader*>(opaque);
    struct stat st{};
    if (whence & AVSEEK_SIZE)
        return ::fstat(r->fd, &st) == 0 ? static_cast<int64_t>(st.st_size) : AVERROR(errno);
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: r->pos = offset; break;
    case SEEK_CUR: r->pos += offset; break;
    case SEEK_END:
        if (::fstat(r->fd, &st) != 0) return AVERROR(errno);
        r->pos = st.st_size + offset;
        break;
    default: return AVERROR(EINVAL);
    }
    return r->pos;
}

} // namespace

AVFormatContext* openMediaFd(int fd, const std::string& name) noexcept {
    constexpr int kBufferSize = 64 * 1024;
    auto* buffer = static_cast<uint8_t*>(av_malloc(kBufferSize));
    if (!buffer) return nullptr;
    auto* reader = new (std::nothrow) Reader{fd};
    AVIOContext* io = reader ? avio_alloc_context(buffer, kBufferSize, 0, reader, readPacket, nullptr, seekTo)
                             : nullptr;
    AVFormatContext* ctx = io ? avformat_alloc_context() : nullptr;
    if (!ctx) {
        if (io) { av_freep(&io->buffer); avio_context_free(&io); } else { av_free(buffer); }
        delete reader;
        return nullptr;
    }
    ctx->pb = io;
    ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
    // On failure this frees ctx, but a custom reader stays ours to free.
    if (avformat_open_input(&ctx, name.c_str(), nullptr, nullptr) < 0) {
        av_freep(&io->buffer);
        avio_context_free(&io);
        delete reader;
        return nullptr;
    }
    return ctx;
}

void closeMediaFd(AVFormatContext*& ctx) noexcept {
    if (!ctx) return;
    AVIOContext* io = ctx->pb;
    avformat_close_input(&ctx);
    if (io) {
        delete static_cast<Reader*>(io->opaque);
        av_freep(&io->buffer);
        avio_context_free(&io);
    }
}

bool decodeImageFd(int fd, const std::string& name, std::vector<uint8_t>& rgba, uint32_t& width,
                   uint32_t& height, std::string& error, uint32_t max_dim) noexcept {
    AVFormatContext* fmt = openMediaFd(fd, name);
    if (!fmt) { error = "not a readable image"; return false; }
    struct Done {
        AVFormatContext*& fmt;
        AVCodecContext* codec = nullptr;
        AVFrame* frame = av_frame_alloc();
        AVPacket* packet = av_packet_alloc();
        ~Done() {
            av_packet_free(&packet);
            av_frame_free(&frame);
            avcodec_free_context(&codec);
            closeMediaFd(fmt);
        }
    } done{fmt};

    const AVCodec* decoder = nullptr;
    if (avformat_find_stream_info(fmt, nullptr) < 0 || !done.frame || !done.packet) {
        error = "not a readable image";
        return false;
    }
    const int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (stream < 0 || !decoder) { error = "no decoder for this image format"; return false; }
    done.codec = avcodec_alloc_context3(decoder);
    if (!done.codec || avcodec_parameters_to_context(done.codec, fmt->streams[stream]->codecpar) < 0 ||
        avcodec_open2(done.codec, decoder, nullptr) < 0) {
        error = "no decoder for this image format";
        return false;
    }

    bool have = false, flushed = false;
    while (!have) {
        const int rc = avcodec_receive_frame(done.codec, done.frame);
        if (rc == 0) { have = true; break; }
        if (rc != AVERROR(EAGAIN) || flushed) break;
        if (av_read_frame(fmt, done.packet) < 0) {
            avcodec_send_packet(done.codec, nullptr);
            flushed = true;
            continue;
        }
        if (done.packet->stream_index == stream) avcodec_send_packet(done.codec, done.packet);
        av_packet_unref(done.packet);
    }
    if (!have || done.frame->width <= 0 || done.frame->height <= 0) {
        error = "the image data is damaged";
        return false;
    }

    const auto src_w = static_cast<uint32_t>(done.frame->width);
    const auto src_h = static_cast<uint32_t>(done.frame->height);
    const double scale = std::min(1.0, static_cast<double>(max_dim) / std::max(src_w, src_h));
    width  = std::max(1u, static_cast<uint32_t>(src_w * scale));
    height = std::max(1u, static_cast<uint32_t>(src_h * scale));

    SwsContext* sws = sws_getContext(static_cast<int>(src_w), static_cast<int>(src_h),
                                     static_cast<AVPixelFormat>(done.frame->format),
                                     static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_RGBA,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) { error = "unsupported pixel format"; return false; }
    rgba.assign(static_cast<size_t>(width) * height * 4, 0);
    uint8_t* dst[4] = {rgba.data(), nullptr, nullptr, nullptr};
    int dst_stride[4] = {static_cast<int>(width * 4), 0, 0, 0};
    const int rows = sws_scale(sws, done.frame->data, done.frame->linesize, 0, static_cast<int>(src_h), dst, dst_stride);
    sws_freeContext(sws);
    if (rows <= 0) { error = "unsupported pixel format"; return false; }
    return true;
}

bool readAllFd(int fd, std::string& out, size_t max_bytes) noexcept {
    out.clear();
    char buf[64 * 1024];
    for (off_t pos = 0;;) {
        const ssize_t n = ::pread(fd, buf, sizeof(buf), pos);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
        if (n == 0) return true;
        if (out.size() + static_cast<size_t>(n) > max_bytes) return false;
        out.append(buf, static_cast<size_t>(n));
        pos += n;
    }
}

} // namespace fbx::xkit
