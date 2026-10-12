#pragma once

// Tethercam wire protocol v1. The byte layout is specified in docs/PROTOCOL.md;
// keep the two in sync (golden vectors live in docs/protocol-vectors/).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace tethercam::proto {

inline constexpr uint8_t kMagic[4] = {'T', 'C', 'A', 'M'};
inline constexpr uint8_t kVersion = 1;
inline constexpr size_t kHeaderSize = 12;
inline constexpr uint32_t kMaxPayload = 16u * 1024 * 1024;
inline constexpr uint16_t kDevicePort = 5321;

enum class MsgType : uint8_t {
    Hello = 0x01,
    Caps = 0x02,
    SetCamera = 0x10,
    SetFormat = 0x11,
    Start = 0x12,
    Stop = 0x13,
    RequestKeyframe = 0x14,
    SetOptions = 0x15,
    VideoConfig = 0x20,
    VideoFrame = 0x21,
    Heartbeat = 0x30,
    Status = 0x31,
    Error = 0x7F,
};

enum class Role : uint8_t { Host = 0, Device = 1 };
enum class Codec : uint8_t { H264 = 0, Hevc = 1 };
enum class Position : uint8_t { Back = 0, Front = 1 };
enum class Lens : uint8_t { Wide = 0, UltraWide = 1, Telephoto = 2, Front = 3 };

enum class ErrorCode : uint16_t {
    UnsupportedVersion = 1,
    CameraPermissionDenied = 2,
    InvalidFormat = 3,
    CameraUnavailable = 4,
    EncoderFailure = 5,
    ThermalShutdown = 6,
};

struct Hello {
    uint8_t min_version = kVersion;
    uint8_t max_version = kVersion;
    Role role = Role::Host;
    std::string app_version;
    std::string name;
    std::string model;
};

struct Format {
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t max_fps = 0;
    bool operator==(const Format&) const = default;
};

struct Camera {
    uint8_t camera_id = 0;
    Position position = Position::Back;
    Lens lens = Lens::Wide;
    std::string label;
    std::vector<Format> formats;
};

struct Caps {
    std::vector<Camera> cameras;
    uint8_t codecs = 1;  // bit0 H.264, bit1 HEVC
};

struct SetCamera {
    uint8_t camera_id = 0;
};

struct SetFormat {
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t fps = 0;
    Codec codec = Codec::H264;
    uint32_t bitrate_kbps = 0;
};

struct Start {};
struct Stop {};
struct RequestKeyframe {};

struct SetOptions {
    uint8_t mirror = 0;
    uint16_t rotation = 0;
    uint8_t dim_preview = 0;
};

struct VideoConfig {
    Codec codec = Codec::H264;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t fps = 0;
    std::vector<uint8_t> parameter_sets;  // Annex-B SPS/PPS(/VPS)
};

struct VideoFrame {
    uint64_t pts_us = 0;
    uint8_t frame_flags = 0;  // bit0 keyframe
    std::vector<uint8_t> data;  // Annex-B access unit
    bool IsKeyframe() const { return frame_flags & 1; }
};

struct Heartbeat {
    uint64_t send_time_us = 0;
};

struct Status {
    uint8_t thermal_state = 0;
    uint8_t battery_percent = 255;
    uint8_t charging = 0;
    uint32_t actual_bitrate_kbps = 0;
    uint16_t actual_fps_x100 = 0;
};

struct Error {
    ErrorCode code = ErrorCode::InvalidFormat;
    std::string message;
};

// A message of a type this build doesn't know. Kept so callers can log and ignore it.
struct Unknown {
    uint8_t type = 0;
};

using Message = std::variant<Hello, Caps, SetCamera, SetFormat, Start, Stop, RequestKeyframe, SetOptions,
                             VideoConfig, VideoFrame, Heartbeat, Status, Error, Unknown>;

// Serializes a full frame (header + payload).
std::vector<uint8_t> Encode(const Message& msg);

// Decodes a payload of the given type. Returns nullopt for a malformed known message.
std::optional<Message> DecodePayload(uint8_t type, std::span<const uint8_t> payload);

// Incremental parser for a TCP byte stream.
class StreamParser {
public:
    enum class Result { NeedMore, Message, Fatal };

    void Feed(std::span<const uint8_t> data);
    // Extracts the next message into `out`. Fatal means the stream is corrupt: close the connection.
    Result Next(Message& out);
    size_t Buffered() const { return buf_.size() - pos_; }

private:
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
};

const char* TypeName(const Message& msg);

}  // namespace tethercam::proto
