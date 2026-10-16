# Performance measurements

Record every measurement with the date, build, hardware and method. Newest first.

## 2026-10-16: Under load, Windows host through Frame Server (fakephone)

**Setup:** `windows\scripts\load-test.ps1` on the 16-thread desktop (RX 9070 XT, hardware decode), Release
build of the M10 tree registered with `dev-install.ps1`: `fakephone --port 5321` → `tethercam-agent
--console --tcp 127.0.0.1:5321` → Frame Server → `vcamcheck read` 1920x1080@60, idle 8 s then 24 burner
threads for 20 s. "New fps" counts frames whose picture changed (the camera repeats the last frame when a
new one is late, so its sample rate always reads ~60), skipping the first second as start-up.

| Run | Idle new fps | load24 new fps | load24 loss | load24 gaps > 50 ms | Sample clock |
|---|---|---|---|---|---|
| 1 | 42.4 | 50.7 | 15.5 % | 2.6 % (longest 76 ms) | 60.0 / 60.0 |
| 2 | 52.9 | 52.0 | 13.3 % | 2.5 % (longest 71 ms) | 60.1 / 59.6 |

**Misses the M10 target** (≤ 10 % loss at load24; idle should be ~60), but not because of load: idle is
as bad and varies run to run. The vcam log shows repeats on 10–26 % of samples at idle (in-process
`vcamcheck direct`: ~1.5 %, 59.2 new fps, longest gap 34 ms). The vcam paces samples on its own 60 Hz
timer, independent of frame arrival; when the two 60 Hz clocks are near phase, a few ms of arrival
jitter beyond the 2 ms grace turns into repeat + skip pairs. Long stalls under load are rare (2.5 %
gaps > 50 ms vs 7.0 % on Linux). Next: pace delivery from frame arrival (risk R10).

## 2026-10-16: Under load and during stalls, Linux host (fakephone)

**Setup:** the M10 load harness: `fakephone --port 5321` → `tethercam-agent --tcp 127.0.0.1:5321
--device /dev/video42 --no-tray --always-on` (1080p60, YUYV) → `v4l2probe /dev/video42`, 20 s per run,
24 busy-loop burners on the 16-thread desktop. Baseline = the build before the M10 changes; runs were
interleaved (base, new, base, new) to cancel machine drift.

| Build | Delivered | Gaps > 50 ms |
|---|---|---|
| baseline | 30.11, 32.51 fps (mean 31.3) | 11.2 %, 8.3 % (mean 9.8 %) |
| M10 | 34.28, 33.98 fps (mean 34.1) | 7.5 %, 6.5 % (mean 7.0 %) |

Idle (no load): 60.00 fps on both, so the difference is load-specific (SSE2 YUYV pack, two-thread
resample, non-blocking writes that drop a frame instead of stalling the session).

**Stall policy:** `SIGSTOP` on `fakephone` for 1.8 s mid-stream, mean luma of every frame read from the
loopback device: the last live frame (luma 56) is held for ~0.85 s after the source stops, then black
(luma 16) until the stall exceeds the 750 ms window; live frames resume ~0.1 s after `SIGCONT`. Before
the change, the first 100 ms of any stall already went black.

**Windows:** measured the same day, see the section above.

## 2026-10-13: Glass-to-glass latency in Streamlabs (app 0.1.10, agent 0.1.11)

Front camera, 1080p60 (Streamlabs source 1920x1080, buffering disabled), original Apple cable into a
motherboard port. A millisecond clock on screen, the phone filming it, and screenshots of the clock next
to the Streamlabs preview: **131, 133, 135, 143 ms** (steady, no drift). Includes camera exposure and
monitor scan-out. The perceived "lag" was A/V and game sync: a USB mic and Game Capture are near-instant,
so Streamlabs needs a ~135 ms Render Delay on Game Capture and Sync Offset on Desktop Audio and Mic.
VDO.Ninja not yet measured the same way (it carries the phone's mic in sync with its video).
A longer third-party USB cable caused repeated USB re-enumeration (device id changes, drops to
"waiting for iPhone"); the original cable is stable.

## 2026-10-13: 4K60 for 30 minutes (app 0.1.10, thermal test)

`tcprobe stream --size 3840x2160 --fps 60 --decode` for 1800 s, back wide lens, iPhone 15 Pro Max on
a stand, charging over USB. Phone status sampled every 2 s (900 samples).

| Time | iOS thermal state | Delivered |
|---|---|---|
| 0–7.6 min | 0 nominal | ~50 fps, ~68 Mbps |
| 7.6–18.3 min | 1 fair | ~50 fps, ~68 Mbps |
| 18.3–30 min | 2 serious | **30 fps, ~35 Mbps**: the app's thermal policy (half bitrate, ≤30 fps) engaged as designed; never reached critical |

75,666 frames, 12.3 GB, hardware decode, **0 decode errors**, no disconnects. The 0.1.9 encoder change
raised 4K60 from ~42 to ~50 fps; full 60 fps at 4K isn't reached on this phone. **Conclusion:**
sustained 4K60 isn't viable on this phone (it throttles to 30 fps after ~18 min);
recommend 4K30 or 1080p60 for long streams.

## 2026-10-13: Soak attempt with Streamlabs (real iPhone)

Release agent, Streamlabs Desktop consuming 1080p30 from "Tethercam (Windows Virtual Camera)", front
camera, phone charging. Sampled `/status` and agent memory every 30 s.

| Window | Result |
|---|---|
| 10:23–10:35 (12 min) | live, 29–31 fps, thermal 0 (nominal), battery 100 %, agent private memory 101.4–101.7 MB (flat) |
| 10:35–10:38 | connected, no consumer (Streamlabs released the camera) |
| 10:38–10:49 | iPhone gone from USB (unplugged or locked); sampler stopped at 10:49 |

Not a valid 1-hour soak: rerun with the phone left in place.

## 2026-10-13: 4K60 from the iPhone (tcprobe, app 0.1.4)

`tcprobe stream --size 3840x2160 --fps 60 --decode` over USB, back wide lens, phone charging.
Ran ~160 s (6549 frames, 1.31 GB) until the installed agent was restarted and took over the phone.

| Measure | Result |
|---|---|
| Delivered frame rate | **~41.5 fps** (phone-reported and PC-received agree), not 60 |
| Bitrate | ~66 Mbps (70 Mbps requested) |
| PC decode | hardware, 41–42 fps, **0 decode errors** |
| Phone thermal state | 0 (nominal) throughout; battery 100 %, charging |

The cap is on the phone. Likely cause: VideoToolbox low-latency rate control, which removes encoder
pipelining. App 0.1.9 uses low-latency mode only up to 1440p and the normal real-time encoder (no
B-frames, max one frame of delay) for 4K. Re-measure 4K60 and a 30-minute thermal run with 0.1.9.

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
