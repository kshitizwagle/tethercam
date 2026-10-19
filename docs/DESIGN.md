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
│   │   ├── Encode/            H264Encoder.swift (VTCompressionSession)
│   │   ├── Net/               Server.swift (USB + Wi-Fi NWListeners), Session.swift (protocol state machine),
│   │   │                      Noise.swift + WifiChannel.swift (Wi-Fi crypto), WifiPairings.swift (Keychain)
│   │   └── Protocol/          Wire.swift (encode/decode, mirrors PROTOCOL.md)
│   └── TethercamTests/        protocol codec + Annex-B conversion tests
├── common/                    static lib shared by both hosts: protocol codec, plist mini, usbmux client,
│   │                          sockets, logging, shm layout (Windows only), Link/SecureLink + Noise +
│   │                          pairing store (Wi-Fi, M11)
│   └── tests/                 GoogleTest (FetchContent), runs on Windows and Linux
├── linux/                     Linux host (M9): CMake root, builds common + tests so far
├── windows/
│   ├── CMakeLists.txt         top level, C++20, /W4 /permissive- /utf-8; adds ../common
│   ├── agent/                 tethercam-agent.exe: session, decoder, scaler, shm writer, tray UI, install
│   ├── vcam/                  tethercam-vcam.dll: activator, media source, stream, shm reader, placeholder
│   ├── tools/tcprobe/         stream stats + .h264 dump
│   ├── tools/fakephone/       device simulator over TCP
│   ├── tests/                 agent GoogleTests (Windows only)
│   └── installer/             WiX 5 MSI
├── third_party/monocypher/    Monocypher 4.0.2, unmodified (hosts and iOS app)
└── .github/workflows/         ci.yml orchestrates windows.yml, linux.yml and ios.yml
```

## Shared code rules

- The protocol codec exists twice (Swift and C++). Both have tests built from the same golden byte vectors,
  stored in `docs/protocol-vectors/v1.txt`, so the two can't drift apart.
- `common` must not depend on Media Foundation or COM, so it can be unit-tested anywhere; Windows-only
  code in it stays behind `#ifdef _WIN32` (sockets, logging) or is only built on Windows (`shm`).
- usbmuxd address: `USBMUXD_SOCKET_ADDRESS` (`host:port` or `UNIX:/path`), else 127.0.0.1:27015 on
  Windows and `/var/run/usbmuxd` on Linux.

## iOS app internals

**Concurrency (Swift 5 language mode, targeted strict-concurrency warnings):**

| Unit | Isolation | Job |
|---|---|---|
| `AppModel` | `@MainActor`, `ObservableObject` with `@Published` properties | UI state: connection, lens, stats |
| `CameraController` | class with `sessionQueue` (serial `DispatchQueue`) | All `AVCaptureSession` configuration |
| capture callback | `videoQueue` (serial, high QoS) | Receives `CMSampleBuffer`, hands it to the encoder |
| `H264Encoder` | VT callback thread → session queue | Encodes, packetizes, enqueues frames |
| `Session` / `Server` | serial session `DispatchQueue` | Protocol state machine, connection, heartbeat timer and bytes-in-flight accounting |

**Session state machine (device):**

```
Idle ──connection──▶ Handshaking ──Hello ok──▶ Ready ──Start──▶ Streaming
  ▲                      │ bad version              ▲  Stop / SetFormat(restart) │
  └──── disconnect ◀─────┴──────────────────────────┴───────────────────────────┘
```

On `SetFormat` or `SetCamera` while streaming: reconfigure capture (`beginConfiguration`/
`commitConfiguration`), rebuild the encoder if the size changed, send `VideoConfig`, force an IDR.

**Backpressure:** `Server` tracks the bytes in flight on the `NWConnection` (send completions). Above
the 250 ms budget it sets `dropUntilKeyframe`, skips frames, and forces an IDR.

The server listens on `127.0.0.1:5321`, reachable from the PC through usbmuxd. With Wireless on it also
listens on port 5322 (Wi-Fi/Ethernet, never cellular) and advertises `_tethercam._tcp`; a Wi-Fi
connection reaches `Session` only after `WifiChannel` completes the Noise handshake, and its traffic is
sealed/opened in records on the session queue. `WifiPair` is honoured only on the USB connection. A new
connection on either port replaces the previous client. When returning to the foreground without a connection, the app
recreates the listener; it also retries listener failures. The camera preview can run while the
phone app is open without encoding or transmitting to the PC.

