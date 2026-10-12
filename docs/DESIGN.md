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

**Threads (as built in v0.1):**

| Thread | Job |
|---|---|
| UI thread | Message loop, tray icon and menu (`agent/tray.cpp`). Refreshed by `PostMessage` from the engine |
| Engine thread | `agent/engine.cpp`: device discovery, the protocol session, H.264 decode, scaling and the shm writer, all in one loop that polls the socket every 20 ms |

v0.1 decodes on the session thread: the hardware decoder returns in a few milliseconds and TCP buffers
absorb short stalls, so a separate decode thread wasn't needed yet (revisit if 4K60 profiling says so).
Hot-plug is handled by polling `ListDevices` once a second instead of a usbmuxd `Listen` socket; the
`Listen` client exists in `common/usbmux` for later. Settings from the tray (lens, mirror, rotation) reach
the engine through atomics and a mutex-protected `Snapshot`.

**Agent state machine:**

```
NoUsbmuxd ◀── usbmuxd unreachable (retry 1 s)
NoDevice ─USB device listed─▶ Connect ─refused─▶ AppNotOpen (retry 1 s)
                                 │ ok
                                 ▼
                         Connected(Ready) ─consumer active─▶ Streaming
                                 ▲                              │ consumer gone 2 s → Stop
                                 └──────────────────────────────┘
any state ── socket error / 3 s silence ──▶ reconnect
Streaming ── Error(CAMERA_UNAVAILABLE) ──▶ PausedOnPhone (until the next VideoConfig)
```

The tray icon has 4 states: grey (no device), amber (app not open, paused, or Apple Devices missing),
green (connected), green with a white dot (streaming).

**Decoder pipeline (`media/src/media.cpp`):**

1. D3D11 device (`D3D11_CREATE_DEVICE_VIDEO_SUPPORT`) + `IMFDXGIDeviceManager`; falls back to software
   decoding if the MFT isn't D3D11-aware or the device can't be created.
2. Microsoft H.264 decoder MFT, `CODECAPI_AVLowLatencyMode`, input `MFVideoFormat_H264`, output `NV12`.
3. Each access unit becomes one `IMFSample`; the parameter sets from `VideoConfig` are prepended to IDRs.
4. On `MF_E_TRANSFORM_STREAM_CHANGE`, renegotiate NV12 and re-read the coded size (e.g. 1920×1088) and
   display aperture (1920×1080).
5. Frames are read with `IMF2DBuffer::Lock2D` (works for both GPU and memory buffers).
6. If the decoded size ≠ the consumer's size: CPU bilinear scale with letterbox (`common/image`). A GPU
   Video Processor path is a later optimization (TECH_SPEC R7).
7. `shm::WriteFrame` copies row by row into the next slot.

## Shared memory layout

Defined in `windows/common/include/tethercam/shm.h`. Little-endian, naturally aligned, version 1:

```c
struct Header {                  // offset 0, 4096 bytes reserved
    uint32_t magic;              // 'TCSH'
    uint32_t version;            // 1
    uint32_t header_size;        // 4096
    uint32_t slot_count;         // 3
    uint64_t slot_offset[3];     // from start of mapping, 4096-aligned
    uint64_t slot_capacity;      // bytes per slot (64 + 4K NV12, rounded to 4096)

    // written by the vcam (consumer side)
    uint32_t consumer_active;    // 0/1
    uint32_t req_width, req_height, req_fps;
    uint32_t control_seq;        // bumped on every change
    uint32_t reserved0;

    // written by the agent (producer side)
    uint64_t agent_heartbeat_ms; // GetTickCount64() of the last update
    uint32_t agent_pid;
    uint32_t latest_slot;        // newest complete frame, 0xFFFFFFFF = none
    uint32_t agent_status;       // AgentStatus enum, drives the placeholder text
    uint32_t reserved1;

    char vcam_message[256];      // vcam's last status line; the agent copies it into its log
};

struct SlotHeader {              // start of each slot, 64 bytes
    uint32_t seq;                // seqlock: odd while being written
    uint32_t width, height, stride;
    uint64_t pts_100ns;
    uint64_t frame_number;
};                               // NV12 data follows at +64 (Y plane, then interleaved UV)
```

**Writer (agent):** next slot = `(latest_slot + 1) % 3`; `seq |= 1` (odd), release fence, copy, `seq + 1`
(even, release), `latest_slot = i`, `SetEvent(FrameReady)`.
**Reader (vcam):** read `latest_slot`, read `seq` (acquire; must be even), copy, acquire fence, re-read
`seq`. If it changed, retry once, then fall back to the previous frame. All shared fields go through
`std::atomic_ref`. A unit test hammers this with a concurrent writer and checks no frame is ever torn.

Staleness: if `agent_heartbeat_ms` is older than 1.5 s, or no new frame number arrived for 750 ms, the vcam
shows the placeholder for `agent_status`.

**Namespace:** objects are named `Global\Tethercam.*.v1`. For development without a registered camera,
`TETHERCAM_SHM_NAMESPACE=Local` switches both sides to the session namespace (used with
`vcamcheck direct`).

## Virtual camera internals

- `DllGetClassObject` (WRL `Module<InProc>`) → `Activator` (`IMFActivate` + forwarded `IMFAttributes`).
  Frame Server calls `ActivateObject` → `MediaSource`.
- `MediaSource` (`IMFMediaSourceEx`, `IMFGetService`, `IKsControl`, `IMFSampleAllocatorControl`) owns one
  `MediaStream` (`IMFMediaStream2`, `IKsControl`) with the 10 NV12 media types from
  `AdvertisedFormats()`. `IKsControl` answers `ERROR_SET_NOT_FOUND` for everything.
- `MediaSource::Start` reads the consumer's chosen media type from the presentation descriptor, queues
  `MENewStream`/`MEUpdatedStream`, starts the stream (which publishes `consumer_active` and the size to
  shm and signals Control) and queues `MESourceStarted`.
- `MediaStream::RequestSample` paces delivery to the negotiated frame rate with a high-resolution waitable
  timer (Frame Server asks for the next sample immediately), allocates from the Frame Server allocator
  when one is provided (`MFSampleAllocatorUsage_UsesProvidedAllocator`), else a 2D memory buffer, fills it
  with a live frame or the placeholder, timestamps it with `MFGetSystemTime()` and queues `MEMediaSample`.
- `Stop`/`Shutdown` clear `consumer_active`. Several consumers share one source through Frame Server
  (`MF_DEVICESTREAM_FRAMESERVER_SHARED`).
- Lock order is source → stream; `GetEvent` (which can block) never runs under a lock; `Shutdown` breaks
  the source ↔ stream reference cycle.
- Logging: `OutputDebugString` (view with DebugView) plus `vcam_message` in shared memory, which the agent
  copies into its log. LocalService can't write to user folders.

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
