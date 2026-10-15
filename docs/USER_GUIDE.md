# Tethercam user guide

Tethercam turns your iPhone into a webcam for Windows 11 or Linux over a USB cable. Video apps see it as a normal
camera called **Tethercam (Windows Virtual Camera)**. Nothing goes over Wi-Fi or the internet: the video
travels through the cable only.

- [What you need](#what-you-need)
- [Install](#install)
- [Everyday use](#everyday-use)
- [The iPhone app](#the-iphone-app)
- [On Windows: tray icon and Tethercam window](#on-windows-tray-icon-and-tethercam-window)
- [Resolution and quality](#resolution-and-quality)
- [Settings](#settings)
- [Updating](#updating)
- [Uninstalling](#uninstalling)
- [Troubleshooting](#troubleshooting)
- [FAQ](#faq)
- [Linux](#linux)

For OBS, Streamlabs, Teams, Zoom and browser setup, see [STREAMING.md](STREAMING.md).

## What you need

| | |
|---|---|
| PC | Windows 11, 64-bit |
| Apple's USB driver | The **Apple Devices** app from the Microsoft Store (or iTunes) |
| iPhone | iOS 16 or later. Tested on iPhone 15 Pro Max |
| Cable | A USB **data** cable (charge-only cables don't work) |
| To install the iPhone app | [AltStore](https://altstore.io) or [Sideloadly](https://sideloadly.io) and a (free) Apple ID |

## Install

### 1. Windows

1. Install **Apple Devices** from the Microsoft Store and open it once.
2. Download `Tethercam-<version>-x64.msi` from the
   [latest release](https://github.com/kshitizwagle/tethercam/releases/latest) and run it.
   It needs administrator rights once: it registers the camera with Windows.
3. Tethercam starts in the tray (notification area). It starts automatically when you sign in.

### 2. iPhone

Tethercam isn't on the App Store yet, so you install it yourself. AltStore is easiest because it also
keeps the app updated:

1. Install AltServer on your PC and **AltStore Classic** on your iPhone using the
   [official Windows installation guide](https://faq.altstore.io/altstore-classic/how-to-install-altstore-windows).
   Follow its iTunes and iCloud prerequisites as well as Tethercam's USB-driver requirement.
2. In AltStore on the iPhone: **Browse → Sources → +** and add
   `https://raw.githubusercontent.com/kshitizwagle/tethercam/main/altstore/source.json`
3. Open the Tethercam source and tap **Get** / **Install**.

Without AltStore: download `Tethercam-unsigned.ipa` from the
[latest release](https://github.com/kshitizwagle/tethercam/releases/latest) and install it with
Sideloadly.

> With a free Apple ID, sideloaded apps stop opening after 7 days until they're refreshed. AltStore
> can refresh them through AltServer over USB or the same Wi-Fi network. For a wired setup, keep
> AltServer running, connect the phone by USB, and use **Refresh All** in AltStore before expiry.
> See [AltServer's connection requirements](https://faq.altstore.io/altstore-classic/altserver).

### 3. First connection

1. Plug the iPhone into the PC with the cable and unlock it.
2. If the iPhone asks **Trust This Computer?**, tap **Trust** and enter your passcode.
3. Open **Tethercam** on the iPhone and allow camera access.
4. On the PC, pick **Tethercam (Windows Virtual Camera)** as the camera in your video app.

## Everyday use

1. Plug in the iPhone and open the Tethercam app. Keep it in front: iOS stops the camera when the app is
   in the background or the phone is locked. The app keeps the screen awake for you.
2. Choose **Tethercam (Windows Virtual Camera)** in your app.

The iPhone camera only streams while an app on the PC is using Tethercam (or the Tethercam window is
open). It stops about 2 seconds after the last app lets go, which saves battery and heat. The phone
charges over the cable while it streams.

Mount the phone in landscape for a normal 16:9 picture. The picture stays upright whichever way you hold
the phone. In portrait you get a tall picture with black bars at the sides.

## The iPhone app

The screen is laid out like the iPhone Camera app:

| Control | What it does |
|---|---|
| Status pill (top) | Grey `Waiting for PC`, green dot `Connected`, red **LIVE** with resolution, fps and bitrate while streaming |
| **Quality** (below status) | Auto, 1080p at 30/60 fps, or 4K at 30/60 fps; unsupported choices are disabled for the current lens |
| Zoom pills (`.5` `1×` `5`) | Switch between the back lenses: ultra wide, wide, telephoto. The numbers are your phone's real zoom factors |
| **WEBCAM** | The mode label (there is only one mode) |
| Mirror (bottom left) | Flip the picture horizontally. Yellow when on |
| Shutter (centre) | Hide or show the preview on the phone to save battery and heat. Streaming continues. The inner shape turns into a red square while live |
| Switch camera (bottom right) | Front ↔ back. Going back returns to the last back lens you used |

The app remembers your lens. The camera preview runs whenever the app is open, even when the PC isn't
using it, so you can frame the shot first.

## On Windows: tray icon and Tethercam window

### Tray icon

The tray icon is the Tethercam app icon with a small status badge:

| Badge | Meaning |
|---|---|
| Grey | No iPhone on USB |
| Amber | iPhone found but the app isn't open, or it's paused (background/locked), or Apple Devices is missing |
| Green | Connected, no app using the camera |
| Red | Live: an app is using the camera |

**Click** the icon to open the Tethercam window. **Right-click** it for the menu:

- Status and current format
- **Open Tethercam**
- Lens list (all lenses on your phone)
- **Mirror**, **Rotate** (0°, 90°, 180°, 270°)
- **Show status card while the camera is offline** (see [Settings](#settings))
- Open log folder, version, **Quit**

### Tethercam window

A live view of the camera with controls that fade out when you leave the mouse still:

| Control | What it does |
|---|---|
| **LIVE** chip (top left) | Resolution, frame rate and bitrate |
| **Quality** (below LIVE) | Choose capture resolution and frame rate; also in the tray's **Capture quality** menu. Press **Q** or **F10** in the window to open it with the keyboard |
| Mirror, Rotate (bottom left) | Same as the tray menu. Rotate turns the picture a further 90° each click |
| Lens pills (bottom centre) | Back lenses; the highlight slides to the one in use |
| Switch camera (bottom right) | Front ↔ back |

While the window is open the iPhone streams even if no app is using Tethercam. Closing it hides it
(Tethercam keeps running in the tray).

Lens and mirror changes are synchronized between the Windows controls and the iPhone app. The Windows
Rotate control adds a rotation adjustment to the phone's automatic orientation handling.

## Resolution and quality

**Quality controls what the phone captures.** Choose **Auto**, **1080p / 30 fps**, **1080p / 60 fps**,
**4K / 30 fps**, or **4K / 60 fps** on either the phone or PC. Update both apps to a build with quality
controls for synchronization. The iPhone 15 Pro Max's front camera can offer the 4K presets too.

**Auto** follows the video app's requested format (1080p30 when only the Windows preview is open).
An explicit preset overrides capture quality. Video apps still choose their output: selecting 4K60
in Tethercam does not force a 1080p30 call to transmit 4K60. For full 4K60 output, select it in your
video app too. The LIVE readout shows actual source size and measured frame rate.

The setting is remembered and synchronized. On reconnect, the phone's saved choice wins unless you
have a pending change made on the PC. If you switch to a lens that cannot support the remembered
preset, capture falls back to an available size/frame rate; the preference stays selected. 4K uses
more power and can warm the phone. While disconnected, the phone's local framing preview stays at
up to 1080p30; the preset applies when the PC requests a stream.

Tethercam offers output formats of 640×480, 1280×720, 1920×1080, 2560×1440 and 3840×2160, each at
30 or 60 fps, and scales/letterboxes the captured picture as needed. Video is H.264, hardware-encoded
on the phone; the PC prefers hardware decoding and can fall back
to software decoding.

| Format | Bitrate |
|---|---|
| 1080p30 | ~12 Mbps |
| 1080p60 | ~20 Mbps |
| 4K30 | ~40 Mbps |
| 4K60 | ~70 Mbps |

Measured on an iPhone 15 Pro Max ([perf.md](perf.md)): 1080p30 and 1080p60 at full rate, 4K30 at
~28–30 fps, glass-to-glass latency ~100 ms at 1080p30. If the phone gets hot, it halves the bitrate and
caps at 30 fps, and stops at "critical" temperature.

4K60 is offered when supported by the phone, but sustained 60 fps is not yet verified. The recorded
0.1.4 test delivered about 41.5 fps; the encoder change in 0.1.9 still needs a device retest.

## Settings

Tethercam saves your choices per Windows user in `HKEY_CURRENT_USER\Software\Tethercam`:

| Value | Set from | Default |
|---|---|---|
| `Camera` | Lens you last picked (PC or iPhone) | Back wide |
| `Mirror` | Mirror | Off |
| `Rotation` | Rotate | 0 |
| `StatusCard` | Tray: Show status card while the camera is offline | Off |
| `Quality` | Quality menu (PC or phone); 0 Auto, 1 1080p30, 2 1080p60, 3 4K30, 4 4K60 | Auto |

**Status card:** while the iPhone isn't streaming (not connected, app closed, phone locked), the camera
shows plain **black** by default, so your stream or call never shows a Tethercam message. Turn the status
card on to see a hint instead ("Connect your iPhone", "Open Tethercam on your iPhone", …), which helps
in video calls.

## Updating

- **Windows:** install the newer MSI over the old one. Quit Tethercam from the tray first, or restart
  it from the Start menu afterwards.
- **iPhone:** AltStore shows updates from the Tethercam source. With Sideloadly, sideload the new IPA.

Every build of `main` that passes CI is published as the latest release.

## Uninstalling

Windows: **Settings → Apps → Installed apps → Tethercam → Uninstall**. This removes the camera, the
app and the startup entry. Your settings key (`HKCU\Software\Tethercam`) and logs
(`%LOCALAPPDATA%\Tethercam`) stay; delete them by hand if you want.

iPhone: delete the app like any other.

## Troubleshooting

Logs: tray → **Open log folder** (`%LOCALAPPDATA%\Tethercam\logs\agent.log`).

| Symptom | Fix |
|---|---|
| Tray badge amber, "Install Apple Devices" | Install **Apple Devices** from the Microsoft Store (or iTunes). Tethercam talks to the iPhone through Apple's USB service |
| Tray badge grey although the phone is plugged in | Unlock the phone, unplug and replug, tap **Trust**. Try another cable or port: charge-only cables don't carry data |
| Amber, "Open Tethercam on your iPhone", but the app is open | Bring the app to the front and unlock the phone. On app 0.1.8 or older, swipe the app away and reopen it (fixed in 0.1.9) |
| Black picture in an app | The iPhone isn't streaming: check the tray badge. Turn on the status card to see why |
| "Tethercam" isn't in an app's camera list | Look for **Tethercam (Windows Virtual Camera)**. Restart the app. Check **Settings → Privacy & security → Camera** allows desktop apps. Reinstall the MSI |
| Another phone-webcam app (e.g. Camo) is picked instead | Choose **Tethercam (Windows Virtual Camera)** explicitly |
| Picture sideways or upside down | Rotation follows the phone automatically; use **Rotate** in the tray or window to adjust |
| Tethercam opens on the iPhone but closes right away | The free-Apple-ID signature expired (7 days). Refresh it in AltStore |
| Two apps show different sizes | Windows shares one camera stream between apps: the first app to open the camera picks the format |

## FAQ

**Does it work over Wi-Fi?** No, by design: USB only. It's more reliable, has lower latency and keeps
the phone charged.

**Does it send anything to the internet?** No. The PC talks to the phone through Apple's local USB
service, and the iPhone app only accepts connections from that cable.

**Audio?** Not yet. Use your PC's microphone.

**Windows 10?** No: the virtual camera API Tethercam uses is Windows 11 only.

**Is it related to Camo?** No. Tethercam is an independent, clean-room, GPL-3.0 project built only on
public Apple and Microsoft APIs.

## Linux

Linux support is new (beta): it has been tested end to end with the simulated phone and with
Ubuntu 24.04, 26.04 and Fedora builds, but not yet with a real iPhone on Linux. Reports welcome.

### What you need

| | |
|---|---|
| Distro | Ubuntu 24.04 or 26.04 (`.deb`), Fedora (`.rpm`). Others: build from source ([DEVELOPMENT.md](DEVELOPMENT.md)) |
| Virtual camera | `v4l2loopback` kernel module (`v4l2loopback-dkms` on Ubuntu; `v4l2loopback` from RPM Fusion on Fedora) |
| iPhone USB service | `usbmuxd` (and `libimobiledevice-utils` for `idevicepair` if pairing needs help) |
| Tray icon | KDE Plasma, or GNOME with the *AppIndicator and KStatusNotifierItem Support* extension (Ubuntu has it on by default) |

### Install

Ubuntu:

```bash
sudo apt install v4l2loopback-dkms usbmuxd libimobiledevice-utils
sudo apt install ./Tethercam-<version>-ubuntu<release>-amd64.deb
```

Fedora (enable [RPM Fusion](https://rpmfusion.org/Configuration) first for v4l2loopback):

```bash
sudo dnf install v4l2loopback usbmuxd libimobiledevice-utils
sudo dnf install ./Tethercam-<version>-fedora-x86_64.rpm
```

The package loads the module with `card_label=Tethercam exclusive_caps=1`
(`/usr/lib/modprobe.d/tethercam.conf`) and enables the `tethercam-agent` user service for every user. Log
out and in once, or start it now with `systemctl --user start tethercam-agent`. Then plug in the iPhone,
tap **Trust**, open Tethercam on the phone, and choose **Tethercam** in your video app.

With **Secure Boot** on, the DKMS module must be signed: during the install you're asked to set a MOK
password; reboot and choose **Enroll MOK** on the blue screen. Until then `modprobe` fails with "Key was
rejected by service".

### How it differs from Windows

- **One format for all apps.** v4l2loopback can't ask each app what it wants, so the camera has one
  format: 1080p30 by default, or what you choose under **Capture quality** in the tray (4K30, 1080p60, ...).
  The format changes while no app is using the camera. Output is YUYV (`--nv12` for NV12).
- **Demand detection** works by noticing which processes have the camera open. Apps in containers that
  hide their processes (some Snap/Flatpak setups) may not be seen; run the agent with `--always-on` then
  (`systemctl --user edit tethercam-agent`, add `ExecStart=` and `ExecStart=/usr/bin/tethercam-agent --always-on`).
- **Preview.** Choose **Open Tethercam** in the tray menu (or start the agent with `--preview`) to see the
  camera in a window, with lens, mirror, rotate and quality controls. While it's open the iPhone keeps
  streaming even if no app uses the camera.
- **Settings** are in `~/.config/tethercam/settings`, **logs** in `~/.local/state/tethercam/agent.log`.
- The status API is the same: `http://127.0.0.1:4790`.

### Troubleshooting (Linux)

| Symptom | Fix |
|---|---|
| Tray: "No virtual camera (load v4l2loopback)" | `sudo modprobe v4l2loopback`; check `v4l2-ctl --list-devices` shows **Tethercam**. Secure Boot: see above |
| No camera in Chrome/Chromium | The module needs `exclusive_caps=1` (set by the package). If v4l2loopback was already loaded with other options: `sudo modprobe -r v4l2loopback && sudo modprobe v4l2loopback` |
| Using OBS's virtual camera too | Give v4l2loopback two devices in `/etc/modprobe.d/`: `options v4l2loopback devices=2 card_label=Tethercam,OBS exclusive_caps=1,1` |
| "Waiting for iPhone" with the phone plugged in | Unlock it and tap **Trust**. Check `idevice_id -l` lists it; if not, `idevicepair pair`. `systemctl status usbmuxd` should be active while the phone is connected |
| Black picture | The phone isn't streaming yet: check the tray status. If the app isn't detected as using the camera, try `--always-on` (above) |
| "software decode" in the tray detail | VA-API isn't available: install your GPU's VA driver (`va-driver-all` on Ubuntu, `mesa-va-drivers`/`intel-media-driver` on Fedora) |
| `tethercam-agent is already running` | The user service is running; use `systemctl --user restart tethercam-agent` |

Uninstall with `sudo apt remove tethercam` or `sudo dnf remove tethercam`.