The encoder uses low-latency rate control up to 1440p. At 4K it uses the normal real-time path,
no B-frames and at most one frame of delay, to address the measured 4K60 throughput cap. Device
validation of that change remains pending in [perf.md](perf.md).

## Windows agent internals

**Threads (as built in v0.1):**

| Thread | Job |
|---|---|
| UI thread | Message loop, tray icon and menu (`agent/tray.cpp`). Refreshed by `PostMessage` from the engine |
| Engine thread | `agent/engine.cpp`: device discovery, the protocol session, H.264 decode, scaling and the shm writer, all in one loop that polls the socket every 20 ms. Sessions run over a `Link`: plain for USB/`--tcp`, `SecureLink` for Wi-Fi. With no USB phone it tries paired phones over Wi-Fi every 3 s (700 ms connect timeout per address); during a Wi-Fi session it checks usbmuxd every 2 s and drops Wi-Fi when a USB phone appears |
| DNS client callbacks | `agent/discovery.cpp`: `DnsServiceBrowse`/`DnsServiceResolve` results for `_tethercam._tcp`, started once a phone is paired; the engine reads a mutex-protected list |
| Status accept thread | Loopback HTTP requests and SSE subscriptions (`agent/status_server.cpp`) |
| Status keepalive thread | Sends SSE keepalive comments every 15 seconds |

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

The tray icon has 4 badges: grey (no device), amber (app not open, paused, or Apple Devices missing),
green (connected), red (streaming).

`agent/preview.cpp` renders the Windows preview and controls with Direct2D. Opening it counts as
consumer demand: it requests 1080p30 when the virtual camera has no active consumer. Otherwise the
virtual camera's requested format takes priority. The engine keeps a reduced preview copy (at most
1280×720); closing the window hides it and removes preview demand. The tray keeps running.

The Quality menu overrides the **source capture** request before `ChooseDeviceFormat` chooses a
supported format. It does not change the virtual camera's negotiated output; the scaler still fits
the source into that output. Presets are Auto, 1080p30/60 and 4K30/60. The phone announces its saved
preference before Caps with `SetQuality`; live changes from either UI trigger format negotiation.
The phone's preference wins reconnect unless Windows has an unsent local change (`QualityPending`).
Windows sends Stop before changing lens/options/format, then Start. iOS capture generations prevent
an obsolete asynchronous configuration completion from re-enabling encoding after Stop or a newer
request. `VideoConfig` updates the agent's actual source size, including thermal/rotation changes.

Lens, mirror, rotation and offline-card preference are saved as strings under
`HKCU\Software\Tethercam` (`Camera`, `Mirror`, `Rotation`, `StatusCard`). Lens selection is persisted
by label, including phone-side changes reported through `Status.camera_id`.

`Quality` stores the preset number; `QualityPending` marks an unsent desktop choice. The phone saves
`captureQuality` in UserDefaults. Unsupported presets are disabled in each lens's menu. A remembered
preset can remain selected after a lens change; `ChooseDeviceFormat` clamps to its available formats.

The agent exposes a read-only HTTP/SSE snapshot on `127.0.0.1:4790`, also in console mode. Engine
notifications broadcast changed JSON to subscribers. See [STATUS_API.md](STATUS_API.md) for the
stable contract and timing limits; the service does not carry video or control commands.

**Decoder pipeline (`media/src/media.cpp`):**

1. D3D11 device (`D3D11_CREATE_DEVICE_VIDEO_SUPPORT`) on the first adapter from
   `EnumAdapterByGpuPreference(HIGH_PERFORMANCE)` (dGPU, then iGPU) that offers the H.264 VLD decoder
   profile, + `IMFDXGIDeviceManager`; falls back to software
   decoding if the MFT isn't D3D11-aware or the device can't be created.
2. Microsoft H.264 decoder MFT, `CODECAPI_AVLowLatencyMode`, input `MFVideoFormat_H264`, output `NV12`.
3. Each access unit becomes one `IMFSample`; the parameter sets from `VideoConfig` are prepended to IDRs.
4. On `MF_E_TRANSFORM_STREAM_CHANGE`, renegotiate NV12 and re-read the coded size (e.g. 1920×1088) and
   display aperture (1920×1080).
