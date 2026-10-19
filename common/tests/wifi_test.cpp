#include <gtest/gtest.h>

#include <future>
#include <random>
#include <string>
#include <thread>

#include "tethercam/link.h"
#include "tethercam/noise.h"
#include "tethercam/pairing.h"

using namespace tethercam;

namespace {

std::vector<uint8_t> Hex(const std::string& s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(uint8_t(std::stoi(s.substr(i, 2), nullptr, 16)));
    return out;
}

std::span<const uint8_t, noise::kKeySize> Key32(const std::vector<uint8_t>& v) {
    return std::span<const uint8_t, noise::kKeySize>(v.data(), noise::kKeySize);
}

// A connected TCP pair on loopback.
std::pair<net::Socket, net::Socket> SocketPair() {
    auto listener = net::ListenTcp("127.0.0.1", 0);
    EXPECT_TRUE(listener.Valid());
    auto client = net::ConnectTcp("127.0.0.1", listener.LocalPort());
    auto server = net::Accept(listener);
    EXPECT_TRUE(client.Valid() && server.Valid());
    return {std::move(client), std::move(server)};
}

std::vector<uint8_t> ReadN(Link& link, size_t n) {
    std::vector<uint8_t> out;
    uint8_t buf[70000];
    while (out.size() < n) {
        if (link.WaitReadable(3000) != 1) break;
        const int got = link.RecvSome({buf, std::min(sizeof(buf), n - out.size())});
        if (got <= 0) break;
        out.insert(out.end(), buf, buf + got);
    }
    return out;
}

}  // namespace

// Noise_NNpsk0_25519_ChaChaPoly_BLAKE2b vector from the cacophony test suite (public domain,
// github.com/haskell-cryptography/cacophony, vectors/cacophony.txt).
TEST(Noise, CacophonyVectorNNpsk0Blake2b) {
    const auto prologue = Hex("4a6f686e2047616c74");
    const auto psk = Hex("54686973206973206d7920417573747269616e20706572737065637469766521");
    const auto init_e = Hex("893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a");
    const auto resp_e = Hex("bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b");
    const struct {
        const char* payload;
        const char* ciphertext;
    } messages[] = {
        {"4c756477696720766f6e204d69736573",
         "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c7944ed63df2f5a12aee1185ee9c50305f2ecf12421dcb53c047a63b784cf7c54a105"},
        {"4d757272617920526f746862617264",
         "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f1448088430e0fe7d5c0c9b92f7478716c8852f1b4f389edb75e3ebe546fafafd5b6f0f7"},
        {"462e20412e20486179656b", "0fe04b26dcfbb69f8a6a94c61f1a26ef88769218d32f5d17c068a6"},
        {"4361726c204d656e676572", "2239e0c540c01f09eb1f4cc5258fd5acfefff18a5a3773f21b7bfe"},
        {"4a65616e2d426170746973746520536179", "f216cb03d30fb8af1e561fe02a07552d09b416e27a75e62c77d193718b0bdaae9f"},
        {"457567656e2042f6686d20766f6e2042617765726b", "6e8baf98b944184dd7dc5ef14d8108cbeca626de118a5460f3308e294d61e17238bce29bd9"},
    };

    noise::Handshake init(true, prologue, Key32(psk), init_e);
    noise::Handshake resp(false, prologue, Key32(psk), resp_e);
    std::vector<uint8_t> payload;

    auto m1 = init.Write(Hex(messages[0].payload));
    EXPECT_EQ(m1, Hex(messages[0].ciphertext));
    ASSERT_TRUE(resp.Read(m1, payload));
    EXPECT_EQ(payload, Hex(messages[0].payload));

    auto m2 = resp.Write(Hex(messages[1].payload));
    EXPECT_EQ(m2, Hex(messages[1].ciphertext));
    ASSERT_TRUE(init.Read(m2, payload));
    EXPECT_EQ(payload, Hex(messages[1].payload));

    ASSERT_TRUE(init.Done() && resp.Done());
    EXPECT_EQ(std::vector<uint8_t>(init.HandshakeHash().begin(), init.HandshakeHash().end()),
              Hex("ed5e9692d0ab507b6c2beec3f584fd5b127817a9d20b26cd50aa72c507260fa31aa7d88dd3723316338af37ce0b4cfb2923aeb848bbf2b934911306f01ffc963"));

    noise::CipherState i_send, i_recv, r_send, r_recv;
    init.Split(i_send, i_recv);
    resp.Split(r_send, r_recv);
    for (size_t i = 2; i < std::size(messages); ++i) {
        const bool from_init = i % 2 == 0;
        const auto pt = Hex(messages[i].payload);
        std::vector<uint8_t> ct(pt.size() + noise::kTagSize), back(pt.size());
        (from_init ? i_send : r_send).Encrypt({}, pt, ct.data());
        EXPECT_EQ(ct, Hex(messages[i].ciphertext)) << "message " << i;
        ASSERT_TRUE((from_init ? r_recv : i_recv).Decrypt({}, ct, back.data()));
        EXPECT_EQ(back, pt);
    }
}

