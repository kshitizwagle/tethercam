#include "tethercam/formats.h"

#include <algorithm>
#include <cmath>

namespace tethercam {

std::span<const MediaFormat> AdvertisedFormats() {
    // 1080p30 first: it's the default most apps pick when they take the first type.
    static constexpr MediaFormat kFormats[] = {
        {1920, 1080, 30}, {1920, 1080, 60}, {3840, 2160, 30}, {3840, 2160, 60}, {2560, 1440, 30},
        {2560, 1440, 60}, {1280, 720, 30},  {1280, 720, 60},  {640, 480, 30},   {640, 480, 60},
    };
    return kFormats;
}

bool ChooseDeviceFormat(std::span<const proto::Format> caps, const MediaFormat& requested, MediaFormat& out) {
    if (caps.empty()) return false;
    const proto::Format* best = nullptr;
    // 1. Exact size match.
    for (const auto& f : caps)
        if (f.width == requested.width && f.height == requested.height) best = &f;
    // 2. Smallest format covering the request in both dimensions (by area).
    if (!best) {
        for (const auto& f : caps) {
            if (f.width >= requested.width && f.height >= requested.height) {
                if (!best || uint64_t(f.width) * f.height < uint64_t(best->width) * best->height) best = &f;
            }
        }
    }
    // 3. Largest available.
    if (!best) {
        for (const auto& f : caps)
            if (!best || uint64_t(f.width) * f.height > uint64_t(best->width) * best->height) best = &f;
    }
    out.width = best->width;
    out.height = best->height;
    out.fps = std::max<uint32_t>(1, std::min<uint32_t>(requested.fps ? requested.fps : 30, best->max_fps));
    return true;
}

uint32_t DefaultBitrateKbps(uint32_t width, uint32_t height, uint32_t fps) {
    // Anchored on 1080p30 = 12 Mbps and 4K30 = 40 Mbps; 60 fps costs ~1.7x.
    const double pixels = double(width) * height;
    const double base = 12000.0 * std::pow(pixels / (1920.0 * 1080.0), 0.87);
    const double rate = fps > 30 ? 1.7 : 1.0;
    return static_cast<uint32_t>(std::clamp(base * rate, 2000.0, 150000.0));
}

}  // namespace tethercam