5. Frames are read with `IMF2DBuffer::Lock2D` (works for both GPU and memory buffers).
6. If the decoded size ≠ the consumer's size: CPU bilinear scale with letterbox (`common/image`). A GPU
   Video Processor path is a later optimization (TECH_SPEC R7).
7. `shm::WriteFrame` copies row by row into the next slot.

## Linux agent internals

`linux/agent` (TECH_SPEC §13). One process, no shared memory and no second component:

- `engine.cpp`: same session loop as the Windows engine (Hello/Caps/SetCamera/SetFormat/Start, heartbeat,
  reconnect, lens/quality persistence). Differences: frames go straight to `V4l2Output`; the consumer
  signal is `CountOtherOpeners(device) > 0` (scanned every 500 ms) or `--always-on`; the output format is
  fixed by the quality setting (Auto = 1080p30) and only changes while no app has the device open; black
  frames go out every 100 ms whenever no decoded frame did, so readers never stall.
- `decoder.cpp`: FFmpeg `h264` with a hardware device (`get_format` picks `AV_PIX_FMT_VAAPI`/`CUDA`),
  frames downloaded with `av_hwframe_transfer_data` to NV12; software output (YUV420P) is interleaved to
  NV12. GPUs are tried discrete first, then integrated (sysfs: NVIDIA is always discrete, amdgpu hides
  `mem_info_vram_vendor` on APUs, Intel's iGPU is `0000:00:02.0`); NVIDIA gets NVDEC before VA-API.
  `TETHERCAM_HW_DEVICE=/dev/dri/renderDN` goes first. A device that can't open, outputs software frames
  (no H.264 hwaccel) or fails a download is skipped for the rest of the run and the next one is tried,
  software last. `LOW_DELAY`, slice threads only.
- `v4l2_output.cpp`: finds the device labelled "Tethercam" (else the first v4l2loopback device, detected
  by its `max_openers` sysfs attribute), `VIDIOC_S_FMT` on the OUTPUT queue (YUYV default, NV12 with
  `--nv12`, BT.601 limited), letterbox-scales with `image::ScaleLetterbox` and `write()`s whole frames.
- `settings.cpp`: `key=value` file, written atomically (temp + rename). Same keys as the Windows registry.
- `preview.cpp`: GTK3 preview window (**Open Tethercam** in the tray, or `--preview`): a `GtkDrawingArea`
  paints a cairo `RGB24` surface (`Nv12ToBgra`), letterboxed, refreshed at ~30 Hz by a `g_timeout` that
  calls `Engine::CopyPreview`; toolbar has lens buttons, Mirror, Rotate and Quality. The engine keeps a
  scaled copy (max 1280 wide) only while `SetPreview(true)`, and an open window counts as a consumer
  (1080p30) so the phone streams without an app. Closing hides the window and stops the feed.
- `tray.cpp`: AyatanaAppIndicator menu rebuilt on the GTK thread (`g_idle_add`) when the status changes;
  optional at build time (`TETHERCAM_TRAY`). `main.cpp` takes a per-user `flock` in `$XDG_RUNTIME_DIR`
  so the systemd user service and a manual start can't both run.
- `encoder.cpp`: libx264/OpenH264 encoder used only by `fakephone` on Linux.

## Shared memory layout

Defined in `common/include/tethercam/shm.h`. Little-endian, naturally aligned, version 1:

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
    uint32_t placeholder_mode;   // 0 = black (default), 1 = text status card

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
shows black by default, or the placeholder for `agent_status` when `placeholder_mode` is 1.

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
- `MediaStream::RequestSample` paces delivery (Frame Server asks for the next sample immediately): one
  sample per frame interval on a fixed cadence, so the average rate is exactly the negotiated fps. Within
  its interval the sample goes out as soon as the agent has published a frame this source has not
  delivered (`FrameSource::WaitForFresh`: FrameReady or the interval's high-resolution waitable timer,
  re-checking `PeekLatest` every 4 ms because the auto-reset event is shared by all instances), and
  repeats the last frame only when the interval ends without one. A slightly late frame is therefore
  shown in the next interval instead of being overwritten by its successor (R10). It allocates from the Frame Server allocator
  when one is provided (`MFSampleAllocatorUsage_UsesProvidedAllocator`), else a 2D memory buffer, fills it
  with a live frame or the placeholder, timestamps it with `MFGetSystemTime()` and queues `MEMediaSample`.
- `Stop`/`Shutdown` clear `consumer_active`. Several consumers share one source through Frame Server
  (`MF_DEVICESTREAM_FRAMESERVER_SHARED`). The flag is shared by every source instance, so a running
  stream re-asserts it (and its size) on each frame when it finds it cleared: an app that swaps instances
  (OBS changing scenes) can have the old instance's `Stop` land after the new one's `Start`, and the
  agent's 2 s grace absorbs the gap.
- Lock order is source → stream; `GetEvent` (which can block) never runs under a lock; `Shutdown` breaks
  the source ↔ stream reference cycle.
- Logging: `OutputDebugString` (view with DebugView) plus `vcam_message` in shared memory, which the agent
  copies into its log. LocalService can't write to user folders.

## Error handling

| Situation | Behavior |
|---|---|
| usbmuxd not running | Tray amber: "Install Apple Devices or iTunes". Retry every 1 s |
| Device not trusted / Connect refused | Tray amber: "Open Tethercam on your iPhone and tap Trust" |
| Phone app backgrounded or locked | Device sends `Error(CAMERA_UNAVAILABLE)` or goes quiet; vcam shows black, or "Paused on iPhone" with the status card enabled |
| Decode error | Drop until the next IDR, send `RequestKeyframe` |
| Agent crash | vcam sees a stale heartbeat → placeholder. The agent restarts with Windows on next login (later: a watchdog) |
| Unsupported requested format | Nearest device format + scaling (never refuse a media type we advertised) |

## Build and CI

- **Windows:** CMake ≥ 3.28, Visual Studio 2026 generator (`windows/CMakePresets.json`), x64 (ARM64 later). Dependencies through `FetchContent`
  only: GoogleTest and WIL (Windows Implementation Libraries, MIT). No vcpkg needed.
  Targets: `tcam_common` (static), `tethercam-agent`, `tethercam-vcam` (DLL + `.def` exporting
  `DllGetClassObject`, `DllCanUnloadNow`, `DllRegisterServer`, `DllUnregisterServer`), `tcprobe`,
  `fakephone`, `tcam_tests`.
- **iOS:** XcodeGen `project.yml`, deployment target iOS 16, Swift 5 language mode. `xcodebuild test` on the simulator
  covers the protocol/packetizer tests (the camera isn't available in the simulator).
- **CI (GitHub Actions):** `ci.yml` calls the reusable Windows and iOS workflows. Successful main
  builds publish a versioned MSI, sideloadable IPA and checksums, then update the AltStore source.
  Documentation-only pushes skip builds. See [DEVELOPMENT.md](DEVELOPMENT.md) for commands and releases.

## Test strategy

| Level | What | Where |
|---|---|---|
| Unit | Protocol codec (golden vectors), plist writer/reader, usbmux packet framing, Annex-B conversion, format chooser, shm seqlock | `common/tests`, `windows/tests`, `ios/TethercamTests` |
| Fuzz-ish | Random/truncated frames into the protocol parser and plist reader (no crashes, bounded allocations) | `common/tests` |
| Integration (no phone) | `fakephone` → agent over TCP (`--tcp 127.0.0.1:5321`) → shm → vcam → Windows Camera app | Manual + scripted with `tcprobe` |
| Wi-Fi transport | Noise vector (C++ and Swift), records, preamble/pairing-file fuzzing, engine pair-then-Wi-Fi test; `fakephone --advertise` + agent end to end | `common/tests/wifi_test.cpp`, `windows/tests/wifi_engine_test.cpp`, `ios/TethercamTests/NoiseTests.swift` |
| Device | Real iPhone over USB: formats, lens switching, hot-plug, lock/unlock, thermals | Manual checklist in MILESTONES exit criteria |
| Compatibility | Camera app, Teams, Zoom, OBS, Chrome, Edge × 720p/1080p/4K | Matrix in `docs/compat.md` (created in M4) |
| Latency | Phone films an on-screen ms clock; screenshot comparison | M3/M5 measurements recorded in `docs/perf.md` |
