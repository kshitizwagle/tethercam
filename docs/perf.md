# Performance measurements

Record every measurement with the date, build, hardware and method. Newest first.

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
