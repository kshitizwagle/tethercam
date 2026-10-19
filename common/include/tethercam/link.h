#pragma once

// The byte stream a protocol session runs over: a plain socket (USB through usbmuxd, or --tcp) or an
// encrypted Wi-Fi connection (docs/PROTOCOL.md § Wireless transport). Sessions only see this interface.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "tethercam/net.h"
#include "tethercam/noise.h"

namespace tethercam {

class Link {
public:
    virtual ~Link() = default;
    virtual bool SendAll(std::span<const uint8_t> data) = 0;
    // Reads up to data.size() bytes. Returns bytes read, 0 on EOF, -1 on error.
    virtual int RecvSome(std::span<uint8_t> data) = 0;
    // Returns 1 readable, 0 timeout, -1 error.
    virtual int WaitReadable(int timeout_ms) = 0;
    // Wakes up a thread blocked in RecvSome or WaitReadable. Safe from another thread.
    virtual void Shutdown() = 0;
    virtual bool Encrypted() const = 0;
};

class PlainLink final : public Link {
public:
    explicit PlainLink(net::Socket s) : s_(std::move(s)) {}
    bool SendAll(std::span<const uint8_t> data) override { return s_.SendAll(data); }
    int RecvSome(std::span<uint8_t> data) override { return s_.RecvSome(data); }
    int WaitReadable(int timeout_ms) override { return s_.WaitReadable(timeout_ms); }
    void Shutdown() override { s_.Shutdown(); }
    bool Encrypted() const override { return false; }

private:
    net::Socket s_;
};

// Noise transport records: u32 LE length, then ChaCha20-Poly1305 ciphertext of at most
// noise::kMaxPlaintext bytes plus the tag. A record that fails authentication ends the link.
class SecureLink final : public Link {
public:
    SecureLink(net::Socket s, noise::CipherState send, noise::CipherState recv)
        : s_(std::move(s)), send_(std::move(send)), recv_(std::move(recv)) {}
    bool SendAll(std::span<const uint8_t> data) override;
    int RecvSome(std::span<uint8_t> data) override;
    int WaitReadable(int timeout_ms) override;
    void Shutdown() override { s_.Shutdown(); }
    bool Encrypted() const override { return true; }

private:
    net::Socket s_;
    noise::CipherState send_, recv_;
    std::vector<uint8_t> out_, record_, plain_;
    size_t plain_pos_ = 0;
    bool failed_ = false;
};

namespace wifi {

using HostId = std::array<uint8_t, 16>;
using DeviceId = std::array<uint8_t, 16>;
using PairKey = noise::Key;

inline constexpr size_t kPreambleSize = 24;
inline constexpr uint8_t kPreambleMagic[4] = {'T', 'C', 'W', 'F'};
inline constexpr uint8_t kPreambleVersion = 1;

std::array<uint8_t, kPreambleSize> Preamble(const HostId& host);
// The host ID of a valid preamble; nullopt for bad magic or an unknown version.
std::optional<HostId> ParsePreamble(std::span<const uint8_t> bytes);

// Host side: sends the preamble and runs the Noise handshake as initiator on a connected socket.
// nullptr if the phone doesn't answer in time or the handshake fails (wrong or forgotten key).
std::unique_ptr<Link> ConnectHost(net::Socket sock, const HostId& host, const PairKey& key, int timeout_ms = 3000);

// Device side (fakephone, tests; the iOS app has its own): reads the preamble, looks up the host's
// key, and answers the handshake. nullptr on any failure.
using KeyLookup = std::function<std::optional<PairKey>(const HostId&)>;
std::unique_ptr<Link> AcceptDevice(net::Socket sock, const KeyLookup& lookup, int timeout_ms = 3000);

}  // namespace wifi
}  // namespace tethercam
