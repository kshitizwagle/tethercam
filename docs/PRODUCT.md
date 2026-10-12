# Tethercam: Product

## Problem

An iPhone has a much better camera than almost any laptop webcam. On a Mac, Continuity Camera makes the
iPhone a webcam for free. On Windows there's no built-in way. The available apps put basic features
(higher resolutions, no watermark, wired use) behind subscriptions or add overlays and accounts nobody
asked for.

Using a phone you already own as a camera should be a basic, free feature.

## Product statement

Tethercam turns an iPhone into a standard Windows webcam over a USB cable, at the highest video
resolution the phone can stream, with as little setup and UI as possible. It's free and GPL-3.0.

## Users

- **Remote workers** on Windows who want a better picture in Teams, Zoom or Meet.
- **Streamers and creators** who want a 1080p60 or 4K source in OBS without buying a camera.
- **Tinkerers and open-source contributors** who want a transparent tool with no telemetry.

## Goals (v1)

1. **Wired only.** USB cable, through Apple's own USB driver. Reliable and low latency, with no
   pairing over the network.
2. **Full resolution.** Expose every video format the phone's camera can stream, up to 3840×2160 at
   60 fps, as standard webcam formats.
3. **Just a webcam.** It shows up as "Tethercam" in every app that lists cameras. No per-app plugins.
4. **Small UI.** iPhone: preview, lens picker, status. Windows: tray icon showing the connection state,
   plus a few settings.
5. **Respect the user.** No account, no telemetry, no watermark, no ads, no network access.

## Non-goals (v1)

- Wi-Fi or any other wireless connection
- Overlays, backgrounds, filters, beauty effects, virtual backgrounds
- Audio (iPhone as a microphone). Planned for after v1.
- Windows 10 (no virtual camera API). A DirectShow fallback might come later.
- A macOS host app (macOS already has Continuity Camera), and Android phones
- Several phones as several cameras at the same time

## User stories

- *As a Teams user*, I plug in my iPhone, open Tethercam on the phone, and pick "Tethercam" as my camera
  in Teams. It works without any settings.
- *As a streamer*, I pick "Tethercam" in OBS and choose 3840×2160 at 30 fps. The phone streams native 4K.
- *As a user with a multi-lens iPhone*, I switch between wide, ultra-wide, telephoto and front cameras from
  the phone or the tray menu.
- *As any user*, if I unplug the phone, apps keep the camera open and show a "Connect your iPhone"
  placeholder. Plugging back in resumes the video within 3 seconds.
- *As any user*, I can mount the phone in landscape or portrait and the picture stays the right way up.

## User experience flow

1. **One-time setup on Windows:** install the Apple Devices app (or iTunes) and then the Tethercam MSI.
   The installer registers the virtual camera and adds the tray agent to startup.
2. **One-time setup on iPhone:** install the Tethercam app, allow camera access, and tap "Trust This
   Computer" the first time the cable is connected.
3. **Every day:** plug in → open Tethercam on the phone (the screen stays on and dims to save power) → the
   tray icon turns green → choose "Tethercam" in your video app.

### Windows tray menu

- Status line: `Connected: <device name>, 1920×1080 @ 30 fps` / `Waiting for iPhone` / error text
- Lens: Wide / Ultra Wide / Telephoto / Front (only the lenses the phone has)
- Mirror preview (front camera) · Rotate
- Preferred quality: Auto (match what the app asks for) / Max
- Open log folder · About · Quit

### iPhone app

- Full-screen camera preview with a status pill (`Waiting for PC` / `Streaming 4K30 · 45 Mbps`)
- Lens switcher, preview dim/blank toggle (saves battery and heat)
- No settings screens in v1 beyond these

## Success metrics

- Works in the Windows Camera app, Teams, Zoom, OBS, Chrome and Edge.
- Glass-to-glass latency: < 100 ms at 1080p30, < 150 ms at 4K30.
- Reconnects within 3 s after replug or phone unlock.
- 1-hour session at 1080p30 with no dropped connection and no growth in agent memory.
- Install to first frame in under 5 minutes for a new user following the README.
