#include "encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include <cstring>

#include "tethercam/log.h"

namespace tethercam::media {

struct H264Encoder::Impl {
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;

    ~Impl() { Reset(); }
    void Reset() {
        avcodec_free_context(&ctx);
        av_frame_free(&frame);
        av_packet_free(&packet);
    }
};

H264Encoder::H264Encoder() : impl_(std::make_unique<Impl>()) {}
H264Encoder::~H264Encoder() = default;

bool H264Encoder::Init(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate_kbps) {
    Impl& e = *impl_;
    e.Reset();
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) codec = avcodec_find_encoder_by_name("libopenh264");
    if (!codec) {
        log::Error("encoder: FFmpeg has neither libx264 nor libopenh264");
        return false;
    }
    e.ctx = avcodec_alloc_context3(codec);
    e.frame = av_frame_alloc();
    e.packet = av_packet_alloc();
    if (!e.ctx || !e.frame || !e.packet) return false;
    e.ctx->width = int(width);
    e.ctx->height = int(height);
    e.ctx->time_base = {1, int(fps)};
    e.ctx->framerate = {int(fps), 1};
    e.ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    e.ctx->bit_rate = int64_t(bitrate_kbps) * 1000;
    e.ctx->gop_size = int(fps) * 2;
    e.ctx->max_b_frames = 0;
    e.ctx->profile = AV_PROFILE_H264_HIGH;
    if (std::strcmp(codec->name, "libx264") == 0) {
        av_opt_set(e.ctx->priv_data, "preset", "veryfast", 0);
        av_opt_set(e.ctx->priv_data, "tune", "zerolatency", 0);
    }
    if (avcodec_open2(e.ctx, codec, nullptr) < 0) {
        log::Error("encoder: cannot open {}", codec->name);
        return false;
    }
    e.frame->format = AV_PIX_FMT_YUV420P;
    e.frame->width = int(width);
    e.frame->height = int(height);
    return av_frame_get_buffer(e.frame, 0) == 0;
}

bool H264Encoder::Encode(const image::Nv12Frame& f, int64_t pts_100ns, bool force_keyframe,
                         std::vector<std::vector<uint8_t>>& out) {
    Impl& e = *impl_;
    if (!e.ctx || av_frame_make_writable(e.frame) < 0) return false;
    // NV12 -> I420.
    const uint32_t w = f.width, h = f.height;
    const uint8_t* y = f.data.data();
    const uint8_t* uv = y + size_t(w) * h;
    for (uint32_t r = 0; r < h; ++r) std::memcpy(e.frame->data[0] + size_t(r) * e.frame->linesize[0], y + size_t(r) * w, w);
    for (uint32_t r = 0; r < h / 2; ++r) {
        uint8_t* u = e.frame->data[1] + size_t(r) * e.frame->linesize[1];
        uint8_t* v = e.frame->data[2] + size_t(r) * e.frame->linesize[2];
        const uint8_t* s = uv + size_t(r) * w;
        for (uint32_t x = 0; x < w / 2; ++x) {
            u[x] = s[2 * x];
            v[x] = s[2 * x + 1];
        }
    }
    e.frame->pts = pts_100ns / 10;  // only ordering matters; no B-frames
    e.frame->pict_type = force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (force_keyframe) e.frame->flags |= AV_FRAME_FLAG_KEY;
    else e.frame->flags &= ~AV_FRAME_FLAG_KEY;
    if (avcodec_send_frame(e.ctx, e.frame) < 0) return false;
    for (;;) {
        const int r = avcodec_receive_packet(e.ctx, e.packet);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
        if (r < 0) return false;
        out.emplace_back(e.packet->data, e.packet->data + e.packet->size);
        av_packet_unref(e.packet);
    }
    return true;
}

}  // namespace tethercam::media
