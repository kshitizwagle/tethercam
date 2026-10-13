# Performance measurements

Record every measurement with the date, build, hardware and method. Newest first.

## 2026-10-13: Glass-to-glass latency (real iPhone)

**Method:** a double-buffered millisecond clock on the left half of the monitor; the iPhone's back camera
films it; the Tethercam window (Release agent, 1080p30, back wide lens) shows the camera on the right
half. Full-screen screenshots show the live clock and the camera's view of it; the difference is the
end-to-end latency (exposure + 33 ms frame interval + encode + USB + decode + display), ±~16 ms for
monitor refresh and timer granularity.

| Clock | Through the camera | Latency |
|---|---|---|
| 24.368 | 24.256 | 112 ms |
| 25.787 | 25.695 | 92 ms |
| 27.196 | 27.085 | 111 ms |
| 28.583 | 28.494 | 89 ms |

**Mean ≈ 101 ms** (89–112 ms), at the 100 ms target. (Earlier samples while the phone was being moved:
115 and 125 ms.)

**Apps on the same day:** Windows Camera app showed live iPhone video while the Tethercam window was also
open (two consumers at once). Edge WebRTC (`getUserMedia`, the engine behind new Teams) got
"Tethercam (Windows Virtual Camera)" at 1920×1080, and 30.0 fps steady in a 640×480 run. Streamlabs
Desktop shows Tethercam live on both canvases.

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
