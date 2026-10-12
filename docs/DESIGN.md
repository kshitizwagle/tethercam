# Tethercam: Design

How the pieces in [TECH_SPEC.md](TECH_SPEC.md) are built inside. Keep this doc current when the code
changes shape.

## Repo layout

```
/
├── README.md  CONTRIBUTING.md  CLAUDE.md  LICENSE  MILESTONES.md  TODO.md
├── docs/                      PRODUCT, TECH_SPEC, DESIGN, PROTOCOL, PLAN
├── ios/
│   ├── project.yml            XcodeGen spec (app target + unit test target)
│   ├── Tethercam/
│   │   ├── App/               TethercamApp.swift, ContentView.swift (SwiftUI)
│   │   ├── Capture/           CameraController.swift (AVCaptureSession, lens/format selection)
│   │   ├── Encode/            H264Encoder.swift (VTCompressionSession), AnnexB.swift
│   │   ├── Net/               Listener.swift (NWListener), Session.swift (protocol state machine)
│   │   └── Protocol/          Messages.swift, Codec.swift (encode/decode, mirrors PROTOCOL.md)
│   └── TethercamTests/        protocol codec + Annex-B conversion tests
├── windows/
│   ├── CMakeLists.txt         top level, C++20, /W4 /permissive- /utf-8
│   ├── common/                static lib: protocol codec, plist mini, usbmux client, shm layout, logging
│   ├── agent/                 tethercam-agent.exe: session, decoder, scaler, shm writer, tray UI, install
│   ├── vcam/                  tethercam-vcam.dll: activator, media source, stream, shm reader, placeholder
│   ├── tools/tcprobe/         stream stats + .h264 dump
│   ├── tools/fakephone/       device simulator over TCP
│   ├── tests/                 GoogleTest (FetchContent)
│   └── installer/             WiX v4 project (M7)
└── .github/workflows/         windows.yml, ios.yml
```

## Shared code rules

- The protocol codec exists twice (Swift and C++). Both have tests built from the same golden byte vectors,
  stored in `docs/protocol-vectors/*.hex`, so the two can't drift apart.
- `windows/common` must not depend on Media Foundation or COM, so it can be unit-tested anywhere.

## iOS app internals

**Concurrency (Swift 6):**

| Unit | Isolation | Job |
|---|---|---|
| `AppModel` | `@MainActor` `@Observable` | UI state: connection, lens, stats |
| `CameraController` | actor + `sessionQueue` (serial `DispatchQueue`) | All `AVCaptureSession` configuration |
| capture callback | `videoQueue` (serial, high QoS) | Receives `CMSampleBuffer`, hands it to the encoder |
| `H264Encoder` | VT callback thread → `sendQueue` | Encodes, packetizes, enqueues frames |
| `Session` | actor | Protocol state machine, owns the `NWConnection`, heartbeat timer |

**Session state machine (device):**

```
Idle ──connection──▶ Handshaking ──Hello ok──▶ Ready ──Start──▶ Streaming
  ▲                      │ bad version              ▲  Stop / SetFormat(restart) │
  └──── disconnect ◀─────┴──────────────────────────┴───────────────────────────┘
```

On `SetFormat` or `SetCamera` while streaming: reconfigure capture (`beginConfiguration`/
`commitConfiguration`), rebuild the encoder if the size changed, send `VideoConfig`, force an IDR.

**Backpressure:** `sendQueue` tracks the bytes in flight on the `NWConnection` (send completions). Above
the 250 ms budget it sets `dropUntilKeyframe`, skips frames, and forces an IDR.

## Windows agent internals

**Threads:**

| Thread | Job |
|---|---|
| UI thread | Message loop, tray icon, menu, `WM_DEVICECHANGE`-free (hot-plug comes from usbmuxd `Listen`) |
| `MuxWatcher` | Blocking socket on the usbmuxd `Listen` connection; posts Attached/Detached |
| `SessionThread` | Owns the device socket: reads frames, sends control, heartbeats (select with 1 s timeout) |
| `DecodeThread` | Bounded queue (max 4 access units, drop-oldest-non-key) → MF decoder → scaler → shm writer |
| `ControlWatcher` | Waits on `Global\Tethercam.Control.v1` / polls for the mapping; turns consumer state into session commands |

Threads talk to each other through small locked queues and a central `AgentState` (mutex + condition
variable). No thread blocks the UI thread.

**Agent state machine:**

```
NoDevice ─Attached─▶ DeviceFound ─Connect ok─▶ Connected(Ready) ─consumer active─▶ Streaming
   ▲  ▲                 │ result 3 (app closed): retry 1 s          │ consumer gone 2 s → Stop
   │  └── Detached ─────┴──────────────── socket error / 3 s silence ┘
```

The tray icon has 4 states: grey (no device), amber (device found, app not open), green (connected),
green + dot (streaming).

**Decoder pipeline:**

1. `MFStartup`, D3D11 device (`D3D11_CREATE_DEVICE_VIDEO_SUPPORT`), `IMFDXGIDeviceManager`.
2. H.264 decoder MFT with the D3D manager, input type `MFVideoFormat_H264`, output `NV12`.
3. Each access unit becomes one `IMFSample` (Annex-B data; the parameter sets from `VideoConfig` are
   prepended before every IDR).
4. On `MF_E_TRANSFORM_STREAM_CHANGE`, renegotiate the output type.
5. If the output size ≠ the target size: Video Processor MFT (GPU) NV12→NV12 scale with letterbox.
6. `CopyResource` to a staging texture, `Map`, copy row by row into the shm slot (respecting both pitches).

## Shared memory layout

All fields are little-endian and naturally aligned. Version 1:

