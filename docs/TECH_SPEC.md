# Tethercam: Technical Specification

Status: draft v1 · Owner: project maintainers · Related: [PRODUCT.md](PRODUCT.md), [DESIGN.md](DESIGN.md),
[PROTOCOL.md](PROTOCOL.md)

## 1. Scope

Tethercam streams video from an iPhone camera to a Windows 11 or Linux PC over USB and exposes it as a
system webcam. Linux support is milestone M9 (§13). v1 is video only, H.264, wired only. See [PRODUCT.md](PRODUCT.md) for goals and non-goals.

## 2. Requirements

### 2.1 Functional

| ID | Requirement |
|---|---|
| F1 | The PC finds an attached iPhone over USB through Apple's usbmuxd service, with no network involved. |
| F2 | The iPhone app reports every camera (lens) and every video resolution with its max frame rate. |
| F3 | A virtual camera named **Tethercam** appears in all Media Foundation and Frame Server apps (Camera app, Teams, Zoom, OBS, Chromium browsers). |
| F4 | The virtual camera offers NV12 at 640×480, 1280×720, 1920×1080, 2560×1440 and 3840×2160, each at 30 and 60 fps. |
| F5 | When an app opens the camera with a given media type, the phone streams that size and rate natively when it can. Otherwise it streams the nearest larger format and the agent scales it. |
| F6 | The camera only runs on the phone while at least one PC app has Tethercam open (saves battery and heat). |
| F7 | The user can switch lens, mirror and rotation from the tray menu or the phone. |
| F8 | With no phone connected, the virtual camera stays usable and shows a "Connect your iPhone" placeholder frame. |
| F9 | Unplug/replug, phone lock/unlock and app restart recover automatically. |

### 2.2 Non-functional

| ID | Requirement |
|---|---|
| N1 | Glass-to-glass latency < 100 ms at 1080p30 and < 150 ms at 4K30 (measured with an on-screen clock). |
| N2 | Recovery < 3 s after replug or unlock. |
| N3 | Agent CPU < 5 % at 1080p30 on a 4-core laptop with a hardware decoder. Memory flat over 1 h. |
| N4 | No network traffic beyond the loopback connection to usbmuxd. No telemetry. |
| N5 | The iOS listener must not be reachable over Wi-Fi or cellular. |
| N6 | A crash in the agent must never crash Frame Server or the consuming app. The vcam falls back to the placeholder. |

### 2.3 Platform requirements

- Windows 11 (build 22000+) x64. ARM64 is a stretch goal; the code should stay architecture-neutral.
- Apple Mobile Device Service (installed by the Apple Devices app or iTunes).
- Linux x86_64 (M9): kernel with `v4l2loopback` (DKMS), `usbmuxd` from the distro, FFmpeg ≥ 6. Ubuntu
  24.04+ and Fedora 40+ are the tested targets.
- iOS 16+ (for the `AVCaptureDevice` and VideoToolbox low-latency APIs we use). Built with Xcode 16+.

## 3. Architecture

```
┌────────────── iPhone ──────────────┐        ┌──────────────────── Windows 11 ─────────────────────────────┐
│ AVCaptureSession (lens, format)    │        │ tethercam-agent.exe (user session, tray)                    │
│   → CVPixelBuffer NV12             │        │   UsbmuxClient ─ TcamSession ─ H264Decoder(MF, D3D11)       │
│ VTCompressionSession (H.264 HW)    │  USB   │        │                       └→ Scaler(VideoProcessor MFT)  │
│   → AVCC → Annex-B packetizer      │◀══════▶│        └ control (SetFormat/Start/Stop) ←┐       │ NV12       │
│ NWListener 127.0.0.1:5321          │usbmuxd │                                          │       ▼            │
│ Session (PROTOCOL.md)              │        │                 Global\Tethercam* shared memory + events    │
└────────────────────────────────────┘        │                                          ▲       │            │
                                              │ FrameServer svc (LocalService)           │       ▼            │
                                              │   tethercam-vcam.dll: MediaSource/Stream ┘ NV12 IMFSamples  │
                                              │        → Teams / Zoom / OBS / Camera app                    │
                                              └─────────────────────────────────────────────────────────────┘
```

### 3.1 Components

