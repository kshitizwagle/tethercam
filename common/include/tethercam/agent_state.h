#pragma once

// Agent state shared by the Windows and Linux hosts: the status enum, the snapshot the UI and the
// status API read, and the status API's JSON (a stable contract, docs/STATUS_API.md).

#include <cstdint>
#include <string>
#include <vector>

#include "tethercam/protocol.h"

namespace tethercam {

// Agent state, shown on the placeholder frame and in the tray. Values are part of the Windows
// shared-memory layout (shm.h), so never renumber them.
enum class AgentStatus : uint32_t {
    None = 0,            // no agent running
    NoDevice = 1,        // waiting for an iPhone on USB
    AppNotOpen = 2,      // iPhone attached, Tethercam app not open / not trusted
    Connected = 3,       // connected, not streaming yet
    Streaming = 4,
    PausedOnPhone = 5,   // app backgrounded or screen locked
    NoUsbmuxd = 6,       // usbmuxd missing: Apple Devices on Windows, the usbmuxd service on Linux
};

// Short log name for a status.
const char* StatusName(AgentStatus s);

namespace shm {
using AgentStatus = tethercam::AgentStatus;
}

namespace agent {

struct Snapshot {
    AgentStatus status = AgentStatus::NoDevice;
    std::string device_name;
    std::string detail;  // e.g. "1920x1080 @ 30 fps, 11.8 Mbps"
    std::vector<proto::Camera> cameras;
    int selected_camera = -1;
    bool mirror = false;
    uint16_t rotation = 0;
    bool status_card = false;  // show the status card instead of black while not live
    proto::Quality quality = proto::Quality::Auto;
    uint32_t width = 0, height = 0, fps = 0;  // format being streamed (0 when idle)
    int thermal = -1;   // iPhone thermal state 0 nominal .. 3 critical, -1 unknown
    int battery = -1;   // iPhone battery percent, -1 unknown
    bool charging = false;
};

// Machine-readable status for the local status API (docs/STATUS_API.md).
std::string StatusJson(const Snapshot& snap);

}  // namespace agent
}  // namespace tethercam
