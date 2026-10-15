#pragma once

// H.264 decoding with FFmpeg: hardware decode on the discrete GPU when there is one, then the
// integrated GPU (VA-API, or NVDEC on NVIDIA), software otherwise.
// Output is NV12, the same as the Windows decoder.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "tethercam/image.h"

namespace tethercam::media {

class H264Decoder {
public:
    // A decoded frame. Valid only during the callback.
    using FrameCallback = std::function<void(const image::Nv12View& frame, int64_t pts_us)>;

    H264Decoder();
    ~H264Decoder();
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // (Re)creates the decoder. With prefer_hardware, tries the GPUs that haven't failed yet first.
    bool Init(bool prefer_hardware);
    bool IsHardware() const;
    // Feeds one Annex-B access unit. Calls `cb` for every frame that comes out.
    // Returns false on a decoder error (caller should request a keyframe).
    bool Decode(std::span<const uint8_t> au, int64_t pts_us, const FrameCallback& cb);
    // Drops buffered frames (e.g. after a stream discontinuity).
    void Flush();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tethercam::media
