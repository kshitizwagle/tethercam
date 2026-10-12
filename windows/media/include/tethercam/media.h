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

class H264Decoder {
public:
    // A decoded frame. Valid only during the callback.
    using FrameCallback = std::function<void(const image::Nv12View& frame, int64_t pts_100ns)>;

    H264Decoder();
    ~H264Decoder();
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // Creates the decoder. With prefer_hardware, tries DXVA through a D3D11 device first.
    bool Init(uint32_t width, uint32_t height, uint32_t fps, bool prefer_hardware);
    bool IsHardware() const;
    // Feeds one Annex-B access unit. Calls `cb` for every frame that comes out.
    // Returns false on a decoder error (caller should request a keyframe).
    bool Decode(std::span<const uint8_t> au, int64_t pts_100ns, const FrameCallback& cb);
    // Drops buffered frames (e.g. after a stream discontinuity).
    void Flush();

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
