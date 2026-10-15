#include "decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "tethercam/log.h"

namespace tethercam::media {
namespace {

// A GPU the decoder can try, best first.
struct HwCandidate {
    AVHWDeviceType type;
    std::string device;  // render node for VA-API; empty = the API's default device
    std::string label;
};

std::string ReadSysfs(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::string s;
    std::getline(f, s);
    return s;
}

// Whether a DRM device is a discrete GPU. There is no generic sysfs flag, so per vendor:
// NVIDIA has no x86 iGPU; amdgpu hides mem_info_vram_vendor on APUs; Intel's iGPU is always
// 0000:00:02.0 (Arc cards sit behind a PCIe port).
bool IsDiscrete(const std::filesystem::path& dev) {
    const std::string vendor = ReadSysfs(dev / "vendor");
    if (vendor == "0x10de") return true;
    if (vendor == "0x1002") return std::filesystem::exists(dev / "mem_info_vram_vendor");
    if (vendor == "0x8086") {
        std::error_code ec;
        return std::filesystem::canonical(dev, ec).filename() != "0000:00:02.0";
    }
    return false;
}

// Discrete GPUs first, then integrated, each in render-node order. TETHERCAM_HW_DEVICE
// (a render node such as /dev/dri/renderD129) is tried before everything else.
std::vector<HwCandidate> HwCandidates() {
    std::vector<HwCandidate> out;
    if (const char* forced = std::getenv("TETHERCAM_HW_DEVICE"); forced && *forced)
        out.push_back({AV_HWDEVICE_TYPE_VAAPI, forced, std::string("VA-API ") + forced + " (TETHERCAM_HW_DEVICE)"});
    struct Node {
        std::string path;
        bool discrete;
        bool nvidia;
    };
    std::vector<Node> nodes;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/sys/class/drm", ec)) {
        const std::string name = e.path().filename().string();
        if (!name.starts_with("renderD")) continue;
        const auto dev = e.path() / "device";
        nodes.push_back({"/dev/dri/" + name, IsDiscrete(dev), ReadSysfs(dev / "vendor") == "0x10de"});
    }
    std::sort(nodes.begin(), nodes.end(), [](const Node& a, const Node& b) {
        if (a.discrete != b.discrete) return a.discrete;
        if (a.path.size() != b.path.size()) return a.path.size() < b.path.size();
        return a.path < b.path;
    });
    bool cuda_added = false;
    for (const Node& n : nodes) {
        const char* kind = n.discrete ? "discrete" : "integrated";
        // NVDEC through CUDA works on the proprietary driver without nvidia-vaapi-driver.
        if (n.nvidia && !cuda_added) {
            out.push_back({AV_HWDEVICE_TYPE_CUDA, "", std::string("NVDEC (") + kind + ")"});
            cuda_added = true;
        }
        out.push_back({AV_HWDEVICE_TYPE_VAAPI, n.path, "VA-API " + n.path + " (" + kind + ")"});
    }
    return out;
}

bool IsHwFormat(int f) { return f == AV_PIX_FMT_VAAPI || f == AV_PIX_FMT_CUDA; }

}  // namespace

struct H264Decoder::Impl {
    AVCodecContext* ctx = nullptr;
    AVBufferRef* hw_device = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* sw_frame = nullptr;
    AVPacket* packet = nullptr;
    bool hardware = false;
    std::string backend = "software";
    std::vector<HwCandidate> candidates;
    size_t next_candidate = 0;  // candidates before this one failed: never retried for this decoder
    std::vector<uint8_t> nv12;  // conversion buffer for planar output

    ~Impl() { Reset(); }
    void Reset() {
        avcodec_free_context(&ctx);
        av_buffer_unref(&hw_device);
        av_frame_free(&frame);
        av_frame_free(&sw_frame);
        av_packet_free(&packet);
        hardware = false;
        backend = "software";
    }

