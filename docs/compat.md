# App compatibility

Tested with the installed virtual camera (MSI or `windows/scripts/dev-install.ps1`). Mark each cell with
the date and result, e.g. `✅ 2026-10-20` or `❌ 2026-10-20 (shows black)`.

| App | 720p30 | 1080p30 | 1080p60 | 4K30 | Notes |
|---|---|---|---|---|---|
| Windows Camera app | | | | | |
| Microsoft Teams (new) | | | | | |
| Zoom | | | | | |
| OBS Studio (Video Capture Device) | | | | | |
| Chrome / Edge (WebRTC, e.g. webcamtests.com) | | | | | |
| Discord | | | | | |

**2026-10-13, through Frame Server** (camera installed by the MSI, fakephone as the device): the Media Foundation source reader (`vcamcheck read`, the same path apps use) gets 1080p30 (29.2 fps), 1080p60 (59.4 fps) and 4K30 (27.5 fps, Debug build) live from "Tethercam (Windows Virtual Camera)", using Frame Server's sample allocator. Real apps still to fill in above.

Earlier, without Frame Server (2026-10-12): the media source works with the Media Foundation
source reader in-process (`vcamcheck direct`) at every advertised size. Registering the camera
machine-wide needs administrator rights, which this development session didn't have.
