# TODO

Work from the top. Tick items in the same commit that finishes them. Milestone exit criteria live in
[MILESTONES.md](MILESTONES.md).

## Next up
- [ ] M10: run the load harness on a Windows machine (target: ≤ 10 % frame loss at load24) and record it in `docs/perf.md`; the Windows build, tests and MSI are green in CI (v0.1.12)
- [ ] M9 Linux host: `sudo apt install ./linux/build/rel/Tethercam-*.deb`, log out/in, check the service + tray autostart; then a real iPhone on Linux (Q7 pairing, Firefox/Chromium/OBS/Zoom rows in compat.md)
- [x] Synchronized quality menus on iPhone, Windows preview and tray: Auto, 1080p30/60, 4K30/60; capture overrides, persistence and capability fallback
- [ ] Build the updated iPhone app on macOS/CI and validate quality switching on the iPhone 15 Pro Max front camera; local Swift protocol checks pass, UIKit/AVFoundation build pending
- [x] App installed via the AltStore source; permission prompt, preview, auto-rotation, Camera-app UI verified (0.1.4)
- [x] Real-device streaming through the virtual camera; lens memory across replug
- [ ] Direct Teams, Zoom and OBS checks; complete the format matrix in `docs/compat.md` (Camera, Edge and Streamlabs spot checks recorded)
- [ ] Retest 4K60 with iPhone app 0.1.9 or newer, then record 30-minute thermals and 4K30 latency in `docs/perf.md`
- [ ] Rerun the 1-hour soak: the first attempt ran 26 min (12 min live, clean) before the setup changed (see `docs/perf.md`). PC sleep/resume and two independent camera clients remain unverified
- [x] Camera registered machine-wide; Frame Server path verified (2026-10-13)
- [x] iPhone visible over USB (`type=USB`, product 0x12a8) after Trust; usbmuxd Connect reaches the phone
- [x] GitHub repo public, CI green (Windows + iOS)

