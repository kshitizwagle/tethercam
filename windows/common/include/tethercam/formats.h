#pragma once

// Format selection and bitrate policy (docs/TECH_SPEC.md §6 and §7).

#include <cstdint>
#include <span>
#include <vector>

#include "tethercam/protocol.h"

namespace tethercam {

struct MediaFormat {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    bool operator==(const MediaFormat&) const = default;
};

// Media types the virtual camera advertises (F4), in preference order (largest first).
std::span<const MediaFormat> AdvertisedFormats();

// Picks the device format for a requested media type: exact match, else the smallest
// format at least as large in both dimensions, else the largest available. fps is clamped
// to the chosen format's max. Returns false if `caps` is empty.
bool ChooseDeviceFormat(std::span<const proto::Format> caps, const MediaFormat& requested, MediaFormat& out);

// Default H.264 bitrate in kbps for a size and rate (bitrate table in TECH_SPEC §6).
uint32_t DefaultBitrateKbps(uint32_t width, uint32_t height, uint32_t fps);

// Size of an NV12 frame with tightly packed rows.
constexpr size_t Nv12Size(uint32_t width, uint32_t height) { return size_t(width) * height * 3 / 2; }

}  // namespace tethercam
