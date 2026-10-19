#include "tethercam/link.h"

#include <algorithm>
#include <cstring>

namespace tethercam {
namespace {

void PutU32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint32_t GetU32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

bool RecvExactWithin(net::Socket& s, std::span<uint8_t> data, int timeout_ms) {
    if (s.WaitReadable(timeout_ms) <= 0) return false;
    return s.RecvExact(data);
}

// Handshake messages travel as u16 LE length + bytes.
bool SendHandshake(net::Socket& s, std::span<const uint8_t> msg) {
    std::vector<uint8_t> out(2 + msg.size());
    out[0] = static_cast<uint8_t>(msg.size());
    out[1] = static_cast<uint8_t>(msg.size() >> 8);
    std::memcpy(out.data() + 2, msg.data(), msg.size());
    return s.SendAll(out);
}

bool RecvHandshake(net::Socket& s, std::vector<uint8_t>& msg, int timeout_ms) {
    uint8_t len[2];
    if (!RecvExactWithin(s, len, timeout_ms)) return false;
    msg.resize(size_t(len[0]) | size_t(len[1]) << 8);
    return msg.empty() || RecvExactWithin(s, msg, timeout_ms);
}

}  // namespace

bool SecureLink::SendAll(std::span<const uint8_t> data) {
    if (failed_) return false;
    // One buffer and one send for the whole message, however many records it takes.
    const size_t records = std::max<size_t>(1, (data.size() + noise::kMaxPlaintext - 1) / noise::kMaxPlaintext);
    out_.resize(data.size() + records * (4 + noise::kTagSize));
    uint8_t* p = out_.data();
    size_t off = 0;
    do {
        const size_t n = std::min(noise::kMaxPlaintext, data.size() - off);
        PutU32(p, static_cast<uint32_t>(n + noise::kTagSize));
        send_.Encrypt({}, data.subspan(off, n), p + 4);
        p += 4 + n + noise::kTagSize;
        off += n;
    } while (off < data.size());
    return s_.SendAll(out_);
}

int SecureLink::WaitReadable(int timeout_ms) {
    if (failed_) return -1;
    if (plain_pos_ < plain_.size()) return 1;
    return s_.WaitReadable(timeout_ms);
}

int SecureLink::RecvSome(std::span<uint8_t> data) {
    if (failed_) return -1;
    while (plain_pos_ >= plain_.size()) {  // an empty record carries nothing: read the next one
        uint8_t hdr[4];
        if (!s_.RecvExact(hdr)) return 0;
        const uint32_t len = GetU32(hdr);
        if (len < noise::kTagSize || len > noise::kMaxMessage) {
            failed_ = true;
            return -1;
        }
        record_.resize(len);
        if (!s_.RecvExact(record_)) return 0;
        plain_.resize(len - noise::kTagSize);
        plain_pos_ = 0;
        if (!recv_.Decrypt({}, record_, plain_.data())) {
            failed_ = true;
            plain_.clear();
            return -1;
        }
    }
    const size_t n = std::min(data.size(), plain_.size() - plain_pos_);
    std::memcpy(data.data(), plain_.data() + plain_pos_, n);
    plain_pos_ += n;
    return static_cast<int>(n);
}

namespace wifi {

std::array<uint8_t, kPreambleSize> Preamble(const HostId& host) {
    std::array<uint8_t, kPreambleSize> p{};
    std::memcpy(p.data(), kPreambleMagic, 4);
    p[4] = kPreambleVersion;
    std::memcpy(p.data() + 8, host.data(), host.size());
    return p;
}

std::optional<HostId> ParsePreamble(std::span<const uint8_t> bytes) {
    if (bytes.size() != kPreambleSize || std::memcmp(bytes.data(), kPreambleMagic, 4) != 0) return std::nullopt;
    if (bytes[4] != kPreambleVersion) return std::nullopt;
    HostId host;
    std::memcpy(host.data(), bytes.data() + 8, host.size());
    return host;
}

std::unique_ptr<Link> ConnectHost(net::Socket sock, const HostId& host, const PairKey& key, int timeout_ms) {
    const auto preamble = Preamble(host);
    if (!sock.SendAll(preamble)) return nullptr;
    noise::Handshake hs(true, preamble, key);
    if (!SendHandshake(sock, hs.Write())) return nullptr;
    std::vector<uint8_t> msg, payload;
    if (!RecvHandshake(sock, msg, timeout_ms) || !hs.Read(msg, payload) || !hs.Done()) return nullptr;
    noise::CipherState send, recv;
    hs.Split(send, recv);
    return std::make_unique<SecureLink>(std::move(sock), std::move(send), std::move(recv));
}

std::unique_ptr<Link> AcceptDevice(net::Socket sock, const KeyLookup& lookup, int timeout_ms) {
    uint8_t preamble[kPreambleSize];
    if (!RecvExactWithin(sock, preamble, timeout_ms)) return nullptr;
    const auto host = ParsePreamble(preamble);
    if (!host) return nullptr;
    const auto key = lookup(*host);
    if (!key) return nullptr;
    noise::Handshake hs(false, preamble, *key);
    std::vector<uint8_t> msg, payload;
    if (!RecvHandshake(sock, msg, timeout_ms) || !hs.Read(msg, payload)) return nullptr;
    if (!SendHandshake(sock, hs.Write()) || !hs.Done()) return nullptr;
    noise::CipherState send, recv;
    hs.Split(send, recv);
    return std::make_unique<SecureLink>(std::move(sock), std::move(send), std::move(recv));
}

}  // namespace wifi
}  // namespace tethercam
