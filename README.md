# Tethercam

**Use your iPhone as a webcam on Windows 11, over a USB cable, at up to 4K. Free and open source.**

**[Project page →](https://kshitizwagle.com.np/projects/tethercam)**

Tethercam turns an iPhone into a standard Windows webcam that Teams, Zoom, OBS, Streamlabs, browsers
and the Camera app can use. No account, no watermark, no Wi-Fi, no overlays: plug in the cable, open
the app, pick **Tethercam (Windows Virtual Camera)**.

> **Status: beta.** Works end to end with a real iPhone 15 Pro Max: 1080p30/60 and 4K30 through the
> Windows camera stack, ~100 ms glass-to-glass latency at 1080p30, hardware encode and decode.
> See [MILESTONES.md](MILESTONES.md) for what's still open.

## Get started

1. **PC:** install [Apple Devices](https://apps.microsoft.com/detail/9np83lwlpz9k) from the Microsoft
   Store, then the latest `Tethercam-<version>-x64.msi` from
   [Releases](https://github.com/kshitizwagle/tethercam/releases/latest).
2. **iPhone:** in AltStore add the source
   `https://raw.githubusercontent.com/kshitizwagle/tethercam/main/altstore/source.json` and install
   Tethercam (or sideload `Tethercam-unsigned.ipa` from the release).
3. Plug in the iPhone, tap **Trust**, open Tethercam, and choose
   **Tethercam (Windows Virtual Camera)** in your app.

Full instructions: **[User guide](docs/USER_GUIDE.md)**. Streaming and call apps:
**[STREAMING.md](docs/STREAMING.md)**.

## Features

- Standard webcam formats from 640×480 to 3840×2160 at 30/60 fps, matched or scaled from the phone;
  sustained 4K60 is still being validated (see [measurements](docs/perf.md))
- All lenses: ultra wide, wide, telephoto, front; switch from the iPhone, the tray or the Tethercam window
- Capture quality on both apps: Auto, 1080p30/60 or 4K30/60, synchronized and remembered per phone/PC
- iPhone app styled like the Camera app; on Windows, a tray icon and a live Tethercam window with the same controls
- Picture stays upright whichever way the phone is held; mirror and rotate on top
- The phone camera only runs while something is using it; lens and settings are remembered
- Outputs black (not a message) while offline, plus a local [status API](docs/STATUS_API.md) so stream
  overlays can show their own "camera off" design
- No network traffic: USB only, loopback only

## How it works

```
iPhone                                    Windows 11 PC
┌──────────────────────┐   USB cable   ┌─────────────────────────┐     ┌────────────────────────┐
│ Tethercam iOS app    │  (usbmuxd,    │ tethercam-agent.exe     │ shm │ tethercam-vcam.dll     │
│ camera → H.264 (HW)  │──TCP tunnel──▶│ H.264 decode (HW) → NV12│────▶│ Windows 11 virtual     │──▶ Teams / Zoom /
│ loopback :5321       │               │ tray, window, status API│     │ camera (Frame Server)  │    OBS / browser
└──────────────────────┘               └─────────────────────────┘     └────────────────────────┘
```

## Documentation

| Doc | For |
|---|---|
| [User guide](docs/USER_GUIDE.md) | Install, daily use, controls, settings, troubleshooting, FAQ |
| [Streaming & apps](docs/STREAMING.md) | OBS, Streamlabs, Teams, Zoom, browsers; hiding the cam when offline |
| [Status API](docs/STATUS_API.md) | `http://127.0.0.1:4790` for overlays and tools |
| [Development](docs/DEVELOPMENT.md) | Building, tools, testing without an iPhone, CI and releases |
| [Product](docs/PRODUCT.md) · [Tech spec](docs/TECH_SPEC.md) · [Design](docs/DESIGN.md) · [Protocol](docs/PROTOCOL.md) | Goals, architecture, internals, wire format |
| [Performance](docs/perf.md) · [Compatibility](docs/compat.md) | Measurements and app checks |
| [Plan](docs/PLAN.md) · [Milestones](MILESTONES.md) · [TODO](TODO.md) | Project status |
| [Contributing](CONTRIBUTING.md) | Clean-room policy, code style |

## License

[GPL-3.0](LICENSE). Tethercam is an independent, clean-room project built on public Apple and
Microsoft APIs. It is not affiliated with Apple, Reincubate (Camo) or any other webcam vendor.
iPhone is a trademark of Apple Inc.