    static AVPixelFormat PickFormat(AVCodecContext* c, const AVPixelFormat* fmts) {
        const bool hw = c->hw_device_ctx != nullptr;
        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
            if (hw && IsHwFormat(*p)) return *p;
        // No hardware surface offered (e.g. unsupported profile): take the first software format.
        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
            const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
            if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
        }
        return AV_PIX_FMT_NONE;
    }

    // Tries the remaining GPUs best-first, then software.
    bool Open(bool want_hw) {
        if (want_hw && candidates.empty() && next_candidate == 0) candidates = HwCandidates();
        while (want_hw && next_candidate < candidates.size()) {
            const HwCandidate& c = candidates[next_candidate];
            if (OpenWith(&c)) return true;
            log::Info("decoder: {} unavailable", c.label);
            ++next_candidate;
        }
        return OpenWith(nullptr);
    }

    bool OpenWith(const HwCandidate* hw) {
        Reset();
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) return false;
        ctx = avcodec_alloc_context3(codec);
        frame = av_frame_alloc();
        sw_frame = av_frame_alloc();
        packet = av_packet_alloc();
        if (!ctx || !frame || !sw_frame || !packet) return false;
        // Low latency: output every frame as soon as it's decoded, no frame-threading delay.
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx->thread_type = FF_THREAD_SLICE;
        ctx->thread_count = 0;
        ctx->get_format = PickFormat;
        if (hw) {
            if (av_hwdevice_ctx_create(&hw_device, hw->type, hw->device.empty() ? nullptr : hw->device.c_str(),
                                       nullptr, 0) < 0)
                return false;
            ctx->hw_device_ctx = av_buffer_ref(hw_device);
            hardware = true;
            backend = hw->label;
        }
        return avcodec_open2(ctx, codec, nullptr) == 0;
    }

    // Hands an NV12 or YUV420P frame to the callback as NV12.
    bool Emit(const AVFrame* f, const FrameCallback& cb) {
        image::Nv12View v;
        v.width = uint32_t(f->width) & ~1u;
        v.height = uint32_t(f->height) & ~1u;
        if (f->format == AV_PIX_FMT_NV12) {
            v.y = f->data[0];
            v.uv = f->data[1];
            v.y_pitch = uint32_t(f->linesize[0]);
            v.uv_pitch = uint32_t(f->linesize[1]);
        } else if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) {
            nv12.resize(size_t(v.width) * v.height * 3 / 2);
            for (uint32_t y = 0; y < v.height; ++y)
                std::memcpy(nv12.data() + size_t(y) * v.width, f->data[0] + size_t(y) * f->linesize[0], v.width);
            uint8_t* uv = nv12.data() + size_t(v.width) * v.height;
            for (uint32_t y = 0; y < v.height / 2; ++y) {
                const uint8_t* u = f->data[1] + size_t(y) * f->linesize[1];
                const uint8_t* w = f->data[2] + size_t(y) * f->linesize[2];
                uint8_t* row = uv + size_t(y) * v.width;
                for (uint32_t x = 0; x < v.width / 2; ++x) {
                    row[2 * x] = u[x];
                    row[2 * x + 1] = w[x];
                }
            }
            v.y = nv12.data();
            v.uv = uv;
            v.y_pitch = v.uv_pitch = v.width;
        } else {
            log::Warn("decoder: unsupported output format {}", av_get_pix_fmt_name(AVPixelFormat(f->format)));
            return false;
        }
        cb(v, f->pts);
        return true;
    }
};

H264Decoder::H264Decoder() : impl_(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() = default;

bool H264Decoder::Init(bool prefer_hardware) {
    const bool ok = impl_->Open(prefer_hardware);
    if (ok) log::Info("decoder: {}", impl_->backend);
    else log::Error("decoder: cannot open the FFmpeg H.264 decoder");
    return ok;
}

bool H264Decoder::IsHardware() const { return impl_->hardware; }

bool H264Decoder::Decode(std::span<const uint8_t> au, int64_t pts_us, const FrameCallback& cb) {
    Impl& d = *impl_;
    if (!d.ctx) return false;
    if (av_new_packet(d.packet, int(au.size())) < 0) return false;
    std::memcpy(d.packet->data, au.data(), au.size());
    d.packet->pts = pts_us;
    const int sent = avcodec_send_packet(d.ctx, d.packet);
    av_packet_unref(d.packet);
    if (sent < 0 && sent != AVERROR(EAGAIN)) return false;
    for (;;) {
        const int r = avcodec_receive_frame(d.ctx, d.frame);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
        if (r < 0) return false;
        const AVFrame* out = d.frame;
        if (d.hardware && !IsHwFormat(d.frame->format)) {
            // FFmpeg dropped the hwaccel (e.g. the GPU has no H.264 decode): try the next GPU.
            log::Warn("decoder: {} cannot decode this stream; trying the next device", d.backend);
            ++d.next_candidate;
            av_frame_unref(d.frame);
            Init(true);
            return false;
        }
        if (IsHwFormat(d.frame->format)) {
            av_frame_unref(d.sw_frame);
            if (av_hwframe_transfer_data(d.sw_frame, d.frame, 0) < 0) {
                log::Warn("decoder: {} download failed; trying the next device", d.backend);
                ++d.next_candidate;
                av_frame_unref(d.frame);
                Init(true);
                return false;
            }
            d.sw_frame->pts = d.frame->pts;
            out = d.sw_frame;
        }
        const bool ok = d.Emit(out, cb);
        av_frame_unref(d.frame);
        if (!ok) return false;
    }
    return true;
}

void H264Decoder::Flush() {
    if (impl_->ctx) avcodec_flush_buffers(impl_->ctx);
}

}  // namespace tethercam::media
