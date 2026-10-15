#pragma once

// H.264 encoding with FFmpeg (libx264, low latency), for fakephone on Linux. Same interface as the
// Windows media::H264Encoder.

#include <cstdint>
#include <memory>
#include <vector>

#include "tethercam/image.h"

namespace tethercam::media {

class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();
    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    bool Init(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate_kbps);
    // Encodes one frame; appends zero or more Annex-B access units (SPS/PPS inline on IDRs) to `out`.
    bool Encode(const image::Nv12Frame& frame, int64_t pts_100ns, bool force_keyframe,
                std::vector<std::vector<uint8_t>>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tethercam::media
