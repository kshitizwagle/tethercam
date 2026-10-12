# Tethercam

**Use your iPhone as a webcam on Windows, over a USB cable, at the full resolution your camera can stream.
Free and open source.**

Tethercam is a clean-room, from-scratch project. It does one thing: it turns an iPhone into a standard
Windows webcam that Zoom, Teams, OBS, the Windows Camera app and browsers can use. It has no overlays, no
effects, no account, no watermark and no wireless mode. You plug in the cable and the camera shows up.

> **Status: pre-alpha, planning stage.** Nothing works yet. See [MILESTONES.md](MILESTONES.md) and
> [TODO.md](TODO.md) for progress.

## How it works

```
iPhone                                    Windows 11 PC
┌──────────────────────┐   USB cable   ┌─────────────────────────┐     ┌────────────────────────┐
│ Tethercam iOS app    │  (usbmuxd,    │ tethercam-agent.exe     │ shm │ tethercam-vcam.dll     │
│ camera → H.264 (HW)  │──TCP tunnel──▶│ H.264 decode (HW) → NV12│────▶│ Windows 11 virtual     │──▶ Zoom / Teams /
│ TCP server :5321     │               │ tray app                │     │ camera (FrameServer)   │    OBS / browser
└──────────────────────┘               └─────────────────────────┘     └────────────────────────┘
```

## Requirements (planned)

- Windows 11 (the virtual camera API, `MFCreateVirtualCamera`, is Windows 11 only)
- Apple's USB driver: the **Apple Devices** app (Microsoft Store) or iTunes
- iPhone with iOS 16 or later, and a USB cable
- To build the iOS app: a Mac with Xcode 16+ and [XcodeGen](https://github.com/yonaskolb/XcodeGen)
- To build the Windows side: Visual Studio 2026 (Desktop C++, Windows 11 SDK 10.0.26100+), which includes CMake

## Documentation

| Doc | What it covers |
|---|---|
| [docs/PRODUCT.md](docs/PRODUCT.md) | Who it's for, goals, non-goals, user experience |
| [docs/TECH_SPEC.md](docs/TECH_SPEC.md) | Requirements, architecture, formats, risks, open questions |
| [docs/DESIGN.md](docs/DESIGN.md) | Internals: processes, threads, IPC, state machines, repo layout, testing |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | Byte-level phone ↔ PC wire protocol |
| [docs/PLAN.md](docs/PLAN.md) | Implementation plan and order of work |
| [MILESTONES.md](MILESTONES.md) | Milestones and exit criteria |
| [TODO.md](TODO.md) | Task checklist |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Clean-room policy, code style |

## License

[GPL-3.0](LICENSE). Tethercam is not affiliated with Apple, Reincubate (Camo) or any other webcam vendor.
iPhone is a trademark of Apple Inc.