TEST(Noise, WrongPskFailsTheHandshake) {
    std::vector<uint8_t> a(32, 1), b(32, 2), payload;
    noise::Handshake init(true, {}, Key32(a));
    noise::Handshake resp(false, {}, Key32(b));
    EXPECT_FALSE(resp.Read(init.Write(), payload));
}

TEST(Noise, PrologueMismatchFailsTheHandshake) {
    std::vector<uint8_t> k(32, 7), payload;
    const uint8_t p1[] = {1}, p2[] = {2};
    noise::Handshake init(true, p1, Key32(k));
    noise::Handshake resp(false, p2, Key32(k));
    EXPECT_FALSE(resp.Read(init.Write(), payload));
}

TEST(Noise, TamperedRecordIsRejected) {
    noise::CipherState a, b;
    std::vector<uint8_t> k(32, 9);
    a.SetKey(Key32(k));
    b.SetKey(Key32(k));
    const std::vector<uint8_t> pt = {1, 2, 3};
    std::vector<uint8_t> ct(pt.size() + noise::kTagSize), out(pt.size());
    a.Encrypt({}, pt, ct.data());
    ct[1] ^= 1;
    EXPECT_FALSE(b.Decrypt({}, ct, out.data()));
}

TEST(WifiLink, PreambleRoundTripsAndRejectsGarbage) {
    wifi::HostId host{};
    for (size_t i = 0; i < host.size(); ++i) host[i] = uint8_t(i * 3);
    const auto p = wifi::Preamble(host);
    EXPECT_EQ(p[0], 'T');
    EXPECT_EQ(p[4], 1);
    ASSERT_TRUE(wifi::ParsePreamble(p));
    EXPECT_EQ(*wifi::ParsePreamble(p), host);
    auto bad = p;
    bad[0] = 'X';
    EXPECT_FALSE(wifi::ParsePreamble(bad));
    bad = p;
    bad[4] = 2;
    EXPECT_FALSE(wifi::ParsePreamble(bad));
    EXPECT_FALSE(wifi::ParsePreamble(std::span<const uint8_t>(p.data(), 10)));

    std::mt19937 rng(5);
    for (int i = 0; i < 2000; ++i) {
        std::vector<uint8_t> junk(wifi::kPreambleSize);
        for (auto& c : junk) c = uint8_t(rng());
        wifi::ParsePreamble(junk);  // must not crash
    }
}

TEST(WifiLink, HandshakeAndLargeMessagesBothWays) {
    auto [client, server] = SocketPair();
    wifi::HostId host{};
    host[0] = 0x42;
    wifi::PairKey key{};
    key[5] = 7;
    auto device = std::async(std::launch::async, [&, s = std::move(server)]() mutable {
        return wifi::AcceptDevice(std::move(s), [&](const wifi::HostId& h) -> std::optional<wifi::PairKey> {
            if (h == host) return key;
            return std::nullopt;
        });
    });
    auto host_link = wifi::ConnectHost(std::move(client), host, key);
    auto device_link = device.get();
    ASSERT_TRUE(host_link && device_link);
    EXPECT_TRUE(host_link->Encrypted());

    // A 4K-IDR-sized message spans many records.
    std::vector<uint8_t> big(1'000'003);
    for (size_t i = 0; i < big.size(); ++i) big[i] = uint8_t(i * 31 + 7);
    std::thread sender([&] { ASSERT_TRUE(device_link->SendAll(big)); });
    EXPECT_EQ(ReadN(*host_link, big.size()), big);
    sender.join();

    const std::vector<uint8_t> small = {9, 8, 7};
    ASSERT_TRUE(host_link->SendAll(small));
    EXPECT_EQ(ReadN(*device_link, small.size()), small);
}

