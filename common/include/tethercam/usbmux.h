#pragma once

// Client for Apple's usbmuxd (Apple Mobile Device Service), which tunnels TCP
// connections to an iPhone over USB. See docs/TECH_SPEC.md §4.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "tethercam/net.h"
#include "tethercam/plist.h"

namespace tethercam::usbmux {

inline constexpr uint16_t kDefaultPort = 27015;
inline constexpr uint32_t kHeaderSize = 16;
inline constexpr uint32_t kProtocolPlist = 1;
inline constexpr uint32_t kMessagePlist = 8;
inline constexpr uint32_t kMaxPacket = 1u << 20;

struct Device {
    uint32_t device_id = 0;
    std::string serial;
    std::string connection_type;  // "USB" or "Network"
    uint32_t product_id = 0;
};

enum class ConnectResult : int {
    Ok = 0,
    BadCommand = 1,
    BadDevice = 2,
    ConnectionRefused = 3,
    BadVersion = 6,
    ServiceUnavailable = -1,  // could not talk to usbmuxd at all
    ProtocolError = -2,
};

struct Event {
    enum class Kind { Attached, Detached, Other } kind = Kind::Other;
    Device device;  // for Detached only device_id is set
};

// Port numbers in Connect are sent byte-swapped (network order read as little-endian).
constexpr int64_t SwapPort(uint16_t port) { return int64_t(((port & 0xFF) << 8) | (port >> 8)); }

// Packet framing, exposed for tests.
std::vector<uint8_t> EncodePacket(const plist::Value& body, uint32_t tag);
std::optional<plist::Value> DecodeBody(std::span<const uint8_t> payload);
std::optional<std::vector<Device>> ParseDeviceList(const plist::Value& reply);
std::optional<Event> ParseEvent(const plist::Value& msg);

// Where usbmuxd listens. Either a Unix socket path or a TCP host:port.
struct ServiceAddr {
    std::string unix_path;
    std::string host;
    uint16_t port = 0;
};
inline constexpr const char* kDefaultUnixPath = "/var/run/usbmuxd";
// Parses a USBMUXD_SOCKET_ADDRESS value ("host:port" or "UNIX:/path", as libimobiledevice does).
std::optional<ServiceAddr> ParseServiceAddress(std::string_view value);
// USBMUXD_SOCKET_ADDRESS if set and valid, else 127.0.0.1:27015 on Windows and /var/run/usbmuxd elsewhere.
ServiceAddr ServiceAddress();

class Client {
public:
    // Lists attached devices. nullopt if usbmuxd is unreachable.
    static std::optional<std::vector<Device>> ListDevices();

    // Opens a tunnel to `port` on the device. On Ok, `out` is a raw byte stream to the device.
    static ConnectResult Connect(uint32_t device_id, uint16_t port, net::Socket& out);

    // Starts a Listen session; call NextEvent repeatedly on the same object.
    bool Listen();
    // Blocks up to timeout_ms. nullopt on timeout; sets `closed` when the connection dropped.
    std::optional<Event> NextEvent(int timeout_ms, bool& closed);
    void Close() { sock_.Shutdown(); }

private:
    net::Socket sock_;
};

const char* ToString(ConnectResult r);

}  // namespace tethercam::usbmux
