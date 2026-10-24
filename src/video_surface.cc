#include "xkit/video_surface.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include "shared/protocol.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>

// FFmpeg C headers
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
}

namespace fbx::xkit {

// ── RenderVideoFrame ──────────────────────────────────────────────────────────

class RenderVideoFrame : public RenderNode {
public:
    RenderVideoFrame(std::shared_ptr<VideoFrameBuffer> buf,
                     float view_w, float view_h) noexcept
        : buf_(std::move(buf)), view_w_(view_w), view_h_(view_h) {}

    [[nodiscard]] Size performLayout(const Constraints& c) noexcept override {
        return c.clamp({view_w_, view_h_});
    }

    void paintSelf(fbx::xrender::DisplayList& dl, Offset off) noexcept override {
        std::lock_guard<std::mutex> lock(buf_->mtx);
        if (buf_->pixels.empty()) return;

        shared::ImageData img{};
        img.x_           = off.x_;
        img.y_           = off.y_;
        img.width_       = size_.width_;
        img.height_      = size_.height_;
        img.texture_id_  = 0;
        img.pixel_ptr_   = buf_->pixels.data();
        img.pixel_width_ = buf_->width;
        img.pixel_height_= buf_->height;
        dl.addCommand(shared::CommandType::DrawImage,
                      {reinterpret_cast<const uint8_t*>(&img), sizeof(img)});
        buf_->dirty.store(false);
    }

private:
    std::shared_ptr<VideoFrameBuffer> buf_;
    float view_w_, view_h_;
};

// ── VideoElement ──────────────────────────────────────────────────────────────

class VideoElement : public Element {
public:
    explicit VideoElement(std::shared_ptr<XVideoSurface> widget,
                          std::shared_ptr<VideoFrameBuffer> buf) {
        widget_ = widget;
        auto w = std::static_pointer_cast<XVideoSurface>(widget_);
        renderNode_ = std::make_shared<RenderVideoFrame>(
            buf, static_cast<float>(w->view_w_), static_cast<float>(w->view_h_));
    }

    void mount(std::shared_ptr<Element> p) noexcept override {
        parent_ = p;
        renderNode_->setOwnerElement(shared_from_this());
    }
    void update(std::shared_ptr<Widget>) noexcept override {}
    void unmount() noexcept override { parent_.reset(); }
};

// ── Opaque FFmpeg state ────────────────────────────────────────────────────────

struct XVideoSurface::FfCtx {
    AVFormatContext* fmt_ctx{nullptr};
    AVCodecContext*  vid_ctx{nullptr};
    AVCodecContext*  aud_ctx{nullptr};
    SwsContext*      sws{nullptr};
    SwrContext*      swr{nullptr};
    AVFrame*         frame{nullptr};
    AVFrame*         rgb_frame{nullptr};
    AVFrame*         aud_frame{nullptr};
    AVPacket*        pkt{nullptr};
    int              vid_stream{-1};
    int              aud_stream{-1};
    double           vid_tb{0.0};

    // Audio SHM ring buffer (write side — fed into AudioService)
    void*    shm_ptr{nullptr};
    size_t   shm_size{0};
    int      shm_fd{-1};
    uint32_t shm_write_off{0};
    uint32_t audio_stream_id{0};

