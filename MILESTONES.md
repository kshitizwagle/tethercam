# Milestones

Status key: ⬜ not started · 🟨 in progress · ✅ done. Details in [docs/PLAN.md](docs/PLAN.md); tasks in
[TODO.md](TODO.md). "fakephone" = verified on Windows with the simulated device; "device" = needs a real
iPhone; "admin" = needs the camera registered machine-wide (elevated install).

| # | Milestone | Status |
|---|---|---|
| M0 | Foundations | ✅ |
| M1 | Risk spikes | ✅ |
| M2 | Stream (phone → PC bytes) | ✅ |
| M3 | Decode + preview | ✅ hardware decode verified; 1080p30 latency measured at ~101 ms mean |
| M4 | Webcam MVP (1080p30) | 🟨 real iPhone verified through Frame Server, Camera app and Edge WebRTC; Teams/Zoom checks pending |
| M5 | Full resolution | 🟨 real-device 4K30 verified; sustained 4K60 and thermal validation pending |
| M6 | Robustness | 🟨 lock/unlock + replug + lens restore verified on device; sleep/resume, 1 h soak pending |
| M7 | Release v0.1 | ✅ every green main build publishes MSI + IPA as Latest; AltStore source auto-updated |
| M8 | Backlog | ⬜ |
| M9 | Linux host | 🟨 agent, tray, packages done and verified with fakephone; package install and real iPhone pending |
| M10 | Under-load performance + GPU path | ✅ Linux A/B; Windows load24 0–0.2 % frame loss through Frame Server |
| M11 | Wireless (Wi-Fi) transport | 🟨 Windows side verified with fakephone (pair → Bonjour → encrypted 1080p30 through the vcam); Linux + iOS written, need CI; real iPhone pending |

## M0: Foundations
**Goal:** a project anyone can clone, understand and build.
**Exit criteria:**
- [x] Product, tech spec, design, protocol and plan docs written
- [x] `windows/` CMake project builds `tcam_common`, `tethercam-agent`, `tethercam-vcam`, tests
- [x] `ios/` XcodeGen project builds the app and test target (macOS CI, 5 Swift tests pass)
- [x] CI builds both on every push (github.com/kshitizwagle/tethercam)

