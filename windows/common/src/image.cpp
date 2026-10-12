#include "tethercam/image.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace tethercam::image {
namespace {

struct Rect {
    uint32_t x, y, w, h;
};

// Largest rect with src's aspect ratio centered in dst, with even coordinates for NV12.
Rect FitRect(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh) {
    uint32_t w = dw, h = static_cast<uint32_t>(uint64_t(dw) * sh / sw);
    if (h > dh) {
        h = dh;
        w = static_cast<uint32_t>(uint64_t(dh) * sw / sh);
    }
    w &= ~1u;
    h &= ~1u;
    return {((dw - w) / 2) & ~1u, ((dh - h) / 2) & ~1u, std::max(w, 2u), std::max(h, 2u)};
}

// Bilinear resample of one plane with `ch` interleaved channels.
void ResamplePlane(const uint8_t* src, uint32_t spitch, uint32_t sw, uint32_t sh, uint8_t* dst, uint32_t dpitch,
                   uint32_t dw, uint32_t dh, int ch, bool mirror) {
    if (sw == dw && sh == dh && !mirror) {
        for (uint32_t r = 0; r < dh; ++r) std::memcpy(dst + size_t(r) * dpitch, src + size_t(r) * spitch, size_t(dw) * ch);
        return;
    }
    // 16.16 fixed point, sampling at pixel centers.
    const int64_t xstep = (int64_t(sw) << 16) / dw;
    const int64_t ystep = (int64_t(sh) << 16) / dh;
    std::vector<int32_t> x0(dw), fx(dw);
    for (uint32_t x = 0; x < dw; ++x) {
        const uint32_t xi = mirror ? dw - 1 - x : x;
        int64_t sx = (int64_t(xi) * xstep) + xstep / 2 - 0x8000;
        sx = std::clamp<int64_t>(sx, 0, (int64_t(sw - 1) << 16));
        x0[x] = int32_t(sx >> 16);
        fx[x] = int32_t(sx & 0xFFFF) >> 8;
    }
    for (uint32_t y = 0; y < dh; ++y) {
        int64_t sy = (int64_t(y) * ystep) + ystep / 2 - 0x8000;
        sy = std::clamp<int64_t>(sy, 0, (int64_t(sh - 1) << 16));
        const uint32_t y0 = uint32_t(sy >> 16);
        const uint32_t y1 = std::min(y0 + 1, sh - 1);
        const int32_t fy = int32_t(sy & 0xFFFF) >> 8;
        const uint8_t* r0 = src + size_t(y0) * spitch;
        const uint8_t* r1 = src + size_t(y1) * spitch;
        uint8_t* out = dst + size_t(y) * dpitch;
        for (uint32_t x = 0; x < dw; ++x) {
            const uint32_t a = uint32_t(x0[x]);
            const uint32_t b = std::min(a + 1, sw - 1);
            for (int c = 0; c < ch; ++c) {
                const int32_t p00 = r0[a * ch + c], p01 = r0[b * ch + c];
                const int32_t p10 = r1[a * ch + c], p11 = r1[b * ch + c];
                const int32_t top = p00 * 256 + (p01 - p00) * fx[x];
                const int32_t bot = p10 * 256 + (p11 - p10) * fx[x];
                const int32_t v = (top * 256 + (bot - top) * fy + (1 << 15)) >> 16;
                out[x * ch + c] = static_cast<uint8_t>(v);
            }
        }
    }
}

}  // namespace

void FillBlack(Nv12Frame& f) {
    std::memset(f.Y(), 16, size_t(f.width) * f.height);
    std::memset(f.UV(), 128, size_t(f.width) * f.height / 2);
}

void ScaleLetterbox(const Nv12View& src, Nv12Frame& dst, bool mirror) {
    const Rect r = FitRect(src.width, src.height, dst.width, dst.height);
    if (r.w != dst.width || r.h != dst.height) FillBlack(dst);
    uint8_t* y = dst.Y() + size_t(r.y) * dst.width + r.x;
    ResamplePlane(src.y, src.y_pitch, src.width, src.height, y, dst.width, r.w, r.h, 1, mirror);
    uint8_t* uv = dst.UV() + size_t(r.y / 2) * dst.width + r.x;
    ResamplePlane(src.uv, src.uv_pitch, src.width / 2, src.height / 2, uv, dst.width, r.w / 2, r.h / 2, 2, mirror);
}