```c
struct TcamShmHeader {            // at offset 0, 4096 bytes reserved
    uint32_t magic;               // 'TCSH'
    uint32_t version;             // 1
    uint32_t header_size;         // 4096
    uint32_t slot_count;          // 3
    uint64_t slot_offset[3];      // from start of mapping, 4096-aligned
    uint64_t slot_capacity;       // bytes per slot (4K NV12 = 3840*2160*3/2)

    // written by vcam (consumer side)
    volatile uint32_t consumer_active;   // 0/1
    uint32_t req_width, req_height, req_fps;
    volatile uint32_t control_seq;       // bumped on every change

    // written by agent (producer side)
    volatile uint64_t agent_heartbeat_qpc;
    volatile uint32_t agent_pid;
    volatile uint32_t latest_slot;       // index of newest complete frame, 0xFFFFFFFF = none
    uint32_t agent_status;               // enum mirrored in the placeholder text
};

struct TcamSlotHeader {           // at the start of each slot, 64 bytes
    volatile uint32_t seq;        // seqlock: odd while being written
    uint32_t width, height, stride_y, stride_uv;
    uint64_t pts_qpc;             // agent QPC at decode output
    uint64_t frame_number;
};                                // NV12 data follows at +64
```

**Writer (agent):** choose the slot that is neither `latest_slot` nor the one the reader is likely on
(round-robin over 3), `seq++` (odd), copy, `seq++` (even), `latest_slot = i`, `SetEvent(FrameReady)`.
**Reader (vcam):** read `latest_slot`, read `seq` (must be even), copy, re-read `seq`. If it changed,
retry once, then use the previous frame. Use `std::atomic_ref` with acquire/release ordering.

Staleness: if `agent_heartbeat_qpc` is older than 1 s or the newest frame is older than 500 ms, the vcam
shows the placeholder (with text matching `agent_status`).

## Virtual camera internals

- `DllGetClassObject` → class factory → `Activator`. FrameServer calls `ActivateObject` → `MediaSource`.
- `MediaSource::Start` makes sure the mapping exists (creating it if needed), sets `consumer_active = 1`
  and the requested type, signals Control, starts the stream and queues `MEStreamStarted`.
- `MediaStream::RequestSample` runs on the FrameServer work queue. It must never block for long: take a
  frame if one is there, otherwise the previous frame or the placeholder. Frame pacing comes from the
  consumer's requests, so we don't run our own timer.
- `Stop`/`Shutdown` set `consumer_active = 0`. Several consumers share one source through Frame Server
  (`MF_DEVICESTREAM_FRAMESERVER_SHARED`).
- Every COM method is guarded: no exceptions escape, `MF_E_SHUTDOWN` after shutdown, and a critical
  section per object.
- Logging: ETW TraceLogging provider `Tethercam.VCam` (no files, since LocalService can't write to user
  folders). Read the logs with `tracelog`/WPR or `tracefmt` during development.

## Error handling

| Situation | Behavior |
|---|---|
| usbmuxd not running | Tray grey: "Install Apple Devices or iTunes". Retry every 5 s |
| Device not trusted / Connect refused | Tray amber: "Open Tethercam on your iPhone and tap Trust" |
| Phone app backgrounded or locked | Device sends `Error(CAMERA_UNAVAILABLE)` or goes quiet; vcam shows "Paused on iPhone" |
| Decode error | Drop until the next IDR, send `RequestKeyframe` |
| Agent crash | vcam sees a stale heartbeat → placeholder. The agent restarts with Windows on next login (later: a watchdog) |
| Unsupported requested format | Nearest device format + scaling (never refuse a media type we advertised) |

## Build and CI

- **Windows:** CMake ≥ 3.28, Visual Studio 2026 generator (`windows/CMakePresets.json`), x64 (ARM64 later). Dependencies through `FetchContent`
  only: GoogleTest and WIL (Windows Implementation Libraries, MIT). No vcpkg needed.
  Targets: `tcam_common` (static), `tethercam-agent`, `tethercam-vcam` (DLL + `.def` exporting
  `DllGetClassObject`, `DllCanUnloadNow`, `DllRegisterServer`, `DllUnregisterServer`), `tcprobe`,
  `fakephone`, `tcam_tests`.
- **iOS:** XcodeGen `project.yml`, deployment target iOS 16, Swift 6. `xcodebuild test` on the simulator
  covers the protocol/packetizer tests (the camera isn't available in the simulator).
- **CI (GitHub Actions):** `windows.yml` (windows-latest: configure, build, ctest), `ios.yml` (macos-latest:
  xcodegen, xcodebuild build + test, unsigned).

## Test strategy

| Level | What | Where |
|---|---|---|
| Unit | Protocol codec (golden vectors), plist writer/reader, usbmux packet framing, Annex-B conversion, format chooser, shm seqlock | `windows/tests`, `ios/TethercamTests` |
| Fuzz-ish | Random/truncated frames into the protocol parser and plist reader (no crashes, bounded allocations) | `windows/tests` |
| Integration (no phone) | `fakephone` → agent over TCP (`--tcp 127.0.0.1:5321`) → shm → vcam → Windows Camera app | Manual + scripted with `tcprobe` |
| Device | Real iPhone over USB: formats, lens switching, hot-plug, lock/unlock, thermals | Manual checklist in MILESTONES exit criteria |
| Compatibility | Camera app, Teams, Zoom, OBS, Chrome, Edge × 720p/1080p/4K | Matrix in `docs/compat.md` (created in M4) |
| Latency | Phone films an on-screen ms clock; screenshot comparison | M3/M5 measurements recorded in `docs/perf.md` |