TEST(WifiLink, UnknownHostIsRefused) {
    auto [client, server] = SocketPair();
    wifi::PairKey key{};
    auto device = std::async(std::launch::async, [s = std::move(server)]() mutable {
        return wifi::AcceptDevice(std::move(s), [](const wifi::HostId&) { return std::optional<wifi::PairKey>{}; }, 1000);
    });
    EXPECT_FALSE(wifi::ConnectHost(std::move(client), wifi::HostId{}, key, 1000));
    EXPECT_FALSE(device.get());
}

TEST(WifiLink, WrongKeyIsRefused) {
    auto [client, server] = SocketPair();
    wifi::PairKey phone_key{}, host_key{};
    host_key[0] = 1;
    auto device = std::async(std::launch::async, [&, s = std::move(server)]() mutable {
        return wifi::AcceptDevice(std::move(s), [&](const wifi::HostId&) { return std::optional(phone_key); }, 1000);
    });
    EXPECT_FALSE(wifi::ConnectHost(std::move(client), wifi::HostId{}, host_key, 1000));
    EXPECT_FALSE(device.get());
}

TEST(WifiLink, CorruptRecordEndsTheLink) {
    wifi::PairKey key{};
    // A hand-made host completes the handshake, then sends a record with a bad tag, or a length past
    // the Noise limit.
    for (const uint32_t len : {20u, 70000u}) {
        SCOPED_TRACE(len);
        auto [c2, s2] = SocketPair();
        auto d2 = std::async(std::launch::async, [&, s = std::move(s2)]() mutable {
            return wifi::AcceptDevice(std::move(s), [&](const wifi::HostId&) { return std::optional(key); });
        });
        auto raw = std::move(c2);
        const auto pre = wifi::Preamble({});
        ASSERT_TRUE(raw.SendAll(pre));
        noise::Handshake hs(true, pre, key);
        auto m1 = hs.Write();
        std::vector<uint8_t> framed = {uint8_t(m1.size()), uint8_t(m1.size() >> 8)};
        framed.insert(framed.end(), m1.begin(), m1.end());
        ASSERT_TRUE(raw.SendAll(framed));
        auto dl = d2.get();
        ASSERT_TRUE(dl);
        std::vector<uint8_t> rec = {uint8_t(len), uint8_t(len >> 8), uint8_t(len >> 16), uint8_t(len >> 24)};
        rec.resize(4 + std::min<uint32_t>(len, 64), 0xAA);
        ASSERT_TRUE(raw.SendAll(rec));
        uint8_t buf[64];
        ASSERT_EQ(dl->WaitReadable(3000), 1);
        EXPECT_EQ(dl->RecvSome(buf), -1);
        EXPECT_EQ(dl->WaitReadable(0), -1);
    }
}

TEST(Pairing, SerializeRoundTripsIncludingEmptyFields) {
    wifi::PairingStore a;
    auto& p = a.ForSerial("00008130-001A2B3C4D5E");
    proto::WifiPaired reply;
    reply.device_id[3] = 9;
    reply.name = "Alex's iPhone";
    reply.address = "";
    EXPECT_TRUE(a.Apply(p.serial, reply));
    a.ForSerial("second");  // unconfirmed, all strings empty
    a.SetManual("second", "192.168.1.20:5322");

    wifi::PairingStore b;
    ASSERT_TRUE(b.Parse(a.Serialize()));
    EXPECT_EQ(b.host_id(), a.host_id());
    ASSERT_EQ(b.phones().size(), 2u);
    EXPECT_EQ(b.phones()[0].serial, "00008130-001A2B3C4D5E");
    EXPECT_EQ(b.phones()[0].name, "Alex's iPhone");
    EXPECT_EQ(b.phones()[0].address, "");
    EXPECT_TRUE(b.phones()[0].confirmed);
    EXPECT_EQ(b.phones()[0].key, a.phones()[0].key);
    EXPECT_EQ(b.phones()[0].device_id, reply.device_id);
    EXPECT_FALSE(b.phones()[1].confirmed);
    EXPECT_EQ(b.phones()[1].manual, "192.168.1.20:5322");
    EXPECT_EQ(b.Confirmed().size(), 1u);
}

