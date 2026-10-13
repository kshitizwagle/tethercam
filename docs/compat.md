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

## Recorded app checks (2026-10-13)

The prior session recorded these checks in [perf.md](perf.md). Exact app versions and some negotiated
formats were not captured, so they do not fill unmeasured cells in the matrix above:

- Windows Camera displayed live iPhone video while the Tethercam preview was also open.
- Edge WebRTC opened the camera at 1920×1080; a separate 640×480 run delivered 30.0 fps.
- Streamlabs Desktop displayed live video on both canvases (format not recorded).

Teams, Zoom, OBS Studio, Chrome and Discord still need direct checks. The Windows preview plus Camera
test is not proof that two independent virtual-camera clients work concurrently; that test remains open.

**2026-10-13, real iPhone 15 Pro Max over USB** through Frame Server: 1080p30, 1080p60 and 4K30 delivered at rate (see `perf.md`); the tray preview window shows live video.

**2026-10-13, through Frame Server** (camera installed by the MSI, fakephone as the device): the Media Foundation source reader (`vcamcheck read`, the same path apps use) gets 1080p30 (29.2 fps), 1080p60 (59.4 fps) and 4K30 (27.5 fps, Debug build) live from "Tethercam (Windows Virtual Camera)", using Frame Server's sample allocator. Real apps still to fill in above.

Earlier, without Frame Server (2026-10-12): the media source works with the Media Foundation
source reader in-process (`vcamcheck direct`) at every advertised size. Registering the camera
machine-wide needs administrator rights, which this development session didn't have.