void BgraToNv12(const uint8_t* bgra, uint32_t pitch, Nv12Frame& dst) {
    const uint32_t w = dst.width, h = dst.height;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* p = bgra + size_t(y) * pitch;
        uint8_t* out = dst.Y() + size_t(y) * w;
        for (uint32_t x = 0; x < w; ++x) {
            const int b = p[x * 4], g = p[x * 4 + 1], r = p[x * 4 + 2];
            out[x] = static_cast<uint8_t>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    for (uint32_t y = 0; y < h / 2; ++y) {
        const uint8_t* p0 = bgra + size_t(2 * y) * pitch;
        const uint8_t* p1 = p0 + pitch;
        uint8_t* out = dst.UV() + size_t(y) * w;
        for (uint32_t x = 0; x < w / 2; ++x) {
            int b = 0, g = 0, r = 0;
            for (const uint8_t* p : {p0 + x * 8, p0 + x * 8 + 4, p1 + x * 8, p1 + x * 8 + 4}) {
                b += p[0];
                g += p[1];
                r += p[2];
            }
            b /= 4;
            g /= 4;
            r /= 4;
            out[2 * x] = static_cast<uint8_t>(std::clamp(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128, 0, 255));
            out[2 * x + 1] = static_cast<uint8_t>(std::clamp(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128, 0, 255));
        }
    }
}

void TestPattern(Nv12Frame& f, uint64_t n) {
    // 75% SMPTE-like bars in Y'CbCr: white, yellow, cyan, green, magenta, red, blue, black.
    static constexpr uint8_t kBars[8][3] = {{180, 128, 128}, {162, 44, 142}, {131, 156, 44}, {112, 72, 58},
                                             {84, 184, 198},  {65, 100, 212}, {35, 212, 114}, {16, 128, 128}};
    const uint32_t w = f.width, h = f.height;
    const uint32_t shift = static_cast<uint32_t>((n * 4) % w);
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* row = f.Y() + size_t(y) * w;
        for (uint32_t x = 0; x < w; ++x) row[x] = kBars[((x + shift) % w) * 8 / w][0];
    }
    for (uint32_t y = 0; y < h / 2; ++y) {
        uint8_t* row = f.UV() + size_t(y) * w;
        for (uint32_t x = 0; x < w / 2; ++x) {
            const auto& bar = kBars[((2 * x + shift) % w) * 8 / w];
            row[2 * x] = bar[1];
            row[2 * x + 1] = bar[2];
        }
    }
    // Binary frame counter: 16 squares along the bottom edge, white = 1.
    const uint32_t sq = std::max(8u, w / 40);
    for (uint32_t bit = 0; bit < 16; ++bit) {
        const uint8_t v = (n >> bit) & 1 ? 235 : 16;
        for (uint32_t y = h - 2 * sq; y < h - sq; ++y)
            std::memset(f.Y() + size_t(y) * w + sq + bit * sq * 2, v, sq);
    }
}

bool SaveBmp(const Nv12View& f, const char* path) {
    const uint32_t row = (f.width * 3 + 3) & ~3u;
    const uint32_t img = row * f.height;
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int off, uint32_t v) {
        for (int i = 0; i < 4; ++i) hdr[off + i] = uint8_t(v >> (8 * i));
    };
    put32(2, 54 + img);
    put32(10, 54);
    put32(14, 40);
    put32(18, f.width);
    put32(22, f.height);
    hdr[26] = 1;
    hdr[28] = 24;
    put32(34, img);
    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "wb") != 0 || !fp) return false;
    fwrite(hdr, 1, sizeof(hdr), fp);
    std::vector<uint8_t> line(row, 0);
    for (int32_t y = int32_t(f.height) - 1; y >= 0; --y) {  // BMP rows are bottom-up
        const uint8_t* yr = f.y + size_t(y) * f.y_pitch;
        const uint8_t* uvr = f.uv + size_t(y / 2) * f.uv_pitch;
        for (uint32_t x = 0; x < f.width; ++x) {
            const int c = yr[x] - 16, d = uvr[(x & ~1u)] - 128, e = uvr[(x & ~1u) + 1] - 128;
            line[x * 3 + 2] = uint8_t(std::clamp((298 * c + 409 * e + 128) >> 8, 0, 255));
            line[x * 3 + 1] = uint8_t(std::clamp((298 * c - 100 * d - 208 * e + 128) >> 8, 0, 255));
            line[x * 3 + 0] = uint8_t(std::clamp((298 * c + 516 * d + 128) >> 8, 0, 255));
        }
        fwrite(line.data(), 1, row, fp);
    }
    fclose(fp);
    return true;
}

}  // namespace tethercam::image
