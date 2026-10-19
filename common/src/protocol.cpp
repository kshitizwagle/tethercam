#include "tethercam/protocol.h"

#include <cstring>

#include "tethercam/bytes.h"

namespace tethercam::proto {
namespace {

template <class... Ts> struct Overloaded : Ts... {
    using Ts::operator()...;
};

uint8_t TypeOf(const Message& msg) {
    return std::visit(Overloaded{
                          [](const Hello&) { return uint8_t(MsgType::Hello); },
                          [](const Caps&) { return uint8_t(MsgType::Caps); },
                          [](const SetCamera&) { return uint8_t(MsgType::SetCamera); },
                          [](const SetFormat&) { return uint8_t(MsgType::SetFormat); },
                          [](const Start&) { return uint8_t(MsgType::Start); },
                          [](const Stop&) { return uint8_t(MsgType::Stop); },
                          [](const RequestKeyframe&) { return uint8_t(MsgType::RequestKeyframe); },
                          [](const SetOptions&) { return uint8_t(MsgType::SetOptions); },
                          [](const SetQuality&) { return uint8_t(MsgType::SetQuality); },
                          [](const VideoConfig&) { return uint8_t(MsgType::VideoConfig); },
                          [](const VideoFrame&) { return uint8_t(MsgType::VideoFrame); },
                          [](const Heartbeat&) { return uint8_t(MsgType::Heartbeat); },
                          [](const Status&) { return uint8_t(MsgType::Status); },
                          [](const Error&) { return uint8_t(MsgType::Error); },
                          [](const WifiPair&) { return uint8_t(MsgType::WifiPair); },
                          [](const WifiPaired&) { return uint8_t(MsgType::WifiPaired); },
                          [](const Unknown& u) { return u.type; },
                      },
                      msg);
}

void WritePayload(ByteWriter& w, const Message& msg) {
    std::visit(Overloaded{
                   [&](const Hello& m) {
                       w.U8(m.min_version);
                       w.U8(m.max_version);
                       w.U8(uint8_t(m.role));
                       w.Str(m.app_version);
                       w.Str(m.name);
                       w.Str(m.model);
                   },
                   [&](const Caps& m) {
                       w.U8(static_cast<uint8_t>(m.cameras.size()));
                       for (const auto& c : m.cameras) {
                           w.U8(c.camera_id);
                           w.U8(uint8_t(c.position));
                           w.U8(uint8_t(c.lens));
                           w.Str(c.label);
                           w.U16(static_cast<uint16_t>(c.formats.size()));
                           for (const auto& f : c.formats) {
                               w.U16(f.width);
                               w.U16(f.height);
                               w.U16(f.max_fps);
                           }
                       }
                       w.U8(m.codecs);
                   },
                   [&](const SetCamera& m) { w.U8(m.camera_id); },
                   [&](const SetQuality& m) { w.U8(uint8_t(m.quality)); },
                   [&](const SetFormat& m) {
                       w.U16(m.width);
                       w.U16(m.height);
                       w.U16(m.fps);
                       w.U8(uint8_t(m.codec));
                       w.U32(m.bitrate_kbps);
                   },
                   [&](const Start&) {},
                   [&](const Stop&) {},
                   [&](const RequestKeyframe&) {},
                   [&](const SetOptions& m) {
                       w.U8(m.mirror);
                       w.U16(m.rotation);
                       w.U8(m.dim_preview);
                   },
                   [&](const VideoConfig& m) {
                       w.U8(uint8_t(m.codec));
                       w.U16(m.width);
                       w.U16(m.height);
                       w.U16(m.fps);
                       w.U32(static_cast<uint32_t>(m.parameter_sets.size()));
                       w.Bytes(m.parameter_sets);
                   },
                   [&](const VideoFrame& m) {
                       w.U64(m.pts_us);
                       w.U8(m.frame_flags);
                       w.Bytes(m.data);
                   },
                   [&](const Heartbeat& m) { w.U64(m.send_time_us); },
                   [&](const Status& m) {
                       w.U8(m.thermal_state);
                       w.U8(m.battery_percent);
                       w.U8(m.charging);
                       w.U32(m.actual_bitrate_kbps);
                       w.U16(m.actual_fps_x100);
                       w.U8(m.camera_id);
                   },
                   [&](const Error& m) {
                       w.U16(uint16_t(m.code));
                       w.Str(m.message);
                   },
                   [&](const WifiPair& m) {
                       w.Bytes(m.host_id);
                       w.Bytes(m.key);
                       w.Str(m.name);
                   },
                   [&](const WifiPaired& m) {
                       w.U8(uint8_t(m.status));
                       w.Bytes(m.device_id);
                       w.Str(m.name);
                       w.U16(m.port);
                       w.Str(m.address);
                   },
                   [&](const Unknown&) {},
               },
               msg);
}

}  // namespace

std::vector<uint8_t> Encode(const Message& msg) {
    ByteWriter w;
    w.Bytes(kMagic);
    w.U8(kVersion);
    w.U8(TypeOf(msg));
    w.U16(0);
    w.U32(0);  // length, patched below
    WritePayload(w, msg);
    auto& d = w.Data();
    const uint32_t len = static_cast<uint32_t>(d.size() - kHeaderSize);
    for (int i = 0; i < 4; ++i) d[8 + i] = static_cast<uint8_t>(len >> (8 * i));
    return w.Take();
}

std::optional<Message> DecodePayload(uint8_t type, std::span<const uint8_t> payload) {
    ByteReader r(payload);
    Message out;
    switch (static_cast<MsgType>(type)) {
    case MsgType::Hello: {
        Hello m;
        m.min_version = r.U8();
        m.max_version = r.U8();
        m.role = Role(r.U8());
        m.app_version = r.Str();
        m.name = r.Str();
        m.model = r.Str();
        out = std::move(m);
        break;
    }
    case MsgType::Caps: {
        Caps m;
        const uint8_t n = r.U8();
        for (uint8_t i = 0; i < n && !r.Failed(); ++i) {
            Camera c;
            c.camera_id = r.U8();
            c.position = Position(r.U8());
            c.lens = Lens(r.U8());
            c.label = r.Str();
            const uint16_t nf = r.U16();
            // Each format is 6 bytes. Reject counts the payload cannot hold before allocating.
            if (static_cast<size_t>(nf) * 6 > r.Remaining()) return std::nullopt;
            for (uint16_t j = 0; j < nf; ++j) {
                Format f;
                f.width = r.U16();
                f.height = r.U16();
                f.max_fps = r.U16();
                c.formats.push_back(f);
            }
            m.cameras.push_back(std::move(c));
        }
        m.codecs = r.U8();
        out = std::move(m);
        break;
    }
    case MsgType::SetCamera:
        out = SetCamera{r.U8()};
        break;
    case MsgType::SetQuality: {
        const auto quality = r.U8();
        if (r.Failed()) return std::nullopt;
        // A preset this build doesn't know (from a newer peer) is ignored, not fatal.
        if (quality > uint8_t(Quality::UHD60)) return Unknown{type};
        out = SetQuality{Quality(quality)};
        break;
    }
    case MsgType::SetFormat: {
        SetFormat m;
        m.width = r.U16();
        m.height = r.U16();
        m.fps = r.U16();
        m.codec = Codec(r.U8());
        m.bitrate_kbps = r.U32();
        out = m;
        break;
    }
    case MsgType::Start:
        out = Start{};
        break;
    case MsgType::Stop:
        out = Stop{};
        break;
    case MsgType::RequestKeyframe:
        out = RequestKeyframe{};
        break;
    case MsgType::SetOptions: {
        SetOptions m;
        m.mirror = r.U8();
        m.rotation = r.U16();
        m.dim_preview = r.U8();
        out = m;
        break;
    }
    case MsgType::VideoConfig: {
        VideoConfig m;
        m.codec = Codec(r.U8());
        m.width = r.U16();
        m.height = r.U16();
        m.fps = r.U16();
        const uint32_t n = r.U32();
        auto ps = r.Bytes(n);
        m.parameter_sets.assign(ps.begin(), ps.end());
        out = std::move(m);
        break;
    }
    case MsgType::VideoFrame: {
        VideoFrame m;
        m.pts_us = r.U64();
        m.frame_flags = r.U8();
        auto d = r.Rest();
        m.data.assign(d.begin(), d.end());
        out = std::move(m);
        break;
    }
    case MsgType::Heartbeat:
        out = Heartbeat{r.U64()};
        break;
    case MsgType::Status: {
        Status m;
        m.thermal_state = r.U8();
        m.battery_percent = r.U8();
        m.charging = r.U8();
        m.actual_bitrate_kbps = r.U32();
        m.actual_fps_x100 = r.U16();
        if (r.Remaining() >= 1) m.camera_id = r.U8();  // optional trailing field
        out = m;
        break;
    }
    case MsgType::Error: {
        Error m;
        m.code = ErrorCode(r.U16());
        m.message = r.Str();
        out = std::move(m);
        break;
    }
    case MsgType::WifiPair: {
        WifiPair m;
        auto id = r.Bytes(m.host_id.size());
        if (!id.empty()) std::memcpy(m.host_id.data(), id.data(), id.size());
        auto key = r.Bytes(m.key.size());
        if (!key.empty()) std::memcpy(m.key.data(), key.data(), key.size());
        m.name = r.Str();
        out = std::move(m);
        break;
    }
    case MsgType::WifiPaired: {
        WifiPaired m;
        m.status = WifiPairStatus(r.U8());
        auto id = r.Bytes(m.device_id.size());
        if (!id.empty()) std::memcpy(m.device_id.data(), id.data(), id.size());
        m.name = r.Str();
        m.port = r.U16();
        m.address = r.Str();
        out = std::move(m);
        break;
    }
    default:
        return Unknown{type};
    }
    if (r.Failed()) return std::nullopt;
    return out;
}

void StreamParser::Feed(std::span<const uint8_t> data) {
    // Compact once the consumed prefix dominates, so the buffer does not grow forever.
    if (pos_ > 0 && pos_ >= buf_.size() / 2) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(pos_));
        pos_ = 0;
    }
    buf_.insert(buf_.end(), data.begin(), data.end());
}

