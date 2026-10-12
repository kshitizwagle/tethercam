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

Verified so far without Frame Server (2026-10-12): the media source works with the Media Foundation
source reader in-process (`vcamcheck direct`) at every advertised size. Registering the camera
machine-wide needs administrator rights, which this development session didn't have.
