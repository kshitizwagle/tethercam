# Performance measurements

Record every measurement with the date, build, hardware and method. Newest first.

## 2026-10-13: Real iPhone over USB

**Setup:** iPhone 15 Pro Max (`iPhone16,2`, USB-C) running Tethercam 0.1.1 (sideloaded via AltStore),
Apple Devices 1.1540, Tethercam 0.1.2 installed from the MSI, same PC as below. Consumer:
`vcamcheck read` (Media Foundation source reader on the registered "Tethercam (Windows Virtual Camera)",
i.e. the Frame Server path apps use). 8 s per run, Debug vcamcheck.

| Consumer format | Delivered | Sample clock | Notes |
|---|---|---|---|
| 1920×1080 @ 30 | 29.6 fps | 30.0 | hardware decode, ~11.4 Mbps |
| 1920×1080 @ 60 | 59.1 fps | 60.1 | 20.4 Mbps requested |
| 3840×2160 @ 30 | 28.4 fps | 29.8 | 40 Mbps requested |

The phone reported 3840×2160 @ 60 on all four lenses (wide, ultra wide, telephoto, front). Lens switching
from the tray worked (wide → front → telephoto) mid-stream. Bug found: the picture was upside down when
the phone was held in the other landscape orientation; fixed by following the phone's orientation
(RotationCoordinator on iOS 17+). Latency and thermals still to measure.

## 2026-10-12: Windows pipeline with fakephone (no iPhone yet)

**Setup:** Release build, AMD Ryzen 7 7700X (16 threads), AMD Radeon RX 9070 XT, Windows 11 build 26300.
`fakephone` (Media Foundation software H.264 encoder, test pattern) → `tethercam-agent --tcp`
(hardware H.264 decode) → shared memory (`TETHERCAM_SHM_NAMESPACE=Local`) → `tethercam-vcam.dll` loaded
in-process by `vcamcheck direct` (Media Foundation source reader). 8 s per run.

| Consumer format | Delivered | Sample clock | Agent CPU (% of whole machine) |
|---|---|---|---|
| 1280×720 @ 60 | 60.1 fps | 60.0 | 0.2 % |
| 1920×1080 @ 30 | 30.1 fps | 30.0 | 0.1 % |
| 1920×1080 @ 60 | 60.1 fps | 60.0 | 0.2 % |
| 2560×1440 @ 30 | 30.1 fps | 30.0 | 0.5 % |
| 3840×2160 @ 30 | 30.1 fps | 30.0 | 0.5 % |

**Soak (Debug build):** 1080p30 for 180 s: 5400/5400 frames at 30.0 fps; agent private memory sampled
every 10 s stayed at 91.8–92.0 MB (flat).

**Recovery:** agent killed while an app was reading the camera, restarted 4 s later: the camera kept
delivering 30 fps (placeholder while the agent was gone), and live frames resumed within ~0.1 s of the
agent restarting, without the app reopening the camera.

**Not measured yet (needs an iPhone):** glass-to-glass latency (target < 100 ms at 1080p30, < 150 ms at
4K30), USB throughput (Lightning vs USB-C), 4K60, thermals. Method for latency: film a millisecond clock
on the PC screen with the iPhone, show Tethercam next to it in the Camera app, screenshot, subtract.