## M1: Risk spikes
**Goal:** answer the questions that could change the architecture (R1–R3, Q1–Q2).
**Exit criteria:**
- [x] USB tunnel to the phone works end to end (streaming verified on device; replaces the echo spike)
- [x] Q1 answered: the loopback-only listener is reachable through usbmuxd
- [x] Frame Server loads the DLL; "Tethercam (Windows Virtual Camera)" streams through the source reader (Camera app itself not checked)
- [x] Shared memory between the agent (user session) and the camera (FrameServer, session 0) works in `Global\`, including the DACL fix
- [x] R3 answered: Apple Devices provides usbmuxd on 27015 (TECH_SPEC)
- [x] Q2 partly answered: per-user registration fails, machine-wide required (TECH_SPEC)

## M2: Stream
**Goal:** real camera video leaves the phone and arrives on the PC.
**Exit criteria:**
- [x] Protocol codec in C++ and Swift; both pass the same golden vectors
- [x] iOS app streams H.264 on `Start` and returns to preview on `Stop` (iPhone 15 Pro Max)
- [x] Stream decodes on the PC (agent hardware decode, 1080p30/60 and 4K30)
- [x] `fakephone` serves an H.264 stream over TCP (generates it live instead of replaying a file)

## M3: Decode + preview
**Goal:** the PC decodes the stream in hardware with low latency.
**Exit criteria:**
- [x] Agent decodes with the MF hardware H.264 decoder into NV12 (D3D11), software fallback
- [x] Decoded frames checked visually (`tcprobe stream --decode --snapshot`, `vcamcheck --save`) instead of a preview window
- [x] Glass-to-glass latency at 1080p30 measured: ~101 ms mean, 89–112 ms; strict <100 ms target not established (`docs/perf.md`)

## M4: Webcam MVP
**Goal:** the iPhone works as a 1080p30 webcam in real apps.
**Exit criteria:**
- [ ] "Tethercam" works in the Windows Camera app, Teams and Zoom at 1080p30 (admin, device)
- [x] Phone camera only runs while an app has Tethercam open (Start on consumer, Stop 2 s after the last one; fakephone)
- [x] Placeholder shown with no agent/phone; live video resumes automatically
- [x] Tray icon shows grey/amber/green states
- [x] `docs/compat.md` started

## M5: Full resolution
**Goal:** every video format the phone supports is usable.
**Exit criteria:**
- [x] 720p, 1080p, 1440p, 4K at 30/60 advertised and negotiated natively when the device offers them (fakephone; 4K60 needs a device that offers it)
- [x] Lens switching, mirror and rotation wired from the tray through the protocol (device behavior unverified)
- [x] Scaling/letterboxing for formats the device can't produce (unit-tested)
- [ ] 4K30 latency < 150 ms; 30 min at 4K60 thermals (device)
- [ ] Lightning vs USB-C throughput results in `docs/perf.md` (device)

## M6: Robustness
**Goal:** it just keeps working.
**Exit criteria:**
- [x] Replug and lock/unlock recover on their own; the chosen lens is restored (device, 0.1.4)
- [x] Agent kill/restart recovers without the consuming app reopening the camera
- [ ] PC sleep/resume (manual test)
- [ ] Two apps use Tethercam at the same time (admin; relies on Frame Server sharing)
- [ ] 1-hour device soak result recorded; prior 3-minute simulated soak was clean with flat memory
- [x] Parser fuzz tests run in the unit tests (CI once the remote exists)

## M7: Release v0.1
**Goal:** a non-developer can install it.
**Exit criteria:**
- [x] MSI builds (WiX 5): Program Files install, COM + virtual camera registration, agent at login, uninstall cleanup
- [x] MSI install / uninstall / reinstall verified (files, HKLM COM key, Run key, camera present/absent)
- [x] iOS app: unsigned sideloadable IPA built by CI (Sideloadly/AltStore)
- [x] User guide + troubleshooting, streaming/app guide, developer guide and README navigation
- [x] Every green build of main is tagged `vX.Y.Z` and released as Latest with MSI + IPA + SHA256SUMS (`ci.yml`; docs-only pushes skip CI)

## M8: Backlog
Audio (virtual microphone), HEVC, Windows 10 DirectShow fallback, focus/exposure, shared D3D11
textures (GPU scaling landed in M10), Swift 6 language mode, ARM64, choosing between several phones,
usbmuxd `Listen` instead of polling.

## M9: Linux host
**Goal:** the same iPhone works as a webcam on Linux desktops, installed from a .deb or .rpm.
**Exit criteria:**
- [x] Shared core moved to top-level `common/` and builds on Windows (MSVC) and Linux (GCC/Clang); unit tests pass on both in CI
- [x] `linux/` agent: usbmuxd over `/var/run/usbmuxd`, FFmpeg decode (VA-API + software fallback), writes NV12/YUYV to a v4l2loopback device (fakephone, 1080p30 VA-API and 4K30 software verified)
- [x] Black frame with no phone; demand-driven Start/Stop when a client opens the camera (fakephone)
- [x] Status API on 127.0.0.1:4790 unchanged (STATUS_API.md contract; shared code + contract test)
- [x] Tray via StatusNotifierItem (KDE; GNOME with the AppIndicator extension); systemd user service starts the agent at login (tray starts on Ubuntu 26.04 GNOME/Wayland; login autostart not yet tried from an installed package)
- [ ] `.deb` + `.rpm` built in CI with dependencies on `v4l2loopback-dkms` and `usbmuxd` (built and tested in Ubuntu 24.04/26.04 and Fedora containers; install → service enabled → agent runs → uninstall → service disabled verified in clean Ubuntu 26.04 and Fedora containers); install on a real desktop with the module loaded pending (admin)
- [ ] Real iPhone verified in Firefox, Chromium, OBS and Zoom on Linux (device)

## M10: Under-load performance + GPU path
**Goal:** the webcam keeps its frame rate and its clock while the machine is busy (the load-test failures).
**Exit criteria:**
- [x] vcam: samples wait on the agent's FrameReady event with a bounded late-frame grace; live frames carry the agent's capture timestamp, repeats advance the clock, `MFSampleExtension_Discontinuity` marks live edges
- [x] vcam: fill goes straight into the sample buffer (one copy instead of two)
- [x] Shared memory: a global frame sequence + `PeekLatest`, so consumers tell fresh from repeat exactly
- [x] Agent: MMCSS "Capture" (high) for the media thread with an above-normal fallback; PTS from QPC anchored to `MFGetSystemTime()`
- [x] Agent: GPU scale + letterbox (`VideoProcessorBlt` on the decode device) with the CPU path as fallback; decoded D3D11 surfaces are handed over without a download (`DecodedFrame`)
- [x] Linux: a stall repeats the last frame (≤ 750 ms) instead of flickering black; non-blocking writes drop frames instead of stalling the session; SSE2 YUYV pack; two-thread resample; `setpriority` + `CPUWeight=500`
- [x] Windows build, tests and MSI green in CI (v0.1.12)
- [x] vcam: samples go out as soon as a new frame arrives within their interval (fixed cadence, repeat only at the interval's end); replaces the free-running timer + 2 ms grace (R10)
- [x] Windows load harness (`windows\scripts\load-test.ps1`; target: ≤ 10 % frame loss at load24): idle 60.0 fps, load24 0–0.2 % loss (2026-10-16); the Linux A/B measured 31.3 → 34.1 fps with fewer long gaps (`docs/perf.md`)

## M11: Wireless (Wi-Fi) transport
**Goal:** a paired PC uses the iPhone over the local Wi-Fi network, encrypted end to end, with USB still
the default (TECH_SPEC §14, PROTOCOL § Wireless transport).
**Exit criteria:**
- [x] Scope change documented: PRODUCT goals/non-goals, TECH_SPEC F10–F12, N4/N5, §14, R11–R12, Q8–Q9, PROTOCOL wireless section
- [x] Monocypher vendored; sessions run over a `Link` on both hosts with no behavior change (Windows tests green; Linux needs CI)
- [x] Noise NNpsk0 sessions + encrypted records pass the published Noise vector (C++ and Swift) and fuzz tests; fakephone → agent over Wi-Fi streams on Windows (fakephone; Linux not built here)
- [x] `WifiPair` over `--tcp` stores the key on the PC (DPAPI) and in fakephone; an unpaired host or wrong key is refused (fakephone; Keychain side needs the device)
- [ ] Bonjour discovery (Windows DnsServiceBrowse, Linux Avahi) + manual address; USB preferred and taken over mid-session (F12) (written; Windows discovery verified with `fakephone --advertise`; Avahi and USB takeover unverified)
- [ ] Tray/GTK: paired phone, editable address, Forget; Status API `transport` field (built; `transport` verified; menus not yet clicked by eye)
- [ ] iOS: Wireless toggle, Wi-Fi listener + Bonjour, `WifiPair` handling, paired PCs with Forget, Keychain store, Local Network permission (Mac/CI) (written; Noise/WifiChannel compiled and checked with swiftc here; app build pending in CI)
- [ ] Real iPhone over Wi-Fi: pair over USB, unplug, 1080p30/60, USB handover, latency and frame loss vs USB in `docs/perf.md` (device)
- [x] USER_GUIDE, README, STATUS_API, DESIGN and CONTRIBUTING updated (Wi-Fi marked as not yet verified with a real iPhone)
