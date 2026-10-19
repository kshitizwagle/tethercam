#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <thread>

#include "../agent/engine.h"
#include "tethercam/media.h"

using namespace tethercam;
using namespace std::chrono_literals;

namespace {

// Reads protocol messages from any link until one of type T arrives (4 s).
template <class T> std::optional<T> ReceiveOn(Link& link, proto::StreamParser& parser) {
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    uint8_t bytes[4096];
    while (std::chrono::steady_clock::now() < deadline) {
        proto::Message message;
        while (parser.Next(message) == proto::StreamParser::Result::Message)
            if (const auto* value = std::get_if<T>(&message)) return *value;
        if (link.WaitReadable(100) != 1) continue;
        const int n = link.RecvSome(bytes);
        if (n <= 0) return {};
        parser.Feed({bytes, size_t(n)});
    }
    return {};
}

bool WaitFor(const std::function<bool()>& cond) {
    for (int i = 0; i < 80; ++i) {
        if (cond()) return true;
        std::this_thread::sleep_for(50ms);
    }
    return false;
}

}  // namespace

// The whole M11 flow against the real engine: pair over the plain link (the USB stand-in), then a
// second agent run reconnects over the encrypted Wi-Fi transport with the stored key.
TEST(WifiEngine, PairsOverPlainLinkThenStreamsOverWifi) {
    media::MfScope mf;
    const auto file = std::filesystem::temp_directory_path() /
                      ("tethercam-pairing-test-" + std::to_string(GetCurrentProcessId()) + ".bin");
    std::filesystem::remove(file);

    // 1. "USB": the engine offers a key; the phone (this test) accepts it.
    auto usb = net::ListenTcp("127.0.0.1", 0);
    ASSERT_TRUE(usb.Valid());
    agent::Engine::Options options;
    options.tcp = "127.0.0.1:" + std::to_string(usb.LocalPort());
    options.persist_settings = false;
    options.shared_memory = false;
    options.pairing_file = file.string();
    proto::WifiPair offer;
    {
        agent::Engine engine(options);
        engine.Start([] {});
        ASSERT_EQ(usb.WaitReadable(4000), 1);
        PlainLink phone(net::Accept(usb));
        proto::StreamParser parser;
        ASSERT_TRUE(ReceiveOn<proto::Hello>(phone, parser));
        const auto got = ReceiveOn<proto::WifiPair>(phone, parser);
        ASSERT_TRUE(got);
        offer = *got;
        EXPECT_FALSE(offer.name.empty());

        proto::Hello hello;
        hello.role = proto::Role::Device;
        hello.name = "Test iPhone";
        ASSERT_TRUE(phone.SendAll(proto::Encode(hello)));
        proto::WifiPaired reply;
        reply.device_id[0] = 0x5A;
        reply.name = "Test iPhone";
        reply.address = "127.0.0.1";
        ASSERT_TRUE(phone.SendAll(proto::Encode(reply)));
        EXPECT_TRUE(WaitFor([&] { return engine.Get().wifi_phone == "Test iPhone"; }));
        EXPECT_EQ(engine.Get().transport, "usb");
        engine.StopThread();
    }
    ASSERT_TRUE(std::filesystem::exists(file));

    // 2. Wi-Fi: a new agent loads the pairing and opens an encrypted session.
    auto wifi_listener = net::ListenTcp("127.0.0.1", 0);
    ASSERT_TRUE(wifi_listener.Valid());
    options.tcp.clear();
    options.wifi = "127.0.0.1:" + std::to_string(wifi_listener.LocalPort());
    agent::Engine engine(options);
    engine.Start([] {});
    EXPECT_EQ(engine.Get().wifi_phone, "Test iPhone");
    ASSERT_EQ(wifi_listener.WaitReadable(4000), 1);
    auto link = wifi::AcceptDevice(net::Accept(wifi_listener), [&](const wifi::HostId& host) -> std::optional<wifi::PairKey> {
        if (host == offer.host_id) return offer.key;
        return std::nullopt;
    });
    ASSERT_TRUE(link);
    proto::StreamParser parser;
    const auto hello = ReceiveOn<proto::Hello>(*link, parser);
    ASSERT_TRUE(hello);
    EXPECT_EQ(hello->role, proto::Role::Host);
    EXPECT_TRUE(WaitFor([&] { return engine.Get().transport == "wifi"; }));
    // No key offer over Wi-Fi.
    EXPECT_FALSE(ReceiveOn<proto::WifiPair>(*link, parser));

    engine.ForgetWifi();
    EXPECT_TRUE(engine.Get().wifi_phone.empty());
    engine.StopThread();
    std::filesystem::remove(file);
}

TEST(WifiEngine, NothingPairedMeansNoWifiAttempt) {
    media::MfScope mf;
    auto wifi_listener = net::ListenTcp("127.0.0.1", 0);
    agent::Engine::Options options;
    options.wifi = "127.0.0.1:" + std::to_string(wifi_listener.LocalPort());
    options.persist_settings = false;
    options.shared_memory = false;
    agent::Engine engine(options);
    engine.Start([] {});
    // Nothing paired: the agent never connects.
    EXPECT_NE(wifi_listener.WaitReadable(1500), 1);
    EXPECT_EQ(engine.Get().status, AgentStatus::NoDevice);
    engine.StopThread();
}
