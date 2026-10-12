#include "tethercam/usbmux.h"

#include <cstdlib>
#include <string_view>

#include "tethercam/bytes.h"
#include "tethercam/version.h"

namespace tethercam::usbmux {
namespace {

plist::Dict BaseRequest(const char* type) {
    plist::Dict d;
    d["MessageType"] = type;
    d["ClientVersionString"] = "tethercam-" + std::string(Version());
    d["ProgName"] = "tethercam";
    d["kLibUSBMuxVersion"] = 3;
    return d;
}

std::optional<plist::Value> ReadPacket(net::Socket& s) {
    uint8_t hdr[kHeaderSize];
    if (!s.RecvExact(hdr)) return std::nullopt;
    ByteReader r(hdr);
    const uint32_t len = r.U32();
    if (len < kHeaderSize || len > kMaxPacket) return std::nullopt;
    std::vector<uint8_t> body(len - kHeaderSize);
    if (!body.empty() && !s.RecvExact(body)) return std::nullopt;
    return DecodeBody(body);
}

net::Socket OpenService() {
    auto [host, port] = ServiceAddress();
    return net::ConnectTcp(host, port, 2000);
}

Device ParseDevice(const plist::Value& entry) {
    Device d;
    const plist::Value* props = entry.Get("Properties");
    const plist::Value& src = props ? *props : entry;
    if (auto v = entry.Get("DeviceID"); v && v->AsInt()) d.device_id = static_cast<uint32_t>(*v->AsInt());
    if (auto v = src.Get("DeviceID"); v && v->AsInt()) d.device_id = static_cast<uint32_t>(*v->AsInt());
    if (auto v = src.Get("SerialNumber"); v && v->AsString()) d.serial = *v->AsString();
    if (auto v = src.Get("ConnectionType"); v && v->AsString()) d.connection_type = *v->AsString();
    if (auto v = src.Get("ProductID"); v && v->AsInt()) d.product_id = static_cast<uint32_t>(*v->AsInt());
    return d;
}

}  // namespace

std::vector<uint8_t> EncodePacket(const plist::Value& body, uint32_t tag) {
    const std::string xml = plist::ToXml(body);
    ByteWriter w;
    w.U32(static_cast<uint32_t>(kHeaderSize + xml.size()));
    w.U32(kProtocolPlist);
    w.U32(kMessagePlist);
    w.U32(tag);
    w.Bytes({reinterpret_cast<const uint8_t*>(xml.data()), xml.size()});
    return w.Take();
}

std::optional<plist::Value> DecodeBody(std::span<const uint8_t> payload) {
    return plist::FromXml(std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
}

std::optional<std::vector<Device>> ParseDeviceList(const plist::Value& reply) {
    auto list = reply.Get("DeviceList");
    if (!list || !list->AsArray()) return std::nullopt;
    std::vector<Device> out;
    for (const auto& e : *list->AsArray()) out.push_back(ParseDevice(e));
    return out;
}

std::optional<Event> ParseEvent(const plist::Value& msg) {
    auto type = msg.Get("MessageType");
    if (!type || !type->AsString()) return std::nullopt;
    Event ev;
    if (*type->AsString() == "Attached") {
        ev.kind = Event::Kind::Attached;
        ev.device = ParseDevice(msg);
    } else if (*type->AsString() == "Detached") {
        ev.kind = Event::Kind::Detached;
        if (auto id = msg.Get("DeviceID"); id && id->AsInt()) ev.device.device_id = static_cast<uint32_t>(*id->AsInt());
    }
    return ev;
}

std::pair<std::string, uint16_t> ServiceAddress() {
    char* env = nullptr;
    size_t len = 0;
    std::pair<std::string, uint16_t> result{"127.0.0.1", kDefaultPort};
    if (_dupenv_s(&env, &len, "USBMUXD_SOCKET_ADDRESS") == 0 && env) {
        std::string_view v(env);
        auto colon = v.rfind(':');
        if (colon != std::string_view::npos) {
            result.first = std::string(v.substr(0, colon));
            result.second = static_cast<uint16_t>(std::atoi(std::string(v.substr(colon + 1)).c_str()));
        }
        free(env);
    }
    return result;
}

std::optional<std::vector<Device>> Client::ListDevices() {
    auto s = OpenService();
    if (!s.Valid()) return std::nullopt;
    if (!s.SendAll(EncodePacket(plist::Value(BaseRequest("ListDevices")), 1))) return std::nullopt;
    auto reply = ReadPacket(s);
    if (!reply) return std::nullopt;
    return ParseDeviceList(*reply);
}

ConnectResult Client::Connect(uint32_t device_id, uint16_t port, net::Socket& out) {
    auto s = OpenService();
    if (!s.Valid()) return ConnectResult::ServiceUnavailable;
    auto req = BaseRequest("Connect");
    req["DeviceID"] = int64_t(device_id);
    req["PortNumber"] = SwapPort(port);
    if (!s.SendAll(EncodePacket(plist::Value(std::move(req)), 2))) return ConnectResult::ServiceUnavailable;
    auto reply = ReadPacket(s);
    if (!reply) return ConnectResult::ProtocolError;
    auto num = reply->Get("Number");
    if (!num || !num->AsInt()) return ConnectResult::ProtocolError;
    const auto r = static_cast<ConnectResult>(*num->AsInt());
    if (r == ConnectResult::Ok) {
        s.SetNoDelay();
        s.SetRecvBuffer(4 << 20);
        out = std::move(s);
    }
    return r;
}

bool Client::Listen() {
    sock_ = OpenService();
    if (!sock_.Valid()) return false;
    if (!sock_.SendAll(EncodePacket(plist::Value(BaseRequest("Listen")), 3))) return false;
    auto reply = ReadPacket(sock_);
    if (!reply) return false;
    auto num = reply->Get("Number");
    return num && num->AsInt() && *num->AsInt() == 0;
}

std::optional<Event> Client::NextEvent(int timeout_ms, bool& closed) {
    closed = false;
    const int w = sock_.WaitReadable(timeout_ms);
    if (w == 0) return std::nullopt;
    if (w < 0) {
        closed = true;
        return std::nullopt;
    }
    auto msg = ReadPacket(sock_);
    if (!msg) {
        closed = true;
        return std::nullopt;
    }
    return ParseEvent(*msg);
}

const char* ToString(ConnectResult r) {
    switch (r) {
    case ConnectResult::Ok: return "ok";
    case ConnectResult::BadCommand: return "bad command";
    case ConnectResult::BadDevice: return "bad device";
    case ConnectResult::ConnectionRefused: return "connection refused (is the Tethercam app open on the iPhone?)";
    case ConnectResult::BadVersion: return "bad version";
    case ConnectResult::ServiceUnavailable: return "usbmuxd unavailable (install Apple Devices or iTunes)";
    case ConnectResult::ProtocolError: return "usbmuxd protocol error";
    }
    return "unknown";
}

}  // namespace tethercam::usbmux
