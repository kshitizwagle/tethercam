#pragma once

// Shared-memory frame exchange between tethercam-agent (producer, user session) and
// tethercam-vcam.dll (consumer, inside the FrameServer service). Layout and protocol are
// specified in docs/DESIGN.md "Shared memory layout"; names and DACL in TECH_SPEC §9.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <span>

#include "tethercam/agent_state.h"

namespace tethercam::shm {

// Object names live in the Global\ namespace (the service is in session 0). For development
// without a registered camera, TETHERCAM_SHM_NAMESPACE=Local moves them to the session namespace.
inline constexpr wchar_t kMappingName[] = L"Tethercam.Frames.v1";
inline constexpr wchar_t kFrameReadyName[] = L"Tethercam.FrameReady.v1";
inline constexpr wchar_t kControlName[] = L"Tethercam.Control.v1";
// SYSTEM and LocalService: full. Interactive users: read/write/execute (execute = SYNCHRONIZE on events).
inline constexpr wchar_t kSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GRGWGX;;;IU)";

inline constexpr uint32_t kMagic = 0x48534354;  // 'TCSH'
inline constexpr uint32_t kVersion = 1;
inline constexpr uint32_t kHeaderBytes = 4096;
inline constexpr uint32_t kSlotCount = 3;
inline constexpr uint32_t kSlotHeaderBytes = 64;
inline constexpr uint32_t kMaxWidth = 3840;
inline constexpr uint32_t kMaxHeight = 2160;
inline constexpr uint32_t kNoSlot = 0xFFFFFFFF;

constexpr uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
inline constexpr uint64_t kSlotCapacity = AlignUp(kSlotHeaderBytes + uint64_t(kMaxWidth) * kMaxHeight * 3 / 2, 4096);
inline constexpr uint64_t kMappingBytes = kHeaderBytes + kSlotCapacity * kSlotCount;

// AgentStatus (agent_status values in the header below) lives in agent_state.h.

struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t slot_count;
    uint64_t slot_offset[kSlotCount];
    uint64_t slot_capacity;

    // Written by the vcam (consumer).
    uint32_t consumer_active;
    uint32_t req_width;
    uint32_t req_height;
    uint32_t req_fps;
    uint32_t control_seq;
    uint32_t reserved0;

    // Written by the agent (producer).
    uint64_t agent_heartbeat_ms;  // GetTickCount64() of the last agent update
    uint32_t agent_pid;
    uint32_t latest_slot;
    uint32_t agent_status;
    uint32_t placeholder_mode;  // what the camera shows while not live: 0 = black, 1 = status card

    // Written by the vcam: last status line, for the agent's log (the vcam cannot write files).
    char vcam_message[256];
};
static_assert(sizeof(Header) <= kHeaderBytes);

struct SlotHeader {
    uint32_t seq;  // seqlock: odd while being written
    uint32_t width;
    uint32_t height;
    uint32_t stride;  // bytes per row for both planes (NV12)
    uint64_t pts_100ns;
    uint64_t frame_number;
    uint8_t reserved[kSlotHeaderBytes - 32];
};
static_assert(sizeof(SlotHeader) == kSlotHeaderBytes);

template <class T> std::atomic_ref<T> Atomic(T& v) { return std::atomic_ref<T>(v); }

// Initializes a freshly created mapping (consumer side).
void InitHeader(void* base);
// Checks that the header describes a layout that fits in `size` bytes.
bool ValidateHeader(const void* base, uint64_t size);

// Producer: writes one NV12 frame. `y` and `uv` are planes with the given pitch.
bool WriteFrame(void* base, uint32_t width, uint32_t height, const uint8_t* y, uint32_t y_pitch, const uint8_t* uv,
                uint32_t uv_pitch, uint64_t pts_100ns);

struct FrameInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t pts_100ns = 0;
    uint64_t frame_number = 0;
};

// Consumer: copies the newest frame into `dst` (tightly packed NV12 at `dst_pitch`) when it
// matches width x height. Returns false if there is no complete frame of that size.
bool ReadLatest(const void* base, uint32_t width, uint32_t height, uint8_t* dst, uint32_t dst_pitch,
                FrameInfo& info);

// Consumer: reads the newest frame's metadata without copying its planes. Cheap enough to call
// before ReadLatest (e.g. to notice that nothing changed and skip the copy).
bool PeekLatest(const void* base, FrameInfo& info);

// Kernel objects. Create* is used by the vcam (LocalService); Open* by the agent.
struct Objects {
    HANDLE mapping = nullptr;
    HANDLE frame_ready = nullptr;
    HANDLE control = nullptr;
    void* view = nullptr;
    void Close();
    ~Objects() { Close(); }
};

// Creates (or opens if it exists) the mapping and events with the DACL above.
bool Create(Objects& out);
// Opens existing objects. Fails quietly when no virtual camera consumer has created them yet.
bool Open(Objects& out);

}  // namespace tethercam::shm
