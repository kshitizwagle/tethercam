#include <gtest/gtest.h>

#include "tethercam/annexb.h"
#include "tethercam/formats.h"
#include "tethercam/image.h"

using namespace tethercam;

TEST(Quality, OverridesCaptureWithoutChangingConsumerOutput) {
    const MediaFormat consumer{1920, 1080, 30};
    const proto::Format caps[] = {{1920, 1080, 60}, {3840, 2160, 60}};
    MediaFormat capture;
    ASSERT_TRUE(ChooseDeviceFormat(caps, QualityFormat(proto::Quality::UHD60, consumer), capture));
    EXPECT_EQ(capture, (MediaFormat{3840, 2160, 60}));
    ASSERT_TRUE(ChooseDeviceFormat(caps, QualityFormat(proto::Quality::Auto, consumer), capture));
    EXPECT_EQ(capture, consumer);
    ASSERT_TRUE(ChooseDeviceFormat(caps, QualityFormat(proto::Quality::HD60, consumer), capture));
    EXPECT_EQ(capture, (MediaFormat{1920, 1080, 60}));
}

TEST(Quality, LensCapabilitiesLimitPresetsAndFallbackRate) {
    const proto::Format caps[] = {{1920, 1080, 60}, {3840, 2160, 30}};
    EXPECT_TRUE(SupportsQuality({}, proto::Quality::Auto));
    EXPECT_TRUE(SupportsQuality(caps, proto::Quality::HD60));
    EXPECT_TRUE(SupportsQuality(caps, proto::Quality::UHD30));
    EXPECT_FALSE(SupportsQuality(caps, proto::Quality::UHD60));
    EXPECT_FALSE(SupportsQuality({}, proto::Quality::UHD60));
    MediaFormat capture;
    ASSERT_TRUE(ChooseDeviceFormat(caps, QualityFormat(proto::Quality::UHD60, {}), capture));
    EXPECT_EQ(capture, (MediaFormat{3840, 2160, 30}));
    const proto::Format front[] = {{1920, 1080, 30}};
    ASSERT_TRUE(ChooseDeviceFormat(front, QualityFormat(proto::Quality::UHD60, {}), capture));
    EXPECT_EQ(capture, (MediaFormat{1920, 1080, 30}));
}

TEST(AnnexB, SplitsAndClassifies) {
    const std::vector<uint8_t> au = {0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x68, 3, 0, 0, 0, 1, 0x65, 9, 9};
    auto nals = annexb::SplitNals(au);
    ASSERT_EQ(nals.size(), 3u);
    EXPECT_EQ(annexb::NalType(nals[0]), 7);
    EXPECT_EQ(nals[0].size(), 3u);
    EXPECT_EQ(annexb::NalType(nals[1]), 8);
    EXPECT_EQ(annexb::NalType(nals[2]), 5);
    EXPECT_TRUE(annexb::HasIdr(au));
    EXPECT_EQ(annexb::ExtractParameterSets(au),
              (std::vector<uint8_t>{0, 0, 0, 1, 0x67, 1, 2, 0, 0, 0, 1, 0x68, 3}));
    EXPECT_EQ(annexb::StripParameterSets(au), (std::vector<uint8_t>{0, 0, 0, 1, 0x65, 9, 9}));
}

TEST(AnnexB, AvccConversion) {
    const std::vector<uint8_t> avcc = {0, 0, 0, 2, 0x41, 7, 0, 0, 0, 1, 0x06};
    EXPECT_EQ(annexb::AvccToAnnexB(avcc), (std::vector<uint8_t>{0, 0, 0, 1, 0x41, 7, 0, 0, 0, 1, 0x06}));
    const std::vector<uint8_t> truncated = {0, 0, 0, 9, 1};
    EXPECT_TRUE(annexb::AvccToAnnexB(truncated).empty());
}

TEST(Image, SameSizeScaleIsCopy) {
    image::Nv12Frame a, b;
    a.Resize(64, 32);
    image::TestPattern(a, 3);
    b.Resize(64, 32);
    image::ScaleLetterbox(a.View(), b);
    EXPECT_EQ(a.data, b.data);
}

TEST(Image, LetterboxAddsBars) {
    image::Nv12Frame src, dst;
    src.Resize(160, 160);  // square into 16:9 => pillarbox
    std::fill(src.data.begin(), src.data.end(), uint8_t(200));
    dst.Resize(320, 180);
    image::ScaleLetterbox(src.View(), dst);
    EXPECT_EQ(dst.Y()[0], 16);                  // left bar
    EXPECT_EQ(dst.Y()[160 + 90 * 320], 200);    // center
    EXPECT_EQ(dst.UV()[0], 128);
}

TEST(Image, MirrorFlipsHorizontally) {
    image::Nv12Frame src, dst;
    src.Resize(8, 2);
    image::FillBlack(src);
    src.Y()[0] = 235;  // top-left white
    src.Y()[8] = 235;
    dst.Resize(8, 2);
    image::ScaleLetterbox(src.View(), dst, true);
    EXPECT_EQ(dst.Y()[7], 235);
    EXPECT_EQ(dst.Y()[0], 16);
}

TEST(Image, Nv12ToBgraRoundTrip) {
    std::vector<uint8_t> bgra(4 * 4 * 2, 255);
    for (int i = 16; i < 32; ++i) bgra[size_t(i)] = (i % 4 == 3) ? 255 : 0;  // second row black
    image::Nv12Frame f;
    f.Resize(4, 2);
    image::BgraToNv12(bgra.data(), 16, f);
    std::vector<uint8_t> back(4 * 4 * 2, 0);
    image::Nv12ToBgra(f.View(), back.data(), 16);
    EXPECT_NEAR(back[0], 255, 2);  // white stays white
    EXPECT_NEAR(back[16], 0, 2);   // black stays black
    EXPECT_EQ(back[3], 255);       // opaque
}

TEST(Image, BgraWhiteAndBlack) {
    std::vector<uint8_t> bgra(4 * 4 * 2, 255);
    for (int i = 16; i < 32; ++i) bgra[size_t(i)] = 0;  // second row black
    image::Nv12Frame f;
    f.Resize(4, 2);
    image::BgraToNv12(bgra.data(), 16, f);
    EXPECT_EQ(f.Y()[0], 235);
    EXPECT_EQ(f.Y()[4], 16);
    EXPECT_NEAR(f.UV()[0], 128, 1);
    EXPECT_NEAR(f.UV()[1], 128, 1);
}