## M10: Under-load performance + GPU path
- [x] vcam: FrameReady wait + 2 ms late-frame grace, capture-clock sample times, discontinuity edges, single-copy fill
- [x] Shared memory: global frame sequence + `PeekLatest` (fresh vs repeat, exactly)
- [x] Agent: MMCSS media thread, host-clock (QPC) PTS, GPU scale via `Nv12Scaler` (`DecodedFrame` surfaces, CPU fallback)
- [x] Linux: 750 ms hold-the-frame stall policy, non-blocking v4l2 writes, SSE2 YUYV pack, two-thread resample, priority attempts
- [x] Windows build + tests + MSI green in CI (MSVC Debug/Release; v0.1.12)
- [ ] Windows load harness (≤ 10 % frame loss at load24) + Streamlabs latency spot check
- [ ] `vcamcheck` sample-time check on Windows (the plan's `--check-pts`, deferred)

## M9: Linux host
- [x] Move `windows/common` → `common/`; POSIX paths in `net`/`log`/`usbmux`; Linux CI job runs the unit tests (shm stays Windows-only)
- [x] Q6 answered (`/proc` opener scan); Q7 needs a real iPhone
- [x] `linux/agent`: usbmux over Unix socket, session loop mirroring Windows (status types/JSON/server shared)
- [x] FFmpeg decoder (VA-API + software) and NV12/YUYV writer to v4l2loopback; black frames while idle
- [x] Status API server on 4790 (moved to `common/`, contract test)
- [x] SNI tray + systemd user unit, single-instance lock
- [x] `.deb`/`.rpm` (CPack) per distro target with `v4l2loopback-dkms`/`usbmuxd` deps and modprobe config; release assets
- [x] USER_GUIDE/DEVELOPMENT/README Linux sections
- [ ] Compat matrix rows for Linux apps (device)
- [ ] Later: one engine shared by both hosts (today `linux/agent/engine.cpp` mirrors the Windows one)

## M1: Risk spikes
- [x] usbmuxd client (`common/usbmux`) + `tcprobe list` against Apple Devices 1.1540
- [x] `tcprobe echo` (Windows side of the echo spike)
- [x] USB transport verified with real-device `tcprobe stream`; separate echo spike superseded (M1)
- [x] Media source DLL (WRL) with placeholder and live frames; verified in-process
- [x] Register/remove via `MFCreateVirtualCamera` (`tethercam-agent --install/--uninstall/--session-camera`)
- [x] Frame Server loads the DLL; the Windows Camera app shows live iPhone video (2026-10-13)
- [x] Shared memory + events with SDDL (fixed: GX for SYNCHRONIZE)

## M2: Stream
- [x] Golden protocol vectors in `docs/protocol-vectors/v1.txt`
- [x] C++ protocol codec + tests (incl. fuzz)
- [x] C++ minimal plist + usbmux client + tests
- [x] Swift protocol codec + tests on the same vectors (macOS CI)
- [x] iOS `CameraController`: lens discovery, caps, format selection, rotation/mirror
- [x] iOS `H264Encoder`: VideoToolbox low-latency, forced IDR, Annex-B, SPS/PPS
- [x] iOS `Session`: state machine, heartbeat, status, backpressure, thermal policy, background pause
- [x] iOS UI: preview, status pill, lens menu, hide-preview toggle, idle timer off
- [x] `tcprobe stream` (usbmuxd or `--tcp`, stats, `--dump`, `--decode`, `--snapshot`)
- [x] `fakephone`: device simulator with live H.264 test pattern, two fake cameras

## M3: Decode + preview
- [x] Agent session loop (Hello, Caps, SetCamera/SetFormat/Start, reconnect)
- [x] MF H.264 decoder with D3D11 hardware path + software fallback, low-latency mode
- [x] Decode errors → flush + RequestKeyframe
- [x] 1080p30 latency measurement with a real iPhone: mean ~101 ms, range 89–112 ms (`docs/perf.md`); 4K30 pending

## M4: Webcam MVP
- [x] `windows/vcam`: Activator / MediaSource / MediaStream, pacing, allocator support
- [x] Shared-memory layout v1 with seqlock + tests
- [x] Agent shm writer; vcam reader with staleness → placeholder
- [x] Placeholder rendering (GDI → NV12), text per agent status
- [x] Demand-driven Start/Stop via the control block
- [x] Tray icon + menu (status, lens, mirror, rotate, logs, quit)
- [ ] Camera app / Teams / Zoom (admin, device) → fill `docs/compat.md`

## M5: Full resolution
- [x] 10 advertised media types; format chooser (exact / covering / largest)
- [x] CPU scaling + letterbox (GPU path landed in M10)
- [x] Bitrate table shared by C++ and Swift
- [x] Lens, mirror, rotation from the tray via `SetCamera` / `SetOptions`
- [x] iOS thermal policy (serious: half bitrate, ≤30 fps; critical: stop)
- [ ] 4K30/4K60 on a device; Lightning vs USB-C numbers

## M6: Robustness
- [x] Agent restart recovery; 3-minute soak with flat memory
- [x] Polling hot-plug (1 s); reconnect on 3 s silence
- [ ] Device: unplug/replug, lock/unlock, background/foreground timings
- [ ] PC sleep/resume
- [ ] Two consumers at once (admin)
- [ ] 1-hour soak with the iPhone

## M7: Release v0.1
- [x] WiX 5 MSI (`windows/installer`), built locally; CI job builds it
- [x] User guide, streaming/app guide, developer guide and README navigation; design and status API aligned with source
- [x] Install/uninstall/reinstall test (fixed: launch condition used msiexec's Windows 8.1 compat version)
- [ ] Code signing decision (unsigned MSI triggers SmartScreen)
- [ ] iOS distribution decision (Q5: TestFlight needs a paid account)
- [x] Release workflow with checksums (MSI + unsigned IPA); pre-release tag v0.1.0-alpha.1

## Session log
- 2026-10-16: Under-load performance + GPU path (M10). Windows: vcam samples wait on the agent's FrameReady event (2 ms late-frame grace) and carry the agent's capture timestamp (monotonic; foreign clock domains ignored; `MFSampleExtension_Discontinuity` on live/placeholder edges); `Fill` writes straight into the sample buffer; shared memory gets a global frame sequence + `PeekLatest`; the agent's media thread runs under MMCSS "Capture" (above-normal fallback) and stamps PTS from QPC anchored to `MFGetSystemTime()`; `Nv12Scaler` scales/letterboxes with `VideoProcessorBlt` on the decode device and the mapped result goes straight into shared memory (the decoded D3D11 surface is offered via `DecodedFrame`; a CPU view downloads lazily only when needed), CPU fallback on any failure. Linux: a ≤750 ms stall repeats the last frame instead of flickering black, v4l2 writes are non-blocking (drop, never stall the session), the YUYV pack is SSE2 and `image` resamples on two threads, plus `setpriority(-5)` where allowed and `CPUWeight=500` in the user unit. Verified: 37 Linux tests pass; load24 A/B 31.3 → 34.1 fps and gaps > 50 ms 9.8 % → 7.0 % (perf.md); SIGSTOP stall test holds the picture then goes black; idle 60.00 fps. Windows side builds green in CI after three MSVC fix rounds (`wil` raw-pointer assignment, `MFGetSystemTime` is in `mfidl.h`, nested `FrameSource::FillResult`, `avrt` for the agent tests); v0.1.12 published.
- 2026-10-15 (3): Linux preview window (GTK3, `linux/agent/preview.cpp`): live feed letterboxed, lens/mirror/rotate/quality toolbar, tray "Open Tethercam" and `--preview`; engine got `SetPreview`/`CopyPreview` and treats an open preview as a consumer. Verified with fakephone (streams, no GTK warnings); window not seen by eye.
- 2026-10-15 (2): GPU preference for decode: Linux tries dGPU then iGPU (VA-API, NVDEC on NVIDIA, `TETHERCAM_HW_DEVICE` override), verified on RX 9070 XT + Raphael with fakephone; Windows picks the adapter via `EnumAdapterByGpuPreference(HIGH_PERFORMANCE)` with an H.264 VLD check (needs a Windows build). Linux app icon fixed (hicolor has no 1024 dir; ship 16–512) and tray falls back to the app icon.
- 2026-10-15: Linux host planned as M9 (TECH_SPEC §13, R8–R9, Q6–Q7; PLAN phase 9; MILESTONES; PRODUCT). Moved the core to `common/`, made sockets/logging/usbmux portable (Unix-socket usbmuxd, `UNIX:` addresses), added `linux/` CMake root and `linux.yml` CI. Then the Linux agent: FFmpeg VA-API/software decode, v4l2loopback output with `/proc` opener detection, settings file, AppIndicator tray, flock single instance, fakephone on Linux, CPack .deb/.rpm per distro + CI matrix. Verified: fakephone → agent → /dev/video42 → v4l2-ctl at 29.6 fps 1080p (VA-API) and 4K30 NV12 (software), start/stop on open/close, status API live/connected; 37 tests pass on Ubuntu 24.04, 26.04 (GCC 15) and Fedora containers. Package install/uninstall verified in clean Ubuntu 26.04 and Fedora containers. Not verified: Windows build (CI), install on this desktop (needs sudo), real iPhone on Linux.
- 2026-10-13 (3): OBS/Streamlabs scene switches no longer stop the iPhone (vcam re-asserts `consumer_active`; v0.1.11). Streamlabs latency measured at ~135 ms glass-to-glass (perf.md); sync fix is on the Streamlabs side. Long USB cable caused drops. Open: measure VDO.Ninja the same way; the remaining ~20–30 ms (vcam pacing phase, 4K encoder frame delay) if worth it.
- 2026-10-13 (5): Added capture quality controls to both apps and a backward-compatible SetQuality protocol message. Added per-lens availability, reconnect policy, source-format reporting and stale capture-completion guards. Windows Debug build and 37 tests pass, including actual engine-loop synchronization/reconnect tests; Swift 6 standalone protocol check passes all 18 golden vectors and quality validation, and changed UI/session files pass syntax parsing. Release companion built at `build/quality-release/agent/Release/tethercam-agent.exe` because the usual Release executable is locked by a running process. Device/UI testing and full iOS build remain pending. Changes are local, uncommitted and not installed/published.
- 2026-10-13 (4): Resumed Claude session 7aa00575-0b51-4328-807d-61ebfe13eca4 after its documentation pass was interrupted. Completed USER_GUIDE, STREAMING, DEVELOPMENT and README; corrected DESIGN and STATUS_API against source and CI. Reconciled recorded latency/app checks without treating Edge as a Teams test or preview demand as a second virtual-camera client. 4K60 retest, 1-hour soak result and sleep/resume remain open. Documentation/link checks only; no new hardware tests.
- 2026-10-13 (3): Real iPhone 15 Pro Max verified over USB (1080p30/60, 4K30 through Frame Server). Fixed: AltStore install (ad-hoc signed IPA), permission prompt, phone preview, upside-down image (auto-rotation), lens reset on reconnect (Status.camera_id + saved choice). Added: Windows preview window with Camera-app controls (Fluent icons, eased motion), Camera-app-style iPhone UI, AltStore source, releases marked Latest.
- 2026-10-13 (2): One `ci` workflow builds Windows + iOS; when both pass on main it tags the next vX.Y.Z (patch bump, `[minor]`/`[major]` in the commit message) and publishes MSI + IPA + checksums; docs-only pushes skip CI. Version is stamped into the binaries, MSI and IPA. Replaces the manual-tag release.yml.
- 2026-10-13: Camera registered (elevated) and verified through Frame Server; MSI install/uninstall verified; repo published, CI green incl. Swift build + tests; CI builds a sideloadable unsigned IPA; release workflow added.
- 2026-10-12 (2): Built the whole Windows side (common, media, vcam, agent, tools, installer) and the iOS app code. Verified the Windows pipeline end to end with fakephone: hardware decode, 720p–4K negotiation, live frames through the media source, agent restart recovery, 3-minute soak. Blocked on: iPhone only visible over Wi-Fi (not trusted over USB), no admin rights for machine-wide camera registration (UAC was declined), no Mac for the iOS build.
- 2026-10-12: Windows skeleton builds with VS 2026; iOS skeleton and CI written. M0 committed (iOS build + CI run still to verify).
- 2026-10-12: Project planned.
