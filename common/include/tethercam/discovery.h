#pragma once

// Bonjour discovery of phones advertising the Wi-Fi transport (_tethercam._tcp, TXT id=<device ID hex>).
// The interface is shared; each host implements StartDiscovery() with its OS resolver
// (windows/agent/discovery.cpp: DnsServiceBrowse; linux/agent/discovery.cpp: Avahi).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tethercam::wifi {

inline constexpr char kServiceType[] = "_tethercam._tcp";

struct Discovered {
    std::string id_hex;  // the phone's device ID (TXT "id"), lowercase hex
    std::string name;    // instance name
    std::string host;    // resolved address
    uint16_t port = 0;
};

class Discovery {
public:
    virtual ~Discovery() = default;
    // Phones currently advertised on the local network. Thread-safe.
    virtual std::vector<Discovered> Found() const = 0;
};

// Starts browsing in the background; nullptr if the OS resolver isn't available.
std::unique_ptr<Discovery> StartDiscovery();

}  // namespace tethercam::wifi
