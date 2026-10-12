# Tethercam

**Use your iPhone as a webcam on Windows, over a USB cable, at the full resolution your camera can stream.
Free and open source.**

Tethercam is a clean-room, from-scratch project. It does one thing: it turns an iPhone into a standard
Windows webcam that Zoom, Teams, OBS, the Windows Camera app and browsers can use. It has no overlays, no
effects, no account, no watermark and no wireless mode. You plug in the cable and the camera shows up.

> **Status: alpha.** The Windows side (USB transport, H.264 hardware decoding, the Windows 11 virtual
> camera, tray agent, installer) is built and tested end to end with a simulated phone. The iPhone app is
> written but hasn't been compiled or run on a device yet. See [MILESTONES.md](MILESTONES.md).

## How it works

```
iPhone                                    Windows 11 PC
┌──────────────────────┐   USB cable   ┌─────────────────────────┐     ┌────────────────────────┐
│ Tethercam iOS app    │  (usbmuxd,    │ tethercam-agent.exe     │ shm │ tethercam-vcam.dll     │
│ camera → H.264 (HW)  │──TCP tunnel──▶│ H.264 decode (HW) → NV12│────▶│ Windows 11 virtual     │──▶ Zoom / Teams /
│ TCP server :5321     │               │ tray app                │     │ camera (FrameServer)   │    OBS / browser
└──────────────────────┘               └─────────────────────────┘     └────────────────────────┘
```

The camera only runs on the phone while an app on the PC has Tethercam open. The phone streams the exact
size the app asked for (up to 3840×2160 at 60 fps) when it can; otherwise the agent scales it.

## Requirements

- Windows 11 (the virtual camera API, `MFCreateVirtualCamera`, is Windows 11 only)
- Apple's USB driver: the **Apple Devices** app (Microsoft Store) or iTunes
- iPhone with iOS 16 or later, and a USB data cable
- To build the iOS app: a Mac with Xcode 16+ and [XcodeGen](https://github.com/yonaskolb/XcodeGen)
- To build the Windows side: Visual Studio 2026 (Desktop C++, Windows 11 SDK 10.0.26100+), which includes CMake

## Install (Windows)

1. Install **Apple Devices** from the Microsoft Store.
2. Run `Tethercam-<version>-x64.msi` (from a release, or build it as described below). It installs to
   `Program Files\Tethercam`, registers the "Tethercam" camera and starts the tray agent at login.
3. Install the iPhone app (build it from `ios/` for now), open it, plug in the cable and tap **Trust**.
4. Pick **Tethercam** as the camera in your video app.

The tray icon shows the state: grey = no iPhone, amber = iPhone found but the app isn't open (or it's
paused), green = connected, green with a white dot = streaming. Right-click it to switch lenses, mirror or
rotate the picture, or open the log folder (`%LOCALAPPDATA%\Tethercam\logs`).

## Build

### Windows

```
cd windows
cmake --preset vs2026-x64
cmake --build --preset debug
ctest --preset debug
```

Release and installer (needs the WiX 5 .NET tool: `dotnet tool install --global wix --version 5.0.2`):

```
cmake --build --preset release
.\installer\build.ps1
```

### iPhone (on a Mac)

```
cd ios
xcodegen
open Tethercam.xcodeproj   # set your signing team, then run on your iPhone
```

## Developing without an iPhone

`fakephone` plays the iPhone's side of the protocol over plain TCP with an H.264 test pattern:

```
build\vs2026-x64\tools\fakephone\Debug\fakephone.exe
build\vs2026-x64\agent\Debug\tethercam-agent.exe --console --tcp 127.0.0.1:5321
```

Other tools:

| Tool | Use |
|---|---|
| `tcprobe list` | Devices that Apple's USB service sees |
| `tcprobe stream [--tcp HOST:PORT] [--decode --snapshot f.bmp] [--dump f.h264]` | Talk to a phone (or fakephone) directly: stats, decode check, raw dump |
| `vcamcheck list` / `vcamcheck read --size 1920x1080 --fps 30 --save f.bmp` | Open the installed camera the way apps do |
| `vcamcheck direct --dll tethercam-vcam.dll` | Load the camera's media source in-process (no registration needed). Set `TETHERCAM_SHM_NAMESPACE=Local` for both this and the agent |

To register a development build of the camera machine-wide, run
`windows\scripts\dev-install.ps1` from an **elevated** PowerShell.

## Troubleshooting

| Symptom | Fix |
|---|---|
| Tray says "Install Apple Devices" | Install the Apple Devices app (or iTunes); Tethercam talks to the iPhone through Apple's USB service |
| iPhone shows up only over Wi-Fi (`tcprobe list` says `type=Network`) | Unlock the phone, reconnect the cable, tap **Trust This Computer**; try another cable/port (charge-only cables don't carry data) |
| Camera shows "Open Tethercam on your iPhone" | Open the app and keep it in the foreground; iOS stops the camera when the app is in the background or the phone is locked |
| "Tethercam" doesn't appear in apps | Reinstall the MSI; check Settings → Privacy & security → Camera allows desktop apps |

## Documentation

| Doc | What it covers |
|---|---|
| [docs/PRODUCT.md](docs/PRODUCT.md) | Who it's for, goals, non-goals, user experience |
| [docs/TECH_SPEC.md](docs/TECH_SPEC.md) | Requirements, architecture, formats, risks, open questions |
| [docs/DESIGN.md](docs/DESIGN.md) | Internals: processes, threads, IPC, state machines, repo layout, testing |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | Byte-level phone ↔ PC wire protocol |
| [docs/PLAN.md](docs/PLAN.md) | Implementation plan and order of work |
| [docs/perf.md](docs/perf.md) / [docs/compat.md](docs/compat.md) | Measurements and app compatibility |
| [MILESTONES.md](MILESTONES.md) | Milestones and exit criteria |
| [TODO.md](TODO.md) | Task checklist |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Clean-room policy, code style |

## License

[GPL-3.0](LICENSE). Tethercam is not affiliated with Apple, Reincubate (Camo) or any other webcam vendor.
iPhone is a trademark of Apple Inc.
