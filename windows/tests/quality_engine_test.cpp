#include <gtest/gtest.h>

#include "../agent/engine.h"
#include "tethercam/media.h"

#include <chrono>
#include <thread>

using namespace tethercam;
using namespace std::chrono_literals;

// Exercise the actual agent protocol loop without accessing the user's phone, registry, or camera.
class QualityEngine : public ::testing::Test {
protected:
    net::Socket listener;
    net::Socket phone;
    proto::StreamParser parser;
    std::unique_ptr<agent::Engine> engine;
    media::MfScope mf;

    void SetUp() override {
        listener = net::ListenTcp("127.0.0.1", 0);
        ASSERT_TRUE(listener.Valid());
        sockaddr_in address{};
        int size = sizeof(address);
        ASSERT_EQ(getsockname(listener.Get(), reinterpret_cast<sockaddr*>(&address), &size), 0);
        agent::Engine::Options options;
        options.tcp = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
        options.persist_settings = false;
        options.shared_memory = false;
        engine = std::make_unique<agent::Engine>(options);
        engine->Start([] {});
        engine->SetPreview(true);
        ASSERT_EQ(listener.WaitReadable(4000), 1);
        phone = net::Accept(listener);
        ASSERT_TRUE(phone.Valid());
    }

    void TearDown() override {
        if (engine) engine->StopThread();
    }

    void Send(const proto::Message& message) { ASSERT_TRUE(phone.SendAll(proto::Encode(message))); }

    void Handshake(proto::Quality quality) {
        proto::Hello hello;
        hello.role = proto::Role::Device;
        Send(hello);
        Send(proto::SetQuality{quality});
        proto::Caps caps;
        proto::Camera front;
        front.camera_id = 0;
        front.position = proto::Position::Front;
        front.lens = proto::Lens::Front;
        front.label = "Test front";
        front.formats = {{1920, 1080, 60}, {3840, 2160, 60}};
        caps.cameras.push_back(front);
        Send(caps);
    }

    template<class T> std::optional<T> Receive() {
        const auto deadline = std::chrono::steady_clock::now() + 4s;
        uint8_t bytes[4096];
        while (std::chrono::steady_clock::now() < deadline) {
            proto::Message message;
            while (parser.Next(message) == proto::StreamParser::Result::Message)
                if (const auto* value = std::get_if<T>(&message)) return *value;
            if (phone.WaitReadable(100) != 1) continue;
            const int n = phone.RecvSome(bytes);
            if (n <= 0) return {};
            parser.Feed({bytes, size_t(n)});
        }
        return {};
    }

    void ExpectFormat(uint16_t width, uint16_t height, uint16_t fps) {
        const auto format = Receive<proto::SetFormat>();
        ASSERT_TRUE(format);
        EXPECT_EQ(format->width, width);
        EXPECT_EQ(format->height, height);
        EXPECT_EQ(format->fps, fps);
        EXPECT_EQ(format->bitrate_kbps, DefaultBitrateKbps(width, height, fps));
        EXPECT_TRUE(Receive<proto::Start>());
    }
};

TEST_F(QualityEngine, PhoneAndDesktopChangesRenegotiateCapture) {
    Handshake(proto::Quality::UHD60);
    ExpectFormat(3840, 2160, 60);
    EXPECT_EQ(engine->Get().quality, proto::Quality::UHD60);

    Send(proto::SetQuality{proto::Quality::HD60});
    ExpectFormat(1920, 1080, 60);
    EXPECT_EQ(engine->Get().quality, proto::Quality::HD60);

    engine->SetQuality(proto::Quality::UHD30);
    const auto quality = Receive<proto::SetQuality>();
    ASSERT_TRUE(quality);
    EXPECT_EQ(quality->quality, proto::Quality::UHD30);
    ExpectFormat(3840, 2160, 30);

    Send(proto::SetQuality{proto::Quality::Auto});
    ExpectFormat(1920, 1080, 30); // Auto restores preview demand, not the previous override.
}

TEST_F(QualityEngine, OfflineDesktopChoiceWinsThenPhonePreferenceWinsReconnect) {
    engine->SetQuality(proto::Quality::UHD60);
    Handshake(proto::Quality::HD30);
    const auto quality = Receive<proto::SetQuality>();
    ASSERT_TRUE(quality);
    EXPECT_EQ(quality->quality, proto::Quality::UHD60);
    ExpectFormat(3840, 2160, 60);

    phone.Shutdown();
    phone.Close();
    parser = {};
    ASSERT_EQ(listener.WaitReadable(5000), 1);
    phone = net::Accept(listener);
    ASSERT_TRUE(phone.Valid());
    Handshake(proto::Quality::HD60);
    ExpectFormat(1920, 1080, 60);
    EXPECT_EQ(engine->Get().quality, proto::Quality::HD60);
}
