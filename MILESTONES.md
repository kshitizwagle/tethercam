# Milestones

Status key: ⬜ not started · 🟨 in progress · ✅ done. Details in [docs/PLAN.md](docs/PLAN.md); tasks in
[TODO.md](TODO.md).

| # | Milestone | Status |
|---|---|---|
| M0 | Foundations | 🟨 |
| M1 | Risk spikes | ⬜ |
| M2 | Stream (phone → PC bytes) | ⬜ |
| M3 | Decode + preview | ⬜ |
| M4 | Webcam MVP (1080p30) | ⬜ |
| M5 | Full resolution | ⬜ |
| M6 | Robustness | ⬜ |
| M7 | Release v0.1 | ⬜ |
| M8 | Backlog | ⬜ |

## M0: Foundations
**Goal:** a project anyone can clone, understand and build.
**Exit criteria:**
- [x] Product, tech spec, design, protocol and plan docs written
- [x] `windows/` CMake project builds empty `tcam_common`, `tethercam-agent`, `tethercam-vcam`, tests
- [ ] `ios/` XcodeGen project builds an empty app and test target (written; verify on the Mac)
- [ ] CI builds both on every push (workflows written; needs a GitHub remote)

## M1: Risk spikes
**Goal:** answer the questions that could change the architecture (R1–R3, Q1–Q2).
**Exit criteria:**
- [ ] A C++ tool connects through usbmuxd to an iOS echo app on port 5321 and round-trips 100 MB intact; throughput recorded
- [ ] Q1 answered (loopback-only listener reachable over USB)
- [ ] A spike DLL appears as "Tethercam" in the Windows Camera app showing a moving test pattern
- [ ] A user-mode process changes the picture through `Global\` shared memory created by the DLL
- [ ] TECH_SPEC updated with findings; fallback chosen if anything failed

## M2: Stream
**Goal:** real camera video leaves the phone and arrives on the PC.
**Exit criteria:**
- [ ] Protocol codec in C++ and Swift, both passing the same golden vectors
- [ ] iOS app streams 1080p30 H.264 on `Start` and stops on `Stop`
- [ ] `tcprobe` (over USB) saves a file that ffplay plays correctly
- [ ] `fakephone` replays that file over TCP

## M3: Decode + preview
**Goal:** the PC decodes the stream in hardware with low latency.
**Exit criteria:**
- [ ] Agent decodes with the MF hardware H.264 decoder into NV12 (D3D11)
- [ ] Debug preview window shows live video from the phone and from `fakephone`
- [ ] Glass-to-glass latency at 1080p30 measured and recorded in `docs/perf.md` (target < 100 ms)

## M4: Webcam MVP
**Goal:** the iPhone works as a 1080p30 webcam in real apps.
**Exit criteria:**
- [ ] "Tethercam" works in the Windows Camera app, Teams and Zoom at 1080p30
- [ ] Phone camera only runs while an app has Tethercam open
- [ ] Placeholder shown with no phone; video returns when the phone connects
- [ ] Tray icon shows grey/amber/green states
- [ ] `docs/compat.md` started

## M5: Full resolution
**Goal:** every video format the phone supports is usable.
**Exit criteria:**
- [ ] 720p, 1080p, 1440p, 4K at 30 and 60 fps selectable in OBS and delivered natively when the phone supports them
- [ ] Lens switching (wide / ultra-wide / tele / front), mirror and rotation work from tray and phone
- [ ] Scaling/letterboxing correct for unsupported requests
- [ ] 4K30 latency < 150 ms; thermal behavior over 30 min at 4K60 recorded
- [ ] Lightning vs USB-C throughput/bitrate results in `docs/perf.md`

## M6: Robustness
**Goal:** it just keeps working.
**Exit criteria:**
- [ ] Replug, lock/unlock, app background/foreground each recover in < 3 s
- [ ] PC sleep/resume and agent kill/restart recover without restarting the consuming app
- [ ] Two apps (e.g. OBS + Camera app) use Tethercam at the same time
- [ ] 1-hour 1080p30 soak: no disconnect, flat memory
- [ ] Parser fuzz tests run in CI

## M7: Release v0.1
**Goal:** a non-developer can install it.
**Exit criteria:**
- [ ] MSI installs, registers the camera, starts the agent at login; uninstall removes everything
- [ ] iOS app installable from source (and TestFlight if Q5 is resolved)
- [ ] User guide + troubleshooting in README
- [ ] Tagged GitHub release with checksums

## M8: Backlog
Audio (virtual microphone), HEVC, Windows 10 DirectShow fallback, focus/exposure, shared D3D11 textures,
ARM64, choosing between several phones.
