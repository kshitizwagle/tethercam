# Milestones

Status key: ⬜ not started · 🟨 in progress · ✅ done. Details in [docs/PLAN.md](docs/PLAN.md); tasks in
[TODO.md](TODO.md). "fakephone" = verified on Windows with the simulated device; "device" = needs a real
iPhone; "admin" = needs the camera registered machine-wide (elevated install).

| # | Milestone | Status |
|---|---|---|
| M0 | Foundations | 🟨 iOS skeleton build + CI run pending |
| M1 | Risk spikes | 🟨 Windows side done; USB echo + Frame Server load pending (device, admin) |
| M2 | Stream (phone → PC bytes) | 🟨 Windows side + iOS code written; iOS build/run pending (Mac, device) |
| M3 | Decode + preview | 🟨 hardware decode verified; real-phone latency pending |
| M4 | Webcam MVP (1080p30) | 🟨 pipeline verified in-process; real apps pending (admin, device) |
| M5 | Full resolution | 🟨 720p–4K negotiation and scaling verified with fakephone; device tests pending |
| M6 | Robustness | 🟨 agent restart + soak verified; phone-side scenarios pending |
| M7 | Release v0.1 | 🟨 MSI builds; install test + release pending |
| M8 | Backlog | ⬜ |

## M0: Foundations
**Goal:** a project anyone can clone, understand and build.
**Exit criteria:**
- [x] Product, tech spec, design, protocol and plan docs written
- [x] `windows/` CMake project builds `tcam_common`, `tethercam-agent`, `tethercam-vcam`, tests
- [ ] `ios/` XcodeGen project builds the app and test target (written; verify on the Mac)
- [ ] CI builds both on every push (workflows written; needs a GitHub remote)

## M1: Risk spikes
**Goal:** answer the questions that could change the architecture (R1–R3, Q1–Q2).
**Exit criteria:**
- [ ] `tcprobe echo` round-trips 100 MB through usbmuxd to the phone; throughput recorded (device)
- [ ] Q1 answered: loopback-only listener reachable over USB (device)
- [ ] "Tethercam" appears in the Windows Camera app (admin)
- [x] Shared memory between agent and camera works, including the DACL fix (session namespace; Global namespace to confirm with admin install)
- [x] R3 answered: Apple Devices provides usbmuxd on 27015 (TECH_SPEC)
- [x] Q2 partly answered: per-user registration fails, machine-wide required (TECH_SPEC)

## M2: Stream
**Goal:** real camera video leaves the phone and arrives on the PC.
**Exit criteria:**
- [x] Protocol codec in C++ and Swift; C++ passes the golden vectors (Swift tests written, run on the Mac)
- [ ] iOS app streams 1080p30 H.264 on `Start` and stops on `Stop` (device)
- [ ] `tcprobe stream --dump` over USB saves a file that ffplay plays (device)
- [x] `fakephone` serves an H.264 stream over TCP (generates it live instead of replaying a file)

## M3: Decode + preview
**Goal:** the PC decodes the stream in hardware with low latency.
**Exit criteria:**
- [x] Agent decodes with the MF hardware H.264 decoder into NV12 (D3D11), software fallback
- [x] Decoded frames checked visually (`tcprobe stream --decode --snapshot`, `vcamcheck --save`) instead of a preview window
- [ ] Glass-to-glass latency at 1080p30 measured (device; method in `docs/perf.md`)

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
- [ ] Replug, lock/unlock, app background/foreground recover in < 3 s (device; code paths written)
- [x] Agent kill/restart recovers without the consuming app reopening the camera
- [ ] PC sleep/resume (manual test)
- [ ] Two apps use Tethercam at the same time (admin; relies on Frame Server sharing)
- [x] 1-hour soak target: 3-minute soak at 1080p30 clean with flat memory (extend to 1 h on a device)
- [x] Parser fuzz tests run in the unit tests (CI once the remote exists)

## M7: Release v0.1
**Goal:** a non-developer can install it.
**Exit criteria:**
- [x] MSI builds (WiX 5): Program Files install, COM + virtual camera registration, agent at login, uninstall cleanup
- [ ] MSI install/uninstall tested on a clean machine (admin)
- [ ] iOS app installable from source (and TestFlight if Q5 is resolved) (Mac)
- [x] User guide + troubleshooting in README
- [ ] Tagged GitHub release with checksums (needs the GitHub repo)

## M8: Backlog
Audio (virtual microphone), HEVC, Windows 10 DirectShow fallback, focus/exposure, GPU scaling and shared
D3D11 textures, Swift 6 language mode, ARM64, choosing between several phones, usbmuxd `Listen` instead of
polling.
