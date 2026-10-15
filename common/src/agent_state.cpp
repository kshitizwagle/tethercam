#include "tethercam/agent_state.h"

#include <format>

#include "tethercam/formats.h"
#include "tethercam/version.h"

namespace tethercam {

const char* StatusName(AgentStatus s) {
    switch (s) {
    case AgentStatus::None: return "none";
    case AgentStatus::NoDevice: return "waiting for iPhone";
    case AgentStatus::AppNotOpen: return "iPhone found, app not open";
    case AgentStatus::Connected: return "connected";
    case AgentStatus::Streaming: return "streaming";
    case AgentStatus::PausedOnPhone: return "paused on iPhone";
#ifdef _WIN32
    case AgentStatus::NoUsbmuxd: return "Apple Devices not installed";
#else
    case AgentStatus::NoUsbmuxd: return "usbmuxd not running";
#endif
    }
    return "?";
}

namespace agent {
namespace {

std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += c;
        else if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\u{:04x}", int(c));
        else out += c;
    }
    return out;
}

const char* StateName(AgentStatus s) {
    switch (s) {
    case AgentStatus::Streaming: return "live";
    case AgentStatus::Connected: return "connected";
    case AgentStatus::NoDevice: return "no_iphone";
    case AgentStatus::AppNotOpen: return "app_not_open";
    case AgentStatus::PausedOnPhone: return "paused";
    case AgentStatus::NoUsbmuxd: return "no_apple_driver";
    default: return "starting";
    }
}

}  // namespace

std::string StatusJson(const Snapshot& snap) {
    std::string camera, lens;
    for (const auto& c : snap.cameras) {
        if (c.camera_id != snap.selected_camera) continue;
        camera = c.label;
        lens = c.position == proto::Position::Front ? "front" : c.lens == proto::Lens::UltraWide ? "ultra_wide"
               : c.lens == proto::Lens::Telephoto ? "telephoto" : "wide";
    }
    const bool live = snap.status == AgentStatus::Streaming && snap.fps > 0;
    return std::format(
        R"({{"state":"{}","live":{},"device":"{}","camera":"{}","lens":"{}","width":{},"height":{},"fps":{},"mirror":{},"rotation":{},"detail":"{}","thermal":{},"battery":{},"charging":{},"version":"{}","quality":"{}"}})",
        StateName(snap.status), live ? "true" : "false", JsonEscape(snap.device_name), JsonEscape(camera), lens,
        snap.width, snap.height, snap.fps, snap.mirror ? "true" : "false", snap.rotation,
        JsonEscape(snap.detail), snap.thermal, snap.battery, snap.charging ? "true" : "false", Version(),
        QualityLabel(snap.quality));
}

}  // namespace agent
}  // namespace tethercam