StreamParser::Result StreamParser::Next(Message& out) {
    if (Buffered() < kHeaderSize) return Result::NeedMore;
    const uint8_t* h = buf_.data() + pos_;
    if (std::memcmp(h, kMagic, 4) != 0) return Result::Fatal;
    const uint8_t type = h[5];
    const uint32_t len = uint32_t(h[8]) | uint32_t(h[9]) << 8 | uint32_t(h[10]) << 16 | uint32_t(h[11]) << 24;
    if (len > kMaxPayload) return Result::Fatal;
    if (Buffered() < kHeaderSize + len) return Result::NeedMore;
    auto payload = std::span<const uint8_t>(buf_.data() + pos_ + kHeaderSize, len);
    auto msg = DecodePayload(type, payload);
    pos_ += kHeaderSize + len;
    if (!msg) return Result::Fatal;
    out = std::move(*msg);
    return Result::Message;
}

const char* TypeName(const Message& msg) {
    static constexpr const char* kNames[] = {
        "Hello",       "Caps",       "SetCamera", "SetFormat", "Start", "Stop",  "RequestKeyframe",
        "SetOptions", "SetQuality", "VideoConfig", "VideoFrame", "Heartbeat", "Status", "Error", "WifiPair",
        "WifiPaired", "Unknown"};
    return kNames[msg.index()];
}

}  // namespace tethercam::proto
