# Using Tethercam in apps and on stream

Tethercam appears in apps as **Tethercam (Windows Virtual Camera)**. The setup instructions below cover
common Windows video apps. Camera, Edge WebRTC and Streamlabs have recorded live-video checks;
Teams, Zoom and OBS Studio still need direct validation. See [compat.md](compat.md).

## Streamlabs Desktop and OBS Studio

1. **Sources → + → Video Capture Device**, name it e.g. `Camera (Tethercam)`.
2. **Device:** `Tethercam (Windows Virtual Camera)`.
3. **Resolution/FPS type:** Custom → `1920x1080` and 30 or 60 fps (or `3840x2160` for 4K). The iPhone
   streams the size you pick.
4. No rotation transform is needed: the picture is already upright (unlike browser-source phone cams
   such as VDO.Ninja, which often arrive sideways).

Streamlabs Dual Output: add the source to both the horizontal and vertical canvases and crop each
copy separately. In this setup, Streamlabs scenes are managed by the streaming project
(`C:\Users\Admin\dev\streaming`), which also hides the camera automatically when it isn't live (below).

## Hide the camera on stream when it isn't live

While the iPhone isn't streaming, the Tethercam camera outputs **black** (unless the tray's status card
is on). To show your own "camera off" design instead, use the
[status API](STATUS_API.md) at `http://127.0.0.1:4790`:

- **Browser-source overlays:** subscribe to `http://127.0.0.1:4790/events` and toggle your camera frame
  on the `live` field. A copy-paste example is in [STATUS_API.md](STATUS_API.md#overlay-example).
- **Tools/dashboards:** follow the same SSE stream and, when `live` is false, hide the camera source or
  enable a filter. The streaming dashboard does exactly this with an opacity-0 Color Correction filter
  named "Hide when no feed" on `Camera (Tethercam)`.

`live` reflects recent received frames. It becomes false after the agent detects a disconnect, pause
or lack of demand; detection is not instantaneous. The Tethercam preview window also counts as demand.

## Microsoft Teams

In Teams' device or video settings, select **Tethercam (Windows Virtual Camera)**. Edge WebRTC was
tested at 1920×1080; that does not establish Teams compatibility or its call resolution.

## Zoom

In Zoom's video settings, select **Tethercam (Windows Virtual Camera)**. An HD setting does not
guarantee 1080p delivery; the calling app determines the transmitted resolution.

## Browsers (Chrome, Edge) and web apps (Meet, Discord web)

Pick **Tethercam (Windows Virtual Camera)** in the site's camera menu or the browser's camera settings.
Browsers ask for the format they want; 1080p is normal for most sites.

## Windows Camera app

Open **Camera** and use the switch-camera button until Tethercam shows. Good for a quick check.

## Several apps at once

Windows shares the camera between apps (for example Streamlabs and Teams at the same time). The first
app to open it chooses the resolution; later apps get the same stream.

## Tips

- Mount the phone in landscape at eye level; a small tripod or monitor clip works well.
- For the best picture, use the back cameras (`1×`, or `.5` for wide shots); the front camera is
  fine for close-ups.
- Leave the phone plugged in: it charges while streaming. At 4K60 it can get warm; Tethercam lowers
  the bitrate automatically if iOS reports high temperature.
- Keep the Tethercam app in front and the phone unlocked; the app keeps the screen awake. Use the
  shutter button to blank the phone's screen while streaming.
