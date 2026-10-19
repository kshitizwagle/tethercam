#pragma once

// The host's Wi-Fi pairings (M11): one random key per phone, handed over USB in WifiPair and
// confirmed by the phone's WifiPaired. Stored per user: DPAPI-protected on Windows
// (%LOCALAPPDATA%\Tethercam\pairing.bin), mode 0600 elsewhere ($XDG_CONFIG_HOME/tethercam/pairing).

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "tethercam/discovery.h"
#include "tethercam/link.h"
#include "tethercam/protocol.h"

namespace tethercam::wifi {

struct PairedPhone {
    std::string serial;       // USB serial (usbmuxd), or "tcp:<host:port>" for fakephone
    PairKey key{};
    bool confirmed = false;   // the phone stored the key (WifiPaired status 0)
    DeviceId device_id{};
    std::string name;
    std::string address;      // Wi-Fi address the phone last reported over USB
    uint16_t port = proto::kWifiPort;
    std::string manual;       // user-entered "host[:port]" for networks that block discovery
};

class PairingStore {
public:
    // An empty path keeps everything in memory (tests, --tcp sessions with --no-persist).
    explicit PairingStore(std::string path = {});
    static std::string DefaultPath();

    // Reads the file, creating a host ID if there is none yet. A missing or unreadable file
    // starts empty. Returns false only if the file exists but couldn't be read.
    bool Load();
    bool Save() const;

    const HostId& host_id() const { return host_id_; }
    std::vector<PairedPhone>& phones() { return phones_; }
    const std::vector<PairedPhone>& phones() const { return phones_; }

    // The phone record for a USB serial, created with a fresh random key if it's new.
    PairedPhone& ForSerial(const std::string& serial);
    // Confirmed phones, most recently confirmed first.
    std::vector<const PairedPhone*> Confirmed() const;
    // Records the phone's reply. Returns true if anything changed.
    bool Apply(const std::string& serial, const proto::WifiPaired& reply);
    void ForgetAll();
    void SetManual(const std::string& serial, const std::string& manual);

    std::string Serialize() const;
    bool Parse(const std::string& text);

private:
    std::string path_;
    HostId host_id_{};
    std::vector<PairedPhone> phones_;
};

// One address to try for a paired phone over Wi-Fi.
struct Candidate {
    PairedPhone phone;
    std::string host;
    uint16_t port = proto::kWifiPort;
};

// Addresses to try, in order, for every confirmed phone: the user's manual address, the phone's
// Bonjour advert, then the address it last reported over USB (duplicates removed). `forced`
// ("host[:port]", --wifi) replaces them all and uses the most recently paired phone.
std::vector<Candidate> Candidates(const PairingStore& store, std::span<const Discovered> found,
                                  const std::string& forced = {});

// This computer's name, sent to the phone in WifiPair.
std::string LocalHostName();

std::string Hex(std::span<const uint8_t> bytes);
// Splits "host[:port]" (IPv6 in brackets); port defaults to `default_port`.
bool SplitHostPort(const std::string& s, std::string& host, uint16_t& port, uint16_t default_port);

}  // namespace tethercam::wifi
