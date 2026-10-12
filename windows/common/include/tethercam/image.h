#pragma once

// CPU image helpers for NV12 frames (BT.601 limited range).

#include <cstdint>
#include <vector>

namespace tethercam::image {

struct Nv12View {
    const uint8_t* y = nullptr;
    const uint8_t* uv = nullptr;
    uint32_t y_pitch = 0;
    uint32_t uv_pitch = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Tightly packed NV12 buffer (Y plane followed by interleaved UV plane).
struct Nv12Frame {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> data;

    void Resize(uint32_t w, uint32_t h) {
        width = w;
        height = h;
        data.resize(size_t(w) * h * 3 / 2);
    }
    uint8_t* Y() { return data.data(); }
    uint8_t* UV() { return data.data() + size_t(width) * height; }
    Nv12View View() const {
        return {data.data(), data.data() + size_t(width) * height, width, width, width, height};
    }
};

void FillBlack(Nv12Frame& f);

// Scales `src` into `dst` (dst size already set), preserving aspect ratio with black bars,
// optionally mirrored horizontally. Bilinear.
void ScaleLetterbox(const Nv12View& src, Nv12Frame& dst, bool mirror = false);

// Converts a top-down BGRA image to NV12 (dst size already set to the same dimensions).
void BgraToNv12(const uint8_t* bgra, uint32_t pitch, Nv12Frame& dst);

// Moving colour bars with a frame-number ticker, for fakephone and tests.
void TestPattern(Nv12Frame& f, uint64_t frame_number);

// Writes an NV12 frame as a 24-bit BMP (for debugging snapshots).
bool SaveBmp(const Nv12View& f, const char* path);

}  // namespace tethercam::image