TEST(Pairing, KeyIsStablePerSerialAndRandomAcrossPhones) {
    wifi::PairingStore s;
    const auto k1 = s.ForSerial("a").key;
    EXPECT_EQ(s.ForSerial("a").key, k1);
    EXPECT_NE(s.ForSerial("b").key, k1);
    EXPECT_NE(k1, wifi::PairKey{});
}

TEST(Pairing, WirelessOffReplyStoresNothing) {
    wifi::PairingStore s;
    proto::WifiPaired off;
    off.status = proto::WifiPairStatus::WirelessOff;
    EXPECT_FALSE(s.Apply("a", off));
    EXPECT_TRUE(s.Confirmed().empty());
}

TEST(Pairing, ParserRejectsGarbage) {
    wifi::PairingStore s;
    EXPECT_FALSE(s.Parse(""));
    EXPECT_FALSE(s.Parse("something else\nhost 00\n"));
    EXPECT_FALSE(s.Parse("tethercam-pairing 1\nhost zz\n"));
    EXPECT_FALSE(s.Parse("tethercam-pairing 1\nphone 61 00 1\n"));
    std::mt19937 rng(11);
    for (int i = 0; i < 500; ++i) {
        std::string junk = "tethercam-pairing 1\n";
        for (int j = 0; j < 80; ++j) junk += char(rng() % 96 + 32);
        s.Parse(junk);  // must not crash
    }
}

TEST(Pairing, SplitHostPort) {
    std::string host;
    uint16_t port = 0;
    ASSERT_TRUE(wifi::SplitHostPort("10.0.0.5", host, port, 5322));
    EXPECT_EQ(host, "10.0.0.5");
    EXPECT_EQ(port, 5322);
    ASSERT_TRUE(wifi::SplitHostPort("phone.local:6000", host, port, 5322));
    EXPECT_EQ(host, "phone.local");
    EXPECT_EQ(port, 6000);
    ASSERT_TRUE(wifi::SplitHostPort("[fe80::1]:7000", host, port, 5322));
    EXPECT_EQ(host, "fe80::1");
    EXPECT_EQ(port, 7000);
    ASSERT_TRUE(wifi::SplitHostPort("fe80::1", host, port, 5322));
    EXPECT_EQ(port, 5322);
    EXPECT_FALSE(wifi::SplitHostPort("", host, port, 5322));
    EXPECT_FALSE(wifi::SplitHostPort("a:0", host, port, 5322));
}

TEST(Pairing, CandidatesOrderAndForced) {
    wifi::PairingStore s;
    proto::WifiPaired reply;
    reply.device_id[0] = 0xAB;
    reply.address = "10.0.0.7";
    reply.port = 5322;
    s.Apply("usb1", reply);
    s.SetManual("usb1", "phone.lan:6000");
    s.ForSerial("unconfirmed");
    wifi::Discovered d;
    d.id_hex = wifi::Hex(reply.device_id);
    d.host = "fe80::5";
    d.port = 5322;
    wifi::Discovered other = d;
    other.id_hex = "00";
    const wifi::Discovered found[] = {d, other};

    auto c = wifi::Candidates(s, found);
    ASSERT_EQ(c.size(), 3u);
    EXPECT_EQ(c[0].host, "phone.lan");
    EXPECT_EQ(c[0].port, 6000);
    EXPECT_EQ(c[1].host, "fe80::5");
    EXPECT_EQ(c[2].host, "10.0.0.7");
    EXPECT_EQ(c[2].phone.serial, "usb1");

    c = wifi::Candidates(s, found, "127.0.0.1:7777");
    ASSERT_EQ(c.size(), 1u);
    EXPECT_EQ(c[0].port, 7777);

    wifi::PairingStore empty;
    EXPECT_TRUE(wifi::Candidates(empty, found, "127.0.0.1").empty());
}