    ~FfCtx() noexcept {
        if (rgb_frame && rgb_frame->data[0]) av_freep(&rgb_frame->data[0]);
        if (frame)     av_frame_free(&frame);
        if (rgb_frame) av_frame_free(&rgb_frame);
        if (aud_frame) av_frame_free(&aud_frame);
        if (pkt)       av_packet_free(&pkt);
        if (sws)       sws_freeContext(sws);
        if (swr)       swr_free(&swr);
        if (vid_ctx)   avcodec_free_context(&vid_ctx);
        if (aud_ctx)   avcodec_free_context(&aud_ctx);
        if (fmt_ctx)   avformat_close_input(&fmt_ctx);
        if (shm_ptr && shm_ptr != MAP_FAILED)
            ::munmap(shm_ptr, shm_size);
        if (shm_fd >= 0) ::close(shm_fd);
    }
};

// ── Constructor / destructor ───────────────────────────────────────────────────

XVideoSurface::XVideoSurface(uint32_t view_w, uint32_t view_h,
                               XBusClient* xbus) noexcept
    : view_w_(view_w), view_h_(view_h), xbus_(xbus) {
    setFixedSize(static_cast<float>(view_w), static_cast<float>(view_h));
    frame_buf_ = std::make_shared<VideoFrameBuffer>();
    frame_buf_->width  = view_w;
    frame_buf_->height = view_h;
    frame_buf_->pixels.resize(static_cast<size_t>(view_w) * view_h * 4, 0);
}

XVideoSurface::~XVideoSurface() noexcept {
    close();
}

// ── Open / close ──────────────────────────────────────────────────────────────

bool XVideoSurface::open(const std::string& path) noexcept {
    close();

    auto ff = std::make_unique<FfCtx>();
    if (avformat_open_input(&ff->fmt_ctx, path.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "[XVideoSurface] Cannot open: " << path << "\n";
        return false;
    }
    if (avformat_find_stream_info(ff->fmt_ctx, nullptr) < 0) {
        std::cerr << "[XVideoSurface] Cannot find stream info\n";
        return false;
    }

    // Best video stream
    const AVCodec* vid_codec = nullptr;
    ff->vid_stream = av_find_best_stream(ff->fmt_ctx, AVMEDIA_TYPE_VIDEO,
                                          -1, -1, &vid_codec, 0);
    if (ff->vid_stream < 0) {
        std::cerr << "[XVideoSurface] No video stream\n";
        return false;
    }
    ff->vid_ctx = avcodec_alloc_context3(vid_codec);
    avcodec_parameters_to_context(ff->vid_ctx,
        ff->fmt_ctx->streams[ff->vid_stream]->codecpar);
    ff->vid_ctx->thread_count = 0;
    if (avcodec_open2(ff->vid_ctx, vid_codec, nullptr) < 0) {
        std::cerr << "[XVideoSurface] Cannot open video codec\n";
        return false;
    }
    ff->vid_tb = av_q2d(ff->fmt_ctx->streams[ff->vid_stream]->time_base);

    if (ff->fmt_ctx->duration != AV_NOPTS_VALUE)
        duration_ = static_cast<double>(ff->fmt_ctx->duration) / AV_TIME_BASE;

    // Best audio stream (optional)
    const AVCodec* aud_codec = nullptr;
    ff->aud_stream = av_find_best_stream(ff->fmt_ctx, AVMEDIA_TYPE_AUDIO,
                                          -1, -1, &aud_codec, 0);
    if (ff->aud_stream >= 0 && aud_codec) {
        ff->aud_ctx = avcodec_alloc_context3(aud_codec);
        avcodec_parameters_to_context(ff->aud_ctx,
            ff->fmt_ctx->streams[ff->aud_stream]->codecpar);
        if (avcodec_open2(ff->aud_ctx, aud_codec, nullptr) < 0) {
            avcodec_free_context(&ff->aud_ctx);
            ff->aud_stream = -1;
        } else {
            ff->swr = swr_alloc();
            av_opt_set_chlayout(ff->swr, "in_chlayout",
                                &ff->aud_ctx->ch_layout, 0);
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            av_opt_set_chlayout(ff->swr, "out_chlayout", &stereo, 0);
            av_opt_set_int(ff->swr, "in_sample_rate",
                           ff->aud_ctx->sample_rate, 0);
            av_opt_set_int(ff->swr, "out_sample_rate", 44100, 0);
            av_opt_set_sample_fmt(ff->swr, "in_sample_fmt",
                                  ff->aud_ctx->sample_fmt, 0);
            av_opt_set_sample_fmt(ff->swr, "out_sample_fmt",
                                  AV_SAMPLE_FMT_S16, 0);
            swr_init(ff->swr);

            // Create SHM buffer for audio (4 MiB ring)
            const size_t kAudioShmSize = 4 * 1024 * 1024;
            ff->shm_fd = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
            if (ff->shm_fd >= 0) {
                ftruncate(ff->shm_fd, static_cast<off_t>(kAudioShmSize));
                ff->shm_ptr = ::mmap(nullptr, kAudioShmSize,
                                     PROT_READ | PROT_WRITE, MAP_SHARED,
                                     ff->shm_fd, 0);
                if (ff->shm_ptr == MAP_FAILED) {
                    ::close(ff->shm_fd);
                    ff->shm_fd = -1;
                    ff->shm_ptr = nullptr;
                } else {
                    ff->shm_size = kAudioShmSize;
                    // Register stream with AudioService via XBus
                    if (xbus_) {
                        shared::AudioStreamRequest req{};
                        req.sample_rate_  = 44100;
                        req.channels_     = 2;
                        req.bit_depth_    = 16;
                        req.buffer_size_  = static_cast<uint32_t>(kAudioShmSize);
                        xserver::UniqueFd dup_fd(::dup(ff->shm_fd));
                        xbus_->Call("com.freebsdx.Audio",
                            static_cast<uint32_t>(shared::AudioOpcode::OpenStream),
                            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                            [ff_raw = ff.get()](const shared::XBusMessage& resp) {
                                uint32_t sid = 0;
                                std::memcpy(&sid, resp.payload_, sizeof(sid));
                                ff_raw->audio_stream_id = sid;
                            },
                            std::move(dup_fd));
                    }
                }
            }
        }
    }

    // Allocate frames + packet
    ff->frame     = av_frame_alloc();
    ff->rgb_frame = av_frame_alloc();
    ff->aud_frame = av_frame_alloc();
    ff->pkt       = av_packet_alloc();

    // Allocate RGB frame buffer
    const int dst_w = static_cast<int>(view_w_);
    const int dst_h = static_cast<int>(view_h_);
    av_image_alloc(ff->rgb_frame->data, ff->rgb_frame->linesize,
                   dst_w, dst_h, AV_PIX_FMT_RGBA, 1);
    ff->rgb_frame->width  = dst_w;
    ff->rgb_frame->height = dst_h;
    ff->rgb_frame->format = AV_PIX_FMT_RGBA;

    ff->sws = sws_getContext(
        ff->vid_ctx->width, ff->vid_ctx->height, ff->vid_ctx->pix_fmt,
        dst_w, dst_h, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    file_path_ = path;
    ff_ = std::move(ff);
    open_.store(true);
    position_.store(0.0);

    if (on_opened_) on_opened_(duration_);
    return true;
}

void XVideoSurface::stopDecode() noexcept {
    running_.store(false);
    playing_.store(false);
    if (decode_thread_.joinable()) decode_thread_.join();
}

void XVideoSurface::close() noexcept {
    stopDecode();
    // Close audio stream
    if (ff_ && ff_->audio_stream_id != 0 && xbus_) {
        uint32_t sid = ff_->audio_stream_id;
        xbus_->Call("com.freebsdx.Audio",
            static_cast<uint32_t>(shared::AudioOpcode::CloseStream),
            {reinterpret_cast<const uint8_t*>(&sid), sizeof(sid)},
            nullptr);
    }
    open_.store(false);
    ff_.reset();
    file_path_.clear();
    duration_ = 0.0;
    position_.store(0.0);
    std::lock_guard<std::mutex> lock(frame_buf_->mtx);
    std::fill(frame_buf_->pixels.begin(), frame_buf_->pixels.end(), 0);
    frame_buf_->dirty.store(true);
}

// ── Playback control ──────────────────────────────────────────────────────────

void XVideoSurface::play() noexcept {
    if (!open_.load()) return;
    playing_.store(true);
    if (!running_.load()) {
        running_.store(true);
        decode_thread_ = std::thread(&XVideoSurface::decodeLoop, this);
    }
}

void XVideoSurface::pause() noexcept { playing_.store(false); }

void XVideoSurface::togglePlayPause() noexcept {
    if (playing_.load()) pause(); else play();
}

void XVideoSurface::seek(double seconds) noexcept {
    seek_target_.store(seconds);
    seek_requested_.store(true);
    if (!running_.load() && open_.load()) {
        running_.store(true);
        decode_thread_ = std::thread(&XVideoSurface::decodeLoop, this);
    }
}

void XVideoSurface::setVolume(float v) noexcept {
    volume_.store(std::clamp(v, 0.0f, 1.0f));
}

void XVideoSurface::setOnPositionChanged(std::function<void(double, double)> cb) noexcept {
    on_position_changed_ = std::move(cb);
}
void XVideoSurface::setOnEndOfStream(std::function<void()> cb) noexcept {
    on_end_of_stream_ = std::move(cb);
}
void XVideoSurface::setOnOpened(std::function<void(double)> cb) noexcept {
    on_opened_ = std::move(cb);
}

// ── Decode loop ───────────────────────────────────────────────────────────────

void XVideoSurface::decodeLoop() noexcept {
    using namespace std::chrono;
    if (!ff_) { running_.store(false); return; }

    FfCtx& f = *ff_;
    auto wall_start   = steady_clock::now();
    double pts_start  = position_.load();
    uint32_t report_n = 0;

    while (running_.load()) {
        // Seek
        if (seek_requested_.load()) {
            seek_requested_.store(false);
            double target = seek_target_.load();
            int64_t ts = static_cast<int64_t>(target / f.vid_tb);
            av_seek_frame(f.fmt_ctx, f.vid_stream, ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(f.vid_ctx);
            if (f.aud_ctx) avcodec_flush_buffers(f.aud_ctx);
            pts_start = target;
            position_.store(target);
            wall_start = steady_clock::now();
        }

        // Pause idle
        if (!playing_.load()) {
            pts_start  = position_.load();
            wall_start = steady_clock::now();
            std::this_thread::sleep_for(milliseconds(16));
            continue;
        }

        // Read packet
        if (av_read_frame(f.fmt_ctx, f.pkt) < 0) {
            running_.store(false);
            playing_.store(false);
            if (on_end_of_stream_) on_end_of_stream_();
            break;
        }

        if (f.pkt->stream_index == f.vid_stream) {
            if (avcodec_send_packet(f.vid_ctx, f.pkt) == 0) {
                while (avcodec_receive_frame(f.vid_ctx, f.frame) == 0) {
                    double pts = (f.frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                 ? f.frame->best_effort_timestamp * f.vid_tb
                                 : position_.load();
                    position_.store(pts);

                    // A/V sync
                    double wall_el = std::chrono::duration_cast<std::chrono::duration<double>>(
                        std::chrono::steady_clock::now() - wall_start).count();
                    double vid_el  = pts - pts_start;
                    double diff    = vid_el - wall_el;
                    if (diff > 0.002)
                        std::this_thread::sleep_for(
                            std::chrono::duration<double>(diff - 0.001));

                    // Scale → RGBA
                    sws_scale(f.sws,
                              f.frame->data, f.frame->linesize, 0,
                              f.vid_ctx->height,
                              f.rgb_frame->data, f.rgb_frame->linesize);

                    // Copy into shared frame buffer
                    const size_t row_bytes = view_w_ * 4;
                    {
                        std::lock_guard<std::mutex> lock(frame_buf_->mtx);
                        for (uint32_t y = 0; y < view_h_; ++y) {
                            std::memcpy(frame_buf_->pixels.data() + y * row_bytes,
                                        f.rgb_frame->data[0] +
                                            y * f.rgb_frame->linesize[0],
                                        row_bytes);
                        }
                        frame_buf_->dirty.store(true);
                    }

                    if (++report_n % 15 == 0 && on_position_changed_)
                        on_position_changed_(pts, duration_);
                }
            }
        } else if (f.aud_ctx && f.pkt->stream_index == f.aud_stream && f.swr) {
            if (avcodec_send_packet(f.aud_ctx, f.pkt) == 0) {
                while (avcodec_receive_frame(f.aud_ctx, f.aud_frame) == 0) {
                    constexpr int kMaxOut = 8192;
                    int16_t pcm[kMaxOut * 2];
                    uint8_t* out_ptrs[] = { reinterpret_cast<uint8_t*>(pcm) };
                    int got = swr_convert(f.swr, out_ptrs, kMaxOut,
                                          const_cast<const uint8_t**>(f.aud_frame->data),
                                          f.aud_frame->nb_samples);
                    if (got > 0 && f.shm_ptr) {
                        float vol = volume_.load();
                        auto* dst = static_cast<int16_t*>(f.shm_ptr);
                        const size_t total_s = f.shm_size / sizeof(int16_t);
                        for (int i = 0; i < got * 2; ++i) {
                            dst[(f.shm_write_off + static_cast<uint32_t>(i)) % total_s] =
                                static_cast<int16_t>(pcm[i] * vol);
                        }
                        f.shm_write_off = static_cast<uint32_t>(
                            (f.shm_write_off + static_cast<uint32_t>(got) * 2) % total_s);
                    }
                }
            }
        }

        av_packet_unref(f.pkt);
    }
    running_.store(false);
}

// ── createElement ──────────────────────────────────────────────────────────────

std::shared_ptr<Element> XVideoSurface::createElement() noexcept {
    return std::make_shared<VideoElement>(
        std::static_pointer_cast<XVideoSurface>(shared_from_this()),
        frame_buf_);
}

} // namespace fbx::xkit