| Component | Language | Runs as | Responsibility |
|---|---|---|---|
| **Tethercam iOS app** | Swift 6 | Foreground app | Capture, hardware encode, serve protocol on port 5321 |
| **tethercam-agent.exe** | C++20 | Interactive user, tray app, auto-start | usbmuxd, protocol, decode, scale, write frames to shared memory, UI |
| **tethercam-vcam.dll** | C++20, WRL COM | Loaded by the FrameServer service (LocalService) | Media source: read frames from shared memory and deliver `IMFSample`s; publish the requested format |
| **tethercam-agent `--install`/`--uninstall`** | C++20 | Admin, run by the installer | `MFCreateVirtualCamera` registration (system lifetime) |
| **tcprobe** | C++20 | Dev tool | Connect via usbmuxd or TCP, print stats, dump `.h264` |
| **fakephone** | C++20 | Dev tool | Act as a device over plain TCP, streaming an `.h264` file, so the Windows side can be developed without an iPhone |

Why split the agent from the vcam DLL: the DLL runs inside a shared system service. Keeping USB, decoding
and UI in a normal user process makes it easier to debug, keeps crashes isolated (N6), and lets the agent
show UI. The cost is one NV12 copy through shared memory (≈ 12.4 MB per 4K frame, ≈ 750 MB/s at 4K60,
which is acceptable; a shared D3D11 texture is a later optimization).

## 4. USB transport (usbmuxd)

Apple Mobile Device Service listens on **TCP 127.0.0.1:27015** on Windows. Allow an override with the
`USBMUXD_SOCKET_ADDRESS` environment variable (same name libimobiledevice uses) for testing.

**Packet:** 16-byte header, then an XML plist payload.

| Offset | Field | Value |
|---|---|---|
| 0 | `length` u32 LE | Header + payload |
| 4 | `version` u32 LE | `1` (plist protocol) |
| 8 | `message` u32 LE | `8` (plist) |
| 12 | `tag` u32 LE | Request ID, echoed in the reply |

**Messages we use** (plist dicts, `MessageType` key):

- `ListDevices` → reply `{DeviceList: [{DeviceID, Properties: {ConnectionType, SerialNumber, ProductID, ...}}]}`.
  Only `ConnectionType == "USB"` devices are used (F1, wired only).
- `Listen` → the socket then receives `Attached` / `Detached` events for hot-plug (F9). Uses its own socket.
- `Connect {DeviceID, PortNumber}` on a fresh socket → reply `Result {Number}`. `0` = OK and the socket is
  now a raw tunnel to the device port. **`PortNumber` is the port byte-swapped** (network order read as a
  little-endian integer): 5321 → `0xC914` = 51476. Other results: 2 = bad device, 3 = connection refused
  (app not running or not listening).
- Every request also sends `ClientVersionString = "tethercam-<ver>"` and `ProgName = "tethercam"`.

The device has to be paired ("Trust This Computer"). usbmuxd handles pairing; we only surface a hint when
`Connect` fails on an untrusted device.

We write our own minimal plist XML writer/reader (only dict, array, string, integer, data and bool are
needed), so there's no libplist dependency.

## 5. iOS app

- **Capture:** `AVCaptureSession` with one `AVCaptureDeviceInput`. Discover lenses with
  `AVCaptureDevice.DiscoverySession` (`.builtInWideAngleCamera`, `.builtInUltraWideCamera`,
  `.builtInTelephotoCamera`; front `.builtInWideAngleCamera`/TrueDepth). For `SetFormat`, pick the
  `AVCaptureDevice.Format` with the matching dimensions, 8-bit 4:2:0 video range
  (`420YpCbCr8BiPlanarVideoRange`), supporting the requested fps, with HDR/Dolby Vision off. Set
  `activeVideoMin/MaxFrameDuration`. Use `AVCaptureVideoDataOutput` with
  `alwaysDiscardsLateVideoFrames = true`. Turn video stabilization off (it adds latency).
- **Rotation/mirror:** `AVCaptureConnection.videoRotationAngle` and `isVideoMirrored`.
- **Encode:** `VTCompressionSession`, H.264 High profile (auto level), with:
  `kVTVideoEncoderSpecification_EnableLowLatencyRateControl = true`, `RealTime = true`,
  `AllowFrameReordering = false`, `ExpectedFrameRate = fps`, `MaxKeyFrameInterval = 2·fps`,
  `AverageBitRate` from the table in §6, and IDR on demand with `kVTEncodeFrameOptionKey_ForceKeyFrame`.
  Output is AVCC (length-prefixed). The packetizer converts it to Annex-B and extracts SPS/PPS into
  `VideoConfig`.
