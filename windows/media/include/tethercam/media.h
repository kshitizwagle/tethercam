#pragma once

// Media Foundation wrappers: H.264 decode (agent), H.264 encode (fakephone), placeholder text.

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "tethercam/image.h"

struct ID3D11Device;
struct ID3D11Texture2D;

namespace tethercam::media {

// RAII MFStartup/MFShutdown + COM init for the calling thread.
class MfScope {
public:
    MfScope();
    ~MfScope();
    bool Ok() const { return ok_; }

private:
    bool ok_ = false;
    bool com_ = false;
};

// One decoded frame, valid only during the callback. Hardware frames carry a D3D11 surface; the
// system-memory view downloads the frame on first use, so a consumer that can scale on the GPU
// never pays for that copy.
struct DecodedFrame {
    // A system-memory NV12 view (downloads a hardware frame once, then cached). Empty when
    // unavailable. Valid only during the callback.
    image::Nv12View View() const { return view_fn ? view_fn() : image::Nv12View{}; }

    // Hardware decode surface, when available; the valid picture is display_* inside the texture.
    ID3D11Texture2D* texture = nullptr;
    UINT subresource = 0;
    uint32_t coded_width = 0, coded_height = 0;
    uint32_t display_width = 0, display_height = 0;

    std::function<image::Nv12View()> view_fn;  // decoder implementation detail
};

class H264Decoder {
public:
    // Called for every decoded frame. Valid only during the callback.
    using FrameCallback = std::function<void(const DecodedFrame& frame, int64_t pts_100ns)>;

    H264Decoder();
    ~H264Decoder();
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // Creates the decoder. With prefer_hardware, tries DXVA through a D3D11 device first.
    bool Init(uint32_t width, uint32_t height, uint32_t fps, bool prefer_hardware);
    bool IsHardware() const;
    // The device backing hardware decoding (nullptr with software decoding), e.g. for Nv12Scaler.
    ID3D11Device* Device() const;
    // Feeds one Annex-B access unit. Calls `cb` for every frame that comes out.
    // Returns false on a decoder error (caller should request a keyframe).
    bool Decode(std::span<const uint8_t> au, int64_t pts_100ns, const FrameCallback& cb);
    // Drops buffered frames (e.g. after a stream discontinuity).
    void Flush();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// GPU NV12 scale + letterbox through the D3D11 video processor, on the decoder's device. Every
// method is best-effort: a failure returns false (or Ok() stays false) and the caller uses the
// CPU path in common/src/image.cpp.
class Nv12Scaler {
public:
    Nv12Scaler();
    ~Nv12Scaler();
    Nv12Scaler(const Nv12Scaler&) = delete;
    Nv12Scaler& operator=(const Nv12Scaler&) = delete;

    bool Init(ID3D11Device* device);
    bool Ok() const;
    // Letterboxes the frame's display aperture into dst_width x dst_height and hands the mapped
    // NV12 result to `write` exactly once (valid only during the call). False -> use the CPU path.
    bool Scale(const DecodedFrame& src, uint32_t dst_width, uint32_t dst_height,
               const std::function<bool(const image::Nv12View&)>& write);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();
    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    bool Init(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate_kbps);
    // Encodes one frame; appends zero or more Annex-B access units to `out`.
    bool Encode(const image::Nv12Frame& frame, int64_t pts_100ns, bool force_keyframe,
                std::vector<std::vector<uint8_t>>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Renders the "connect your iPhone" placeholder: dark background, centered title and detail.
void RenderPlaceholder(image::Nv12Frame& frame, const std::wstring& title, const std::wstring& detail);

}  // namespace tethercam::media
