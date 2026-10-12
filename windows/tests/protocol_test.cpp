#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <random>
#include <sstream>

#include "tethercam/protocol.h"

using namespace tethercam::proto;

namespace {

std::vector<uint8_t> Hex(const std::string& s) {
    std::vector<uint8_t> out;
    std::string digits;
    for (char c : s)
        if (std::isxdigit(static_cast<unsigned char>(c))) digits += c;
    for (size_t i = 0; i + 1 < digits.size(); i += 2) out.push_back(uint8_t(std::stoi(digits.substr(i, 2), nullptr, 16)));
    return out;
}

std::map<std::string, std::vector<uint8_t>> LoadVectors() {
    std::ifstream f(TETHERCAM_VECTORS);
    std::map<std::string, std::vector<uint8_t>> out;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string name;
        ss >> name;
        std::string rest;
        std::getline(ss, rest);
        out[name] = Hex(rest);
    }
    return out;
}

Message DecodeOne(const std::vector<uint8_t>& frame) {
    StreamParser p;
    p.Feed(frame);
    Message m;
    EXPECT_EQ(p.Next(m), StreamParser::Result::Message);
    EXPECT_EQ(p.Buffered(), 0u);
    return m;
}

}  // namespace

TEST(Protocol, VectorsFileHasAllTypes) { EXPECT_EQ(LoadVectors().size(), 13u); }

TEST(Protocol, EveryVectorRoundTrips) {
    for (const auto& [name, bytes] : LoadVectors()) {
        SCOPED_TRACE(name);
        Message m = DecodeOne(bytes);
        EXPECT_NE(m.index(), Message(Unknown{}).index());
        EXPECT_EQ(Encode(m), bytes);
    }
}

TEST(Protocol, VectorsDecodeToExpectedValues) {
    auto v = LoadVectors();
    auto hello = std::get<Hello>(DecodeOne(v["hello_device"]));
    EXPECT_EQ(hello.role, Role::Device);
    EXPECT_EQ(hello.app_version, "0.1");
    EXPECT_EQ(hello.name, "A");

    auto caps = std::get<Caps>(DecodeOne(v["caps_one_camera"]));
    ASSERT_EQ(caps.cameras.size(), 1u);
    EXPECT_EQ(caps.cameras[0].label, "W");
    ASSERT_EQ(caps.cameras[0].formats.size(), 1u);
    EXPECT_EQ(caps.cameras[0].formats[0], (Format{1920, 1080, 60}));

    auto sf = std::get<SetFormat>(DecodeOne(v["set_format_1080p30"]));
    EXPECT_EQ(sf.width, 1920);
    EXPECT_EQ(sf.height, 1080);
    EXPECT_EQ(sf.fps, 30);

    auto vf = std::get<VideoFrame>(DecodeOne(v["video_frame_idr"]));
    EXPECT_EQ(vf.pts_us, 1000000u);
    EXPECT_TRUE(vf.IsKeyframe());
    EXPECT_EQ(vf.data, (std::vector<uint8_t>{0, 0, 0, 1, 0x65, 0x88}));

    auto st = std::get<Status>(DecodeOne(v["status"]));
    EXPECT_EQ(st.actual_bitrate_kbps, 12000u);
    EXPECT_EQ(st.actual_fps_x100, 2997);

    auto opts = std::get<SetOptions>(DecodeOne(v["set_options"]));
    EXPECT_EQ(opts.rotation, 90);
}

TEST(Protocol, ParserHandlesByteAtATimeDelivery) {
    VideoFrame f;
    f.pts_us = 42;
    f.data.assign(5000, 0xAB);
    auto bytes = Encode(f);
    auto more = Encode(Heartbeat{7});
    bytes.insert(bytes.end(), more.begin(), more.end());

    StreamParser p;
    std::vector<std::string> got;
    for (uint8_t b : bytes) {
        p.Feed({&b, 1});
        Message m;
        while (p.Next(m) == StreamParser::Result::Message) got.push_back(TypeName(m));
    }
    EXPECT_EQ(got, (std::vector<std::string>{"VideoFrame", "Heartbeat"}));
}

TEST(Protocol, UnknownTypeIsSkipped) {
    std::vector<uint8_t> frame = {'T', 'C', 'A', 'M', 1, 0x55, 0, 0, 3, 0, 0, 0, 1, 2, 3};
    auto hb = Encode(Heartbeat{1});
    frame.insert(frame.end(), hb.begin(), hb.end());
    StreamParser p;
    p.Feed(frame);
    Message m;
    ASSERT_EQ(p.Next(m), StreamParser::Result::Message);
    EXPECT_EQ(std::get<Unknown>(m).type, 0x55);
    ASSERT_EQ(p.Next(m), StreamParser::Result::Message);
    EXPECT_TRUE(std::holds_alternative<Heartbeat>(m));
}

TEST(Protocol, BadMagicAndOversizeAreFatal) {
    StreamParser p;
    std::vector<uint8_t> bad = {'X', 'C', 'A', 'M', 1, 1, 0, 0, 0, 0, 0, 0};
    p.Feed(bad);
    Message m;
    EXPECT_EQ(p.Next(m), StreamParser::Result::Fatal);

    StreamParser p2;
    std::vector<uint8_t> huge = {'T', 'C', 'A', 'M', 1, 0x21, 0, 0, 0xFF, 0xFF, 0xFF, 0x7F};
    p2.Feed(huge);
    EXPECT_EQ(p2.Next(m), StreamParser::Result::Fatal);
}

TEST(Protocol, TruncatedKnownMessageIsFatal) {
    // SetFormat with only 3 payload bytes.
    std::vector<uint8_t> f = {'T', 'C', 'A', 'M', 1, 0x11, 0, 0, 3, 0, 0, 0, 1, 2, 3};
    StreamParser p;
    p.Feed(f);
    Message m;
    EXPECT_EQ(p.Next(m), StreamParser::Result::Fatal);
}

TEST(Protocol, CapsWithImpossibleFormatCountIsRejected) {
    // One camera claiming 65535 formats in a tiny payload must not allocate or crash.
    std::vector<uint8_t> payload = {1, 0, 0, 0, 0, 0, 0xFF, 0xFF, 1};
    EXPECT_FALSE(DecodePayload(uint8_t(MsgType::Caps), payload).has_value());
}

TEST(Protocol, FuzzRandomPayloadsNeverCrash) {
    std::mt19937 rng(1234);
    for (int i = 0; i < 20000; ++i) {
        std::vector<uint8_t> payload(rng() % 64);
        for (auto& b : payload) b = uint8_t(rng());
        const uint8_t types[] = {0x01, 0x02, 0x10, 0x11, 0x15, 0x20, 0x21, 0x30, 0x31, 0x7F};
        (void)DecodePayload(types[rng() % std::size(types)], payload);
    }
    for (int i = 0; i < 2000; ++i) {
        std::vector<uint8_t> stream(rng() % 256);
        for (auto& b : stream) b = uint8_t(rng());
        if (stream.size() >= 4 && rng() % 2) std::copy(kMagic, kMagic + 4, stream.begin());
        StreamParser p;
        p.Feed(stream);
        Message m;
        for (int k = 0; k < 10 && p.Next(m) == StreamParser::Result::Message; ++k) {
        }
    }
    SUCCEED();
}