- **Network:** `NWListener` bound to **127.0.0.1:5321** (N5). usbmuxd delivers USB connections on the
  device's loopback, which needs checking in M1 (open question Q1). Single client at a time; a new
  connection replaces the old one.
- **Lifecycle:** `isIdleTimerDisabled = true` while the app is in the foreground. Capture stops when the app
  goes to the background or the screen locks (an iOS rule we can't work around). On return, the app sends
  a fresh `VideoConfig` and IDR. Watch `ProcessInfo.thermalState`: at `.serious`, halve the bitrate and
  cap at 30 fps; at `.critical`, stop with `Error(THERMAL_SHUTDOWN)`.
- **Backpressure:** if the socket send queue holds more than ~250 ms of data, drop non-key frames until the
  next keyframe and request an IDR from the encoder.

## 6. Formats, bitrates and bandwidth

"Full resolution" means the highest *video* format the active lens reports (typically 3840×2160 @ 60 fps on
recent iPhones; 48 MP stills don't apply to video).

| Format | Default bitrate | Raw NV12 (for comparison) |
|---|---|---|
| 1280×720 @ 30 | 6 Mbps | 332 Mbps |
| 1920×1080 @ 30 | 12 Mbps | 746 Mbps |
| 1920×1080 @ 60 | 20 Mbps | 1.5 Gbps |
| 2560×1440 @ 30 | 20 Mbps | 1.3 Gbps |
| 3840×2160 @ 30 | 40 Mbps | 3.0 Gbps |
| 3840×2160 @ 60 | 70 Mbps | 6.0 Gbps |

Lightning iPhones are USB 2.0 (≈ 200–280 Mbps usable through usbmuxd), so compressed H.264 is required;
raw video or MJPEG at 4K doesn't fit. USB-C iPhone 15 Pro and later can use USB 3, but we don't depend on
it. A user setting can raise the bitrate up to 150 Mbps for USB-C devices.

H.264 is the default because every Windows PC can decode it in hardware. HEVC decoding on Windows needs a
separately installed Store extension, so HEVC stays an opt-in for later (M8).

## 7. Windows agent

- **Discovery:** `Listen` socket for hot-plug, then `ListDevices` at startup. The first USB device wins;
  device selection is a later option.
- **Session:** connect to device port 5321. Retry every 1 s on result 3 (app not open). Show tray state.
- **Demand-driven streaming (F6):** the agent watches the shared-memory control block. When the vcam reports
  `consumer_active = 1` with a requested media type, the agent sends `SetCamera`/`SetFormat`/`Start`. When
  it drops to 0 for 2 s, it sends `Stop`.
- **Format choice (F5):** exact match from `Caps` if available; otherwise the smallest device format that
  is ≥ the requested size in both dimensions (or the largest available), with fps = min(requested,
  max_fps). If the decoded size ≠ the requested size, scale with the Video Processor MFT (GPU) and
  letterbox when aspect ratios differ.
- **Decode:** Microsoft H.264 decoder MFT (`CLSID_MSH264DecoderMFT`) with a D3D11 device manager
  (`MFT_MESSAGE_SET_D3D_MANAGER`), `CODECAPI_AVLowLatencyMode = TRUE`, NV12 output. On decode errors,
  send `RequestKeyframe`. Map the GPU texture to a staging texture and copy it into the shared-memory slot.
- **UI:** `Shell_NotifyIcon` tray with the menu from PRODUCT.md. Logs go to `%LOCALAPPDATA%\Tethercam\logs`.

## 8. Virtual camera (Frame Server media source)

- **Registration:** `MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource,
  MFVirtualCameraLifetime_System, MFVirtualCameraAccess_AllUsers, L"Tethercam", L"{CLSID}", nullptr, 0,
  &vcam)` then `vcam->Start(nullptr)`. Run by the installer as admin; `--uninstall` calls `Remove()`.
  The COM server is registered under HKLM (`regsvr32`), and the DLL lives in
  `%ProgramFiles%\Tethercam\` so the LocalService account can load it.
- **COM objects** (patterned on Microsoft's documentation and the MIT-licensed VCamSample):
  - `Activator`: `IMFActivate`, creates the source.
  - `MediaSource`: `IMFMediaSourceEx`, `IMFGetService`, `IKsControl` (returns
    `HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND)` for unsupported properties), with an event queue.
    One stream, presentation descriptor with the F4 media types.
  - `MediaStream`: `IMFMediaStream2`, `IKsControl`. Stream attributes: `MF_DEVICESTREAM_STREAM_CATEGORY =
    PINNAME_VIDEO_CAPTURE`, `MF_DEVICESTREAM_STREAM_ID = 0`, `MF_DEVICESTREAM_FRAMESERVER_SHARED = 1`,
    `MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES = MFFrameSourceTypes_Color`.
  - `RequestSample` → take the newest complete slot from shared memory (or the placeholder if the
    newest is older than 500 ms, or there's no agent) → copy into an `IMFSample` (from an
    `IMFVideoSampleAllocatorEx` when the pipeline provides a D3D manager, otherwise a memory buffer) → set
    sample time from the QPC clock → `MEMediaSample`.
- **Placeholder:** generated NV12 frame (dark background with "Connect your iPhone and open Tethercam"),
  rendered once per media type with Direct2D/DirectWrite into a cached buffer.

## 9. Shared memory IPC

Created by **the vcam DLL** inside FrameServer (LocalService has `SeCreateGlobalPrivilege`; a normal user
process doesn't), and opened by the agent. Names live in the `Global\` namespace because the service is in
session 0 and the agent is in the user's session.

- `Global\Tethercam.Frames.v1`: file mapping, sized for 3 × 4K NV12 slots + header (~38 MB).
- `Global\Tethercam.FrameReady.v1`: auto-reset event signalled by the agent after each frame.
- `Global\Tethercam.Control.v1`: auto-reset event signalled by the vcam when the requested format or
  consumer state changes.
- **DACL (SDDL):** `D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GRGWGX;;;IU)`: SYSTEM and LocalService full access,
  interactive users read/write/execute (execute includes SYNCHRONIZE, needed to wait on the events). Any logged-in user can supply frames. That's acceptable for a webcam, and
  the docs say so.

The header layout (versioned, seqlock per slot) is in [DESIGN.md § Shared memory](DESIGN.md#shared-memory-layout).

If the DLL hasn't been loaded (no app has opened the camera), the mapping doesn't exist yet. The agent
polls for it every 500 ms, so it's idle exactly when no consumer exists.

## 10. Security and privacy

- No outbound network. The only socket the agent opens is to 127.0.0.1:27015.
- The iOS listener binds to loopback only (N5).
- The vcam DLL parses nothing from the network. It only copies NV12 bytes with bounds-checked sizes from a
  header it validates (magic, version, dimensions ≤ 4096×2160, offsets inside the mapping).
- All protocol parsing has length checks and is covered by fuzz-style unit tests.

## 11. Risks

| Risk | Impact | Mitigation / spike |
|---|---|---|
| R1 Global shared-memory permissions between FrameServer (session 0) and the user agent | Blocks the architecture | M1 spike. Fallback: the agent registers a tiny companion Windows service that brokers the section, or frames go over a named pipe. **2026-10-12:** the DACL needs `GX` for interactive users (SYNCHRONIZE on the events); fixed and verified in the session namespace. Cross-session (`Global\`) still to verify with the camera registered |
| R2 FrameServer can't load the DLL (path ACLs, COM registration, signing) | Camera doesn't appear | M1 spike with a static-pattern source; install under Program Files; check with Event Viewer → Camera Frame Server logs. **2026-10-12:** a per-user (HKCU) COM registration is not enough: `IMFVirtualCamera::Start` fails with `0x80070003`. Machine-wide (HKLM) registration, i.e. admin, is required (the MSI does this) |
| R3 Apple Devices app (Store) doesn't expose usbmuxd on 27015 the same way iTunes does | No transport | **Resolved 2026-10-12:** Apple Devices 1.1540 (`AppleMobileDeviceProcess.exe`) serves the usbmuxd plist protocol on 127.0.0.1:27015; `ListDevices` works. Note: a phone that isn't trusted/unlocked shows up only as `ConnectionType=Network` |
| R4 Some apps reject NV12-only cameras | Missing in some apps | Also offer YUY2 (CPU conversion) if testing shows a need |
| R5 Thermal throttling at 4K60 | Dropped fps | Thermal policy (§5); recommend a phone holder without a case; measure in M5 |
| R6 iOS kills capture when the screen locks or the app is backgrounded | Video stops | Keep the screen on, dim the preview; clear "Paused on iPhone" placeholder; fast resume |
| R8 v4l2loopback is an out-of-tree kernel module (Secure Boot signing, kernel upgrades) | Camera missing on Linux | Depend on the distro's `v4l2loopback-dkms`; the agent detects a missing `/dev/video*` loopback device and says how to fix it; document MOK enrolment for Secure Boot |
| R9 v4l2loopback has no per-open media-type negotiation like Frame Server | Linux can't honour F5 per app | The agent picks the output format from the tray/quality setting; consumers get that size. Scaling stays in the agent |
| R7 4K60 NV12 copy cost (~750 MB/s) | CPU/latency | Measure in M5; optimize with shared D3D11 textures (`IDXGIResource1::CreateSharedHandle`) if needed |

## 12. Open questions

| # | Question | Plan to answer |
|---|---|---|
| Q1 | Does a usbmuxd-tunnelled connection reach an `NWListener` bound to 127.0.0.1 on the device? | M1 echo spike |
| Q2 | Is `MFVirtualCameraAccess_AllUsers` + `Lifetime_System` the right combination, or should each user register their own? | Partly answered: per-user COM registration fails (see R2), so the MSI registers machine-wide with AllUsers + System lifetime. Confirm on an elevated install |
| Q3 | Which media type does each target app (Teams, Zoom, Chrome, OBS) actually pick from our list? | M4 compatibility matrix |
| Q4 | Can the agent change the stream size mid-session without the consumer re-opening the camera, e.g. on a lens switch with a different max size? | **Answered by design:** the agent always delivers the consumer's size and scales/letterboxes whatever the phone sends (implemented) |
| Q6 | How does the Linux agent learn that a client opened the camera (F6)? | **Answered 2026-10-15:** the agent scans `/proc/*/fd` every 500 ms for other processes holding the device (works on every v4l2loopback version; verified with `v4l2-ctl` on 0.15.3: start on open, stop 2 s after close). Processes in a hidden PID namespace aren't seen; `--always-on` covers that. The v4l2loopback client-usage event is a possible refinement |
| Q7 | Does `usbmuxd` on Linux handle the Trust/pairing prompt without extra tools? | Open (needs a real iPhone on Linux). The agent only needs usbmuxd's `Connect`, which works once the phone is paired; `idevicepair pair` is the documented fallback |
| Q5 | Can App Store distribution happen without a paid developer account? (Not needed for development: free accounts can sideload for 7 days.) | Before M7 |

## 13. Linux host (M9)

Same phone app, same protocol and same Status API; only the PC side changes.

| Concern | Windows | Linux |
|---|---|---|
| USB transport | Apple usbmuxd on 127.0.0.1:27015 | Open-source `usbmuxd` on the Unix socket `/var/run/usbmuxd` (same plist protocol) |
| Decode | MF H.264 MFT, D3D11 | FFmpeg `h264` with VA-API/NVDEC hwaccel (dGPU, then iGPU), software fallback, low-delay flags |
| Virtual camera | FrameServer media source DLL + shared memory | `v4l2loopback` device labelled "Tethercam"; the agent writes frames to it directly (no second process, no shm) |
| Demand signal (F6) | `consumer_active` in the control block | Other processes holding the device open (Q6) |
| Formats (F4/F5) | Negotiated per app | Agent-chosen (R9); NV12 and YUYV offered |
| UI | Win32 tray | StatusNotifierItem tray (AyatanaAppIndicator); settings in `$XDG_CONFIG_HOME/tethercam/settings`, logs in `$XDG_STATE_HOME/tethercam` |
| Autostart | Run key | systemd user unit `tethercam-agent.service` |
| Packaging | WiX MSI | `.deb` + `.rpm`; module config `options v4l2loopback exclusive_caps=1 card_label=Tethercam` |

`exclusive_caps=1` is required so Chromium/WebRTC lists the loopback device as a capture device.
Code layout: the platform-neutral core lives in `common/` (sockets and logging have POSIX paths; usbmux
reaches `/var/run/usbmuxd` by default on Linux; the status types, status JSON and status server are
shared). `linux/agent` holds the Linux-only parts; its session loop mirrors `windows/agent/engine.cpp`
(keep the two in step until a shared engine exists). Packages are built per distro release because each
links that release's FFmpeg ABI.
