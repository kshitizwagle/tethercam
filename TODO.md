# TODO

Work from the top. Tick items in the same commit that finishes them. Milestone exit criteria live in
[MILESTONES.md](MILESTONES.md).

## Next up (needs you: Mac, iPhone, admin rights, GitHub)
- [ ] **Mac:** `cd ios && xcodegen`, build, fix any compile errors (the Swift code has never been compiled), run `WireTests`, install on the iPhone
- [ ] **iPhone:** unlock, plug in, tap Trust; `tcprobe list` must show `type=USB` (today it only shows `type=Network`)
- [ ] **Device:** `tcprobe stream --decode --snapshot snap.bmp` against the real app; answer Q1
- [ ] **Admin:** run `windows\scripts\dev-install.ps1` elevated (or install the MSI), then `vcamcheck list` / `vcamcheck read`, and the Windows Camera app
- [ ] **GitHub:** create the repo (public, GPL-3.0), push, confirm both CI workflows pass

## M1: Risk spikes
- [x] usbmuxd client (`common/usbmux`) + `tcprobe list` against Apple Devices 1.1540
- [x] `tcprobe echo` (Windows side of the echo spike)
- [ ] iOS echo: the real app's server is used instead of a separate echo app; verify a 100 MB round trip with a debug echo mode, or skip in favour of `tcprobe stream`
- [x] Media source DLL (WRL) with placeholder and live frames; verified in-process
- [x] Register/remove via `MFCreateVirtualCamera` (`tethercam-agent --install/--uninstall/--session-camera`)
- [ ] Confirm Frame Server loads the DLL and the camera plays in the Windows Camera app (admin)
- [x] Shared memory + events with SDDL (fixed: GX for SYNCHRONIZE)

## M2: Stream
- [x] Golden protocol vectors in `docs/protocol-vectors/v1.txt`
- [x] C++ protocol codec + tests (incl. fuzz)
- [x] C++ minimal plist + usbmux client + tests
- [x] Swift protocol codec + tests on the same vectors (not compiled yet)
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
- [ ] Latency measurement with a real iPhone (`docs/perf.md`)

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
- [x] CPU scaling + letterbox (GPU path is M8)
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
- [x] User guide + troubleshooting in README
- [ ] Install/uninstall test on a clean Windows 11 machine
- [ ] Code signing decision (unsigned MSI triggers SmartScreen)
- [ ] iOS distribution decision (Q5: TestFlight needs a paid account)
- [ ] Release workflow with checksums; tag v0.1.0

## Session log
- 2026-10-12 (2): Built the whole Windows side (common, media, vcam, agent, tools, installer) and the iOS app code. Verified the Windows pipeline end to end with fakephone: hardware decode, 720p–4K negotiation, live frames through the media source, agent restart recovery, 3-minute soak. Blocked on: iPhone only visible over Wi-Fi (not trusted over USB), no admin rights for machine-wide camera registration (UAC was declined), no Mac for the iOS build.
- 2026-10-12: Windows skeleton builds with VS 2026; iOS skeleton and CI written. M0 committed (iOS build + CI run still to verify).
- 2026-10-12: Project planned.
