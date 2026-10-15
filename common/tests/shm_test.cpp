#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "tethercam/shm.h"

using namespace tethercam;

namespace {

struct Mapping {
    std::vector<uint8_t> mem = std::vector<uint8_t>(shm::kMappingBytes + 64);
    void* base() { return mem.data() + (64 - reinterpret_cast<uintptr_t>(mem.data()) % 64) % 64; }
};

std::vector<uint8_t> Frame(uint32_t w, uint32_t h, uint8_t fill) { return std::vector<uint8_t>(size_t(w) * h * 3 / 2, fill); }

}  // namespace

TEST(Shm, InitAndValidate) {
    Mapping m;
    shm::InitHeader(m.base());
    EXPECT_TRUE(shm::ValidateHeader(m.base(), shm::kMappingBytes));
    EXPECT_FALSE(shm::ValidateHeader(m.base(), shm::kMappingBytes - 1));
    static_cast<shm::Header*>(m.base())->magic = 0;
    EXPECT_FALSE(shm::ValidateHeader(m.base(), shm::kMappingBytes));
}

TEST(Shm, NoFrameBeforeFirstWrite) {
    Mapping m;
    shm::InitHeader(m.base());
    auto dst = Frame(64, 32, 0);
    shm::FrameInfo info;
    EXPECT_FALSE(shm::ReadLatest(m.base(), 64, 32, dst.data(), 64, info));
}

TEST(Shm, WriteThenReadNewest) {
    Mapping m;
    shm::InitHeader(m.base());
    for (uint8_t v = 1; v <= 5; ++v) {
        auto f = Frame(64, 32, v);
        ASSERT_TRUE(shm::WriteFrame(m.base(), 64, 32, f.data(), 64, f.data() + 64 * 32, 64, v * 100));
    }
    auto dst = Frame(64, 32, 0);
    shm::FrameInfo info;
    ASSERT_TRUE(shm::ReadLatest(m.base(), 64, 32, dst.data(), 64, info));
    EXPECT_EQ(dst.front(), 5);
    EXPECT_EQ(dst.back(), 5);
    EXPECT_EQ(info.pts_100ns, 500u);
}

TEST(Shm, SizeMismatchIsNotRead) {
    Mapping m;
    shm::InitHeader(m.base());
    auto f = Frame(64, 32, 9);
    ASSERT_TRUE(shm::WriteFrame(m.base(), 64, 32, f.data(), 64, f.data() + 64 * 32, 64, 0));
    auto dst = Frame(128, 64, 0);
    shm::FrameInfo info;
    EXPECT_FALSE(shm::ReadLatest(m.base(), 128, 64, dst.data(), 128, info));
}

TEST(Shm, RejectsOversizeAndOddFrames) {
    Mapping m;
    shm::InitHeader(m.base());
    uint8_t px = 0;
    EXPECT_FALSE(shm::WriteFrame(m.base(), 4096, 2160, &px, 0, &px, 0, 0));
    EXPECT_FALSE(shm::WriteFrame(m.base(), 63, 32, &px, 0, &px, 0, 0));
}

TEST(Shm, PitchedCopy) {
    Mapping m;
    shm::InitHeader(m.base());
    // Source with pitch 80 for a 64-wide frame: padding bytes must not leak into the output.
    std::vector<uint8_t> src(80 * 32 * 3 / 2, 0xEE);
    for (int r = 0; r < 48; ++r)
        for (int c = 0; c < 64; ++c) src[size_t(r) * 80 + size_t(c)] = uint8_t(r);
    ASSERT_TRUE(shm::WriteFrame(m.base(), 64, 32, src.data(), 80, src.data() + 80 * 32, 80, 0));
    auto dst = Frame(64, 32, 0);
    shm::FrameInfo info;
    ASSERT_TRUE(shm::ReadLatest(m.base(), 64, 32, dst.data(), 64, info));
    EXPECT_EQ(dst[64 * 3], 3);         // row 3 of Y
    EXPECT_EQ(dst[64 * 32 + 64], 33);  // row 1 of UV = source row 33
    for (uint8_t b : dst) EXPECT_NE(b, 0xEE);
}

TEST(Shm, ConcurrentReadsNeverTear) {
    Mapping m;
    shm::InitHeader(m.base());
    constexpr uint32_t W = 320, Hh = 180;
    std::atomic<bool> stop = false;
    std::thread writer([&] {
        auto f = Frame(W, Hh, 0);
        for (uint32_t n = 0; !stop; ++n) {
            std::fill(f.begin(), f.end(), uint8_t(n));
            shm::WriteFrame(m.base(), W, Hh, f.data(), W, f.data() + W * Hh, W, n);
        }
    });
    auto dst = Frame(W, Hh, 0);
    int good = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (good < 500 && std::chrono::steady_clock::now() < deadline) {
        shm::FrameInfo info;
        if (!shm::ReadLatest(m.base(), W, Hh, dst.data(), W, info)) continue;
        const uint8_t v = dst[0];
        for (uint8_t b : dst) ASSERT_EQ(b, v) << "torn frame";
        ++good;
    }
    stop = true;
    writer.join();
    EXPECT_GT(good, 0);
}
